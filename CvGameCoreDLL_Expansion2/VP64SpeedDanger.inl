// VP64SpeedDanger.inl: the implementation of VP64SpeedDanger.h (c5-64bit-patch ticket 4807). Included once, at the end
// of CvDangerPlots.cpp, only with VP64_SPEED_DANGER.
//
// Ownership (the two-thread rule of the speedup epic): the entries are read and written only on the game-core thread,
// whose GC.GetPathFinder() is the core finder that ran their searches; a call on any other thread (the ui thread's
// danger reconstruction after a load is one) runs the original search, uncached. The stamps and epochs are shared:
// they describe shared game state, any thread's setter may bump them, a bump takes the next value of an interlocked
// counter, and a bump can only make an entry miss. The counter and the epochs are read before the search, so a change
// made during it voids the entry. Every static of this file and its answer: research/08-performance/speedup-danger.md
// section 2.1.
#include "VP64SpeedVerify.h"
#include <map>
#include <vector>
#include <cstring>
#include <cstdio>

#if defined(VP64_SPEED_DANGER_STATS) || defined(VP64_SPEED_DANGER_VERIFY) || defined(VP64_SPEED_DANGER_SPOIL) || defined(VP64_SPEED_DANGER_SPOIL_EVERY) || \
	defined(VP64_SPEED_DANGER_DG7_VERIFY) || defined(VP64_SPEED_DANGER_DG7_SPOIL) || defined(VP64_SPEED_DANGER_DG7_SPOIL_EVERY)
#define VP64_DANGER_STATS_ON 1
#define VP64_DANGER_STAT(x) (x)
#else
#define VP64_DANGER_STATS_ON 0
#define VP64_DANGER_STAT(x) ((void)0)
#endif

namespace VP64Danger
{
namespace
{
	typedef unsigned int u32;
	typedef unsigned long long u64;

	// everything of the unit and of the search data the search reads that a setter does not stamp (compared by value)
	struct Snap
	{
		int iPlot, iFlags, iStartMoves, iMinMovesLeft, iMaxTurns, ePath;
		int iMoves, iMaxMoves, iBaseLand, iBaseWater, iDamage, eUnitType, eAIType, eDomain, iExotic;
		bool bEmbarked, bOutOfAttacks, bHuman, bAutomated, bCargo, bCanEverEmbark;

		bool SameKey(const Snap& r) const
		{
			return iPlot == r.iPlot && iFlags == r.iFlags && iStartMoves == r.iStartMoves && iMinMovesLeft == r.iMinMovesLeft &&
				iMaxTurns == r.iMaxTurns && ePath == r.ePath;
		}
		bool SameUnit(const Snap& r) const
		{
			return iMoves == r.iMoves && iMaxMoves == r.iMaxMoves && iBaseLand == r.iBaseLand && iBaseWater == r.iBaseWater &&
				iDamage == r.iDamage && eUnitType == r.eUnitType && eAIType == r.eAIType && eDomain == r.eDomain &&
				iExotic == r.iExotic && bEmbarked == r.bEmbarked && bOutOfAttacks == r.bOutOfAttacks && bHuman == r.bHuman &&
				bAutomated == r.bAutomated && bCargo == r.bCargo && bCanEverEmbark == r.bCanEverEmbark;
		}
	};

#if defined(VP64_SPEED_DANGER_DG7)
	// DG7: unit state the derivation of the tiles reads that the reach's Snap does not hold (compared by value)
	struct DSnap
	{
		int iRange, iVisRange, eFacing, iSeeThrough;
		bool bRanged, bIgnoreLOS, bNoAttackOcean, bEmbarkDeep, bAllTerrain, bConvert, bAlwaysHostile;

		bool Same(const DSnap& r) const
		{
			return iRange == r.iRange && iVisRange == r.iVisRange && eFacing == r.eFacing && iSeeThrough == r.iSeeThrough &&
				bRanged == r.bRanged && bIgnoreLOS == r.bIgnoreLOS && bNoAttackOcean == r.bNoAttackOcean &&
				bEmbarkDeep == r.bEmbarkDeep && bAllTerrain == r.bAllTerrain && bConvert == r.bConvert &&
				bAlwaysHostile == r.bAlwaysHostile;
		}
	};

	// the kept tiles of one reach entry; they go with it
	struct Dg7
	{
		bool bKept;
		DSnap snap;
		u32 uCreated;               // the event counter before the derivation
		u32 uLandEpoch;
		std::vector<int> region;    // ranged: the plots within GetRange() + 1 of a reach plot (melee: none beyond the reach's)
		std::vector<int> attack;
		std::vector<int> capture;
		Dg7() : bKept(false), uCreated(0), uLandEpoch(0) {}
	};
#endif

	struct Entry
	{
		Snap snap;
		int eTeam;
		int eOwner;
		u32 uCreated;          // the event counter before the search: a region stamp above it is a change since
		u32 uTeamEpoch;
		u32 uPlayerEpoch;
		std::vector<int> region;
		ReachablePlots result;
#if defined(VP64_SPEED_DANGER_VERIFY)
		u32 aTeamEpochs[MAX_TEAMS];      // every team's and player's epoch at the search: the detail log names the ones
		u32 aPlayerEpochs[MAX_PLAYERS];  // that moved before a difference (inputs of other teams are not checked)
		std::vector<int> parents;        // per closed node of the search: plot, parent plot (-1), turns, moves
#endif
#if defined(VP64_SPEED_DANGER_DG7)
		Dg7 dg7;
#endif
		u64 uBytes;
	};

	enum MissReason { MR_COLD, MR_KEY, MR_UNIT, MR_TEAM, MR_PLAYER, MR_REGION, MR_REGION_TEAM, MR_COUNT };
	enum UncReason { UR_THREAD, UR_PARTIAL, UR_NOPLOT, UR_ZOC, UR_UNIT, UR_LUA, UR_NESTED, UR_COUNT };
	enum NotStored { NS_JUMP, NS_NESTED, NS_GEN, NS_COUNT };
	enum EventKind { EV_PLOT, EV_TEAM_PLOT, EV_UNIT, EV_TEAM, EV_PLAYER, EV_PLOTS, EV_COUNT };
#if defined(VP64_SPEED_DANGER_DG7)
	enum DMiss { DM_NONE, DM_SNAP, DM_LAND, DM_REGION, DM_REGION_TEAM, DM_COUNT };
#endif

#if VP64_DANGER_STATS_ON
	const char* const kMissNames[MR_COUNT] = { "cold", "key", "unit", "team", "player", "region", "region_team" };
	const char* const kUncNames[UR_COUNT] = { "thread", "partial", "noplot", "zoc", "unit", "lua", "nested" };
	const char* const kNotStoredNames[NS_COUNT] = { "jump", "nested", "gen" };
	const char* const kEventNames[EV_COUNT] = { "plot", "team_plot", "unit", "team", "player", "plots" };

	struct Stats
	{
		u64 lookups, hits, miss[MR_COUNT], unc[UR_COUNT], stores, notStored[NS_COUNT], verified, spoiled;
		u64 entriesPeak, bytesPeak, regionPlots;
		u64 ev[EV_COUNT];
#if defined(VP64_SPEED_DANGER_DG7)
		u64 dLookups, dHits, dMiss[5], dNoEntry, dStores, dVerified, dSpoiled, dRegionPlots;
#endif
	};
#if defined(VP64_SPEED_DANGER_DG7)
	const char* const kDMissNames[5] = { "none", "snap", "land", "region", "region_team" };
#endif
#endif

