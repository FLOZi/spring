/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/Platform/Win/win32.h"

#include "PathEstimator.h"

#include <fstream>
#include <boost/bind.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/thread/thread.hpp>

#include "minizip/zip.h"

#include "PathFinder.h"
#include "PathFinderDef.h"
#include "PathFlowMap.hpp"
#include "PathLog.h"
#include "Game/LoadScreen.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"
#include "Net/Protocol/NetProtocol.h"
#include "System/ThreadPool.h"
#include "System/TimeProfiler.h"
#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/Archives/IArchive.h"
#include "System/FileSystem/ArchiveLoader.h"
#include "System/FileSystem/DataDirsAccess.h"
#include "System/FileSystem/FileSystem.h"
#include "System/FileSystem/FileQueryFlags.h"


CONFIG(int, MaxPathCostsMemoryFootPrint).defaultValue(512).minimumValue(64).description("Maximum memusage (in MByte) of mutlithreaded pathcache generator at loading time.");



static const std::string GetPathCacheDir() {
	return (FileSystem::GetCacheDir() + "/paths/");
}

static size_t GetNumThreads() {
	const size_t numThreads = std::max(0, configHandler->GetInt("PathingThreadCount"));
	const size_t numCores = Threading::GetLogicalCpuCores();
	return ((numThreads == 0)? numCores: numThreads);
}



CPathEstimator::CPathEstimator(CPathFinder* pf, unsigned int BLOCK_SIZE, const std::string& cacheFileName, const std::string& mapFileName)
	: IPathFinder(BLOCK_SIZE)
	, BLOCKS_TO_UPDATE(SQUARES_TO_UPDATE / (BLOCK_SIZE * BLOCK_SIZE) + 1)
	, nextOffsetMessageIdx(0)
	, nextCostMessageIdx(0)
	, pathChecksum(0)
	, offsetBlockNum(nbrOfBlocks.x * nbrOfBlocks.y)
	, costBlockNum(nbrOfBlocks.x * nbrOfBlocks.y)
	, pathFinder(pf)
	, blockUpdatePenalty(0)
{
	vertexCosts.resize(moveDefHandler->GetNumMoveDefs() * blockStates.GetSize() * PATH_DIRECTION_VERTICES, PATHCOST_INFINITY);

	// load precalculated data if it exists
	InitEstimator(cacheFileName, mapFileName);
}


CPathEstimator::~CPathEstimator()
{
	delete pathCache[0]; pathCache[0] = NULL;
	delete pathCache[1]; pathCache[1] = NULL;
}


const int2* CPathEstimator::GetDirectionVectorsTable() {
	return (&PE_DIRECTION_VECTORS[0]);
}


void CPathEstimator::InitEstimator(const std::string& cacheFileName, const std::string& map)
{
	const unsigned int numThreads = GetNumThreads();

	if (threads.size() != numThreads) {
		threads.resize(numThreads);
		pathFinders.resize(numThreads);
	}

	pathFinders[0] = pathFinder;

	// Not much point in multithreading these...
	InitBlocks();

	if (!ReadFile(cacheFileName, map)) {
		// start extra threads if applicable, but always keep the total
		// memory-footprint made by CPathFinder instances within bounds
		const unsigned int minMemFootPrint = sizeof(CPathFinder) + pathFinder->GetMemFootPrint();
		const unsigned int maxMemFootPrint = configHandler->GetInt("MaxPathCostsMemoryFootPrint") * 1024 * 1024;
		const unsigned int numExtraThreads = Clamp(int(maxMemFootPrint / minMemFootPrint) - 1, 0, int(numThreads) - 1);
		const unsigned int reqMemFootPrint = minMemFootPrint * (numExtraThreads + 1);

		{
			char calcMsg[512];
			const char* fmtString = (numExtraThreads > 0)?
				"PathCosts: creating PE%u cache with %u PF threads (%u MB)":
				"PathCosts: creating PE%u cache with %u PF thread (%u MB)";

			sprintf(calcMsg, fmtString, BLOCK_SIZE, numExtraThreads + 1, reqMemFootPrint / (1024 * 1024));
			loadscreen->SetLoadMessage(calcMsg);
		}

		// note: only really needed if numExtraThreads > 0
		pathBarrier = new boost::barrier(numExtraThreads + 1);

		for (unsigned int i = 1; i <= numExtraThreads; i++) {
			pathFinders[i] = new CPathFinder();
			threads[i] = new boost::thread(boost::bind(&CPathEstimator::CalcOffsetsAndPathCosts, this, i));
		}

		// Use the current thread as thread zero
		CalcOffsetsAndPathCosts(0);

		for (unsigned int i = 1; i <= numExtraThreads; i++) {
			threads[i]->join();
			delete threads[i];
			delete pathFinders[i];
		}

		delete pathBarrier;

		loadscreen->SetLoadMessage("PathCosts: writing", true);
		WriteFile(cacheFileName, map);
		loadscreen->SetLoadMessage("PathCosts: written", true);
	}

	pathCache[0] = new CPathCache(nbrOfBlocks.x, nbrOfBlocks.y);
	pathCache[1] = new CPathCache(nbrOfBlocks.x, nbrOfBlocks.y);
}


