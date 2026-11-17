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
//       from the fresh one (a missing input) and recomputes whose result equals the stored one, and classify why each
//       stored search was not reused (the first mismatch: search inputs with or without the destination list, or a
//       touched plot's group: revealed, owner and war, terrain and improvement, city).
// Fingerprint width (VP64_PROTO_TRADEPATH_FP): "wide" = the first build's raw properties; default "narrow" (see
// VP64PTPlotFp).
// Counters: if VP64_PROTO_DIR is set, one CSV line per rebuild scope in %VP64_PROTO_DIR%/ProtoTradePath.csv.

#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
struct VP64PTEntry
{
	unsigned long long cfg;
	unsigned long long cfgBase;     // diagnostics: cfg without the destination list
	std::vector<std::pair<int, unsigned long long> > touched;
	std::vector<unsigned int> diag; // diagnostics (mode 2 only): 4 group hashes per touched plot
	std::map<int, SPath> result;
	size_t bytes;
	VP64PTEntry() : cfg(0), cfgBase(0), bytes(0) {}
};

// why a stored search was not reused (diagnostics; the first mismatch found decides)
enum VP64PTReason
{
	VP64PT_R_NONE = 0, VP64PT_R_NO_ENTRY, VP64PT_R_CFG_DEST, VP64PT_R_CFG_OTHER, VP64PT_R_PLOT_REVEAL, VP64PT_R_PLOT_OWNER,
	VP64PT_R_PLOT_TERRAIN, VP64PT_R_PLOT_CITY, VP64PT_R_PLOT_UNKNOWN, VP64PT_R_COUNT
};
const char* const VP64PT_REASON_NAMES[VP64PT_R_COUNT] = { "none", "no_entry", "cfg_dest", "cfg_other", "plot_reveal",
	"plot_owner", "plot_terrain", "plot_city", "plot_unknown" };

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
	unsigned long long pendKey, pendCfg, pendCfgBase;
	bool pendHadEntry;
	int pendReason;
	int nMiss[VP64PT_R_COUNT], nMissEqual[VP64PT_R_COUNT];
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
		pendKey(0), pendCfg(0), pendCfgBase(0), pendHadEntry(false), pendReason(0), storeBytes(0), csv(NULL), csvTried(false)
		{ ResetScope(); }
	void ResetScope()
	{
		for (int i = 0; i < VP64PT_R_COUNT; i++)
			nMiss[i] = nMissEqual[i] = 0;
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

// Fingerprint width (environment variable VP64_PROTO_TRADEPATH_FP, read once): "wide" = the first build's (b1) raw plot
// properties and the destination list in the search fingerprint; anything else (default) = "narrow": only what the
// callbacks can distinguish (see below), and no destination list (the search runs with no destination and keeps the
// closed nodes that are cities; a city plot is "isCity" in the touched plot's hash, so a destination that matters is a
// touched plot).
int VP64PTNarrow()
{
	static int s_iNarrow = -1;
	if (s_iNarrow < 0)
	{
		const char* s = getenv("VP64_PROTO_TRADEPATH_FP");
		s_iNarrow = (s && strcmp(s, "wide") == 0) ? 0 : 1;
	}
	return s_iNarrow;
}

// Every plot property the trade-path callbacks (TradePathLandCost/Valid, TradePathWaterCost/Valid) and the player
// predicates they call (isValidMovePlot, isCoastalCityOrPassableImprovement) read for one plot and one player, in four
// groups (g, optional: the groups' low 32 bits, for the diagnostics). Narrow: the owner only as isOwned, owner == player
// (passing-route and improvement yields, the canal shortcut) and team == team (the woodland trait); the revealed
// improvement only as "is a barbarian camp"; the improvement type, the visible resource and the city's passing-route
// yields only on the player's own plots (the only place the callbacks read them).
unsigned long long VP64PTPlotFp(const CvPlot* p, PlayerTypes ePlayer, TeamTypes eTeam, unsigned int* g = NULL)
{
	const bool bNarrow = VP64PTNarrow() != 0;
	const TeamTypes ePlotTeam = p->getTeam();
	const PlayerTypes eOwner = p->getOwner();
	const bool bOwn = (eOwner == ePlayer);
	unsigned long long h0 = 1, h1 = 2, h2 = 3, h3 = 4;
	// group 0: what the team has seen
	const bool bRevealed = p->isRevealed(eTeam);
	h0 = VP64PTMix(h0, (unsigned long long)(bRevealed ? 1 : 0));
	if (bNarrow && !bRevealed)
	{
		// both Valid callbacks reject a plot the team has not revealed before they read anything else, so it is never
		// expanded and never read as a "from" plot (the origin, a city of the player, is revealed): its other
		// properties cannot change the search
		if (g)
		{
			g[0] = (unsigned int)h0; g[1] = 2; g[2] = 3; g[3] = 4;
		}
		return VP64PTMix(0x6A09E667F3BCC909ULL, h0);
	}
	if (bNarrow)
	{
		h0 = VP64PTMix(h0, (unsigned long long)((p->getRevealedImprovementType(eTeam) == (ImprovementTypes)GD_INT_GET(BARBARIAN_CAMP_IMPROVEMENT)) ? 1 : 0));
		h0 = VP64PTMix(h0, (unsigned long long)(long long)(bOwn ? p->getResourceType(eTeam) : (ResourceTypes)-2));
	}
	else
	{
		h0 = VP64PTMix(h0, (unsigned long long)(long long)p->getRevealedImprovementType(eTeam));
		h0 = VP64PTMix(h0, (unsigned long long)(long long)p->getResourceType(eTeam));
	}
	// group 1: owner and war
	h1 = VP64PTMix(h1, (unsigned long long)(p->isOwned() ? 1 : 0));
	h1 = VP64PTMix(h1, (unsigned long long)((ePlotTeam != NO_TEAM && GET_TEAM(eTeam).isAtWar(ePlotTeam)) ? 1 : 0));
	if (bNarrow)
	{
		h1 = VP64PTMix(h1, (unsigned long long)(bOwn ? 1 : 0));
		h1 = VP64PTMix(h1, (unsigned long long)((ePlotTeam == eTeam) ? 1 : 0));
	}
	else
	{
		h1 = VP64PTMix(h1, (unsigned long long)(long long)eOwner);
		h1 = VP64PTMix(h1, (unsigned long long)(long long)ePlotTeam);
	}
	// group 2: terrain, feature, route, improvement and the player predicates
	unsigned long long bits = 0;
	bits |= (unsigned long long)(p->isWater() ? 1 : 0) << 0;
	bits |= (unsigned long long)(p->isDeepWater() ? 1 : 0) << 1;
	bits |= (unsigned long long)(p->isShallowWater() ? 1 : 0) << 2;
	bits |= (unsigned long long)(p->isIce() ? 1 : 0) << 3;
	bits |= (unsigned long long)(p->isFlatlands() ? 1 : 0) << 4;
	bits |= (unsigned long long)(p->isRoute() ? 1 : 0) << 5;
	bits |= (unsigned long long)(p->IsImprovementPassable() ? 1 : 0) << 6;
	bits |= (unsigned long long)(p->isValidMovePlot(ePlayer, false) ? 1 : 0) << 7;
	bits |= (unsigned long long)(p->isCoastalCityOrPassableImprovement(ePlayer, false, false) ? 1 : 0) << 8;
	bits |= (unsigned long long)(p->isRiver() ? 1 : 0) << 9;
	bits |= (unsigned long long)(p->IsRoutePillaged() ? 1 : 0) << 10;
	bits |= (unsigned long long)(p->IsImprovementPillaged() ? 1 : 0) << 11;
	h2 = VP64PTMix(h2, bits);
	h2 = VP64PTMix(h2, (unsigned long long)(long long)p->getFeatureType());
	h2 = VP64PTMix(h2, (unsigned long long)(long long)p->getTerrainType());
	h2 = VP64PTMix(h2, (unsigned long long)(long long)p->getRouteType());
	h2 = VP64PTMix(h2, (unsigned long long)(long long)((bOwn || !bNarrow) ? p->getImprovementType() : (ImprovementTypes)-2));
	// group 3: city and its passing-route yields
	h3 = VP64PTMix(h3, (unsigned long long)(p->isCity() ? 1 : 0));
	if (p->isCity() && (bOwn || !bNarrow))
	{
		const CvCity* pCity = p->getPlotCity();
		if (pCity)
			for (int iYield = 0; iYield < NUM_YIELD_TYPES; iYield++)
				h3 = VP64PTMix(h3, (unsigned long long)(long long)pCity->GetYieldFromPassingTR((YieldTypes)iYield));
	}
	if (g)
	{
		g[0] = (unsigned int)h0; g[1] = (unsigned int)h1; g[2] = (unsigned int)h2; g[3] = (unsigned int)h3;
	}
	unsigned long long h = 0x6A09E667F3BCC909ULL;
	h = VP64PTMix(h, h0);
	h = VP64PTMix(h, h1);
	h = VP64PTMix(h, h2);
	h = VP64PTMix(h, h3);
	return h;
}

// diagnostics only (mode 2): the same properties in four groups, 32-bit each; never used to decide a reuse
void VP64PTPlotGroups(const CvPlot* p, PlayerTypes ePlayer, TeamTypes eTeam, unsigned int* g)
{
	VP64PTPlotFp(p, ePlayer, eTeam, g);
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
unsigned long long VP64PTCfg(const CvPlot* pStart, const vector<CvPlot*>& vDest, const SPathFinderUserData& d,
	unsigned long long* pBase)
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
	*pBase = h;
	if (!VP64PTNarrow())
	{
		h = VP64PTMix(h, (unsigned long long)vDest.size());
		for (size_t i = 0; i < vDest.size(); i++)
			h = VP64PTMix(h, (unsigned long long)(long long)(vDest[i] ? vDest[i]->GetPlotIndex() : -1));
	}
	return h;
}

size_t VP64PTEntryBytes(const VP64PTEntry& e)
{
	size_t n = sizeof(VP64PTEntry) + 32 + e.touched.capacity() * sizeof(e.touched[0]) + e.diag.capacity() * sizeof(unsigned int);
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
			{
				fprintf(g_vp64pt.csv, "turn,player,mode,narrow,searches,reused,recomputed,no_entry,verify_would_reuse,verify_false_reuse,"
					"recomputed_equal,touched_checked,validate_us,search_us,store_us,scope_us,store_entries,store_bytes");
				for (int i = 1; i < VP64PT_R_COUNT; i++)
					fprintf(g_vp64pt.csv, ",miss_%s,miss_equal_%s", VP64PT_REASON_NAMES[i], VP64PT_REASON_NAMES[i]);
				fprintf(g_vp64pt.csv, "\n");
			}
		}
	}
	if (g_vp64pt.csv)
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		double us = 1e6 / (double)f.QuadPart;
		fprintf(g_vp64pt.csv, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lld,%.0f,%.0f,%.0f,%.0f,%d,%llu", GC.getGame().getGameTurn(),
			(int)g_vp64pt.ePlayer, g_vp64pt.mode, VP64PTNarrow(), g_vp64pt.nSearch, g_vp64pt.nReuse, g_vp64pt.nRecompute, g_vp64pt.nNoEntry,
			g_vp64pt.nVerifyHit, g_vp64pt.nFalseReuse, g_vp64pt.nRecomputeEqual, g_vp64pt.nTouchedChecked,
			g_vp64pt.qValidate * us, g_vp64pt.qSearch * us, g_vp64pt.qStore * us, qScope * us, (int)g_vp64pt.store.size(),
			(unsigned long long)g_vp64pt.storeBytes);
		for (int i = 1; i < VP64PT_R_COUNT; i++)
			fprintf(g_vp64pt.csv, ",%d,%d", g_vp64pt.nMiss[i], g_vp64pt.nMissEqual[i]);
		fprintf(g_vp64pt.csv, "\n");
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
	unsigned long long cfgBase = 0;
	unsigned long long cfg = VP64PTCfg(pStart, vDest, d, &cfgBase);
	bool bValid = false;
	int iReason = VP64PT_R_NONE;
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
					iReason = VP64PT_R_PLOT_UNKNOWN;
					if (g_vp64pt.mode == 2 && it->second.diag.size() == 4 * t.size())
					{
						unsigned int g[4];
						VP64PTPlotGroups(GC.getMap().plotByIndexUnchecked(t[i].first), g_vp64pt.ePlayer, g_vp64pt.eTeam, g);
						const unsigned int* o = &it->second.diag[4 * i];
						if (g[0] != o[0])
							iReason = VP64PT_R_PLOT_REVEAL;
						else if (g[1] != o[1])
							iReason = VP64PT_R_PLOT_OWNER;
						else if (g[2] != o[2])
							iReason = VP64PT_R_PLOT_TERRAIN;
						else if (g[3] != o[3])
							iReason = VP64PT_R_PLOT_CITY;
					}
					break;
				}
			}
		}
		else
			iReason = (it->second.cfgBase == cfgBase) ? VP64PT_R_CFG_DEST : VP64PT_R_CFG_OTHER;
	}
	else
	{
		g_vp64pt.nNoEntry++;
		iReason = VP64PT_R_NO_ENTRY;
	}
	if (!bValid)
		g_vp64pt.nMiss[iReason]++;
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
	g_vp64pt.pendCfgBase = cfgBase;
	g_vp64pt.pendReason = bValid ? VP64PT_R_NONE : iReason;
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
		{
			g_vp64pt.nRecomputeEqual++;
			g_vp64pt.nMissEqual[g_vp64pt.pendReason]++;
		}
	}
	g_vp64pt.storeBytes -= e.bytes;
	e.cfg = g_vp64pt.pendCfg;
	e.cfgBase = g_vp64pt.pendCfgBase;
	e.result = result;
	e.touched.clear();
	e.touched.reserve(g_vp64pt.rec.size());
	for (size_t i = 0; i < g_vp64pt.rec.size(); i++)
		e.touched.push_back(std::make_pair(g_vp64pt.rec[i], VP64PTPlotFpMemo(g_vp64pt.rec[i])));
	e.diag.clear();
	if (g_vp64pt.mode == 2)
	{
		e.diag.resize(4 * g_vp64pt.rec.size());
		for (size_t i = 0; i < g_vp64pt.rec.size(); i++)
			VP64PTPlotGroups(GC.getMap().plotByIndexUnchecked(g_vp64pt.rec[i]), g_vp64pt.ePlayer, g_vp64pt.eTeam, &e.diag[4 * i]);
	}
	e.bytes = VP64PTEntryBytes(e);
	g_vp64pt.storeBytes += e.bytes;
	g_vp64pt.qStore += VP64PTTicks(t0);
}
