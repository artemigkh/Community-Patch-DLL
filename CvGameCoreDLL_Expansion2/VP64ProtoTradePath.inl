// VP64_PROTO_TRADEPATH: prototype (a) of the x64 turn-time study (repository c5-64bit-patch, epic 0437, ticket 4673).
// Evidence only: compiled in only with -DVP64_PROTO_TRADEPATH, which no product build defines. Included at the end of
// CvAStar.cpp inside `#if defined(VP64_PROTO_TRADEPATH)`.
//
// What it does: CvGameTrade::UpdateTradePathCache rebuilds a player's whole trade-path cache after every invalidation
// (a war anywhere, any finished worker build, a city coming into view), and most of the rebuild is the two
// GetMultiplePaths searches per origin city. This file keeps, per (player, origin city plot, trade path type), the
// result of the last search together with a fingerprint of everything that search read: the player-level and
// search-level inputs (team, the traits and abilities TradePathInitialize reads, era, the SPathFinderUserData fields,
// the destination list) and, for every plot the four trade-path callbacks touched (as "to" or "from" plot), a 64-bit
// hash of every plot property those callbacks read for this player. A later search with the same key reuses the stored
// result when every fingerprint is equal; otherwise it runs and is stored again. The scoring (AddTradePathToCache: danger,
// passing-route yields, terrain) is not touched: it runs at every rebuild as in the stock code, because its inputs
// (approaches, visibility, units, dominance zones) change without any invalidation.
//
// Runtime mode (environment variable VP64_PROTO_TRADEPATH_MODE, read once):
//   0 = off (the stock code path; the define is compiled in but does nothing),
//   1 = reuse (default),
//   2 = verify: decide as in 1, but always run the search and use its result; count reuses whose stored result differs
//       from the fresh one (a missing input) and recomputes whose result equals the stored one.
// Counters: if VP64_PROTO_DIR is set, one CSV line per rebuild scope in %VP64_PROTO_DIR%/ProtoTradePath.csv.

#include <unordered_map>
#include <cstdio>
#include <cstdlib>

namespace
{
struct VP64PTEntry
{
	unsigned long long cfg;
	std::vector<std::pair<int, unsigned long long> > touched;
	std::map<int, SPath> result;
	size_t bytes;
	VP64PTEntry() : cfg(0), bytes(0) {}
};

struct VP64PTState
{
	int mode;                       // -1 = not read yet
	bool scope;                     // inside UpdateTradePathCache on the core thread
	PlayerTypes ePlayer;
	TeamTypes eTeam;
	unsigned int memoGen;
	std::vector<unsigned int> memoStamp;
	std::vector<unsigned long long> memoFp;
	bool recording;
	unsigned int recGen;
	std::vector<unsigned int> recStamp;
	std::vector<int> rec;
	unsigned long long pendKey, pendCfg;
	bool pendHadEntry;
	LARGE_INTEGER pendT0;
	std::unordered_map<unsigned long long, VP64PTEntry> store;
	size_t storeBytes;
	FILE* csv;
	bool csvTried;
	// per scope
	int nSearch, nReuse, nRecompute, nVerifyHit, nFalseReuse, nRecomputeEqual, nNoEntry;
	long long nTouchedChecked;
	LARGE_INTEGER tScope0;
	long long qValidate, qSearch, qStore;
	VP64PTState() : mode(-1), scope(false), ePlayer(NO_PLAYER), eTeam(NO_TEAM), memoGen(0), recording(false), recGen(0),
		pendKey(0), pendCfg(0), pendHadEntry(false), storeBytes(0), csv(NULL), csvTried(false) { ResetScope(); }
	void ResetScope()
	{
		nSearch = nReuse = nRecompute = nVerifyHit = nFalseReuse = nRecomputeEqual = nNoEntry = 0;
		nTouchedChecked = 0;
		qValidate = qSearch = qStore = 0;
	}
};

VP64PTState g_vp64pt;

inline unsigned long long VP64PTMix(unsigned long long h, unsigned long long v)
{
	// splitmix64 step over the running value
	h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
	h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL;
	h ^= h >> 27; h *= 0x94D049BB133111EBULL;
	h ^= h >> 31;
	return h;
}

int VP64PTMode()
{
	if (g_vp64pt.mode < 0)
	{
		const char* s = getenv("VP64_PROTO_TRADEPATH_MODE");
		g_vp64pt.mode = (s && *s) ? atoi(s) : 1;
		if (g_vp64pt.mode < 0 || g_vp64pt.mode > 2)
			g_vp64pt.mode = 1;
	}
	return g_vp64pt.mode;
}

long long VP64PTTicks(const LARGE_INTEGER& t0)
{
	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);
	return t1.QuadPart - t0.QuadPart;
}

