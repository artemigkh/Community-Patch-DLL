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

#if defined(VP64_SPEED_DANGER_STATS) || defined(VP64_SPEED_DANGER_VERIFY) || defined(VP64_SPEED_DANGER_SPOIL) || defined(VP64_SPEED_DANGER_SPOIL_EVERY)
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
		u64 uBytes;
	};

	enum MissReason { MR_COLD, MR_KEY, MR_UNIT, MR_TEAM, MR_PLAYER, MR_REGION, MR_REGION_TEAM, MR_COUNT };
	enum UncReason { UR_THREAD, UR_PARTIAL, UR_NOPLOT, UR_ZOC, UR_UNIT, UR_LUA, UR_NESTED, UR_COUNT };
	enum NotStored { NS_JUMP, NS_NESTED, NS_GEN, NS_COUNT };
	enum EventKind { EV_PLOT, EV_TEAM_PLOT, EV_UNIT, EV_TEAM, EV_PLAYER, EV_PLOTS, EV_COUNT };

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
	};
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
	void Compare(const CvUnit* pUnit, const ReachablePlots& kept, const ReachablePlots& fresh)
	{
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
			const ReachablePlots kept = it->second.result;
			t_iDepth++;
			ReachablePlots fresh = Search(pUnit, iFlags, zoc);
			t_iDepth--;
			VP64_DANGER_STAT(p->st.verified++);
			Compare(pUnit, kept, fresh);
			if (!bHadLock)
				gDLL->ReleaseGameCoreLock();
			return fresh;
#else
			ReachablePlots r = it->second.result;
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

} // namespace VP64Danger