void CPathEstimator::InitBlocks()
{
	for (unsigned int idx = 0; idx < blockStates.GetSize(); idx++) {
		blockStates.peNodeOffsets[idx].resize(moveDefHandler->GetNumMoveDefs());
	}
}


__FORCE_ALIGN_STACK__
void CPathEstimator::CalcOffsetsAndPathCosts(unsigned int threadNum)
{
	// reset FPU state for synced computations
	streflop::streflop_init<streflop::Simple>();

	if (threadNum > 0) {
		// FIXME: not running any thread on core 0 is a big perf-hit
		// Threading::SetAffinity(1 << threadNum);
		Threading::SetAffinity(~0);
		Threading::SetThreadName(IntToString(threadNum, "pathhelper%i"));
	}

	// NOTE: EstimatePathCosts() [B] is temporally dependent on CalculateBlockOffsets() [A],
	// A must be completely finished before B_i can be safely called. This means we cannot
	// let thread i execute (A_i, B_i), but instead have to split the work such that every
	// thread finishes its part of A before any starts B_i.
	const unsigned int maxBlockIdx = blockStates.GetSize() - 1;
	int i;

	while ((i = --offsetBlockNum) >= 0)
		CalculateBlockOffsets(maxBlockIdx - i, threadNum);

	pathBarrier->wait();

	while ((i = --costBlockNum) >= 0)
		EstimatePathCosts(maxBlockIdx - i, threadNum);
}


void CPathEstimator::CalculateBlockOffsets(unsigned int blockIdx, unsigned int threadNum)
{
	const int2 blockPos = BlockIdxToPos(blockIdx);

	if (threadNum == 0 && blockIdx >= nextOffsetMessageIdx) {
		nextOffsetMessageIdx = blockIdx + blockStates.GetSize() / 16;
		net->Send(CBaseNetProtocol::Get().SendCPUUsage(BLOCK_SIZE | (blockIdx << 8)));
	}

	for (unsigned int i = 0; i < moveDefHandler->GetNumMoveDefs(); i++) {
		const MoveDef* md = moveDefHandler->GetMoveDefByPathType(i);

		if (md->udRefCount > 0) {
			blockStates.peNodeOffsets[blockIdx][md->pathType] = FindOffset(*md, blockPos.x, blockPos.y);
		}
	}
}


void CPathEstimator::EstimatePathCosts(unsigned int blockIdx, unsigned int threadNum)
{
	const int2 blockPos = BlockIdxToPos(blockIdx);

	if (threadNum == 0 && blockIdx >= nextCostMessageIdx) {
		nextCostMessageIdx = blockIdx + blockStates.GetSize() / 16;

		char calcMsg[128];
		sprintf(calcMsg, "PathCosts: precached %d of %d blocks", blockIdx, blockStates.GetSize());

		net->Send(CBaseNetProtocol::Get().SendCPUUsage(0x1 | BLOCK_SIZE | (blockIdx << 8)));
		loadscreen->SetLoadMessage(calcMsg, (blockIdx != 0));
	}

	for (unsigned int i = 0; i < moveDefHandler->GetNumMoveDefs(); i++) {
		const MoveDef* md = moveDefHandler->GetMoveDefByPathType(i);

		if (md->udRefCount > 0) {
			CalculateVertices(*md, blockPos, threadNum);
		}
	}
}