// Every plot property the trade-path callbacks (TradePathLandCost/Valid, TradePathWaterCost/Valid) and the player
// predicates they call (isValidMovePlot, isCoastalCityOrPassableImprovement) read for one plot and one player.
unsigned long long VP64PTPlotFp(const CvPlot* p, PlayerTypes ePlayer, TeamTypes eTeam)
{
	unsigned long long h = 0x6A09E667F3BCC909ULL;
	const TeamTypes ePlotTeam = p->getTeam();
	unsigned long long bits = 0;
	bits |= (unsigned long long)(p->isRevealed(eTeam) ? 1 : 0) << 0;
	bits |= (unsigned long long)(p->isCity() ? 1 : 0) << 1;
	bits |= (unsigned long long)(p->isWater() ? 1 : 0) << 2;
	bits |= (unsigned long long)(p->isDeepWater() ? 1 : 0) << 3;
	bits |= (unsigned long long)(p->isShallowWater() ? 1 : 0) << 4;
	bits |= (unsigned long long)(p->isIce() ? 1 : 0) << 5;
	bits |= (unsigned long long)(p->isFlatlands() ? 1 : 0) << 6;
	bits |= (unsigned long long)(p->isOwned() ? 1 : 0) << 7;
	bits |= (unsigned long long)(p->isRoute() ? 1 : 0) << 8;
	bits |= (unsigned long long)(p->IsImprovementPassable() ? 1 : 0) << 9;
	bits |= (unsigned long long)(p->isValidMovePlot(ePlayer, false) ? 1 : 0) << 10;
	bits |= (unsigned long long)(p->isCoastalCityOrPassableImprovement(ePlayer, false, false) ? 1 : 0) << 11;
	bits |= (unsigned long long)((ePlotTeam != NO_TEAM && GET_TEAM(eTeam).isAtWar(ePlotTeam)) ? 1 : 0) << 12;
	bits |= (unsigned long long)(p->isRiver() ? 1 : 0) << 13;
	bits |= (unsigned long long)(p->IsRoutePillaged() ? 1 : 0) << 14;
	bits |= (unsigned long long)(p->IsImprovementPillaged() ? 1 : 0) << 15;
	h = VP64PTMix(h, bits);
	h = VP64PTMix(h, (unsigned long long)(long long)p->getOwner());
	h = VP64PTMix(h, (unsigned long long)(long long)ePlotTeam);
	h = VP64PTMix(h, (unsigned long long)(long long)p->getRevealedImprovementType(eTeam));
	h = VP64PTMix(h, (unsigned long long)(long long)p->getFeatureType());
	h = VP64PTMix(h, (unsigned long long)(long long)p->getTerrainType());
	h = VP64PTMix(h, (unsigned long long)(long long)p->getRouteType());
	h = VP64PTMix(h, (unsigned long long)(long long)p->getImprovementType());
	h = VP64PTMix(h, (unsigned long long)(long long)p->getResourceType(eTeam));
	if (p->isCity())
	{
		const CvCity* pCity = p->getPlotCity();
		if (pCity)
			for (int iYield = 0; iYield < NUM_YIELD_TYPES; iYield++)
				h = VP64PTMix(h, (unsigned long long)(long long)pCity->GetYieldFromPassingTR((YieldTypes)iYield));
	}
	return h;
}