	struct State
	{
		volatile LONG uEvent;          // every event takes the next value (InterlockedIncrement)
		volatile LONG iNumPlots;       // the size of the arrays below, published after them; 0 = none
		u32* plotStamp;                // [plot]: the counter at the plot's last event
		u32* teamPlotStamp;            // [plot * MAX_TEAMS + team]: the same for one team's view of the plot
		u32* mark;                     // [plot]: region marks (core thread only)
		u32 uMark;
		volatile LONG teamEpoch[MAX_TEAMS];
		volatile LONG playerEpoch[MAX_PLAYERS];
		bool bPartial;                 // no caching until the first game turn start after a reset
		std::map<u64, Entry> cache;    // core thread only
		u64 bytes;
		int iTurn;
#if defined(VP64_SPEED_DANGER_DG7)
		u64 uDg7Key;                   // the entry the last Reach hit or stored (core thread only)
		bool bDg7Valid;
		volatile LONG landEpoch;       // CvMap::recalculateLandmasses (any thread, interlocked)
#endif
#if VP64_DANGER_STATS_ON
		Stats st;
		FILE* fStats;
#endif
	};

	// Created at the first game turn start (core thread) and never freed (no static destruction order with CvMap at
	// exit). Before it exists every event is dropped and nothing is cached.
	State* volatile s_pState = NULL;
	// the nesting depth of Reach on this thread, and whether a nested Reach ran inside the outer one's search
	thread_local int t_iDepth = 0;
	thread_local bool t_bNested = false;

	inline State* Get() { return s_pState; }

	inline u32 NextEvent(State* p) { return (u32)InterlockedIncrement(&p->uEvent); }

	inline void BumpPlotIndex(State* p, int i)
	{
		const LONG n = p->iNumPlots;
		if (i >= 0 && i < n)
			p->plotStamp[i] = NextEvent(p);
	}

	inline void BumpTeamPlotIndex(State* p, int i, int eTeam)
	{
		const LONG n = p->iNumPlots;
		if (i >= 0 && i < n && eTeam >= 0 && eTeam < MAX_TEAMS)
			p->teamPlotStamp[(size_t)i * MAX_TEAMS + eTeam] = NextEvent(p);
	}

	inline void BumpTeam(State* p, int eTeam)
	{
		if (eTeam >= 0 && eTeam < MAX_TEAMS)
			InterlockedIncrement(&p->teamEpoch[eTeam]);
	}

	inline void BumpPlayer(State* p, int ePlayer)
	{
		if (ePlayer >= 0 && ePlayer < MAX_PLAYERS)
		{
			InterlockedIncrement(&p->playerEpoch[ePlayer]);
			// team games (hand-over 4.6 item 8): a member's change voids its team's entries too
			BumpTeam(p, (int)GET_PLAYER((PlayerTypes)ePlayer).getTeam());
		}
	}

	void FreeArrays(State* p)
	{
		p->iNumPlots = 0;
		MemoryBarrier();
		delete[] p->plotStamp;
		delete[] p->teamPlotStamp;
		delete[] p->mark;
		p->plotStamp = NULL;
		p->teamPlotStamp = NULL;
		p->mark = NULL;
		p->uMark = 0;
	}

	bool EnsureArrays(State* p)
	{
		if (p->iNumPlots > 0)
			return true;
		const int n = GC.getMap().numPlots();
		if (n <= 0)
			return false;
		p->plotStamp = new u32[n];
		p->teamPlotStamp = new u32[(size_t)n * MAX_TEAMS];
		p->mark = new u32[n];
		memset(p->plotStamp, 0, sizeof(u32) * n);
		memset(p->teamPlotStamp, 0, sizeof(u32) * (size_t)n * MAX_TEAMS);
		memset(p->mark, 0, sizeof(u32) * n);
		p->uMark = 0;
		MemoryBarrier();
		p->iNumPlots = n;
		return true;
	}

	u64 EntryBytes(const Entry& e)
	{
		// the map node, the region and the result's storage with its index (pairs of int and size_t)
		return 64 + sizeof(Entry) + e.region.capacity() * sizeof(int) + e.result.size() * (sizeof(SMovePlot) + 16);
	}

	inline u64 UnitKey(int eOwner, int iID) { return ((u64)(u32)eOwner << 32) | (u64)(u32)iID; }

#if VP64_DANGER_STATS_ON
	void WriteStats(State* p)
	{
		if (p->iTurn < 0)
			return;
		if (p->fStats == NULL)
		{
			char szPath[1024];
			const char* szDir = getenv("VP64_SPEED_DIR");
			if (szDir && *szDir)
				_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "%s\\VP64DangerStats.csv", szDir);
			else
				_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "VP64DangerStats.csv");
			if (fopen_s(&p->fStats, szPath, "w") != 0)
				p->fStats = NULL;
			if (p->fStats == NULL)
				return;
			fprintf(p->fStats, "turn,lookups,hits");
			for (int i = 0; i < MR_COUNT; i++)
				fprintf(p->fStats, ",miss_%s", kMissNames[i]);
			for (int i = 0; i < UR_COUNT; i++)
				fprintf(p->fStats, ",unc_%s", kUncNames[i]);
			fprintf(p->fStats, ",stores");
			for (int i = 0; i < NS_COUNT; i++)
				fprintf(p->fStats, ",notstored_%s", kNotStoredNames[i]);
			fprintf(p->fStats, ",verified,spoiled,entries_peak,bytes_peak,region_plots,stamp_bytes");
			for (int i = 0; i < EV_COUNT; i++)
				fprintf(p->fStats, ",ev_%s", kEventNames[i]);
#if defined(VP64_SPEED_DANGER_DG7)
			fprintf(p->fStats, ",dg7_lookups,dg7_hits");
			for (int i = 0; i < DM_COUNT; i++)
				fprintf(p->fStats, ",dg7_miss_%s", kDMissNames[i]);
			fprintf(p->fStats, ",dg7_noentry,dg7_stores,dg7_verified,dg7_spoiled,dg7_region_plots");
#endif
			fprintf(p->fStats, "\n");
		}
		const Stats& s = p->st;
		fprintf(p->fStats, "%d,%llu,%llu", p->iTurn, s.lookups, s.hits);
		for (int i = 0; i < MR_COUNT; i++)
			fprintf(p->fStats, ",%llu", s.miss[i]);
		for (int i = 0; i < UR_COUNT; i++)
			fprintf(p->fStats, ",%llu", s.unc[i]);
		fprintf(p->fStats, ",%llu", s.stores);
		for (int i = 0; i < NS_COUNT; i++)
			fprintf(p->fStats, ",%llu", s.notStored[i]);
		const u64 uStampBytes = (u64)p->iNumPlots * (2 + MAX_TEAMS) * sizeof(u32);
		fprintf(p->fStats, ",%llu,%llu,%llu,%llu,%llu,%llu", s.verified, s.spoiled, s.entriesPeak, s.bytesPeak, s.regionPlots,
			uStampBytes);
		for (int i = 0; i < EV_COUNT; i++)
			fprintf(p->fStats, ",%llu", s.ev[i]);
#if defined(VP64_SPEED_DANGER_DG7)
		fprintf(p->fStats, ",%llu,%llu", s.dLookups, s.dHits);
		for (int i = 0; i < DM_COUNT; i++)
			fprintf(p->fStats, ",%llu", s.dMiss[i]);
		fprintf(p->fStats, ",%llu,%llu,%llu,%llu,%llu", s.dNoEntry, s.dStores, s.dVerified, s.dSpoiled, s.dRegionPlots);
#endif
		fprintf(p->fStats, "\n");
		fflush(p->fStats);
	}
