// VP64SpeedTrade.inl: the trade package of the x64 speedup work (c5-64bit-patch, ticket 4808; defines and design in
// VP64SpeedTrade.h and research/08-performance/speedup-trade.md of that repository). Included at the end of CvAStar.cpp
// inside `#if defined(VP64_SPEED_TRADE) || defined(VP64_SPEED_TRADE_TR7) || defined(VP64_SPEED_TRADE_TR8)`.
//
// TR3 (VP64_SPEED_TRADE), from the prototype (a) of epic 0437 (fork branch x64-proto-0437 5a6c1a25d, ticket 4673):
// CvGameTrade::UpdateTradePathCache rebuilds a player's whole trade-path cache after every invalidation, two
// GetMultiplePaths searches per origin city. Per (player, path type, origin plot) this file keeps the last search's
// result together with what that search read: a fingerprint of the search's own inputs (SPathFinderUserData, the player
// abilities and traits the callbacks read, the team) and the set of plots the four trade-path callbacks touched (as
// "to" or "from" plot), with one 64-bit hash per block of 64 touched plots over each plot's signature (everything the
// callbacks read of that plot for this player). A later search with the same key and fingerprint whose touched plots
// all hash the same returns the kept result: the step finder is deterministic, reads the map only through the
// callbacks, and a plot the search never touched was never adjacent to an expanded node.
//
// Exactness by construction: the reuse rebuilds the result exactly as the search builds it (each path a walk of the
// search tree's parent chain, reversed; iTurnSliceGenerated and sConfig as a fresh search sets them now). The verify
// build (VP64_SPEED_TRADE_VERIFY) searches every time, plays the fresh result and compares it with the kept one at every
// would-be reuse.
//
// Two threads (the Architect's review of 2026-11-17, section 2): the lookup and the store run inside GetMultiplePaths
// with the game-core lock held, after Configure, and only in an open scope on the thread that opened it (the core
// thread's UpdateTradePathCache); the touch hook records only on that thread while a search of the scope runs. The
// search raises no Lua hook (no hook in CvAStar.cpp, in isValidMovePlot or isCoastalCityOrPassableImprovement), so no
// other thread runs DLL code between lookup and store. The per-scope memo of plot signatures is valid only while the
// core thread kept the lock since the memo's generation began: LockYield (every place the DLL lets go of the lock with
// it held: the five LuaSupport::Call* and the tactical AI's two hand-overs) bumps a counter, and a lookup that sees it
// moved starts a new memo generation. Every static and its answer: speedup-trade.md section 2.3.

#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#if defined(VP64_SPEED_TRADE_VERIFY) || defined(VP64_SPEED_TRADE_SPOIL) || defined(VP64_SPEED_TRADE_SPOIL_EVERY)
#include "VP64SpeedVerify.h"
#endif

#ifndef VP64_SPEED_TRADE_CAP_MB
#define VP64_SPEED_TRADE_CAP_MB 32
#endif