/**
 * Finds a square accessable by the given MoveDef within the given block
 */
int2 CPathEstimator::FindOffset(const MoveDef& moveDef, unsigned int blockX, unsigned int blockZ) const
{
	// lower corner position of block
	const unsigned int lowerX = blockX * BLOCK_SIZE;
	const unsigned int lowerZ = blockZ * BLOCK_SIZE;
	const unsigned int blockArea = (BLOCK_SIZE * BLOCK_SIZE) / SQUARE_SIZE;

	unsigned int bestPosX = BLOCK_SIZE >> 1;
	unsigned int bestPosZ = BLOCK_SIZE >> 1;

	float bestCost = std::numeric_limits<float>::max();
	float speedMod = CMoveMath::GetPosSpeedMod(moveDef, lowerX, lowerZ);
	bool curblock = (speedMod == 0.0f) || CMoveMath::IsBlockedStructure(moveDef, lowerX, lowerZ, NULL);

	// search for an accessible position within this block
	for (unsigned int z = 0; z < BLOCK_SIZE; ++z) {
		bool zcurblock = curblock;

		for (unsigned int x = 0; x < BLOCK_SIZE; ++x) {
			if (!curblock) {
				const float dx = x - (float)(BLOCK_SIZE - 1) / 2.0f;
				const float dz = z - (float)(BLOCK_SIZE - 1) / 2.0f;
				const float cost = (dx * dx + dz * dz) + (blockArea / (0.001f + speedMod));

				if (cost < bestCost) {
					bestCost = cost;
					bestPosX = x;
					bestPosZ = z;
				}
			}

			// if last position was not blocked, then we do not need to check the entire square
			speedMod = CMoveMath::GetPosSpeedMod(moveDef, lowerX + x, lowerZ + z);
			curblock = (speedMod == 0.0f) || (curblock ?
				CMoveMath::IsBlockedStructure(moveDef, lowerX + x, lowerZ + z, NULL) :
				CMoveMath::IsBlockedStructureXmax(moveDef, lowerX + x, lowerZ + z, NULL));
		}

		speedMod = CMoveMath::GetPosSpeedMod(moveDef, lowerX, lowerZ + z);
		curblock = (speedMod == 0.0f) || (zcurblock ?
			CMoveMath::IsBlockedStructure(moveDef, lowerX, lowerZ + z, NULL) :
			CMoveMath::IsBlockedStructureZmax(moveDef, lowerX, lowerZ + z, NULL));
	}

	// return the offset found
	return int2(blockX * BLOCK_SIZE + bestPosX, blockZ * BLOCK_SIZE + bestPosZ);
}


/**
 * Calculate all vertices connected from the given block
 */
void CPathEstimator::CalculateVertices(const MoveDef& moveDef, int2 block, unsigned int thread)
{
	// see code comment of GetBlockVertexOffset() for more info why those directions are choosen
	CalculateVertex(moveDef, block, PATHDIR_LEFT,     thread);
	CalculateVertex(moveDef, block, PATHDIR_LEFT_UP,  thread);
	CalculateVertex(moveDef, block, PATHDIR_UP,       thread);
	CalculateVertex(moveDef, block, PATHDIR_RIGHT_UP, thread);
}


/**
 * Calculate requested vertex
 */