unsigned long long VP64PTPlotFpMemo(int iIndex)
{
	if (g_vp64pt.memoStamp[iIndex] != g_vp64pt.memoGen)
	{
		g_vp64pt.memoStamp[iIndex] = g_vp64pt.memoGen;
		g_vp64pt.memoFp[iIndex] = VP64PTPlotFp(GC.getMap().plotByIndexUnchecked(iIndex), g_vp64pt.ePlayer, g_vp64pt.eTeam);
	}
	return g_vp64pt.memoFp[iIndex];
}

// The search-level inputs: SPathFinderUserData, what TradePathInitialize and the cost callbacks read from the player,
// and the destination list.
unsigned long long VP64PTCfg(const CvPlot* pStart, const vector<CvPlot*>& vDest, const SPathFinderUserData& d)
{
	unsigned long long h = 0xBB67AE8584CAA73BULL;
	h = VP64PTMix(h, (unsigned long long)(long long)d.ePath);
	h = VP64PTMix(h, (unsigned long long)(long long)d.eRoute);
	h = VP64PTMix(h, (unsigned long long)(long long)d.eBuild);
	h = VP64PTMix(h, (unsigned long long)(long long)d.eRoutePurpose);
	h = VP64PTMix(h, (unsigned long long)(d.bUseRivers ? 1 : 0));
	h = VP64PTMix(h, (unsigned long long)(long long)d.iFlags);
	h = VP64PTMix(h, (unsigned long long)(long long)d.ePlayer);
	h = VP64PTMix(h, (unsigned long long)(long long)d.eEnemy);
	h = VP64PTMix(h, (unsigned long long)(long long)d.iUnitID);
	h = VP64PTMix(h, (unsigned long long)(long long)d.iMaxTurns);
	h = VP64PTMix(h, (unsigned long long)(long long)d.iMaxNormalizedDistance);
	h = VP64PTMix(h, (unsigned long long)(long long)d.iMinMovesLeft);
	h = VP64PTMix(h, (unsigned long long)(long long)d.iStartMoves);
	h = VP64PTMix(h, (unsigned long long)(long long)pStart->GetPlotIndex());
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
	h = VP64PTMix(h, bits);
	h = VP64PTMix(h, (unsigned long long)(long long)kPlayer.getTeam());
	h = VP64PTMix(h, (unsigned long long)(long long)kPlayer.GetCurrentEra());
	h = VP64PTMix(h, (unsigned long long)vDest.size());
	for (size_t i = 0; i < vDest.size(); i++)
		h = VP64PTMix(h, (unsigned long long)(long long)(vDest[i] ? vDest[i]->GetPlotIndex() : -1));
	return h;
}

size_t VP64PTEntryBytes(const VP64PTEntry& e)
{
	size_t n = sizeof(VP64PTEntry) + 32 + e.touched.capacity() * sizeof(e.touched[0]);
	for (std::map<int, SPath>::const_iterator it = e.result.begin(); it != e.result.end(); ++it)
		n += 48 + sizeof(SPath) + it->second.vPlots.capacity() * sizeof(SPathNode);
	return n;
}

bool VP64PTSamePaths(const std::map<int, SPath>& a, const std::map<int, SPath>& b)
{
	if (a.size() != b.size())
		return false;
	std::map<int, SPath>::const_iterator ia = a.begin(), ib = b.begin();
	for (; ia != a.end(); ++ia, ++ib)
	{
		if (ia->first != ib->first)
			return false;
		const SPath& x = ia->second;
		const SPath& y = ib->second;
		if (x.iTotalCost != y.iTotalCost || x.iNormalizedDistanceRaw != y.iNormalizedDistanceRaw || x.iTotalTurns != y.iTotalTurns)
			return false;
		if (x.vPlots.size() != y.vPlots.size())
			return false;
		for (size_t i = 0; i < x.vPlots.size(); i++)
			if (!(x.vPlots[i] == y.vPlots[i]))
				return false;
	}
	return true;
}
} // namespace