namespace VP64Trade
{
namespace
{
// ---- statistics (VP64_SPEED_TRADE_STATS only writes them) ----
struct Stats
{
	long long nSearch, nReuse, nRecompute, nNoEntry, nCfgMiss, nPlotMiss, nVerifyHit, nFalseReuse, nRecomputeEqual;
	long long nTouchedChecked, nNotStoredCap, nNotStoredFail, nErasedLost, nMemoRegen, nNested;
	long long nTr7Fast, nTr7Fallback, nTr8;
	long long nStored, nPathNodesFresh;
	void Reset() { memset(this, 0, sizeof(*this)); }
};
Stats g_stats;
FILE* g_pCsv = NULL;
bool g_bCsvTried = false;

volatile LONG g_iLockYield = 0;

inline unsigned long long Mix(unsigned long long h, unsigned long long v)
{
	// splitmix64 step over the running value (the prototype's)
	h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
	h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL;
	h ^= h >> 27; h *= 0x94D049BB133111EBULL;
	h ^= h >> 31;
	return h;
}

#if defined(VP64_SPEED_TRADE)
struct TreeNode
{
	SPathNode n;
	int iParent;     // -1 = the root (the origin plot)
};

struct Dest
{
	int iPlot;       // the destination city's plot index (the result's key)
	int iNode;       // its node in the tree
	int iCost, iDist, iTurns;
};

struct Entry
{
	unsigned long long uCfg;
	int nTouched;
	bool bVisited;                              // looked up in the open scope
	std::vector<unsigned long long> aBits;      // the touched set over the map's plots
	std::vector<unsigned long long> aBlocks;    // one hash per 64 touched plots, in plot-index order
	std::vector<TreeNode> aNodes;               // the search tree's nodes that lie on a returned path
	std::vector<Dest> aDests;                   // in the result's key order
	size_t uBytes;
	Entry() : uCfg(0), nTouched(0), bVisited(false), uBytes(0) {}
	void Swap(Entry& o)
	{
		std::swap(uCfg, o.uCfg); std::swap(nTouched, o.nTouched); std::swap(bVisited, o.bVisited);
		aBits.swap(o.aBits); aBlocks.swap(o.aBlocks); aNodes.swap(o.aNodes); aDests.swap(o.aDests);
		std::swap(uBytes, o.uBytes);
	}
};

// the heap blocks of an entry, counted as the prototype counted its own (VP64PTEntryBytes): the hash node with its key
// and the entry, plus each vector's capacity
inline size_t EntryBytes(const Entry& e)
{
	return sizeof(Entry) + 32 + e.aBits.capacity() * sizeof(unsigned long long) + e.aBlocks.capacity() * sizeof(unsigned long long)
		+ e.aNodes.capacity() * sizeof(TreeNode) + e.aDests.capacity() * sizeof(Dest);
}

#if defined(VP64_SPEED_TRADE_TR4)
const int kTypes = 2;   // 0 = water, 1 = land: each path type has its own plot signature
#else
const int kTypes = 1;   // one signature for both path types (the prototype's level 2)
#endif

struct State
{
	int nDepth;                 // nested UpdateTradePathCache scopes on the scope thread (none expected)
	bool bScope;
	DWORD uThread;
	PlayerTypes ePlayer;
	TeamTypes eTeam;
	int nPlots, nWords;
	unsigned int uMemoGen;
	LONG iMemoYield;            // g_iLockYield when the memo generation began
	std::vector<unsigned int> aMemoStamp[kTypes];
	std::vector<unsigned long long> aMemo[kTypes];
	bool bRecording;
	unsigned int uRecGen;
	std::vector<unsigned int> aRecStamp;
	std::vector<int> aRec;
	unsigned int uTreeGen;
	std::vector<unsigned int> aTreeStamp;
	std::vector<int> aTreeIdx;
	unsigned long long uPendKey, uPendCfg;
	int iPendType;
	int iPendStart;
	bool bPendHadEntry;
	std::unordered_map<unsigned long long, Entry> store;
	size_t uStoreBytes;
	State() : nDepth(0), bScope(false), uThread(0), ePlayer(NO_PLAYER), eTeam(NO_TEAM), nPlots(0), nWords(0), uMemoGen(0),
		iMemoYield(0), bRecording(false), uRecGen(0), uTreeGen(0), uPendKey(0), uPendCfg(0), iPendType(0), iPendStart(-1),
		bPendHadEntry(false), uStoreBytes(0) {}
};
State g_s;

inline int TypeIndex(PathType ePath)
{
#if defined(VP64_SPEED_TRADE_TR4)
	return ePath == PT_TRADE_WATER ? 0 : 1;
#else
	(void)ePath;
	return 0;
#endif
}

// The river edges a callback can read through IsAlongSameRiver (IsRiverSide reads this plot's NE/W/NW flags or a
// neighbour's) and isRiverCrossing (the crossing mask): rivers are fixed after map generation, but a mod's Lua can set
// them (CvLuaPlot SetNEOfRiver ...), and the hand-over's section 4.3 item 1 asks for them to be hashed.
inline unsigned long long RiverBits(const CvPlot* p)
{
	unsigned long long r = 0;
	r |= (unsigned long long)(p->isNEOfRiver() ? 1 : 0) << 0;
	r |= (unsigned long long)(p->isWOfRiver() ? 1 : 0) << 1;
	r |= (unsigned long long)(p->isNWOfRiver() ? 1 : 0) << 2;
	for (int iDir = 0; iDir < NUM_DIRECTION_TYPES; iDir++)
		r |= (unsigned long long)(p->isRiverCrossing((DirectionTypes)iDir) ? 1 : 0) << (3 + iDir);
	r |= (unsigned long long)(p->isRiver() ? 1 : 0) << 9;
	return r;
}

#if !defined(VP64_SPEED_TRADE_TR4)
// TR3: the prototype's level-2 signature (VP64PTPlotFp with level 2, 5a6c1a25d), one for both path types, plus the
// river edges. Each bit only where a callback reads it; 2 = "not read here".
unsigned long long PlotSig(const CvPlot* p, int, PlayerTypes ePlayer, TeamTypes eTeam)
{
	const TeamTypes ePlotTeam = p->getTeam();
	const bool bOwn = (p->getOwner() == ePlayer);
	unsigned long long h0 = 1, h1 = 2, h2 = 3, h3 = 4;
	const bool bRevealed = p->isRevealed(eTeam);
	h0 = Mix(h0, (unsigned long long)(bRevealed ? 1 : 0));
	if (!bRevealed)
		return Mix(0x6A09E667F3BCC909ULL, h0); // both Valid callbacks reject it before reading anything else
	h0 = Mix(h0, (unsigned long long)((p->getRevealedImprovementType(eTeam) == (ImprovementTypes)GD_INT_GET(BARBARIAN_CAMP_IMPROVEMENT)) ? 1 : 0));
	const bool bCity = p->isCity();
	const bool bCoastalPred = p->isCoastalCityOrPassableImprovement(ePlayer, false, false);
	const bool bWar = (ePlotTeam != NO_TEAM && GET_TEAM(eTeam).isAtWar(ePlotTeam));
	const bool bWarRead = bCity || bCoastalPred;
	const bool bOwnedRead = p->isIce() || bCity || bCoastalPred;
	const bool bOwnRead = bCity || p->getImprovementType() != NO_IMPROVEMENT;
	const FeatureTypes eFeature = p->getFeatureType();
	const bool bTeamRead = (eFeature == FEATURE_FOREST || eFeature == FEATURE_JUNGLE);
	h1 = Mix(h1, bOwnedRead ? (unsigned long long)(p->isOwned() ? 1 : 0) : 2ULL);
	h1 = Mix(h1, bWarRead ? (unsigned long long)(bWar ? 1 : 0) : 2ULL);
	h1 = Mix(h1, bOwnRead ? (unsigned long long)(bOwn ? 1 : 0) : 2ULL);
	h1 = Mix(h1, bTeamRead ? (unsigned long long)((ePlotTeam == eTeam) ? 1 : 0) : 2ULL);
	unsigned long long bits = 0;
	bits |= (unsigned long long)(p->isWater() ? 1 : 0) << 0;
	bits |= (unsigned long long)(p->isDeepWater() ? 1 : 0) << 1;
	bits |= (unsigned long long)(p->isShallowWater() ? 1 : 0) << 2;
	bits |= (unsigned long long)(p->isIce() ? 1 : 0) << 3;
	bits |= (unsigned long long)(p->isFlatlands() ? 1 : 0) << 4;
	bits |= (unsigned long long)(p->isRoute() ? 1 : 0) << 5;
	bits |= (unsigned long long)(p->IsImprovementPassable() ? 1 : 0) << 6;
	bits |= (unsigned long long)(p->isValidMovePlot(ePlayer, false) ? 1 : 0) << 7;
	bits |= (unsigned long long)(bCoastalPred ? 1 : 0) << 8;
	bits |= RiverBits(p) << 9;
	h2 = Mix(h2, bits);
	h2 = Mix(h2, (unsigned long long)(long long)eFeature);
	h2 = Mix(h2, (unsigned long long)(long long)p->getTerrainType());
	h2 = Mix(h2, (unsigned long long)(long long)p->getRouteType());
	h2 = Mix(h2, (unsigned long long)(long long)(bOwn ? p->getImprovementType() : (ImprovementTypes)-2));
	h3 = Mix(h3, (unsigned long long)(bCity ? 1 : 0));
	if (bCity && bOwn)
	{
		const CvCity* pCity = p->getPlotCity();
		if (pCity)
			for (int iYield = 0; iYield < NUM_YIELD_TYPES; iYield++)
				h3 = Mix(h3, (unsigned long long)(long long)pCity->GetYieldFromPassingTR((YieldTypes)iYield));
	}
	unsigned long long h = 0x6A09E667F3BCC909ULL;
	h = Mix(h, h0);
	h = Mix(h, h1);
	h = Mix(h, h2);
	h = Mix(h, h3);
	return h;
}
#else
// TR4: what the callbacks of one path type derive from the plot, as the callbacks compute it.
//
// Water (TradePathWaterValid/Cost; searches with MOVEFLAG_PRETEND_CANALS never reach the store): as the node: revealed
// (else rejected first), unowned ice, deep water (cost and the ocean test), shallow water, water, the coastal predicate,
// a city, passable improvement (cost of a land node); as the parent of a shallow node: owned, at war with its team and
// the coastal predicate, only together.
unsigned long long PlotSigWater(const CvPlot* p, PlayerTypes ePlayer, TeamTypes eTeam)
{
	if (!p->isRevealed(eTeam))
		return 0x510E527FADE682D1ULL;
	const bool bWater = p->isWater();
	const bool bCity = p->isCity();
	const bool bOwned = p->isOwned();
	const bool bCoastalPred = p->isCoastalCityOrPassableImprovement(ePlayer, false, false);
	const TeamTypes ePlotTeam = p->getTeam();
	const bool bEnemyCoastal = bOwned && bCoastalPred && ePlotTeam != NO_TEAM && GET_TEAM(eTeam).isAtWar(ePlotTeam);
	unsigned long long bits = 1;
	bits |= (unsigned long long)((p->isIce() && !bOwned) ? 1 : 0) << 1;
	bits |= (unsigned long long)(p->isDeepWater() ? 1 : 0) << 2;
	bits |= (unsigned long long)(p->isShallowWater() ? 1 : 0) << 3;
	bits |= (unsigned long long)(bWater ? 1 : 0) << 4;
	bits |= (unsigned long long)(bCoastalPred ? 1 : 0) << 5;
	bits |= (unsigned long long)(bEnemyCoastal ? 1 : 0) << 6;
	bits |= (unsigned long long)(bCity ? 1 : 0) << 7;
	if (!bWater && !bCity)
		bits |= (unsigned long long)(p->IsImprovementPassable() ? 1 : 0) << 8;
	return Mix(0x9B05688C2B3E6C1FULL, bits);
}

// Land (TradePathLandValid/Cost): as the node: revealed (else rejected first); a city (accepted; then no further test);
// water, a revealed barbarian camp, isValidMovePlot (in that order, each only while the earlier ones pass); the cost's
// reads of the node: route (any, railroad), the terrain and feature penalty as the callback decides it, woodland, the
// team test of the woodland bonus (only where it is read), the extra-yield discount min(20, ...) as the callback computes
// it (own plots: a city's first positive passing-route yield, the improvement's route yields for the era's route),
// flatlands; as the parent: an owned city at war (the Valid test) and its road or railroad; the river edges of both.
unsigned long long PlotSigLand(const CvPlot* p, PlayerTypes ePlayer, TeamTypes eTeam)
{
	if (!p->isRevealed(eTeam))
		return 0x1F83D9ABFB41BD6BULL;
	unsigned long long h = 0x5BE0CD19137E2179ULL;
	const bool bCity = p->isCity();
	unsigned long long bits = 1;
	bits |= (unsigned long long)(bCity ? 1 : 0) << 1;
	if (!bCity)
	{
		const bool bWater = p->isWater();
		bits |= (unsigned long long)(bWater ? 1 : 0) << 2;
		if (!bWater)
		{
			const bool bCamp = p->getRevealedImprovementType(eTeam) == (ImprovementTypes)GD_INT_GET(BARBARIAN_CAMP_IMPROVEMENT);
			bits |= (unsigned long long)(bCamp ? 1 : 0) << 3;
			if (!bCamp)
				bits |= (unsigned long long)(p->isValidMovePlot(ePlayer, false) ? 1 : 0) << 4;
		}
	}
	else
	{
		const TeamTypes ePlotTeam = p->getTeam();
		const bool bEnemyCity = p->isOwned() && ePlotTeam != NO_TEAM && GET_TEAM(eTeam).isAtWar(ePlotTeam);
		bits |= (unsigned long long)(bEnemyCity ? 1 : 0) << 5;
	}
	const RouteTypes eRoute = p->getRouteType();
	const int iRouteCat = (eRoute == ROUTE_RAILROAD) ? 3 : (eRoute == ROUTE_ROAD) ? 2 : p->isRoute() ? 1 : 0;
	bits |= (unsigned long long)iRouteCat << 6;
	const FeatureTypes eFeature = p->getFeatureType();
	const TerrainTypes eTerrain = p->getTerrainType();
	const bool bWoodland = (eFeature == FEATURE_FOREST || eFeature == FEATURE_JUNGLE);
	const bool bPenalty = (eTerrain == TERRAIN_DESERT || eTerrain == TERRAIN_SNOW || eFeature == FEATURE_FOREST || eFeature == FEATURE_JUNGLE || eFeature == FEATURE_MARSH) &&
		eFeature != FEATURE_FLOOD_PLAINS && eFeature != FEATURE_OASIS;
	bits |= (unsigned long long)(bWoodland ? 1 : 0) << 8;
	bits |= (unsigned long long)(bPenalty ? 1 : 0) << 9;
	bits |= (unsigned long long)(p->isFlatlands() ? 1 : 0) << 10;
	// the woodland bonus reads the plot's team only with the trait, without the alternate trait, on forest and jungle
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	CvPlayerTraits* pTraits = kPlayer.GetPlayerTraits();
	if (bWoodland && pTraits && pTraits->IsWoodlandMovementBonus() && !MOD_BALANCE_ALTERNATE_IROQUOIS_TRAIT)
		bits |= (unsigned long long)((p->getTeam() == kPlayer.getTeam()) ? 2 : 1) << 11;
	bits |= RiverBits(p) << 13;
	h = Mix(h, bits);
	// the extra-yield discount as TradePathLandCost computes it for this plot as the node
	int iExtraYieldDiscount = 0;
	if (ePlayer == p->getOwner())
	{
		if (bCity)
		{
			for (int iYield = 0; iYield < NUM_YIELD_TYPES; iYield++)
			{
				if (p->getPlotCity()->GetYieldFromPassingTR((YieldTypes)iYield) > 0)
				{
					iExtraYieldDiscount += p->getPlotCity()->GetYieldFromPassingTR((YieldTypes)iYield);
					break;
				}
			}
		}
		if (p->getImprovementType() != NO_IMPROVEMENT)
		{
			RouteTypes eRouteTypeForImprovement = (kPlayer.GetCurrentEra() >= 4) ? ROUTE_RAILROAD : ROUTE_ROAD;
			for (int iYield = 0; iYield < NUM_YIELD_TYPES; iYield++)
				iExtraYieldDiscount += GC.getImprovementInfo(p->getImprovementType())->GetRouteYieldChanges(eRouteTypeForImprovement, iYield);
		}
	}
	h = Mix(h, (unsigned long long)(long long)min(20, iExtraYieldDiscount));
	return h;
}

unsigned long long PlotSig(const CvPlot* p, int iType, PlayerTypes ePlayer, TeamTypes eTeam)
{
	return iType == 0 ? PlotSigWater(p, ePlayer, eTeam) : PlotSigLand(p, ePlayer, eTeam);
}
#endif

inline unsigned long long PlotSigMemo(int iType, int iIndex)
{
	unsigned int& uStamp = g_s.aMemoStamp[iType][iIndex];
	if (uStamp != g_s.uMemoGen)
	{
		uStamp = g_s.uMemoGen;
		g_s.aMemo[iType][iIndex] = PlotSig(GC.getMap().plotByIndexUnchecked(iIndex), iType, g_s.ePlayer, g_s.eTeam);
	}
	return g_s.aMemo[iType][iIndex];
}

void NewMemoGeneration()
{
	g_s.uMemoGen++;
	if (g_s.uMemoGen == 0)
	{
		for (int t = 0; t < kTypes; t++)
			std::fill(g_s.aMemoStamp[t].begin(), g_s.aMemoStamp[t].end(), 0u);
		g_s.uMemoGen = 1;
	}
	g_s.iMemoYield = g_iLockYield;
}

// The search's own inputs: SPathFinderUserData, what TradePathInitialize and the predicates read from the player, the
// team; TR3 also the era (TR4 folds it into the land signature, its only reader).
unsigned long long Cfg(const CvPlot* pStart, const SPathFinderUserData& d)
{
	unsigned long long h = 0xBB67AE8584CAA73BULL;
	h = Mix(h, (unsigned long long)(long long)d.ePath);
	h = Mix(h, (unsigned long long)(long long)d.eRoute);
	h = Mix(h, (unsigned long long)(long long)d.eBuild);
	h = Mix(h, (unsigned long long)(long long)d.eRoutePurpose);
	h = Mix(h, (unsigned long long)(d.bUseRivers ? 1 : 0));
	h = Mix(h, (unsigned long long)(long long)d.iFlags);
	h = Mix(h, (unsigned long long)(long long)d.ePlayer);
	h = Mix(h, (unsigned long long)(long long)d.eEnemy);
	h = Mix(h, (unsigned long long)(long long)d.iUnitID);
	h = Mix(h, (unsigned long long)(long long)d.iMaxTurns);
	h = Mix(h, (unsigned long long)(long long)d.iMaxNormalizedDistance);
	h = Mix(h, (unsigned long long)(long long)d.iMinMovesLeft);
	h = Mix(h, (unsigned long long)(long long)d.iStartMoves);
	h = Mix(h, (unsigned long long)(long long)pStart->GetPlotIndex());
	CvPlayer& kPlayer = GET_PLAYER(d.ePlayer);
	unsigned long long bits = 0;
	bits |= (unsigned long long)(kPlayer.CanCrossOcean() ? 1 : 0) << 0;
	bits |= (unsigned long long)(kPlayer.CanEmbark() ? 1 : 0) << 1;
	bits |= (unsigned long long)(kPlayer.CanCrossMountain() ? 1 : 0) << 2;
	bits |= (unsigned long long)(kPlayer.CanCrossIce() ? 1 : 0) << 3;
	CvPlayerTraits* pTraits = kPlayer.GetPlayerTraits();
	if (pTraits)
	{
		bits |= (unsigned long long)(pTraits->IsRiverTradeRoad() ? 1 : 0) << 4;
		bits |= (unsigned long long)(pTraits->IsWoodlandMovementBonus() ? 1 : 0) << 5;
		bits |= (unsigned long long)1 << 6;
	}
	h = Mix(h, bits);
	h = Mix(h, (unsigned long long)(long long)kPlayer.getTeam());
#if !defined(VP64_SPEED_TRADE_TR4)
	h = Mix(h, (unsigned long long)(long long)kPlayer.GetCurrentEra());
#endif
	return h;
}

// walks the touched set in plot-index order and compares each block's hash; stops at the first block that differs
bool Validate(const Entry& e, int iType)
{
	int nSeen = 0;
	size_t iBlock = 0;
	unsigned long long h = 0x3C6EF372FE94F82BULL;
	for (int w = 0; w < (int)e.aBits.size(); w++)
	{
		unsigned long long word = e.aBits[w];
		while (word)
		{
			unsigned long iBit;
			_BitScanForward64(&iBit, word);
			word &= word - 1;
			h = Mix(h, PlotSigMemo(iType, w * 64 + (int)iBit));
			g_stats.nTouchedChecked++;
			if ((++nSeen & 63) == 0)
			{
				if (iBlock >= e.aBlocks.size() || e.aBlocks[iBlock] != h)
					return false;
				iBlock++;
				h = 0x3C6EF372FE94F82BULL;
			}
		}
	}
	if (nSeen & 63)
	{
		if (iBlock >= e.aBlocks.size() || e.aBlocks[iBlock] != h)
			return false;
		iBlock++;
	}
	return nSeen == e.nTouched && iBlock == e.aBlocks.size();
}

// the result exactly as GetMultiplePaths builds it (the walk of the parent chain, reversed), with the turn slice and
// the finder's configuration of now, as a fresh search sets them
void Rebuild(const Entry& e, const SPathFinderUserData& cur, map<int, SPath>& out)
{
	out.clear();
	const int iTurnSlice = GC.getGame().getTurnSlice();
	for (size_t d = 0; d < e.aDests.size(); d++)
	{
		const Dest& kDest = e.aDests[d];
		SPath path;
		path.iTurnSliceGenerated = iTurnSlice;
		path.sConfig = cur;
		path.iTotalCost = kDest.iCost;
		path.iNormalizedDistanceRaw = kDest.iDist;
		path.iTotalTurns = kDest.iTurns;
		for (int i = kDest.iNode; i >= 0; i = e.aNodes[i].iParent)
			path.vPlots.push_back(e.aNodes[i].n);
		std::reverse(path.vPlots.begin(), path.vPlots.end());
		out.insert(out.end(), std::make_pair(kDest.iPlot, path));
	}
}

#if defined(VP64_SPEED_TRADE_VERIFY)
bool SameConfig(const SPathFinderUserData& a, const SPathFinderUserData& b)
{
	return a.ePath == b.ePath && a.eRoute == b.eRoute && a.eBuild == b.eBuild && a.eRoutePurpose == b.eRoutePurpose &&
		a.bUseRivers == b.bUseRivers && a.iFlags == b.iFlags && a.ePlayer == b.ePlayer && a.eEnemy == b.eEnemy &&
		a.iUnitID == b.iUnitID && a.iMaxTurns == b.iMaxTurns && a.iMaxNormalizedDistance == b.iMaxNormalizedDistance &&
		a.iMinMovesLeft == b.iMinMovesLeft && a.iStartMoves == b.iStartMoves;
}

// every field of every path (the prototype's VP64PTSamePaths plus the turn slice and the configuration)
bool SamePaths(const map<int, SPath>& a, const map<int, SPath>& b)
{
	if (a.size() != b.size())
		return false;
	map<int, SPath>::const_iterator ia = a.begin(), ib = b.begin();
	for (; ia != a.end(); ++ia, ++ib)
	{
		if (ia->first != ib->first)
			return false;
		const SPath& x = ia->second;
		const SPath& y = ib->second;
		if (x.iTotalCost != y.iTotalCost || x.iNormalizedDistanceRaw != y.iNormalizedDistanceRaw || x.iTotalTurns != y.iTotalTurns ||
			x.iTurnSliceGenerated != y.iTurnSliceGenerated || !SameConfig(x.sConfig, y.sConfig))
			return false;
		if (x.vPlots.size() != y.vPlots.size())
			return false;
		for (size_t i = 0; i < x.vPlots.size(); i++)
			if (!(x.vPlots[i] == y.vPlots[i]))
				return false;
	}
	return true;
}
#endif

// the touched set and its block hashes, from the plots the search touched (aRec)
void BuildTouched(Entry& e, int iType)
{
	e.aBits.assign(g_s.nWords, 0ULL);
	for (size_t i = 0; i < g_s.aRec.size(); i++)
	{
		const int iIndex = g_s.aRec[i];
		e.aBits[iIndex >> 6] |= 1ULL << (iIndex & 63);
	}
	e.nTouched = (int)g_s.aRec.size();
	e.aBlocks.clear();
	e.aBlocks.reserve((e.nTouched + 63) / 64);
	int nSeen = 0;
	unsigned long long h = 0x3C6EF372FE94F82BULL;
	for (int w = 0; w < g_s.nWords; w++)
	{
		unsigned long long word = e.aBits[w];
		while (word)
		{
			unsigned long iBit;
			_BitScanForward64(&iBit, word);
			word &= word - 1;
			h = Mix(h, PlotSigMemo(iType, w * 64 + (int)iBit));
			if ((++nSeen & 63) == 0)
			{
				e.aBlocks.push_back(h);
				h = 0x3C6EF372FE94F82BULL;
			}
		}
	}
	if (nSeen & 63)
		e.aBlocks.push_back(h);
}

// the tree of the result's paths: each path walked from its destination back towards the origin until it meets a
// node already in the tree (all paths of one search are branches of the closed nodes' parent tree, and in the step
// finder a plot has one node); false (not stored) when a path does not look like the search's own walk
bool BuildTree(Entry& e, const map<int, SPath>& result, int iStart)
{
	CvMap& kMap = GC.getMap();
	g_s.uTreeGen++;
	if (g_s.uTreeGen == 0)
	{
		std::fill(g_s.aTreeStamp.begin(), g_s.aTreeStamp.end(), 0u);
		g_s.uTreeGen = 1;
	}
	e.aNodes.clear();
	e.aDests.clear();
	e.aDests.reserve(result.size());
	for (map<int, SPath>::const_iterator it = result.begin(); it != result.end(); ++it)
	{
		const SPath& path = it->second;
		const int nLen = (int)path.vPlots.size();
		if (nLen <= 0 || nLen > 10000)
			return false; // the search's failsafe may have cut it
		if (kMap.plotNum(path.vPlots[0].x, path.vPlots[0].y) != iStart)
			return false;
		int iDestNode = -1;
		int iChild = -1;
		int i = nLen - 1;
		for (; i >= 0; i--)
		{
			const SPathNode& n = path.vPlots[i];
			if (!kMap.isPlot(n.x, n.y))
				return false;
			const int iPlot = kMap.plotNum(n.x, n.y);
			if (g_s.aTreeStamp[iPlot] == g_s.uTreeGen)
			{
				// already in the tree: the rest of this path must be that node's chain
				int iNode = g_s.aTreeIdx[iPlot];
				if (iDestNode < 0)
					iDestNode = iNode;
				if (iChild >= 0)
					e.aNodes[iChild].iParent = iNode;
				for (int k = i; k >= 0; k--)
				{
					if (iNode < 0 || !(e.aNodes[iNode].n == path.vPlots[k]))
						return false;
					iNode = e.aNodes[iNode].iParent;
				}
				if (iNode != -1)
					return false;
				break;
			}
			TreeNode t;
			t.n = n;
			t.iParent = -1;
			const int iNode = (int)e.aNodes.size();
			e.aNodes.push_back(t);
			g_s.aTreeStamp[iPlot] = g_s.uTreeGen;
			g_s.aTreeIdx[iPlot] = iNode;
			if (iDestNode < 0)
				iDestNode = iNode;
			if (iChild >= 0)
				e.aNodes[iChild].iParent = iNode;
			iChild = iNode;
		}
		Dest kDest;
		kDest.iPlot = it->first;
		kDest.iNode = iDestNode;
		kDest.iCost = path.iTotalCost;
		kDest.iDist = path.iNormalizedDistanceRaw;
		kDest.iTurns = path.iTotalTurns;
		e.aDests.push_back(kDest);
		g_stats.nPathNodesFresh += nLen;
	}
	if (e.aNodes.capacity() > e.aNodes.size())
		std::vector<TreeNode>(e.aNodes).swap(e.aNodes);
	return true;
}

void EraseEntry(std::unordered_map<unsigned long long, Entry>::iterator it)
{
	g_s.uStoreBytes -= it->second.uBytes;
	g_s.store.erase(it);
}
#endif // VP64_SPEED_TRADE

#if defined(VP64_SPEED_TRADE_STATS)
void WriteRow()
{
	if (!g_bCsvTried)
	{
		g_bCsvTried = true;
		char szPath[1024];
		const char* szDir = getenv("VP64_SPEED_DIR");
		if (szDir && *szDir)
			_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "%s\\VP64SpeedTrade.csv", szDir);
		else
			_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "VP64SpeedTrade.csv");
		if (fopen_s(&g_pCsv, szPath, "w") != 0)
			g_pCsv = NULL;
		if (g_pCsv)
			fprintf(g_pCsv, "turn,player,searches,reused,recomputed,no_entry,cfg_miss,plot_miss,verify_would_reuse,false_reuse,"
				"recomputed_equal,touched_checked,not_stored_cap,not_stored_fail,erased_lost,memo_regen,nested,stored,path_nodes_fresh,"
				"tr7_fast,tr7_fallback,tr8,store_entries,store_bytes,bytes_bits,bytes_blocks,bytes_nodes,bytes_dests,bytes_entries,"
				"tree_nodes,dests,touched\n");
	}
	if (!g_pCsv)
		return;
	size_t uBits = 0, uBlocks = 0, uNodes = 0, uDests = 0, uEntries = 0;
	long long nNodes = 0, nDests = 0, nTouched = 0;
	int nEntries = 0;
	PlayerTypes ePlayer = NO_PLAYER;
#if defined(VP64_SPEED_TRADE)
	ePlayer = g_s.ePlayer;
	for (std::unordered_map<unsigned long long, Entry>::const_iterator it = g_s.store.begin(); it != g_s.store.end(); ++it)
	{
		const Entry& e = it->second;
		uBits += e.aBits.capacity() * sizeof(unsigned long long);
		uBlocks += e.aBlocks.capacity() * sizeof(unsigned long long);
		uNodes += e.aNodes.capacity() * sizeof(TreeNode);
		uDests += e.aDests.capacity() * sizeof(Dest);
		uEntries += sizeof(Entry) + 32;
		nNodes += (long long)e.aNodes.size();
		nDests += (long long)e.aDests.size();
		nTouched += e.nTouched;
	}
	nEntries = (int)g_s.store.size();
#endif
	const Stats& s = g_stats;
	fprintf(g_pCsv, "%d,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%lld,%lld\n",
		GC.getGamePointer() ? GC.getGame().getGameTurn() : -1, (int)ePlayer, s.nSearch, s.nReuse, s.nRecompute, s.nNoEntry, s.nCfgMiss,
		s.nPlotMiss, s.nVerifyHit, s.nFalseReuse, s.nRecomputeEqual, s.nTouchedChecked, s.nNotStoredCap, s.nNotStoredFail,
		s.nErasedLost, s.nMemoRegen, s.nNested, s.nStored, s.nPathNodesFresh, s.nTr7Fast, s.nTr7Fallback, s.nTr8, nEntries,
#if defined(VP64_SPEED_TRADE)
		(unsigned long long)g_s.uStoreBytes,
#else
		0ULL,
#endif
		(unsigned long long)uBits, (unsigned long long)uBlocks, (unsigned long long)uNodes, (unsigned long long)uDests,
		(unsigned long long)uEntries, nNodes, nDests, nTouched);
	fflush(g_pCsv);
}
#endif
} // namespace