void CPathEstimator::CalculateVertex(
	const MoveDef& moveDef,
	int2 parentBlock,
	unsigned int direction,
	unsigned int threadNum)
{
	const int2 childBlock = parentBlock + PE_DIRECTION_VECTORS[direction];
	const unsigned int parentBlockNbr = BlockPosToIdx(parentBlock);
	const unsigned int childBlockNbr  = BlockPosToIdx(childBlock);
	const unsigned int vertexNbr =
		moveDef.pathType * blockStates.GetSize() * PATH_DIRECTION_VERTICES +
		parentBlockNbr * PATH_DIRECTION_VERTICES +
		direction;

	// outside map?
	if ((unsigned)childBlock.x >= nbrOfBlocks.x || (unsigned)childBlock.y >= nbrOfBlocks.y) {
		vertexCosts[vertexNbr] = PATHCOST_INFINITY;
		return;
	}


	// start position within parent block
	const int2 parentSquare = blockStates.peNodeOffsets[parentBlockNbr][moveDef.pathType];

	// goal position within child block
	const int2 childSquare = blockStates.peNodeOffsets[childBlockNbr][moveDef.pathType];
	const float3 startPos  = SquareToFloat3(parentSquare.x, parentSquare.y);
	const float3 goalPos   = SquareToFloat3(childSquare.x, childSquare.y);

	// keep search exactly contained within the two blocks
	CRectangularSearchConstraint pfDef(startPos, goalPos, BLOCK_SIZE);

	// we never want to allow searches from
	// any blocked starting positions (otherwise PE and PF can disagree)
	// note: PE itself should ensure this never happens to begin with?
	//
	// be more lenient for normal searches so players can "unstuck" units
	//
	// blocked goal positions are always early-outs (no searching needed)
	const bool strtBlocked = ((CMoveMath::IsBlocked(moveDef, startPos, nullptr) & CMoveMath::BLOCK_STRUCTURE) != 0);
	const bool goalBlocked = pfDef.GoalIsBlocked(moveDef, CMoveMath::BLOCK_STRUCTURE, nullptr);
	if (strtBlocked || goalBlocked) {
		vertexCosts[vertexNbr] = PATHCOST_INFINITY;
		return;
	}

	// find path from parent to child block
	//
	// since CPathFinder::GetPath() is not thread-safe, use
	// this thread's "private" CPathFinder instance (rather
	// than locking pathFinder->GetPath()) if we are in one
	IPath::Path path;
	IPath::SearchResult result = pathFinders[threadNum]->GetPath(moveDef, pfDef, nullptr, startPos, path, MAX_SEARCHED_NODES_PF >> 2, false, false, true, true);

	// store the result
	if (result == IPath::Ok) {
		vertexCosts[vertexNbr] = path.pathCost;
	} else {
		vertexCosts[vertexNbr] = PATHCOST_INFINITY;
	}
}


/**
 * Mark affected blocks as obsolete
 */
void CPathEstimator::MapChanged(unsigned int x1, unsigned int z1, unsigned int x2, unsigned z2)
{
	// find the upper and lower corner of the rectangular area
	const auto mmx = std::minmax(x1, x2);
	const auto mmz = std::minmax(z1, z2);
	const int lowerX = Clamp<int>(mmx.first  / BLOCK_SIZE, 0, int(nbrOfBlocks.x - 1));
	const int upperX = Clamp<int>(mmx.second / BLOCK_SIZE, 0, int(nbrOfBlocks.x - 1));
	const int lowerZ = Clamp<int>(mmz.first  / BLOCK_SIZE, 0, int(nbrOfBlocks.y - 1));
	const int upperZ = Clamp<int>(mmz.second / BLOCK_SIZE, 0, int(nbrOfBlocks.y - 1));

	// mark the blocks inside the rectangle, enqueue them
	// from upper to lower because of the placement of the
	// bi-directional vertices
	for (int z = upperZ; z >= lowerZ; z--) {
		for (int x = upperX; x >= lowerX; x--) {
			const int idx = BlockPosToIdx(int2(x,z));
			if ((blockStates.nodeMask[idx] & PATHOPT_OBSOLETE) != 0)
				continue;

			updatedBlocks.emplace_back(x, z);
			blockStates.nodeMask[idx] |= PATHOPT_OBSOLETE;
		}
	}
}


/**
 * Update some obsolete blocks using the FIFO-principle
 */