// called by the four trade-path callbacks for every plot they read
static void VP64ProtoTouch(const CvPlot* pPlot)
{
	if (!g_vp64pt.recording || !pPlot)
		return;
	int i = pPlot->GetPlotIndex();
	if (g_vp64pt.recStamp[i] != g_vp64pt.recGen)
	{
		g_vp64pt.recStamp[i] = g_vp64pt.recGen;
		g_vp64pt.rec.push_back(i);
	}
}

// CvGameTrade::UpdateTradePathCache: around the origin-city loop
void VP64ProtoTradeBegin(PlayerTypes ePlayer)
{
	if (VP64PTMode() == 0 || !gDLL->IsGameCoreThread())
		return;
	int nPlots = GC.getMap().numPlots();
	if ((int)g_vp64pt.memoStamp.size() != nPlots)
	{
		g_vp64pt.memoStamp.assign(nPlots, 0);
		g_vp64pt.memoFp.assign(nPlots, 0);
		g_vp64pt.recStamp.assign(nPlots, 0);
		g_vp64pt.memoGen = 0;
		g_vp64pt.recGen = 0;
	}
	g_vp64pt.memoGen++;
	if (g_vp64pt.memoGen == 0)
	{
		std::fill(g_vp64pt.memoStamp.begin(), g_vp64pt.memoStamp.end(), 0u);
		g_vp64pt.memoGen = 1;
	}
	g_vp64pt.scope = true;
	g_vp64pt.recording = false;
	g_vp64pt.ePlayer = ePlayer;
	g_vp64pt.eTeam = GET_PLAYER(ePlayer).getTeam();
	g_vp64pt.ResetScope();
	QueryPerformanceCounter(&g_vp64pt.tScope0);
}

void VP64ProtoTradeEnd()
{
	if (!g_vp64pt.scope)
		return;
	g_vp64pt.scope = false;
	g_vp64pt.recording = false;
	long long qScope = VP64PTTicks(g_vp64pt.tScope0);
	if (!g_vp64pt.csvTried)
	{
		g_vp64pt.csvTried = true;
		const char* dir = getenv("VP64_PROTO_DIR");
		if (dir && *dir)
		{
			std::string path = std::string(dir) + "\\ProtoTradePath.csv";
			g_vp64pt.csv = fopen(path.c_str(), "w");
			if (g_vp64pt.csv)
				fprintf(g_vp64pt.csv, "turn,player,mode,searches,reused,recomputed,no_entry,verify_would_reuse,verify_false_reuse,"
					"recomputed_equal,touched_checked,validate_us,search_us,store_us,scope_us,store_entries,store_bytes\n");
		}
	}
	if (g_vp64pt.csv)
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		double us = 1e6 / (double)f.QuadPart;
		fprintf(g_vp64pt.csv, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lld,%.0f,%.0f,%.0f,%.0f,%d,%llu\n", GC.getGame().getGameTurn(),
			(int)g_vp64pt.ePlayer, g_vp64pt.mode, g_vp64pt.nSearch, g_vp64pt.nReuse, g_vp64pt.nRecompute, g_vp64pt.nNoEntry,
			g_vp64pt.nVerifyHit, g_vp64pt.nFalseReuse, g_vp64pt.nRecomputeEqual, g_vp64pt.nTouchedChecked,
			g_vp64pt.qValidate * us, g_vp64pt.qSearch * us, g_vp64pt.qStore * us, qScope * us, (int)g_vp64pt.store.size(),
			(unsigned long long)g_vp64pt.storeBytes);
		fflush(g_vp64pt.csv);
	}
}