void LockYield()
{
	InterlockedIncrement(&g_iLockYield);
}

void CountTR7(bool bFast)
{
#if defined(VP64_SPEED_TRADE_STATS)
	if (bFast)
		g_stats.nTr7Fast++;
	else
		g_stats.nTr7Fallback++;
#else
	(void)bFast;
#endif
}

void CountTR8()
{
#if defined(VP64_SPEED_TRADE_STATS)
	g_stats.nTr8++;
#endif
}

#if defined(VP64_SPEED_TRADE)
void Begin(PlayerTypes ePlayer)
{
	if (!gDLL->IsGameCoreThread())
		return;
	const DWORD uThread = GetCurrentThreadId();
	if (g_s.bScope)
	{
		// a nested rebuild on the scope's own thread (none expected): it searches as stock unless it is for the same
		// player (the lookup checks the player); only the outermost End closes the scope
		if (g_s.uThread == uThread)
		{
			g_s.nDepth++;
			g_stats.nNested++;
		}
		return;
	}
	const int nPlots = GC.getMap().numPlots();
	if (nPlots <= 0)
		return;
	if (nPlots != g_s.nPlots)
	{
		// a new map (GameReset normally ran): the plot indices of every kept entry are meaningless
		g_s.store.clear();
		g_s.uStoreBytes = 0;
		g_s.nPlots = nPlots;
		g_s.nWords = (nPlots + 63) / 64;
		for (int t = 0; t < kTypes; t++)
		{
			g_s.aMemoStamp[t].assign(nPlots, 0u);
			g_s.aMemo[t].assign(nPlots, 0ULL);
		}
		g_s.aRecStamp.assign(nPlots, 0u);
		g_s.aTreeStamp.assign(nPlots, 0u);
		g_s.aTreeIdx.assign(nPlots, 0);
		g_s.uMemoGen = 0;
		g_s.uRecGen = 0;
		g_s.uTreeGen = 0;
	}
	NewMemoGeneration();
	g_s.bScope = true;
	g_s.nDepth = 1;
	g_s.uThread = uThread;
	g_s.bRecording = false;
	g_s.ePlayer = ePlayer;
	g_s.eTeam = GET_PLAYER(ePlayer).getTeam();
#if defined(VP64_SPEED_TRADE_STATS)
	g_stats.Reset();
#endif
}