#endif

	void Snapshot(const CvUnit* pUnit, const SPathFinderUserData& data, Snap& s)
	{
		s.iPlot = pUnit->plot()->GetPlotIndex();
		s.iFlags = data.iFlags;
		s.iStartMoves = data.iStartMoves;
		s.iMinMovesLeft = data.iMinMovesLeft;
		s.iMaxTurns = data.iMaxTurns;
		s.ePath = (int)data.ePath;
		s.iMoves = pUnit->getMoves();
		s.iMaxMoves = pUnit->maxMoves();
		s.iBaseLand = pUnit->baseMoves(false);
		s.iBaseWater = pUnit->baseMoves(true);
		s.iDamage = pUnit->getDamage();
		s.eUnitType = (int)pUnit->getUnitType();
		s.eAIType = (int)pUnit->AI_getUnitAIType();
		s.eDomain = (int)pUnit->getDomainType();
		s.iExotic = pUnit->getNumExoticGoods();
		s.bEmbarked = pUnit->isEmbarked();
		s.bOutOfAttacks = pUnit->isOutOfAttacks();
		s.bHuman = pUnit->isHuman(ISHUMAN_AI_UNITS);
		s.bAutomated = pUnit->IsAutomated();
		s.bCargo = pUnit->isCargo();
		s.bCanEverEmbark = pUnit->CanEverEmbark();
	}

	inline void Mark(State* p, int i, std::vector<int>& region)
	{
		if (i >= 0 && i < p->iNumPlots && p->mark[i] != p->uMark)
		{
			p->mark[i] = p->uMark;
			region.push_back(i);
		}
	}

	// the unit's own plot, the plots the search closed, their neighbours (and theirs for iRings 2); for a search that can
	// airlift from a city, two rings around every city of the owner and its allied minors (UnitPathGetExtraChildren's
	// targets and what canAirliftAt reads there). Returns false for a search that could sealift or change an admiral
	// port (it reads cities anywhere: not stored).
	bool BuildRegion(State* p, const CvUnit* pUnit, int iRings, std::vector<int>& region)
	{
		const std::vector<CvAStarNode*>& closed = GC.GetPathFinder().VP64ClosedNodes();
		if (++p->uMark == 0)
		{
			memset(p->mark, 0, sizeof(u32) * (size_t)p->iNumPlots);
			p->uMark = 1;
		}
		CvMap& kMap = GC.getMap();
		region.clear();
		region.reserve(closed.size() * 3 + 8);
		Mark(p, pUnit->plot()->GetPlotIndex(), region);
		bool bAirlift = false;
		for (std::vector<CvAStarNode*>::const_iterator it = closed.begin(); it != closed.end(); ++it)
		{
			const CvAStarNode* node = *it;
			const CvPathNodeCacheData& c = node->m_kCostCacheData;
			if (c.bCanSealiftFromPlotCity || c.bCanChangePortFromPlot)
				return false;
			if (c.bCanAirliftFromPlotCity)
				bAirlift = true;
			Mark(p, kMap.plotNum(node->m_iX, node->m_iY), region);
		}
		size_t begin = 0;
		for (int r = 0; r < iRings; r++)
		{
			const size_t end = region.size();
			for (size_t k = begin; k < end; k++)
			{
				const CvPlot* pPlot = kMap.plotByIndexUnchecked(region[k]);
				for (int d = 0; d < NUM_DIRECTION_TYPES; d++)
				{
					const CvPlot* pAdj = plotDirection(pPlot->getX(), pPlot->getY(), (DirectionTypes)d);
					if (pAdj)
						Mark(p, pAdj->GetPlotIndex(), region);
				}
			}
			begin = end;
		}
		if (bAirlift)
		{
			const PlayerTypes eOwner = pUnit->getOwner();
			for (int iP = 0; iP < MAX_PLAYERS; iP++)
			{
				CvPlayer& kP = GET_PLAYER((PlayerTypes)iP);
				if (!kP.isAlive())
					continue;
				if (iP != (int)eOwner && !(kP.isMinorCiv() && kP.GetMinorCivAI()->GetAlly() == eOwner))
					continue;
				int iLoop = 0;
				for (CvCity* pCity = kP.firstCity(&iLoop); pCity != NULL; pCity = kP.nextCity(&iLoop))
				{
					for (int k = 0; k < RING2_PLOTS; k++)
					{
						const CvPlot* pRing = iterateRingPlots(pCity->plot(), k);
						if (pRing)
							Mark(p, pRing->GetPlotIndex(), region);
					}
				}
			}
		}
		return true;
	}

	// MR_COUNT when the entry still holds
	int Check(State* p, const Entry& e, const Snap& s)
	{
		if (!e.snap.SameKey(s))
			return MR_KEY;
		if (!e.snap.SameUnit(s))
			return MR_UNIT;
		if ((u32)p->teamEpoch[e.eTeam] != e.uTeamEpoch)
			return MR_TEAM;
		if ((u32)p->playerEpoch[e.eOwner] != e.uPlayerEpoch)
			return MR_PLAYER;
		const u32 created = e.uCreated;
		const u32* stamps = p->plotStamp;
		const u32* team = p->teamPlotStamp;
		const size_t t = (size_t)e.eTeam;
		for (std::vector<int>::const_iterator it = e.region.begin(); it != e.region.end(); ++it)
		{
			if (stamps[*it] > created)
				return MR_REGION;
			if (team[(size_t)(*it) * MAX_TEAMS + t] > created)
				return MR_REGION_TEAM;
		}
		return MR_COUNT;
	}

	ReachablePlots Search(const CvUnit* pUnit, int iFlags, const PlotIndexContainer& zoc)
	{
		return TacticalAIHelpers::GetAllPlotsInReachThisTurn(pUnit, pUnit->plot(), iFlags, 0, pUnit->maxMoves(), zoc);
	}

	inline unsigned short NextGeneration(unsigned short id)
	{
		unsigned short n = (unsigned short)(id + 1);
		return n == 0xFFFF ? (unsigned short)1 : n;
	}

