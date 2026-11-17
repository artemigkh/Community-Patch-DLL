// VP64ProtoReach.inl: the implementation of VP64ProtoReach.h (ticket 4674, evidence only). Included once, at the end
// of CvDangerPlots.cpp, only when VP64_PROTO_REACH is defined.
//
// Why the key and the invalidation keep the result (the full reasoning is in the knowledge base of the 64-bit patch,
// research/08-performance/turn-time-study.md, "W6 phase 2: prototype (b)"):
// - the danger maps ask for an enemy unit's reach from the unit's own point of view: SPathFinderUserData(pUnit, flags,
//   1) carries the unit's owner and id, flags, max turns 1, start moves = maxMoves(); the observing player enters only
//   through plotsToIgnoreForZOC, which is empty unless MOD_COMBATAI_TWO_PASS_DANGER is on (it is off in VP); a call
//   with a non-empty list is not cached. So every observer of the same unit asks the same question.
// - the answer reads (a) the unit: its plot, moves, max and base moves (both domains), damage, embark and cargo state,
//   type, AI type, attacks left, human/automated, promotions (a promotion change drops the entry); (b) its team and
//   player: techs (routes, bridges, embarkation, passability), war and peace, open borders, vassals, policies,
//   buildings (team and player epochs); (c) the plots the search touched and their neighbours: terrain, feature, route
//   and pillage, improvement and pillage, owner, cities, every unit on them, and what the unit's team sees and has
//   revealed there (plot stamps, and plot-and-team stamps for the team's view); a unit that can embark also reads
//   enemy ships next to a water plot it might enter, so its region has two rings; (d) a minor's ally and border
//   obstacles anywhere (a global epoch). Explorers (their end-turn cost reads city-state friendship), units with the
//   unwelcome-evangelist promotion and searches that reached an airlift, sealift or admiral-port node are not cached.
// - the cache is emptied at every game turn start (disengagement, the barbarian release turn and new moves are turn
//   inputs).
// The identity runs decide: a run whose game differs from the uncached one voids the variant.
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace VP64Reach
{
namespace
{
	typedef unsigned long long u64;
	typedef unsigned int u32;

	enum Mode { MODE_ON, MODE_VERIFY, MODE_OFF };

	struct Snap
	{
		int iPlot, iFlags, iStartMoves, iMinMovesLeft, iMaxTurns, ePath;
		int iMoves, iMaxMoves, iBaseLand, iBaseWater, iDamage, eUnitType, eAIType, eDomain, iExotic;
		bool bEmbarked, bOutOfAttacks, bHuman, bAutomated, bCargo, bCanEverEmbark;

		bool operator==(const Snap& r) const
		{
			return iPlot == r.iPlot && iFlags == r.iFlags && iStartMoves == r.iStartMoves && iMinMovesLeft == r.iMinMovesLeft &&
				iMaxTurns == r.iMaxTurns && ePath == r.ePath && iMoves == r.iMoves && iMaxMoves == r.iMaxMoves &&
				iBaseLand == r.iBaseLand && iBaseWater == r.iBaseWater && iDamage == r.iDamage && eUnitType == r.eUnitType &&
				eAIType == r.eAIType && eDomain == r.eDomain && iExotic == r.iExotic && bEmbarked == r.bEmbarked && bOutOfAttacks == r.bOutOfAttacks &&
				bHuman == r.bHuman && bAutomated == r.bAutomated && bCargo == r.bCargo && bCanEverEmbark == r.bCanEverEmbark;
		}
		bool SameKey(const Snap& r) const
		{
			return iPlot == r.iPlot && iFlags == r.iFlags && iStartMoves == r.iStartMoves && iMinMovesLeft == r.iMinMovesLeft &&
				iMaxTurns == r.iMaxTurns && ePath == r.ePath;
		}
	};

	struct Entry
	{
		Snap snap;
		int eTeam;
		u32 uCreated;          // the event counter when the search ran: a region stamp above it means a change since
		u32 uTeamEpoch, uPlayerEpoch, uGlobalEpoch;
		std::vector<int> region;
		ReachablePlots result;
		u64 uBytes;
		bool bAirlift;         // the search reached an airlift city (the region holds the airlift cities' plots)
	};

	enum MissReason { MR_COLD, MR_KEY, MR_UNIT, MR_TEAM, MR_PLAYER, MR_GLOBAL, MR_REGION, MR_REGION_TEAM, MR_COUNT };
	const char* const MR_NAMES[MR_COUNT] = { "cold", "key", "unit", "team", "player", "global", "region", "region_team" };

	struct TurnStats
	{
		u64 lookups, hits, miss[MR_COUNT], uncacheable, jumps, zoc, verified, mismatches, stores;
		u64 tMissCompute, tHitFresh, tAll, tJump, tMissAirlift, tHitFreshAirlift;
		u64 jumpKind[5];   // [1] airlift searches stored, [2] hits on airlift entries, [3] sealift, [4] admiral port (not cached)
		u64 evPlot, evTeamPlot, evUnit, evTeam, evPlayer, evGlobal[GR_COUNT], evGlobalEpoch;
		u64 bytesPeak, entriesPeak;
	};

	bool s_bInit = false;
	Mode s_eMode = MODE_ON;
	double s_dTicksPerMs = 1.0;
	std::string s_dir;
	FILE* s_fTurns = NULL;
	FILE* s_fMismatch = NULL;
	int s_iTurn = -1;
	int s_bPartial = 1;
	u64 s_turnT0 = 0;

	u32 s_uEvent = 0;                       // every plot event takes the next value
	int s_iNumPlots = 0;
	std::vector<u32> s_plotStamp;           // [plot]: the counter at the plot's last change
	std::vector<u32> s_teamPlotStamp;       // [plot * MAX_TEAMS + team]: the same for one team's view of the plot
	std::vector<u32> s_mark;                // scratch marks for the region
	u32 s_uMark = 0;
	u32 s_teamEpoch[MAX_TEAMS];
	u32 s_playerEpoch[MAX_PLAYERS];
	u32 s_globalEpoch = 0;

	std::map<u64, Entry> s_cache;
	u64 s_bytes = 0;
	u64 s_bytesPeakAll = 0;
	TurnStats s_st;

	inline u64 Now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return (u64)t.QuadPart;
	}
	inline double Ms(u64 ticks) { return (double)ticks / s_dTicksPerMs; }
	inline u64 UnitKey(int eOwner, int iID) { return ((u64)(u32)eOwner << 32) | (u64)(u32)iID; }

	FILE* Open(const char* name, const char* header)
	{
		std::string p = s_dir + "\\" + name;
		FILE* f = fopen(p.c_str(), "w");
		if (f)
		{
			fprintf(f, "%s\n", header);
			fflush(f);
		}
		return f;
	}

	void Init()
	{
		if (s_bInit)
			return;
		s_bInit = true;
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		s_dTicksPerMs = (double)f.QuadPart / 1000.0;
		char buf[MAX_PATH];
		DWORD n = GetEnvironmentVariableA("VP64_PROTO_REACH_DIR", buf, MAX_PATH);
		s_dir = (n > 0 && n < MAX_PATH) ? std::string(buf) : std::string(".");
		n = GetEnvironmentVariableA("VP64_PROTO_REACH_MODE", buf, MAX_PATH);
		std::string mode = (n > 0 && n < MAX_PATH) ? std::string(buf) : std::string("on");
		s_eMode = (mode == "verify") ? MODE_VERIFY : (mode == "off") ? MODE_OFF : MODE_ON;
		memset(s_teamEpoch, 0, sizeof(s_teamEpoch));
		memset(s_playerEpoch, 0, sizeof(s_playerEpoch));
		memset(&s_st, 0, sizeof(s_st));
		s_iTurn = GC.getGame().getGameTurn();
		s_bPartial = 1;
		s_turnT0 = Now();
		std::string hdr = "turn,partial,mode,wall_ms,lookups,hits,uncacheable,jumps,zoc";
		for (int i = 0; i < MR_COUNT; i++)
			hdr += std::string(",miss_") + MR_NAMES[i];
		hdr += ",stores,verified,mismatches,ms_all,ms_miss_compute,ms_hit_fresh,ms_overhead,entries_peak,bytes_peak,bytes_stamps"
			",ev_plot,ev_team_plot,ev_unit,ev_team,ev_player,ev_ally,ev_obstacle,ev_friends,ev_intrude,ev_global"
			",ms_jump,airlift_stored,airlift_hits,jump_sealift,jump_port,ms_miss_airlift,ms_hit_fresh_airlift";
		s_fTurns = Open("ReachCache.csv", hdr.c_str());
		s_fMismatch = Open("ReachCacheMismatch.csv", "turn,owner,unit,unit_type,x,y,cached_plots,fresh_plots,only_cached,only_fresh,"
			"field_diffs,first_diff_plot,first_diff_in_region,first_diff_stamp,first_diff_team_stamp,created,event_now,"
			"diff_owner,diff_city,diff_units,diff_revealed,diff_visible,diff_route,diff_feature,diff_improvement,"
			"team_epoch,player_epoch,global_epoch");
		FILE* fi = Open("ReachCacheInfo.txt", "VP64ProtoReach prototype (b), ticket 4674");
		if (fi)
		{
			fprintf(fi, "mode=%s\nqpc_ticks_per_ms=%.3f\nstart_turn=%d\n", mode.c_str(), s_dTicksPerMs, s_iTurn);
			fclose(fi);
		}
	}

	bool EnsureMap()
	{
		Init();
		if (s_iNumPlots > 0)
			return true;
		int n = GC.getMap().numPlots();
		if (n <= 0)
			return false;
		s_iNumPlots = n;
		s_plotStamp.assign(n, 0);
		s_teamPlotStamp.assign((size_t)n * MAX_TEAMS, 0);
		s_mark.assign(n, 0);
		return true;
	}

	u64 StampBytes()
	{
		return (u64)(s_plotStamp.capacity() + s_teamPlotStamp.capacity() + s_mark.capacity()) * sizeof(u32);
	}

	u64 EntryBytes(const Entry& e)
	{
		// the map node, the region, the result's storage and its index (pairs of int and size_t)
		return 64 + sizeof(Entry) + e.region.capacity() * sizeof(int) + e.result.size() * (sizeof(SMovePlot) + 16);
	}

	void Drop(std::map<u64, Entry>::iterator it)
	{
		s_bytes -= it->second.uBytes;
		s_cache.erase(it);
	}

	void WriteTurn()
	{
		if (!s_fTurns || s_iTurn < 0)
			return;
		const TurnStats& s = s_st;
		fprintf(s_fTurns, "%d,%d,%s,%.3f,%llu,%llu,%llu,%llu,%llu", s_iTurn, s_bPartial,
			s_eMode == MODE_VERIFY ? "verify" : s_eMode == MODE_OFF ? "off" : "on", Ms(Now() - s_turnT0),
			s.lookups, s.hits, s.uncacheable, s.jumps, s.zoc);
		for (int i = 0; i < MR_COUNT; i++)
			fprintf(s_fTurns, ",%llu", s.miss[i]);
		fprintf(s_fTurns, ",%llu,%llu,%llu,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%llu,%llu,%llu,%llu,%.3f,%.3f\n",
			s.stores, s.verified, s.mismatches, Ms(s.tAll), Ms(s.tMissCompute), Ms(s.tHitFresh), Ms(s.tAll - s.tMissCompute - s.tHitFresh),
			s.entriesPeak, s.bytesPeak, StampBytes(), s.evPlot, s.evTeamPlot, s.evUnit, s.evTeam, s.evPlayer,
			s.evGlobal[GR_MINOR_ALLY], s.evGlobal[GR_BORDER_OBSTACLE], s.evGlobal[GR_MINOR_FRIENDS], s.evGlobal[GR_MINOR_INTRUDE],
			s.evGlobalEpoch, Ms(s.tJump), s.jumpKind[1], s.jumpKind[2], s.jumpKind[3], s.jumpKind[4], Ms(s.tMissAirlift),
			Ms(s.tHitFreshAirlift));
		fflush(s_fTurns);
	}

	inline void BumpPlot(int iPlot)
	{
		if (iPlot >= 0 && iPlot < s_iNumPlots)
			s_plotStamp[iPlot] = ++s_uEvent;
	}

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

	// the plots the last search of the path finder closed, their neighbours (and theirs for iRings 2); 0, or the kind
	// of jump (3 sealift, 4 admiral port) when a closed node could jump that way: such a search reads cities anywhere,
	// so it is not cached. An airlift from a city (bAirlift) is cached: its extra children are the plots around every
	// airlift city of the owner and its allied minors (UnitPathGetExtraChildren), and canAirliftAt reads those plots
	// and their neighbours, so the region also takes two rings around every city of the owner and its allied minors
	// (a city gained or lost, an ally change and a building change bump the owner's epoch or the city's plot). An
	// airlift from an improvement adds no children (UnitPathGetExtraChildren: "todo"), so it is an ordinary node.
	int BuildRegion(const CvUnit* pUnit, int iRings, std::vector<int>& region, bool& bAirlift)
	{
		const std::vector<CvAStarNode*>& closed = GC.GetPathFinder().VP64ClosedNodes();
		if (++s_uMark == 0)
		{
			std::fill(s_mark.begin(), s_mark.end(), 0);
			s_uMark = 1;
		}
		CvMap& kMap = GC.getMap();
		region.clear();
		bAirlift = false;
		for (std::vector<CvAStarNode*>::const_iterator it = closed.begin(); it != closed.end(); ++it)
		{
			const CvAStarNode* node = *it;
			const CvPathNodeCacheData& c = node->m_kCostCacheData;
			if (c.bCanAirliftFromPlotCity)
				bAirlift = true;
			if (c.bCanSealiftFromPlotCity)
				return 3;
			if (c.bCanChangePortFromPlot)
				return 4;
			int i = kMap.plotNum(node->m_iX, node->m_iY);
			if (i >= 0 && i < s_iNumPlots && s_mark[i] != s_uMark)
			{
				s_mark[i] = s_uMark;
				region.push_back(i);
			}
		}
		size_t begin = 0;
		for (int r = 0; r < iRings; r++)
		{
			size_t end = region.size();
			for (size_t k = begin; k < end; k++)
			{
				CvPlot* pPlot = kMap.plotByIndexUnchecked(region[k]);
				for (int d = 0; d < NUM_DIRECTION_TYPES; d++)
				{
					CvPlot* pAdj = plotDirection(pPlot->getX(), pPlot->getY(), (DirectionTypes)d);
					if (!pAdj)
						continue;
					int j = pAdj->GetPlotIndex();
					if (s_mark[j] != s_uMark)
					{
						s_mark[j] = s_uMark;
						region.push_back(j);
					}
				}
			}
			begin = end;
		}
		if (bAirlift)
		{
			PlayerTypes eOwner = pUnit->getOwner();
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
						CvPlot* pRing = iterateRingPlots(pCity->plot(), k);
						if (!pRing)
							continue;
						int j = pRing->GetPlotIndex();
						if (s_mark[j] != s_uMark)
						{
							s_mark[j] = s_uMark;
							region.push_back(j);
						}
					}
				}
			}
		}
		return 0;
	}

	// MR_COUNT when the entry still holds
	int Check(const Entry& e, const Snap& s, int eOwner)
	{
		if (!e.snap.SameKey(s))
			return MR_KEY;
		if (!(e.snap == s))
			return MR_UNIT;
		if (e.uTeamEpoch != s_teamEpoch[e.eTeam])
			return MR_TEAM;
		if (e.uPlayerEpoch != s_playerEpoch[eOwner])
			return MR_PLAYER;
		if (e.uGlobalEpoch != s_globalEpoch)
			return MR_GLOBAL;
		const u32 created = e.uCreated;
		const u32* stamps = s_plotStamp.empty() ? NULL : &s_plotStamp[0];
		const u32* team = s_teamPlotStamp.empty() ? NULL : &s_teamPlotStamp[0];
		for (std::vector<int>::const_iterator it = e.region.begin(); it != e.region.end(); ++it)
		{
			if (stamps[*it] > created)
				return MR_REGION;
			if (team[(size_t)(*it) * MAX_TEAMS + e.eTeam] > created)
				return MR_REGION_TEAM;
		}
		return MR_COUNT;
	}

	void Compare(const CvUnit* pUnit, const Entry& e, const ReachablePlots& fresh)
	{
		// both are sorted by plot index (createIndex keeps the insertion order in storage, so compare by lookup)
		std::map<int, SMovePlot> a, b;
		for (ReachablePlots::const_iterator it = e.result.begin(); it != e.result.end(); ++it)
			a.insert(std::make_pair(it->iPlotIndex, *it));
		for (ReachablePlots::const_iterator it = fresh.begin(); it != fresh.end(); ++it)
			b.insert(std::make_pair(it->iPlotIndex, *it));
		int onlyA = 0, onlyB = 0, fields = 0, first = -1;
		bool bOrder = (e.result.size() == fresh.size());
		if (bOrder)
		{
			ReachablePlots::const_iterator ia = e.result.begin(), ib = fresh.begin();
			for (; ia != e.result.end(); ++ia, ++ib)
				if (ia->iPlotIndex != ib->iPlotIndex)
				{
					bOrder = false;
					break;
				}
		}
		for (std::map<int, SMovePlot>::const_iterator it = a.begin(); it != a.end(); ++it)
		{
			std::map<int, SMovePlot>::const_iterator jt = b.find(it->first);
			if (jt == b.end())
			{
				onlyA++;
				if (first < 0)
					first = it->first;
			}
			else if (it->second.iPathLength != jt->second.iPathLength || it->second.iMovesLeft != jt->second.iMovesLeft ||
				it->second.iNormalizedDistanceRaw != jt->second.iNormalizedDistanceRaw)
			{
				fields++;
				if (first < 0)
					first = it->first;
			}
		}
		for (std::map<int, SMovePlot>::const_iterator it = b.begin(); it != b.end(); ++it)
			if (a.find(it->first) == a.end())
			{
				onlyB++;
				if (first < 0)
					first = it->first;
			}
		if (onlyA == 0 && onlyB == 0 && fields == 0 && bOrder)
			return;
		s_st.mismatches++;
		if (!s_fMismatch)
			return;
		bool bInRegion = first >= 0 && std::find(e.region.begin(), e.region.end(), first) != e.region.end();
		const CvPlot* pD = first >= 0 ? GC.getMap().plotByIndexUnchecked(first) : NULL;
		fprintf(s_fMismatch, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u\n", s_iTurn,
			(int)pUnit->getOwner(), pUnit->GetID(),
			(int)pUnit->getUnitType(), pUnit->getX(), pUnit->getY(), (int)e.result.size(), (int)fresh.size(), onlyA, onlyB,
			fields + (bOrder ? 0 : 1000000), first, bInRegion ? 1 : 0, first >= 0 ? s_plotStamp[first] : 0,
			first >= 0 ? s_teamPlotStamp[(size_t)first * MAX_TEAMS + e.eTeam] : 0, e.uCreated, s_uEvent,
			pD ? (int)pD->getOwner() : -2, pD ? (int)pD->isCity() : -1, pD ? pD->getNumUnits() : -1,
			pD ? (int)pD->isRevealed((TeamTypes)e.eTeam) : -1, pD ? (int)pD->isVisible((TeamTypes)e.eTeam) : -1,
			pD ? (int)pD->getRouteType() : -2, pD ? (int)pD->getFeatureType() : -2, pD ? (int)pD->getImprovementType() : -2,
			s_teamEpoch[e.eTeam], s_playerEpoch[(int)pUnit->getOwner()], s_globalEpoch);
		fflush(s_fMismatch);
	}

	ReachablePlots Search(const CvUnit* pUnit, int iFlags, const PlotIndexContainer& zoc)
	{
		return TacticalAIHelpers::GetAllPlotsInReachThisTurn(pUnit, pUnit->plot(), iFlags, 0, pUnit->maxMoves(), zoc);
	}
}