void CPathEstimator::Update()
{
	pathCache[0]->Update();
	pathCache[1]->Update();

	const auto numMoveDefs = moveDefHandler->GetNumMoveDefs();

	// determine how many blocks we should update
	int blocksToUpdate = 0;
	int consumeBlocks = 0;
	{
		const int progressiveUpdates = updatedBlocks.size() * numMoveDefs * ((BLOCK_SIZE >= 16)? 1.0f : 0.6f) * modInfo.pfUpdateRate;
		const int MIN_BLOCKS_TO_UPDATE = std::max<int>(BLOCKS_TO_UPDATE >> 1, 4U);
		const int MAX_BLOCKS_TO_UPDATE = std::max<int>(BLOCKS_TO_UPDATE << 1, MIN_BLOCKS_TO_UPDATE);
		blocksToUpdate = Clamp(progressiveUpdates, MIN_BLOCKS_TO_UPDATE, MAX_BLOCKS_TO_UPDATE);

		blockUpdatePenalty = std::max(0, blockUpdatePenalty - blocksToUpdate);

		if (blockUpdatePenalty > 0)
			blocksToUpdate = std::max(0, blocksToUpdate - blockUpdatePenalty);

		// we have to update blocks for all movedefs (cause PATHOPT_OBSOLETE is per block and not movedef)
		consumeBlocks = int(progressiveUpdates != 0) * int(ceil(float(blocksToUpdate) / numMoveDefs)) * numMoveDefs;

		blockUpdatePenalty += consumeBlocks;
	}

	if (blocksToUpdate == 0)
		return;

	if (updatedBlocks.empty())
		return;

	struct SingleBlock {
		int2 blockPos;
		const MoveDef* moveDef;
		SingleBlock(const int2& pos, const MoveDef* md) : blockPos(pos), moveDef(md) {}
	};
	std::vector<SingleBlock> consumedBlocks;
	consumedBlocks.reserve(consumeBlocks);

	// get blocks to update
	while (!updatedBlocks.empty()) {
		int2& pos = updatedBlocks.front();
		const int idx = BlockPosToIdx(pos);

		if ((blockStates.nodeMask[idx] & PATHOPT_OBSOLETE) == 0) {
			updatedBlocks.pop_front();
			continue;
		}

		if (consumedBlocks.size() >= blocksToUpdate) {
			break;
		}

		// no need to check for duplicates, because FindOffset is deterministic
		// so even when we compute it multiple times the result will be the same
		for (unsigned int i = 0; i < numMoveDefs; i++) {
			const MoveDef* md = moveDefHandler->GetMoveDefByPathType(i);
			if (md->udRefCount > 0)
				consumedBlocks.emplace_back(pos, md);
		}
		updatedBlocks.pop_front();
	}

	// FindOffset (threadsafe)
	{
		SCOPED_TIMER("CPathEstimator::FindOffset");
		for_mt(0, consumedBlocks.size(), [&](const int n) {
			// copy the next block in line
			const SingleBlock sb = consumedBlocks[n];
			const int blockN = BlockPosToIdx(sb.blockPos);
			const MoveDef* currBlockMD = sb.moveDef;
			blockStates.peNodeOffsets[blockN][currBlockMD->pathType] = FindOffset(*currBlockMD, sb.blockPos.x, sb.blockPos.y);
		});
	}

	// CalculateVertices (not threadsafe)
	{
		SCOPED_TIMER("CPathEstimator::CalculateVertices");
		for (unsigned int n = 0; n < consumedBlocks.size(); ++n) {
			// copy the next block in line
			const SingleBlock sb = consumedBlocks[n];
			const int blockN = BlockPosToIdx(sb.blockPos);
			const MoveDef* currBlockMD = sb.moveDef;
			const MoveDef* nextBlockMD = ((n + 1) < consumedBlocks.size())? consumedBlocks[n + 1].moveDef: NULL;

			CalculateVertices(*currBlockMD, sb.blockPos);

			// each MapChanged() call adds AT MOST <moveDefs.size()> SingleBlock's
			// in ascending pathType order per (x, z) PE-block, therefore when the
			// next SingleBlock's pathType is less or equal to the current we know
			// that all have been processed (for one PE-block)
			if (nextBlockMD == NULL || nextBlockMD->pathType <= currBlockMD->pathType) {
				blockStates.nodeMask[blockN] &= ~PATHOPT_OBSOLETE;
			}
		}
	}
}


const CPathCache::CacheItem* CPathEstimator::GetCache(const int2 strtBlock, const int2 goalBlock, float goalRadius, int pathType, const bool synced) const
{
	return pathCache[synced]->GetCachedPath(strtBlock, goalBlock, goalRadius, pathType);
}


void CPathEstimator::AddCache(const IPath::Path* path, const IPath::SearchResult result, const int2 strtBlock, const int2 goalBlock, float goalRadius, int pathType, const bool synced)
{
	pathCache[synced]->AddPath(path, result, strtBlock, goalBlock, goalRadius, pathType);
}


/**
 * Performs the actual search.
 */