#if defined(VP64_SPEED_DANGER_VERIFY)
	u64 HashResult(const ReachablePlots& r)
	{
		u64 h = 1469598103934665603ULL;
		for (ReachablePlots::const_iterator it = r.begin(); it != r.end(); ++it)
		{
			const int v[4] = { it->iPlotIndex, it->iPathLength, it->iMovesLeft, it->iNormalizedDistanceRaw };
			for (int i = 0; i < 4; i++)
			{
				h ^= (u64)(u32)v[i];
				h *= 1099511628211ULL;
			}
		}
		return h;
	}

	// the cached and the fresh result, element by element in order (plot, turns, moves left, normalised distance)
	FILE* s_fMismatch = NULL;   // verify builds: one detail line per difference (core thread only)

	// a node's cached plot flags as one number (the detail log compares the kept search's with the fresh one's)
	int NodeFlags(const CvPathNodeCacheData& c)
	{
		return (c.bIsRevealedToTeam ? 1 : 0) | (c.bPlotVisibleToTeam ? 2 : 0) | (c.bCanEnterTerrainPermanent ? 4 : 0) |
			(c.bCanEnterTerritoryPermanent ? 8 : 0) | (c.bCanEnterTerrainIntermediate ? 16 : 0) |
			(c.bCanEnterTerritoryIntermediate ? 32 : 0) | (c.bIsNonEnemyCity ? 64 : 0) | (c.bIsEnemyCity ? 128 : 0) |
			(c.bIsVisibleEnemyCombatUnit ? 256 : 0) | (c.bUnitStackingLimitReached ? 512 : 0) | (c.bIsValidRoute ? 1024 : 0) |
			(c.bIsNonNativeDomain ? 2048 : 0) | (c.bIsVisibleEnemyUnit ? 4096 : 0) | (c.bIsVisibleNeutralCombatUnit ? 8192 : 0) |
			(c.bCanAirliftFromPlotCity ? 16384 : 0);
	}

	void PlotList(char* sz, size_t n, const std::vector<int>& v)
	{
		sz[0] = 0;
		size_t len = 0;
		for (size_t i = 0; i < v.size() && i < 6; i++)
			len += _snprintf_s(sz + len, n - len, _TRUNCATE, "%s%d", i ? ";" : "", v[i]);
	}

	// a difference in detail: which plots differ, whether they lie in the entry's region, their stamps against the
	// entry's creation, and what the plot holds now (VP64DangerMismatch.csv beside the kit's files)
	void LogMismatch(State* p, const CvUnit* pUnit, const Entry& e, const ReachablePlots& fresh, const char* szKind)
	{
		if (s_fMismatch == NULL)
		{
			char szPath[1024];
			const char* szDir = getenv("VP64_SPEED_DIR");
			if (szDir && *szDir)
				_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "%s\\VP64DangerMismatch.csv", szDir);
			else
				_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "VP64DangerMismatch.csv");
			if (fopen_s(&s_fMismatch, szPath, "w") != 0)
				s_fMismatch = NULL;
			if (s_fMismatch == NULL)
				return;
			fprintf(s_fMismatch, "kind,turn,owner,unit,unit_type,ai_type,x,y,human,embarked,kept,fresh,only_kept,only_fresh,"
				"field_diff,created,event_now,team_epoch_kept,team_epoch_now,player_epoch_kept,player_epoch_now,"
				"plot,plot_x,plot_y,in_region,plot_stamp,team_stamp,owner_now,team_now,city,units,revealed,visible,"
				"revealed_owner,route,route_pillaged,feature,improvement,impr_pillaged,plot_type,terrain,"
				"kept_turns,kept_moves,kept_dist,node_gen_fresh,node_open,node_turns,node_moves,node_cost,node_end_ok,"
				"c_revealed,c_visible,c_terrain_perm,c_territory_perm,c_nonenemycity,c_enemycity,c_visenemycombat,"
				"c_stacklimit,c_route,c_nonnative,parent_x,parent_y,parent_stamp,parent_team_stamp,parent_in_region,"
				"epochs_moved\n");
		}
		std::map<int, const SMovePlot*> a, b;
		for (ReachablePlots::const_iterator it = e.result.begin(); it != e.result.end(); ++it)
			a[it->iPlotIndex] = &*it;
		for (ReachablePlots::const_iterator it = fresh.begin(); it != fresh.end(); ++it)
			b[it->iPlotIndex] = &*it;
		std::vector<int> onlyA, onlyB, fields;
		for (std::map<int, const SMovePlot*>::const_iterator it = a.begin(); it != a.end(); ++it)
		{
			std::map<int, const SMovePlot*>::const_iterator jt = b.find(it->first);
			if (jt == b.end())
				onlyA.push_back(it->first);
			else if (it->second->iPathLength != jt->second->iPathLength || it->second->iMovesLeft != jt->second->iMovesLeft ||
				it->second->iNormalizedDistanceRaw != jt->second->iNormalizedDistanceRaw)
				fields.push_back(it->first);
		}
		for (std::map<int, const SMovePlot*>::const_iterator it = b.begin(); it != b.end(); ++it)
			if (a.find(it->first) == a.end())
				onlyB.push_back(it->first);
		const int iPlot = !onlyA.empty() ? onlyA[0] : !onlyB.empty() ? onlyB[0] : !fields.empty() ? fields[0] : -1;
		char szA[128], szB[128], szF[128];
		PlotList(szA, sizeof(szA), onlyA);
		PlotList(szB, sizeof(szB), onlyB);
		PlotList(szF, sizeof(szF), fields);
		const TeamTypes eTeam = (TeamTypes)e.eTeam;
		fprintf(s_fMismatch, "%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%u,%u,%u,%u,%u,%u", szKind, p->iTurn,
			(int)pUnit->getOwner(), pUnit->GetID(), (int)pUnit->getUnitType(), (int)pUnit->AI_getUnitAIType(), pUnit->getX(),
			pUnit->getY(), (int)pUnit->isHuman(ISHUMAN_AI_UNITS), (int)pUnit->isEmbarked(), (int)e.result.size(),
			(int)fresh.size(), szA, szB, szF, e.uCreated, (u32)p->uEvent, e.uTeamEpoch, (u32)p->teamEpoch[e.eTeam],
			e.uPlayerEpoch, (u32)p->playerEpoch[e.eOwner]);
		const CvPlot* pD = iPlot >= 0 ? GC.getMap().plotByIndexUnchecked(iPlot) : NULL;
		if (pD)
		{
			bool bIn = false;
			for (size_t k = 0; k < e.region.size(); k++)
				if (e.region[k] == iPlot)
				{
					bIn = true;
					break;
				}
			fprintf(s_fMismatch, ",%d,%d,%d,%d,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", iPlot, pD->getX(), pD->getY(),
				(int)bIn, p->plotStamp[iPlot], p->teamPlotStamp[(size_t)iPlot * MAX_TEAMS + e.eTeam], (int)pD->getOwner(),
				(int)pD->getTeam(), (int)pD->isCity(), pD->getNumUnits(), (int)pD->isRevealed(eTeam), (int)pD->isVisible(eTeam),
				(int)pD->getRevealedOwner(eTeam), (int)pD->getRouteType(), (int)pD->IsRoutePillaged(), (int)pD->getFeatureType(),
				(int)pD->getImprovementType(), (int)pD->IsImprovementPillaged(), (int)pD->getPlotType(), (int)pD->getTerrainType());
			// the kept answer for the plot, and the core finder's main-layer node for it after the fresh search
			std::map<int, const SMovePlot*>::const_iterator ka = a.find(iPlot);
			fprintf(s_fMismatch, ",%d,%d,%d", ka != a.end() ? ka->second->iPathLength : -1, ka != a.end() ? ka->second->iMovesLeft : -1,
				ka != a.end() ? ka->second->iNormalizedDistanceRaw : -1);
			const CvTwoLayerPathFinder& kFinder = GC.GetPathFinder();
			const CvAStarNode* pN = kFinder.GetNode(pD->getX(), pD->getY());
			if (pN)
			{
				const CvPathNodeCacheData& c = pN->m_kCostCacheData;
				const CvAStarNode* pPar = pN->m_pParent;
				const int iPar = pPar ? GC.getMap().plotNum(pPar->m_iX, pPar->m_iY) : -1;
				bool bParIn = false;
				for (size_t k = 0; iPar >= 0 && k < e.region.size(); k++)
					if (e.region[k] == iPar)
					{
						bParIn = true;
						break;
					}
				fprintf(s_fMismatch, ",%d,%d,%d,%d,%ld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%d",
					(int)(c.iGenerationID == kFinder.GetCurrentGenerationID()), (int)pN->m_bIsOpen, (int)pN->m_iTurns,
					(int)pN->m_iMoves, pN->m_iKnownCost, (int)kFinder.CanEndTurnAtNode(pN), (int)c.bIsRevealedToTeam,
					(int)c.bPlotVisibleToTeam, (int)c.bCanEnterTerrainPermanent, (int)c.bCanEnterTerritoryPermanent,
					(int)c.bIsNonEnemyCity, (int)c.bIsEnemyCity, (int)c.bIsVisibleEnemyCombatUnit, (int)c.bUnitStackingLimitReached,
					(int)c.bIsValidRoute, (int)c.bIsNonNativeDomain, pPar ? (int)pPar->m_iX : -1, pPar ? (int)pPar->m_iY : -1,
					iPar >= 0 ? p->plotStamp[iPar] : 0u, iPar >= 0 ? p->teamPlotStamp[(size_t)iPar * MAX_TEAMS + e.eTeam] : 0u,
					(int)bParIn);
			}
			else
				fprintf(s_fMismatch, ",,,,,,,,,,,,,,,,,,,,");
			// the teams and players whose epoch moved since the search (only the unit's own are checked); then the kept
			// search's parent of the plot (the closed nodes at the search), its state now and the fresh node for it
			if (strcmp(szKind, "hit") == 0)
			{
				int iKP = -2, iKT = -1, iKM = -1, iKF = -1;
				for (size_t k = 0; k + 4 < e.parents.size(); k += 5)
					if (e.parents[k] == iPlot)
					{
						iKP = e.parents[k + 1];
						iKT = e.parents[k + 2];
						iKM = e.parents[k + 3];
						iKF = e.parents[k + 4];
						break;
					}
				fprintf(s_fMismatch, ",kp=%d node_flags kept=%d fresh=%d", iKP, iKF, pN ? NodeFlags(pN->m_kCostCacheData) : -1);
				if (iKP >= 0)
				{
					const CvPlot* pP = GC.getMap().plotByIndexUnchecked(iKP);
					int iPT = -1, iPM = -1, iPF = -1;
					for (size_t k = 0; k + 4 < e.parents.size(); k += 5)
						if (e.parents[k] == iKP)
						{
							iPT = e.parents[k + 2];
							iPM = e.parents[k + 3];
							iPF = e.parents[k + 4];
							break;
						}
					const CvAStarNode* pPN = kFinder.GetNode(pP->getX(), pP->getY());
					fprintf(s_fMismatch, " parent_flags kept=%d fresh=%d", iPF, pPN ? NodeFlags(pPN->m_kCostCacheData) : -1);
					fprintf(s_fMismatch, " (%d;%d) d=%d node_t/m=%d/%d parent_kept_t/m=%d/%d stamp=%u team_stamp=%u owner=%d city=%d water=%d "
						"route=%d units=%d visible=%d fresh_t/m=%d/%d fresh_gen=%d fresh_open=%d airlift_city=%d nonnative=%d",
						pP->getX(), pP->getY(), plotDistance(pP->getX(), pP->getY(), pD->getX(), pD->getY()), iKT, iKM, iPT, iPM,
						p->plotStamp[iKP], p->teamPlotStamp[(size_t)iKP * MAX_TEAMS + e.eTeam], (int)pP->getOwner(),
						(int)pP->isCity(), (int)pP->isWater(), (int)pP->getRouteType(), pP->getNumUnits(), (int)pP->isVisible(eTeam),
						pPN ? (int)pPN->m_iTurns : -1, pPN ? (int)pPN->m_iMoves : -1,
						pPN ? (int)(pPN->m_kCostCacheData.iGenerationID == kFinder.GetCurrentGenerationID()) : -1,
						pPN ? (int)pPN->m_bIsOpen : -1, pPN ? (int)pPN->m_kCostCacheData.bCanAirliftFromPlotCity : -1,
						pPN ? (int)pPN->m_kCostCacheData.bIsNonNativeDomain : -1);
				}
				fprintf(s_fMismatch, ",");
				for (int i = 0; i < MAX_TEAMS; i++)
					if ((u32)p->teamEpoch[i] != e.aTeamEpochs[i])
						fprintf(s_fMismatch, "t%d;", i);
				for (int i = 0; i < MAX_PLAYERS; i++)
					if ((u32)p->playerEpoch[i] != e.aPlayerEpochs[i])
						fprintf(s_fMismatch, "p%d;", i);
			}
			fprintf(s_fMismatch, "\n");
		}
		else
			fprintf(s_fMismatch, ",-1\n");
		fflush(s_fMismatch);
	}

	void Compare(State* p, const CvUnit* pUnit, const Entry& e, const ReachablePlots& fresh)
	{
		const ReachablePlots& kept = e.result;
		int iFirst = -2;
		ReachablePlots::const_iterator a = kept.begin(), b = fresh.begin();
		for (; a != kept.end() && b != fresh.end(); ++a, ++b)
		{
			if (a->iPlotIndex != b->iPlotIndex || a->iPathLength != b->iPathLength || a->iMovesLeft != b->iMovesLeft ||
				a->iNormalizedDistanceRaw != b->iNormalizedDistanceRaw)
			{
				iFirst = a->iPlotIndex;
				break;
			}
		}
		if (iFirst == -2 && kept.size() != fresh.size())
			iFirst = -1;
		if (iFirst == -2)
		{
			VP64_SPEED_CHECK_K("DANGER", "reach", (long long)kept.size(), (long long)fresh.size(), pUnit->getOwner(), pUnit->GetID(), 0, 0);
			return;
		}
		LogMismatch(p, pUnit, e, fresh, "hit");
		u64 hk = HashResult(kept), hf = HashResult(fresh);
		if (hk == hf)
			hk ^= 1;
		VP64_SPEED_CHECK_K("DANGER", "reach", (long long)hk, (long long)hf, pUnit->getOwner(), pUnit->GetID(), iFirst,
			(long long)kept.size() * 100000 + (long long)fresh.size());
	}