void End()
{
	if (!g_s.bScope || GetCurrentThreadId() != g_s.uThread)
		return;
	if (--g_s.nDepth > 0)
		return;
	g_s.bScope = false;
	g_s.bRecording = false;
	// entries of this player that the scope did not look up belong to cities the player no longer has
	for (std::unordered_map<unsigned long long, Entry>::iterator it = g_s.store.begin(); it != g_s.store.end();)
	{
		if ((PlayerTypes)(it->first >> 40) != g_s.ePlayer)
		{
			++it;
			continue;
		}
		if (!it->second.bVisited)
		{
			std::unordered_map<unsigned long long, Entry>::iterator itErase = it++;
			EraseEntry(itErase);
			g_stats.nErasedLost++;
			continue;
		}
		it->second.bVisited = false;
		++it;
	}
#if defined(VP64_SPEED_TRADE_STATS)
	WriteRow();
#endif
}

void GameReset()
{
	std::unordered_map<unsigned long long, Entry>().swap(g_s.store);
	g_s.uStoreBytes = 0;
	g_s.bScope = false;
	g_s.nDepth = 0;
	g_s.bRecording = false;
	g_s.nPlots = 0;
	g_s.nWords = 0;
	for (int t = 0; t < kTypes; t++)
	{
		std::vector<unsigned int>().swap(g_s.aMemoStamp[t]);
		std::vector<unsigned long long>().swap(g_s.aMemo[t]);
	}
	std::vector<unsigned int>().swap(g_s.aRecStamp);
	std::vector<int>().swap(g_s.aRec);
	std::vector<unsigned int>().swap(g_s.aTreeStamp);
	std::vector<int>().swap(g_s.aTreeIdx);
}
#else
void Begin(PlayerTypes) {}
void End() {}
void GameReset() {}
#endif // VP64_SPEED_TRADE
} // namespace VP64Trade