void PlotEvent(const CvPlot* pPlot)
{
	if (!pPlot || !EnsureMap())
		return;
	s_st.evPlot++;
	BumpPlot(pPlot->GetPlotIndex());
}

void PlotXYEvent(int iX, int iY)
{
	if (!EnsureMap())
		return;
	PlotEvent(GC.getMap().plot(iX, iY));
}

void TeamPlotEvent(const CvPlot* pPlot, int eTeam)
{
	if (!pPlot || eTeam < 0 || eTeam >= MAX_TEAMS || !EnsureMap())
		return;
	s_st.evTeamPlot++;
	int i = pPlot->GetPlotIndex();
	if (i >= 0 && i < s_iNumPlots)
		s_teamPlotStamp[(size_t)i * MAX_TEAMS + eTeam] = ++s_uEvent;
}

void UnitEvent(const CvUnit* pUnit)
{
	if (!pUnit || !EnsureMap())
		return;
	s_st.evUnit++;
	std::map<u64, Entry>::iterator it = s_cache.find(UnitKey(pUnit->getOwner(), pUnit->GetID()));
	if (it != s_cache.end())
		Drop(it);
	if (pUnit->plot())
		BumpPlot(pUnit->plot()->GetPlotIndex());
}

void UnitMove(const CvPlot* pOld, const CvPlot* pNew)
{
	if (!EnsureMap())
		return;
	s_st.evUnit++;
	if (pOld)
		BumpPlot(pOld->GetPlotIndex());
	if (pNew)
		BumpPlot(pNew->GetPlotIndex());
}