#endif

#if defined(VP64_SPEED_DANGER_SPOIL) || defined(VP64_SPEED_DANGER_SPOIL_EVERY)
	// a wrong kept result the danger maps feel: the last reachable plot dropped (its danger from this unit is lost); a
	// result of one plot or none gets a changed or an extra plot instead
	void SpoilResult(const CvUnit* pUnit, ReachablePlots& r)
	{
		if (r.size() > 1)
		{
			ReachablePlots s;
			ReachablePlots::const_iterator last = r.end();
			--last;
			for (ReachablePlots::const_iterator it = r.begin(); it != last; ++it)
				s.insertNoIndex(*it);
			s.createIndex();
			r = s;
		}
		else if (r.begin() != r.end())
			r.begin()->iMovesLeft += 1;
		else
		{
			r.insertNoIndex(SMovePlot(pUnit->plot()->GetPlotIndex(), 0, 1, 1));
			r.createIndex();
		}
	}
#endif
}

void PlotEvent(const CvPlot* pPlot)
{
	State* p = Get();
	if (!p || !pPlot)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_PLOT]++);
	BumpPlotIndex(p, pPlot->GetPlotIndex());
}

void PlotAndAdjacentEvent(const CvPlot* pPlot)
{
	State* p = Get();
	if (!p || !pPlot)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_PLOT]++);
	BumpPlotIndex(p, pPlot->GetPlotIndex());
	for (int d = 0; d < NUM_DIRECTION_TYPES; d++)
	{
		const CvPlot* pAdj = plotDirection(pPlot->getX(), pPlot->getY(), (DirectionTypes)d);
		if (pAdj)
			BumpPlotIndex(p, pAdj->GetPlotIndex());
	}
}

void TeamPlotEvent(const CvPlot* pPlot, int eTeam)
{
	State* p = Get();
	if (!p || !pPlot)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_TEAM_PLOT]++);
	BumpTeamPlotIndex(p, pPlot->GetPlotIndex(), eTeam);
}

void UnitEvent(const CvUnit* pUnit)
{
	State* p = Get();
	if (!p || !pUnit)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_UNIT]++);
	const CvPlot* pPlot = pUnit->plot();
	if (pPlot)
		BumpPlotIndex(p, pPlot->GetPlotIndex());
}

void UnitMove(const CvPlot* pOld, const CvPlot* pNew)
{
	State* p = Get();
	if (!p)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_UNIT]++);
	if (pOld)
		BumpPlotIndex(p, pOld->GetPlotIndex());
	if (pNew)
		BumpPlotIndex(p, pNew->GetPlotIndex());
}

void TeamEvent(int eTeam)
{
	State* p = Get();
	if (!p)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_TEAM]++);
	BumpTeam(p, eTeam);
}

void PlayerEvent(int ePlayer)
{
	State* p = Get();
	if (!p)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_PLAYER]++);
	BumpPlayer(p, ePlayer);
}

void PlayerPlotsEvent(int ePlayer)
{
	State* p = Get();
	if (!p || ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_PLOTS]++);
	const int n = (int)p->iNumPlots;
	CvMap& kMap = GC.getMap();
	for (int i = 0; i < n && i < kMap.numPlots(); i++)
		if ((int)kMap.plotByIndexUnchecked(i)->getOwner() == ePlayer)
			BumpPlotIndex(p, i);
}

void TeamPlotsEvent(int eTeam)
{
	State* p = Get();
	if (!p || eTeam < 0 || eTeam >= MAX_TEAMS)
		return;
	VP64_DANGER_STAT(p->st.ev[EV_PLOTS]++);
	const int n = (int)p->iNumPlots;
	CvMap& kMap = GC.getMap();
	for (int i = 0; i < n && i < kMap.numPlots(); i++)
		if ((int)kMap.plotByIndexUnchecked(i)->getTeam() == eTeam)
			BumpPlotIndex(p, i);
}