IPath::SearchResult CPathEstimator::DoSearch(const MoveDef& moveDef, const CPathFinderDef& peDef, const CSolidObject* owner, bool synced)
{
	bool foundGoal = false;

	// get the goal square offset
	const int2 goalSqrOffset = peDef.GoalSquareOffset(BLOCK_SIZE);

	while (!openBlocks.empty() && (openBlockBuffer.GetSize() < maxBlocksToBeSearched)) {
		// get the open block with lowest cost
		PathNode* ob = const_cast<PathNode*>(openBlocks.top());
		openBlocks.pop();

		// check if the block has been marked as unaccessible during its time in the queue
		if (blockStates.nodeMask[ob->nodeNum] & (PATHOPT_BLOCKED | PATHOPT_CLOSED))
			continue;

		// no, check if the goal is already reached
		const unsigned int xBSquare = blockStates.peNodeOffsets[ob->nodeNum][moveDef.pathType].x;
		const unsigned int zBSquare = blockStates.peNodeOffsets[ob->nodeNum][moveDef.pathType].y;
		const unsigned int xGSquare = ob->nodePos.x * BLOCK_SIZE + goalSqrOffset.x;
		const unsigned int zGSquare = ob->nodePos.y * BLOCK_SIZE + goalSqrOffset.y;

		if (peDef.IsGoal(xBSquare, zBSquare) || peDef.IsGoal(xGSquare, zGSquare)) {
			mGoalBlockIdx = ob->nodeNum;
			mGoalHeuristic = 0.0f;
			foundGoal = true;
			break;
		}

		// no, test the 8 surrounding blocks
		// NOTE: each of these calls increments openBlockBuffer.idx by 1, so
		// maxBlocksToBeSearched is always less than <MAX_SEARCHED_NODES_PE - 8>
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_LEFT,       PATHOPT_OPEN, 1.f, true, synced);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_LEFT_UP,    PATHOPT_OPEN, 1.f, true, synced);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_UP,         PATHOPT_OPEN, 1.f, true, synced);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_RIGHT_UP,   PATHOPT_OPEN, 1.f, true, synced);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_RIGHT,      PATHOPT_OPEN, 1.f, true, synced);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_RIGHT_DOWN, PATHOPT_OPEN, 1.f, true, synced);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_DOWN,       PATHOPT_OPEN, 1.f, true, synced);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_LEFT_DOWN,  PATHOPT_OPEN, 1.f, true, synced);

		// mark this block as closed
		blockStates.nodeMask[ob->nodeNum] |= PATHOPT_CLOSED;
	}

	// we found our goal
	if (foundGoal)
		return IPath::Ok;

	// we could not reach the goal
	if (openBlockBuffer.GetSize() >= maxBlocksToBeSearched)
		return IPath::GoalOutOfRange;

	// search could not reach the goal due to the unit being locked in
	if (openBlocks.empty())
		return IPath::GoalOutOfRange;

	// should never happen
	LOG_L(L_ERROR, "%s - Unhandled end of search!", __FUNCTION__);
	return IPath::Error;
}


/**
 * Test the accessability of a block and its value,
 * possibly also add it to the open-blocks pqueue.
 */