#if defined(VP64_SPEED_TRADE)
// called by the four trade-path callbacks for every plot they read
static inline void VP64TradeTouch(const CvPlot* pPlot)
{
	using namespace VP64Trade;
	if (!g_s.bRecording || !pPlot || GetCurrentThreadId() != g_s.uThread)
		return;
	const int i = pPlot->GetPlotIndex();
	if (g_s.aRecStamp[i] != g_s.uRecGen)
	{
		g_s.aRecStamp[i] = g_s.uRecGen;
		g_s.aRec.push_back(i);
	}
}

// CvPathFinder::GetMultiplePaths, after Configure with the lock held. 0 = not ours (search as stock), 1 = reused (out
// holds the result), 2 = search and store, 3 = verify build with a valid entry: search, compare, store.
static int VP64TradeLookup(const CvPlot* pStart, const SPathFinderUserData& d, const SPathFinderUserData& cur, map<int, SPath>& out)
{
	using namespace VP64Trade;
	if (!g_s.bScope || g_s.bRecording || !pStart || GetCurrentThreadId() != g_s.uThread)
		return 0;
	if ((d.ePath != PT_TRADE_WATER && d.ePath != PT_TRADE_LAND) || d.ePlayer != g_s.ePlayer)
		return 0;
	if (d.iFlags & CvUnit::MOVEFLAG_PRETEND_CANALS)
		return 0; // the canal shortcut reads the visible resource and the parent's water: not in the signatures
	if (g_s.iMemoYield != g_iLockYield)
	{
		NewMemoGeneration();
		g_stats.nMemoRegen++;
	}
	g_stats.nSearch++;
	const int iType = TypeIndex(d.ePath);
	const unsigned long long uKey = ((unsigned long long)(unsigned int)d.ePlayer << 40) | ((unsigned long long)(unsigned int)d.ePath << 32)
		| (unsigned long long)(unsigned int)pStart->GetPlotIndex();
	const unsigned long long uCfg = Cfg(pStart, d);
	bool bValid = false;
	bool bHadEntry = false;
	std::unordered_map<unsigned long long, Entry>::iterator it = g_s.store.find(uKey);
	if (it != g_s.store.end())
	{
		bHadEntry = true;
		it->second.bVisited = true;
		if (it->second.uCfg == uCfg)
		{
			bValid = Validate(it->second, iType);
			if (!bValid)
				g_stats.nPlotMiss++;
		}
		else
			g_stats.nCfgMiss++;
	}
	else
		g_stats.nNoEntry++;
#if !defined(VP64_SPEED_TRADE_VERIFY)
	if (bValid)
	{
		Rebuild(it->second, cur, out);
		g_stats.nReuse++;
		return 1;
	}
#else
	(void)cur;
	(void)out;
#endif
	if (bValid)
		g_stats.nVerifyHit++;
	g_stats.nRecompute++;
	g_s.uPendKey = uKey;
	g_s.uPendCfg = uCfg;
	g_s.iPendType = iType;
	g_s.iPendStart = pStart->GetPlotIndex();
	g_s.bPendHadEntry = bHadEntry;
	g_s.uRecGen++;
	if (g_s.uRecGen == 0)
	{
		std::fill(g_s.aRecStamp.begin(), g_s.aRecStamp.end(), 0u);
		g_s.uRecGen = 1;
	}
	g_s.aRec.clear();
	g_s.bRecording = true;
	return bValid ? 3 : 2;
}