PlotScope::PlotScope(const CvPlot* pPlot) : m_iPlot(pPlot ? pPlot->GetPlotIndex() : -1)
{
	State* p = Get();
	if (p)
		BumpPlotIndex(p, m_iPlot);
}

PlotScope::~PlotScope()
{
	State* p = Get();
	if (p)
		BumpPlotIndex(p, m_iPlot);
}

PlayerScope::PlayerScope(int ePlayer) : m_ePlayer(ePlayer)
{
	PlayerEvent(m_ePlayer);
}

PlayerScope::~PlayerScope()
{
	PlayerEvent(m_ePlayer);
}

TeamScope::TeamScope(int eTeam) : m_eTeam(eTeam)
{
	TeamEvent(m_eTeam);
}

TeamScope::~TeamScope()
{
	TeamEvent(m_eTeam);
}

void TurnStart(int iTurn)
{
	State* p = Get();
	if (!p)
	{
		p = new State();
		p->uEvent = 0;
		p->iNumPlots = 0;
		p->plotStamp = NULL;
		p->teamPlotStamp = NULL;
		p->mark = NULL;
		p->uMark = 0;
		for (int i = 0; i < MAX_TEAMS; i++)
			p->teamEpoch[i] = 0;
		for (int i = 0; i < MAX_PLAYERS; i++)
			p->playerEpoch[i] = 0;
		p->bPartial = true;
		p->bytes = 0;
		p->iTurn = -1;
#if defined(VP64_SPEED_DANGER_DG7)
		p->uDg7Key = 0;
		p->bDg7Valid = false;
		p->landEpoch = 0;
#endif
#if VP64_DANGER_STATS_ON
		memset(&p->st, 0, sizeof(p->st));
		p->fStats = NULL;
#endif
		MemoryBarrier();
		s_pState = p;
	}
#if VP64_DANGER_STATS_ON
	WriteStats(p);
	memset(&p->st, 0, sizeof(p->st));
#endif
	p->cache.clear();
	p->bytes = 0;
	p->iTurn = iTurn;
#if defined(VP64_SPEED_DANGER_DG7)
	p->bDg7Valid = false;
#endif
	if (!EnsureArrays(p))
	{
		p->bPartial = true;
		return;
	}
	// the counter never wraps under a live entry: the cache is empty here, so the stamps can restart
	if ((u32)p->uEvent > 0xF0000000u)
	{
		memset(p->plotStamp, 0, sizeof(u32) * (size_t)p->iNumPlots);
		memset(p->teamPlotStamp, 0, sizeof(u32) * (size_t)p->iNumPlots * MAX_TEAMS);
		p->uEvent = 0;
	}
	p->bPartial = false;
}

void GameReset()
{
	State* p = Get();
	if (!p)
		return;
#if VP64_DANGER_STATS_ON
	WriteStats(p);
	memset(&p->st, 0, sizeof(p->st));
#endif
	p->bPartial = true;
	p->cache.clear();
	p->bytes = 0;
	p->iTurn = -1;
#if defined(VP64_SPEED_DANGER_DG7)
	p->bDg7Valid = false;
#endif
	FreeArrays(p);
}