bool CPathEstimator::TestBlock(
	const MoveDef& moveDef,
	const CPathFinderDef& peDef,
	const PathNode* parentOpenBlock,
	const CSolidObject* /*owner*/,
	const unsigned int pathDir,
	const unsigned int /*blockStatus*/,
	float /*speedMod*/,
	bool /*withinConstraints*/,
	bool synced
) {
	testedBlocks++;

	// initial calculations of the new block
	const int2 block = parentOpenBlock->nodePos + PE_DIRECTION_VECTORS[pathDir];
	const unsigned int blockIdx = BlockPosToIdx(block);

	// bounds-check
	if ((unsigned)block.x >= nbrOfBlocks.x) return false;
	if ((unsigned)block.y >= nbrOfBlocks.y) return false;

	// check if the block is unavailable
	if (blockStates.nodeMask[blockIdx] & (PATHOPT_BLOCKED | PATHOPT_CLOSED))
		return false;


	const int2 square = blockStates.peNodeOffsets[blockIdx][moveDef.pathType];

	const unsigned int vertexIdx =
		moveDef.pathType * blockStates.GetSize() * PATH_DIRECTION_VERTICES +
		parentOpenBlock->nodeNum * PATH_DIRECTION_VERTICES +
		GetBlockVertexOffset(pathDir, nbrOfBlocks.x);

	assert((unsigned)vertexIdx < vertexCosts.size());
	if (vertexCosts[vertexIdx] >= PATHCOST_INFINITY)
		return false;


	// check if the block is blocked or out of constraints
	if (!peDef.WithinConstraints(square.x, square.y)) {
		blockStates.nodeMask[blockIdx] |= PATHOPT_BLOCKED;
		dirtyBlocks.push_back(blockIdx);
		return false;
	}

	// evaluate this node (NOTE the max-resolution indexing for {flow,extra}Cost)
	const float flowCost  = (PathFlowMap::GetInstance())->GetFlowCost(square.x, square.y, moveDef, PathDir2PathOpt(pathDir));
	const float extraCost = blockStates.GetNodeExtraCost(square.x, square.y, synced);
	const float nodeCost  = vertexCosts[vertexIdx] + flowCost + extraCost;

	const float gCost = parentOpenBlock->gCost + nodeCost;
	const float hCost = peDef.Heuristic(square.x, square.y);
	const float fCost = gCost + hCost;

	if (blockStates.nodeMask[blockIdx] & PATHOPT_OPEN) {
		// already in the open set
		if (blockStates.fCost[blockIdx] <= fCost)
			return true;

		blockStates.nodeMask[blockIdx] &= ~PATHOPT_CARDINALS;
	}

	// look for improvements
	if (hCost < mGoalHeuristic) {
		mGoalBlockIdx = blockIdx;
		mGoalHeuristic = hCost;
	}

	// store this block as open
	openBlockBuffer.SetSize(openBlockBuffer.GetSize() + 1);
	assert(openBlockBuffer.GetSize() < MAX_SEARCHED_NODES_PE);

	PathNode* ob = openBlockBuffer.GetNode(openBlockBuffer.GetSize());
		ob->fCost   = fCost;
		ob->gCost   = gCost;
		ob->nodePos = block;
		ob->nodeNum = blockIdx;
	openBlocks.push(ob);

	blockStates.SetMaxCost(NODE_COST_F, std::max(blockStates.GetMaxCost(NODE_COST_F), fCost));
	blockStates.SetMaxCost(NODE_COST_G, std::max(blockStates.GetMaxCost(NODE_COST_G), gCost));

	// mark this block as open
	blockStates.fCost[blockIdx] = fCost;
	blockStates.gCost[blockIdx] = gCost;
	blockStates.nodeMask[blockIdx] |= (pathDir | PATHOPT_OPEN);
	blockStates.peParentNodePos[blockIdx] = parentOpenBlock->nodePos;

	dirtyBlocks.push_back(blockIdx);
	return true;
}


/**
 * Recreate the path taken to the goal
 */
void CPathEstimator::FinishSearch(const MoveDef& moveDef, IPath::Path& foundPath) const
{
	unsigned int blockIdx = mGoalBlockIdx;

	while (blockIdx != mStartBlockIdx) {
		// use offset defined by the block
		const int2 square = blockStates.peNodeOffsets[blockIdx][moveDef.pathType];
		float3 pos(square.x * SQUARE_SIZE, 0.0f, square.y * SQUARE_SIZE);
		pos.y = CMoveMath::yLevel(moveDef, square.x, square.y);

		foundPath.path.push_back(pos);

		// next step backwards
		blockIdx = BlockPosToIdx(blockStates.peParentNodePos[blockIdx]);
	}

	if (!foundPath.path.empty()) {
		foundPath.pathGoal = foundPath.path.front();
	}

	// set some additional information
	foundPath.pathCost = blockStates.fCost[mGoalBlockIdx] - mGoalHeuristic;
}


/**
 * Try to read offset and vertices data from file, return false on failure
 * TODO: Read-error-check.
 */