void TeamEvent(int eTeam)
{
	Init();
	if (eTeam < 0 || eTeam >= MAX_TEAMS)
		return;
	s_st.evTeam++;
	s_teamEpoch[eTeam]++;
}

void PlayerEvent(int ePlayer)
{
	Init();
	if (ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return;
	s_st.evPlayer++;
	s_playerEpoch[ePlayer]++;
}

void GlobalEvent(int eReason)
{
	Init();
	if (eReason >= 0 && eReason < GR_COUNT)
		s_st.evGlobal[eReason]++;
	s_st.evGlobalEpoch++;
	s_globalEpoch++;
}

void PlayerPlotsEvent(int ePlayer, int eReason)
{
	if (ePlayer < 0 || ePlayer >= MAX_PLAYERS || !EnsureMap())
		return;
	if (eReason >= 0 && eReason < GR_COUNT)
		s_st.evGlobal[eReason]++;
	CvMap& kMap = GC.getMap();
	for (int i = 0; i < s_iNumPlots; i++)
		if ((int)kMap.plotByIndexUnchecked(i)->getOwner() == ePlayer)
			BumpPlot(i);
}

void TeamPlotsEvent(int eTeam, int eReason)
{
	if (eTeam < 0 || eTeam >= MAX_TEAMS || !EnsureMap())
		return;
	if (eReason >= 0 && eReason < GR_COUNT)
		s_st.evGlobal[eReason]++;
	CvMap& kMap = GC.getMap();
	for (int i = 0; i < s_iNumPlots; i++)
		if ((int)kMap.plotByIndexUnchecked(i)->getTeam() == eTeam)
			BumpPlot(i);
}

void TurnStart(int iTurn)
{
	Init();
	WriteTurn();
	s_cache.clear();
	s_bytes = 0;
	memset(&s_st, 0, sizeof(s_st));
	s_iTurn = iTurn;
	s_bPartial = 0;
	s_turnT0 = Now();
}

ReachablePlots DangerReach(const CvUnit* pUnit, int iFlags, const PlotIndexContainer& zoc)
{
	bool bHadLock = gDLL->HasGameCoreLock();
	if (!bHadLock)
		gDLL->GetGameCoreLock();
	u64 t0 = Now();
	Init();
	ReachablePlots result;
	s_st.lookups++;

	if (s_eMode == MODE_OFF || !pUnit || !pUnit->plot() || !EnsureMap())
	{
		u64 a = Now();
		result = Search(pUnit, iFlags, zoc);
		s_st.tMissCompute += Now() - a;
		s_st.uncacheable++;
	}
	else
	{
		// the same data as GetAllPlotsInReachThisTurn builds, for the key
		int iDataFlags = iFlags;
		if (!zoc.empty())
			iDataFlags |= CvUnit::MOVEFLAG_SELECTIVE_ZOC;
		SPathFinderUserData data(pUnit, iDataFlags, 1);
		data.iMinMovesLeft = 0;
		if (pUnit->maxMoves() > -1)
			data.iStartMoves = pUnit->maxMoves();
		Snap snap;
		Snapshot(pUnit, data, snap);
		int eOwner = (int)pUnit->getOwner();
		u64 key = UnitKey(eOwner, pUnit->GetID());
		// nothing is cached before the session's first game turn start: the load and the steps after it set state
		// without the setters the invalidation hooks (s_bPartial is cleared by TurnStart)
		bool bCacheable = !s_bPartial && zoc.empty() && snap.eAIType != UNITAI_EXPLORE && snap.eAIType != UNITAI_EXPLORE_SEA &&
			!pUnit->isHasPromotion((PromotionTypes)GD_INT_GET(PROMOTION_UNWELCOME_EVANGELIST));
		if (!zoc.empty())
			s_st.zoc++;

		std::map<u64, Entry>::iterator it = bCacheable ? s_cache.find(key) : s_cache.end();
		int reason = (it == s_cache.end()) ? MR_COLD : Check(it->second, snap, eOwner);
		if (bCacheable && reason == MR_COUNT)
		{
			s_st.hits++;
			if (it->second.bAirlift)
				s_st.jumpKind[2]++;
			if (s_eMode == MODE_VERIFY)
			{
				u64 a = Now();
				result = Search(pUnit, iFlags, zoc);
				s_st.tHitFresh += Now() - a;
				if (it->second.bAirlift)
					s_st.tHitFreshAirlift += Now() - a;
				s_st.verified++;
				Compare(pUnit, it->second, result);
			}
			else
				result = it->second.result;
		}
		else
		{
			if (bCacheable)
				s_st.miss[reason]++;
			else
				s_st.uncacheable++;
			u64 a = Now();
			result = Search(pUnit, iFlags, zoc);
			u64 tSearch = Now() - a;
			s_st.tMissCompute += tSearch;
			if (bCacheable)
			{
				if (it != s_cache.end())
					Drop(it);
				Entry e;
				e.snap = snap;
				e.eTeam = (int)pUnit->getTeam();
				e.uCreated = s_uEvent;
				e.uTeamEpoch = s_teamEpoch[e.eTeam];
				e.uPlayerEpoch = s_playerEpoch[eOwner];
				e.uGlobalEpoch = s_globalEpoch;
				bool bAirlift = false;
				int iJump = BuildRegion(pUnit, snap.bCanEverEmbark ? 2 : 1, e.region, bAirlift);
				e.bAirlift = bAirlift;
				if (bAirlift)
				{
					s_st.tMissAirlift += tSearch;
					s_st.jumpKind[1]++;
				}
				if (iJump == 0)
				{
					e.result = result;
					e.uBytes = EntryBytes(e);
					s_bytes += e.uBytes;
					s_cache.insert(std::make_pair(key, e));
					s_st.stores++;
					if (s_bytes > s_st.bytesPeak)
						s_st.bytesPeak = s_bytes;
					if (s_bytes > s_bytesPeakAll)
						s_bytesPeakAll = s_bytes;
					if ((u64)s_cache.size() > s_st.entriesPeak)
						s_st.entriesPeak = (u64)s_cache.size();
				}
				else
				{
					s_st.jumps++;
					s_st.jumpKind[iJump]++;
					s_st.tJump += tSearch;
				}
			}
		}
	}

	u64 dt = Now() - t0;
	s_st.tAll += dt;
	if (!bHadLock)
		gDLL->ReleaseGameCoreLock();
	return result;
}

} // namespace VP64Reach