ReachablePlots Reach(const CvUnit* pUnit, int iFlags, const PlotIndexContainer& zoc)
{
	State* p = Get();
	if (!pUnit || !pUnit->plot())
		return TacticalAIHelpers::GetAllPlotsInReachThisTurn(pUnit, pUnit ? pUnit->plot() : NULL, iFlags, 0, pUnit ? pUnit->maxMoves() : -1, zoc);
	// entries belong to the game-core thread and its path finder (two-thread rule): any other thread searches as stock
	if (!p || !gDLL->IsGameCoreThread())
		return Search(pUnit, iFlags, zoc);

	const bool bHadLock = gDLL->HasGameCoreLock();
	if (!bHadLock)
		gDLL->GetGameCoreLock();
	VP64_DANGER_STAT(p->st.lookups++);
#if defined(VP64_SPEED_DANGER_DG7)
	p->bDg7Valid = false;
#endif

	int eUnc = UR_COUNT;
	if (t_iDepth > 0)
		eUnc = UR_NESTED;
	else if (p->bPartial || p->iNumPlots <= 0)
		eUnc = UR_PARTIAL;
	else if (!zoc.empty())
		eUnc = UR_ZOC;
	else if (pUnit->AI_getUnitAIType() == UNITAI_EXPLORE || pUnit->AI_getUnitAIType() == UNITAI_EXPLORE_SEA ||
		pUnit->isHasPromotion((PromotionTypes)GD_INT_GET(PROMOTION_UNWELCOME_EVANGELIST)))
		eUnc = UR_UNIT;
	else if (MOD_EVENTS_AIRLIFT || MOD_EVENTS_SEALIFT || (MOD_EVENTS_CAN_MOVE_INTO && pUnit->getUnitInfo().IsSendCanMoveIntoEvent()))
		eUnc = UR_LUA;

	if (eUnc != UR_COUNT)
	{
		VP64_DANGER_STAT(p->st.unc[eUnc]++);
		if (t_iDepth > 0)
			t_bNested = true;
		t_iDepth++;
		ReachablePlots r = Search(pUnit, iFlags, zoc);
		t_iDepth--;
		if (!bHadLock)
			gDLL->ReleaseGameCoreLock();
		return r;
	}

	// the same search data GetAllPlotsInReachThisTurn builds (with an empty ZOC list), for the key
	SPathFinderUserData data(pUnit, iFlags, 1);
	data.iMinMovesLeft = 0;
	if (pUnit->maxMoves() > -1)
		data.iStartMoves = pUnit->maxMoves();
	Snap snap;
	Snapshot(pUnit, data, snap);
	const int eOwner = (int)pUnit->getOwner();
	const u64 key = UnitKey(eOwner, pUnit->GetID());

	int eReason = MR_COLD;
	{
		std::map<u64, Entry>::iterator it = p->cache.find(key);
		if (it != p->cache.end())
			eReason = Check(p, it->second, snap);
		if (eReason == MR_COUNT)
		{
			VP64_DANGER_STAT(p->st.hits++);
			// the negative controls spoil a kept result when it is used (the n-th hit, or every n-th): a store-time spoil
			// is caught only if that entry is hit later, which the 1000th store on M was not (2026-11-17)
#if defined(VP64_SPEED_DANGER_SPOIL)
			if (VP64_SPEED_SPOIL_NOW("DANGER", VP64_SPEED_DANGER_SPOIL))
			{
				SpoilResult(pUnit, it->second.result);
				VP64_DANGER_STAT(p->st.spoiled++);
			}
#endif
#if defined(VP64_SPEED_DANGER_SPOIL_EVERY)
			if (VP64_SPEED_SPOIL_EVERY("DANGER", VP64_SPEED_DANGER_SPOIL_EVERY))
			{
				SpoilResult(pUnit, it->second.result);
				VP64_DANGER_STAT(p->st.spoiled++);
			}
#endif
#if defined(VP64_SPEED_DANGER_VERIFY)
			const Entry kept = it->second;   // a copy: no entry is held across the search
			t_iDepth++;
			ReachablePlots fresh = Search(pUnit, iFlags, zoc);
			t_iDepth--;
			VP64_DANGER_STAT(p->st.verified++);
			Compare(p, pUnit, kept, fresh);
#if defined(VP64_SPEED_DANGER_DG7)
			p->uDg7Key = key;
			p->bDg7Valid = true;
#endif
			if (!bHadLock)
				gDLL->ReleaseGameCoreLock();
			return fresh;
#else
			ReachablePlots r = it->second.result;
#if defined(VP64_SPEED_DANGER_DG7)
			p->uDg7Key = key;
			p->bDg7Valid = true;
#endif
			if (!bHadLock)
				gDLL->ReleaseGameCoreLock();
			return r;
#endif
		}
	}

	// a miss: search (no entry is held across it), then store under the state read before it
	VP64_DANGER_STAT(p->st.miss[eReason]++);
	const u32 uCreated = (u32)p->uEvent;
	const int eTeam = (int)pUnit->getTeam();
	const u32 uTeamEpoch = (u32)p->teamEpoch[eTeam];
	const u32 uPlayerEpoch = (u32)p->playerEpoch[eOwner];
	const unsigned short uGenBefore = GC.GetPathFinder().GetCurrentGenerationID();
	t_bNested = false;
	t_iDepth++;
	ReachablePlots result = Search(pUnit, iFlags, zoc);
	t_iDepth--;
	const bool bNested = t_bNested;
	t_bNested = false;
	const bool bGenOk = GC.GetPathFinder().GetCurrentGenerationID() == NextGeneration(uGenBefore);
#if defined(VP64_SPEED_DANGER_DIAG) && defined(VP64_SPEED_DANGER_VERIFY)
	// diagnosis only (changes the finder's history, so the game is not the pin's): the same question asked again at
	// once; a different answer means the search depends on what the finder's earlier searches left in its nodes
	if (bGenOk && !bNested)
	{
		t_iDepth++;
		ReachablePlots again = Search(pUnit, iFlags, zoc);
		t_iDepth--;
		if (HashResult(again) != HashResult(result) || again.size() != result.size())
		{
			Entry d;
			d.snap = snap;
			d.eTeam = eTeam;
			d.eOwner = eOwner;
			d.uCreated = uCreated;
			d.uTeamEpoch = uTeamEpoch;
			d.uPlayerEpoch = uPlayerEpoch;
			d.result = result;
			d.uBytes = 0;
			LogMismatch(p, pUnit, d, again, "store_again");
		}
	}
#endif

	std::map<u64, Entry>::iterator it = p->cache.find(key);
	if (it != p->cache.end())
	{
		p->bytes -= it->second.uBytes;
		p->cache.erase(it);
	}
	if (bNested || !bGenOk || p->bPartial || p->iNumPlots <= 0)
	{
		VP64_DANGER_STAT(p->st.notStored[bNested ? NS_NESTED : NS_GEN]++);
	}
	else
	{
		Entry e;
		e.snap = snap;
		e.eTeam = eTeam;
		e.eOwner = eOwner;
		e.uCreated = uCreated;
		e.uTeamEpoch = uTeamEpoch;
		e.uPlayerEpoch = uPlayerEpoch;
#if defined(VP64_SPEED_DANGER_VERIFY)
		for (int i = 0; i < MAX_TEAMS; i++)
			e.aTeamEpochs[i] = (u32)p->teamEpoch[i];
		for (int i = 0; i < MAX_PLAYERS; i++)
			e.aPlayerEpochs[i] = (u32)p->playerEpoch[i];
#endif
#if defined(VP64_SPEED_DANGER_VERIFY)
		{
			const std::vector<CvAStarNode*>& closed = GC.GetPathFinder().VP64ClosedNodes();
			e.parents.reserve(closed.size() * 5);
			for (std::vector<CvAStarNode*>::const_iterator ci = closed.begin(); ci != closed.end(); ++ci)
			{
				const CvAStarNode* n = *ci;
				e.parents.push_back(GC.getMap().plotNum(n->m_iX, n->m_iY));
				e.parents.push_back(n->m_pParent ? GC.getMap().plotNum(n->m_pParent->m_iX, n->m_pParent->m_iY) : -1);
				e.parents.push_back((int)n->m_iTurns);
				e.parents.push_back((int)n->m_iMoves);
				e.parents.push_back(NodeFlags(n->m_kCostCacheData));
			}
		}
#endif
		if (!BuildRegion(p, pUnit, snap.bCanEverEmbark ? 2 : 1, e.region))
		{
			VP64_DANGER_STAT(p->st.notStored[NS_JUMP]++);
		}
		else
		{
			e.result = result;
			e.uBytes = EntryBytes(e);
			p->bytes += e.uBytes;
			VP64_DANGER_STAT(p->st.stores++);
			VP64_DANGER_STAT(p->st.regionPlots += e.region.size());
			p->cache.insert(std::make_pair(key, e));
#if defined(VP64_SPEED_DANGER_DG7)
			p->uDg7Key = key;
			p->bDg7Valid = true;
#endif
#if VP64_DANGER_STATS_ON
			if (p->bytes > p->st.bytesPeak)
				p->st.bytesPeak = p->bytes;
			if ((u64)p->cache.size() > p->st.entriesPeak)
				p->st.entriesPeak = (u64)p->cache.size();
#endif
		}
	}
	if (!bHadLock)
		gDLL->ReleaseGameCoreLock();
	return result;
}

#if defined(VP64_SPEED_DANGER_DG7)
namespace
{
	void DSnapshot(const CvUnit* pUnit, DSnap& s)
	{
		s.bRanged = pUnit->IsCanAttackRanged();
		s.iRange = pUnit->GetRange();
		s.iVisRange = pUnit->visibilityRange();
		s.eFacing = (int)pUnit->getFacingDirection(true);
		s.iSeeThrough = pUnit->GetSeeThrough();
		s.bIgnoreLOS = pUnit->IsRangeAttackIgnoreLOS();
		s.bNoAttackOcean = pUnit->isNoAttackInOcean();
		s.bEmbarkDeep = pUnit->IsEmbarkDeepWater();
		s.bAllTerrain = pUnit->canMoveAllTerrain();
		s.bConvert = pUnit->isConvertUnit();
		s.bAlwaysHostile = pUnit->getAlwaysHostileCount() > 0;
	}

	// what CvDangerPlots::UpdateDangerSingleUnit derives from the reach (stock's code, into two lists)
	void Derive(const CvUnit* pUnit, ReachablePlots& reach, bool bRanged, std::vector<int>& attack, std::vector<int>& capture)
	{
		attack.clear();
		capture.clear();
		CvMap& kMap = GC.getMap();
		if (bRanged)
		{
			attack = TacticalAIHelpers::GetPlotsUnderRangedAttackFrom(pUnit, reach, false, false);
			for (ReachablePlots::iterator it = reach.begin(); it != reach.end(); ++it)
				if (pUnit->isNativeDomain(kMap.plotByIndexUnchecked(it->iPlotIndex)))
					capture.push_back(it->iPlotIndex);
		}
		else
		{
			for (ReachablePlots::iterator it = reach.begin(); it != reach.end(); ++it)
			{
				const CvPlot* pMoveTile = kMap.plotByIndexUnchecked(it->iPlotIndex);
				if (pUnit->isNativeDomain(pMoveTile))
				{
					attack.push_back(it->iPlotIndex);
					if (!pMoveTile->isEnemyCity(*pUnit))
						capture.push_back(it->iPlotIndex);
				}
			}
		}
	}

	// the reach plots and every plot within iRings hex steps of one (breadth first through plotDirection: the map's wrap)
	void BuildDRegion(State* p, const ReachablePlots& reach, int iRings, std::vector<int>& region)
	{
		if (++p->uMark == 0)
		{
			memset(p->mark, 0, sizeof(u32) * (size_t)p->iNumPlots);
			p->uMark = 1;
		}
		CvMap& kMap = GC.getMap();
		region.clear();
		for (ReachablePlots::const_iterator it = reach.begin(); it != reach.end(); ++it)
			Mark(p, it->iPlotIndex, region);
		size_t begin = 0;
		for (int r = 0; r < iRings; r++)
		{
			const size_t end = region.size();
			for (size_t k = begin; k < end; k++)
			{
				const CvPlot* pPlot = kMap.plotByIndexUnchecked(region[k]);
				for (int d = 0; d < NUM_DIRECTION_TYPES; d++)
				{
					const CvPlot* pAdj = plotDirection(pPlot->getX(), pPlot->getY(), (DirectionTypes)d);
					if (pAdj)
						Mark(p, pAdj->GetPlotIndex(), region);
				}
			}
			begin = end;
		}
	}