bool CPathEstimator::ReadFile(const std::string& cacheFileName, const std::string& map)
{
	const unsigned int hash = Hash();
	char hashString[64] = {0};

	sprintf(hashString, "%u", hash);
	LOG("[PathEstimator::%s] hash=%s", __FUNCTION__, hashString);

	std::string filename = GetPathCacheDir() + map + hashString + "." + cacheFileName + ".zip";
	if (!FileSystem::FileExists(filename))
		return false;
	// open file for reading from a suitable location (where the file exists)
	IArchive* pfile = archiveLoader.OpenArchive(dataDirsAccess.LocateFile(filename), "sdz");

	if (!pfile || !pfile->IsOpen()) {
		delete pfile;
		return false;
	}

	char calcMsg[512];
	sprintf(calcMsg, "Reading Estimate PathCosts [%d]", BLOCK_SIZE);
	loadscreen->SetLoadMessage(calcMsg);

	std::auto_ptr<IArchive> auto_pfile(pfile);
	IArchive& file(*pfile);

	const unsigned fid = file.FindFile("pathinfo");

	if (fid < file.NumFiles()) {
		pathChecksum = file.GetCrc32(fid);

		std::vector<boost::uint8_t> buffer;
		file.GetFile(fid, buffer);

		if (buffer.size() < 4)
			return false;

		unsigned filehash = *((unsigned*)&buffer[0]);
		if (filehash != hash)
			return false;

		unsigned pos = sizeof(unsigned);

		// Read block-center-offset data.
		const unsigned blockSize = moveDefHandler->GetNumMoveDefs() * sizeof(int2);
		if (buffer.size() < pos + blockSize * blockStates.GetSize())
			return false;

		for (int blocknr = 0; blocknr < blockStates.GetSize(); blocknr++) {
			std::memcpy(&blockStates.peNodeOffsets[blocknr][0], &buffer[pos], blockSize);
			pos += blockSize;
		}

		// Read vertices data.
		if (buffer.size() < pos + vertexCosts.size() * sizeof(float))
			return false;
		std::memcpy(&vertexCosts[0], &buffer[pos], vertexCosts.size() * sizeof(float));

		// File read successful.
		return true;
	}

	return false;
}


/**
 * Try to write offset and vertex data to file.
 */
void CPathEstimator::WriteFile(const std::string& cacheFileName, const std::string& map)
{
	// We need this directory to exist
	if (!FileSystem::CreateDirectory(GetPathCacheDir()))
		return;

	const unsigned int hash = Hash();
	char hashString[64] = {0};

	sprintf(hashString, "%u", hash);
	LOG("[PathEstimator::%s] hash=%s", __FUNCTION__, hashString);

	const std::string filename = GetPathCacheDir() + map + hashString + "." + cacheFileName + ".zip";

	// open file for writing in a suitable location
	zipFile file = zipOpen(dataDirsAccess.LocateFile(filename, FileQueryFlags::WRITE).c_str(), APPEND_STATUS_CREATE);

	if (file == NULL)
		return;

	zipOpenNewFileInZip(file, "pathinfo", NULL, NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_BEST_COMPRESSION);

	// Write hash. (NOTE: this also affects the CRC!)
	zipWriteInFileInZip(file, (void*) &hash, 4);

	// Write block-center-offsets.
	for (int blocknr = 0; blocknr < blockStates.GetSize(); blocknr++)
		zipWriteInFileInZip(file, (void*) &blockStates.peNodeOffsets[blocknr][0], moveDefHandler->GetNumMoveDefs() * sizeof(int2));

	// Write vertices.
	zipWriteInFileInZip(file, &vertexCosts[0], vertexCosts.size() * sizeof(float));

	zipCloseFileInZip(file);
	zipClose(file, NULL);


	// get the CRC over the written path data
	IArchive* pfile = archiveLoader.OpenArchive(dataDirsAccess.LocateFile(filename), "sdz");

	if (pfile == NULL)
		return;

	if (pfile->IsOpen()) {
		assert(pfile->FindFile("pathinfo") < pfile->NumFiles());
		pathChecksum = pfile->GetCrc32(pfile->FindFile("pathinfo"));
	}

	delete pfile;
}


/**
 * Returns a hash-code identifying the dataset of this estimator.
 * FIXME: uses checksum of raw heightmap (before Lua has seen it)
 */
unsigned int CPathEstimator::Hash() const
{
	return (readMap->GetMapChecksum() + moveDefHandler->GetCheckSum() + BLOCK_SIZE + PATHESTIMATOR_VERSION);
}