// CvPathFinder::GetMultiplePaths, at its start. Returns 0 = not ours (run as stock), 1 = reuse: out holds the result,
// 2 = run the search and store it, 3 = verify mode with a valid entry: run, compare, store.
static int VP64ProtoTradeLookup(const CvPlot* pStart, const vector<CvPlot*>& vDest, const SPathFinderUserData& d, map<int, SPath>& out)
{
	if (!g_vp64pt.scope || !pStart || (d.ePath != PT_TRADE_WATER && d.ePath != PT_TRADE_LAND) || d.ePlayer != g_vp64pt.ePlayer)
		return 0;
	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	g_vp64pt.nSearch++;
	unsigned long long key = ((unsigned long long)(unsigned int)d.ePlayer << 40) | ((unsigned long long)(unsigned int)d.ePath << 32)
		| (unsigned long long)(unsigned int)pStart->GetPlotIndex();
	unsigned long long cfg = VP64PTCfg(pStart, vDest, d);
	bool bValid = false;
	std::unordered_map<unsigned long long, VP64PTEntry>::const_iterator it = g_vp64pt.store.find(key);
	if (it != g_vp64pt.store.end())
	{
		if (it->second.cfg == cfg)
		{
			bValid = true;
			const std::vector<std::pair<int, unsigned long long> >& t = it->second.touched;
			for (size_t i = 0; i < t.size(); i++)
			{
				g_vp64pt.nTouchedChecked++;
				if (VP64PTPlotFpMemo(t[i].first) != t[i].second)
				{
					bValid = false;
					break;
				}
			}
		}
	}
	else
		g_vp64pt.nNoEntry++;
	g_vp64pt.qValidate += VP64PTTicks(t0);
	if (bValid && g_vp64pt.mode == 1)
	{
		out = it->second.result;
		g_vp64pt.nReuse++;
		return 1;
	}
	if (bValid)
		g_vp64pt.nVerifyHit++;
	g_vp64pt.nRecompute++;
	g_vp64pt.pendKey = key;
	g_vp64pt.pendCfg = cfg;
	g_vp64pt.pendHadEntry = (it != g_vp64pt.store.end());
	g_vp64pt.recGen++;
	if (g_vp64pt.recGen == 0)
	{
		std::fill(g_vp64pt.recStamp.begin(), g_vp64pt.recStamp.end(), 0u);
		g_vp64pt.recGen = 1;
	}
	g_vp64pt.rec.clear();
	g_vp64pt.recording = true;
	QueryPerformanceCounter(&g_vp64pt.pendT0);
	return bValid ? 3 : 2;
}

// CvPathFinder::GetMultiplePaths, before its final return
static void VP64ProtoTradeStore(int iState, const map<int, SPath>& result)
{
	if (iState < 2 || !g_vp64pt.recording)
		return;
	g_vp64pt.recording = false;
	g_vp64pt.qSearch += VP64PTTicks(g_vp64pt.pendT0);
	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	VP64PTEntry& e = g_vp64pt.store[g_vp64pt.pendKey];
	if (g_vp64pt.mode == 2 && g_vp64pt.pendHadEntry)
	{
		bool bSame = VP64PTSamePaths(e.result, result);
		if (iState == 3 && !bSame)
			g_vp64pt.nFalseReuse++;
		if (iState == 2 && bSame)
			g_vp64pt.nRecomputeEqual++;
	}
	g_vp64pt.storeBytes -= e.bytes;
	e.cfg = g_vp64pt.pendCfg;
	e.result = result;
	e.touched.clear();
	e.touched.reserve(g_vp64pt.rec.size());
	for (size_t i = 0; i < g_vp64pt.rec.size(); i++)
		e.touched.push_back(std::make_pair(g_vp64pt.rec[i], VP64PTPlotFpMemo(g_vp64pt.rec[i])));
	e.bytes = VP64PTEntryBytes(e);
	g_vp64pt.storeBytes += e.bytes;
	g_vp64pt.qStore += VP64PTTicks(t0);
}