// CvPathFinder::GetMultiplePaths, before its final return (lock still held)
static void VP64TradeStore(int iState, const map<int, SPath>& result, const SPathFinderUserData& cur)
{
	using namespace VP64Trade;
	if (iState < 2 || !g_s.bRecording || GetCurrentThreadId() != g_s.uThread)
		return;
	g_s.bRecording = false;
	std::unordered_map<unsigned long long, Entry>::iterator it = g_s.store.find(g_s.uPendKey);
#if defined(VP64_SPEED_TRADE_VERIFY)
	if (it != g_s.store.end() && (iState == 3 || g_s.bPendHadEntry))
	{
		map<int, SPath> kept;
		Rebuild(it->second, cur, kept);
		const bool bSame = SamePaths(kept, result);
		if (iState == 3)
		{
			VP64_SPEED_CHECK_K("TRADE", "GetMultiplePaths", bSame ? 1 : 0, 1, (long long)(g_s.uPendKey >> 40),
				(long long)((g_s.uPendKey >> 32) & 0xFF), (long long)(g_s.uPendKey & 0xFFFFFFFFULL), (long long)result.size());
			if (!bSame)
				g_stats.nFalseReuse++;
		}
		else if (bSame)
			g_stats.nRecomputeEqual++;
	}
#else
	(void)cur;
#endif
	Entry kNew;
	BuildTouched(kNew, g_s.iPendType);
	if (!BuildTree(kNew, result, g_s.iPendStart))
	{
		if (it != g_s.store.end())
			EraseEntry(it);
		g_stats.nNotStoredFail++;
		return;
	}
	kNew.uCfg = g_s.uPendCfg;
	kNew.bVisited = true;
	kNew.uBytes = EntryBytes(kNew);
	const size_t uOld = (it != g_s.store.end()) ? it->second.uBytes : 0;
	if (g_s.uStoreBytes - uOld + kNew.uBytes > (size_t)(VP64_SPEED_TRADE_CAP_MB) * 1024 * 1024)
	{
		if (it != g_s.store.end())
			EraseEntry(it);
		g_stats.nNotStoredCap++;
		return;
	}
#if defined(VP64_SPEED_TRADE_SPOIL)
	// the verify run's negative control: one kept result wrong, which the next would-be reuse of this key must catch
	if (VP64_SPEED_SPOIL_NOW("TRADE", VP64_SPEED_TRADE_SPOIL) && !kNew.aDests.empty())
		kNew.aDests[0].iDist += 1;
#elif defined(VP64_SPEED_TRADE_SPOIL_EVERY)
	// the identity job's negative control: every n-th kept result wrong (every destination out of range)
	if (VP64_SPEED_SPOIL_EVERY("TRADE", VP64_SPEED_TRADE_SPOIL_EVERY))
		for (size_t d = 0; d < kNew.aDests.size(); d++)
			kNew.aDests[d].iDist = kNew.aDests[d].iDist * 4 + 1;
#endif
	Entry& e = g_s.store[g_s.uPendKey];
	g_s.uStoreBytes -= e.uBytes;
	e.Swap(kNew);
	g_s.uStoreBytes += e.uBytes;
	g_stats.nStored++;
}
#endif // VP64_SPEED_TRADE