	// DM_COUNT when the kept tiles still hold (the reach entry itself was just hit or stored)
	int DCheck(State* p, const Dg7& d, const DSnap& s, int eTeam)
	{
		if (!d.bKept)
			return DM_NONE;
		if (!d.snap.Same(s))
			return DM_SNAP;
		if ((u32)p->landEpoch != d.uLandEpoch)
			return DM_LAND;
		const u32 created = d.uCreated;
		const u32* stamps = p->plotStamp;
		const u32* team = p->teamPlotStamp;
		const size_t t = (size_t)eTeam;
		for (std::vector<int>::const_iterator it = d.region.begin(); it != d.region.end(); ++it)
		{
			if (stamps[*it] > created)
				return DM_REGION;
			if (team[(size_t)(*it) * MAX_TEAMS + t] > created)
				return DM_REGION_TEAM;
		}
		return DM_COUNT;
	}

	u64 DgBytes(const Dg7& d)
	{
		return (d.region.capacity() + d.attack.capacity() + d.capture.capacity()) * sizeof(int);
	}

#if defined(VP64_SPEED_DANGER_DG7_SPOIL) || defined(VP64_SPEED_DANGER_DG7_SPOIL_EVERY)
	// a wrong kept list the danger maps feel: the last attack tile dropped (else the last capture tile; else one added)
	void SpoilTiles(const CvUnit* pUnit, Dg7& d)
	{
		if (!d.attack.empty())
			d.attack.pop_back();
		else if (!d.capture.empty())
			d.capture.pop_back();
		else
			d.attack.push_back(pUnit->plot()->GetPlotIndex());
	}
#endif

#if defined(VP64_SPEED_DANGER_DG7_VERIFY)
	u64 HashList(const std::vector<int>& v)
	{
		u64 h = 1469598103934665603ULL;
		for (size_t i = 0; i < v.size(); i++)
		{
			h ^= (u64)(u32)v[i];
			h *= 1099511628211ULL;
		}
		return h ^ (u64)v.size();
	}

	// the kept and the fresh list, element by element in order
	void DCompare(const CvUnit* pUnit, const char* szSite, const std::vector<int>& kept, const std::vector<int>& fresh)
	{
		int iFirst = -1;
		const size_t n = kept.size() < fresh.size() ? kept.size() : fresh.size();
		for (size_t i = 0; i < n; i++)
			if (kept[i] != fresh[i])
			{
				iFirst = (int)i;
				break;
			}
		if (iFirst < 0 && kept.size() != fresh.size())
			iFirst = (int)n;
		if (iFirst < 0)
		{
			VP64_SPEED_CHECK_K("DANGER_DG7", szSite, (long long)kept.size(), (long long)fresh.size(), pUnit->getOwner(), pUnit->GetID(), 0, 0);
			return;
		}
		u64 hk = HashList(kept), hf = HashList(fresh);
		if (hk == hf)
			hk ^= 1;
		VP64_SPEED_CHECK_K("DANGER_DG7", szSite, (long long)hk, (long long)hf, pUnit->getOwner(), pUnit->GetID(), iFirst,
			(long long)kept.size() * 100000 + (long long)fresh.size());
	}
#endif
}

void LandmassEvent()
{
	State* p = Get();
	if (!p)
		return;
	InterlockedIncrement(&p->landEpoch);
}

bool Tiles(const CvUnit* pUnit, ReachablePlots& reach, std::vector<int>& attack, std::vector<int>& capture)
{
	State* p = Get();
	// core thread only, as the reach entries (two-thread rule); elsewhere the caller derives as stock
	if (!p || !pUnit || !pUnit->plot() || !gDLL->IsGameCoreThread())
		return false;
	const bool bHadLock = gDLL->HasGameCoreLock();
	if (!bHadLock)
		gDLL->GetGameCoreLock();
	VP64_DANGER_STAT(p->st.dLookups++);
	const u64 key = UnitKey((int)pUnit->getOwner(), pUnit->GetID());
	std::map<u64, Entry>::iterator it = p->cache.end();
	// only the entry the last Reach call hit or stored for this unit: its kept reach is the one being played
	if (p->bDg7Valid && p->uDg7Key == key && !p->bPartial && p->iNumPlots > 0)
		it = p->cache.find(key);
	if (it == p->cache.end())
	{
		VP64_DANGER_STAT(p->st.dNoEntry++);
		if (!bHadLock)
			gDLL->ReleaseGameCoreLock();
		return false;
	}
	DSnap ds;
	DSnapshot(pUnit, ds);
	const int eReason = DCheck(p, it->second.dg7, ds, it->second.eTeam);
	if (eReason == DM_COUNT)
	{
		VP64_DANGER_STAT(p->st.dHits++);
#if defined(VP64_SPEED_DANGER_DG7_SPOIL)
		if (VP64_SPEED_SPOIL_NOW("DANGER_DG7", VP64_SPEED_DANGER_DG7_SPOIL))
		{
			SpoilTiles(pUnit, it->second.dg7);
			VP64_DANGER_STAT(p->st.dSpoiled++);
		}
#endif
#if defined(VP64_SPEED_DANGER_DG7_SPOIL_EVERY)
		if (VP64_SPEED_SPOIL_EVERY("DANGER_DG7", VP64_SPEED_DANGER_DG7_SPOIL_EVERY))
		{
			SpoilTiles(pUnit, it->second.dg7);
			VP64_DANGER_STAT(p->st.dSpoiled++);
		}
#endif
#if defined(VP64_SPEED_DANGER_DG7_VERIFY)
		// derive again from the reach this call plays, compare, play the fresh lists
		const std::vector<int> keptA = it->second.dg7.attack;
		const std::vector<int> keptC = it->second.dg7.capture;
		Derive(pUnit, reach, ds.bRanged, attack, capture);
		VP64_DANGER_STAT(p->st.dVerified++);
		DCompare(pUnit, "attack", keptA, attack);
		DCompare(pUnit, "capture", keptC, capture);
#else
		attack = it->second.dg7.attack;
		capture = it->second.dg7.capture;
#endif
		if (!bHadLock)
			gDLL->ReleaseGameCoreLock();
		return true;
	}

	// a miss: derive (no entry is held across it), then keep the lists under the state read before it
	VP64_DANGER_STAT(p->st.dMiss[eReason]++);
	const u32 uCreated = (u32)p->uEvent;
	const u32 uLand = (u32)p->landEpoch;
	Derive(pUnit, reach, ds.bRanged, attack, capture);
	it = p->cache.find(key);
	if (it != p->cache.end() && p->bDg7Valid && p->uDg7Key == key)
	{
		Entry& e = it->second;
		Dg7& d = e.dg7;
		p->bytes -= e.uBytes;
		d.bKept = false;
		d.snap = ds;
		d.uCreated = uCreated;
		d.uLandEpoch = uLand;
		d.region.clear();
		if (ds.bRanged)
		{
			const int iRange = ds.iRange < 1 ? 1 : ds.iRange > 5 ? 5 : ds.iRange;
			BuildDRegion(p, reach, iRange + 1, d.region);
		}
		d.attack = attack;
		d.capture = capture;
		d.bKept = true;
		e.uBytes = EntryBytes(e) + DgBytes(d);
		p->bytes += e.uBytes;
		VP64_DANGER_STAT(p->st.dStores++);
		VP64_DANGER_STAT(p->st.dRegionPlots += d.region.size());
#if VP64_DANGER_STATS_ON
		if (p->bytes > p->st.bytesPeak)
			p->st.bytesPeak = p->bytes;
#endif
	}
	if (!bHadLock)
		gDLL->ReleaseGameCoreLock();
	return true;
}
#endif

} // namespace VP64Danger
