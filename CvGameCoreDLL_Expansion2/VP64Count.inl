// VP64Count.inl: the counting builds' implementation (VP64Count.h says what they count). Included once, at the end
// of CvGame.cpp, only when one of VP64_COUNT_TRADEPATH, VP64_COUNT_REACH, VP64_COUNT_PARALLEL is defined.
// Reads game state, hashes it and times it; writes CSV files into %VP64_COUNT_DIR% (else the working folder).
// Nothing here changes a decision of the game: no game object is written, no random number is drawn.
#include <unordered_map>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

namespace VP64Count
{
namespace
{
	const int NFRAMES = 10;
	const char* const EV_NAMES[EV_COUNT] = {
		"route", "route_pillage", "improvement", "improvement_pillage", "feature", "owner",
		"revealed", "revealed_improvement", "revealed_route", "visibility",
		"war", "peace", "city_found", "city_acquire", "city_kill",
		"unit_move", "unit_create", "unit_death", "danger_update" };

	SRWLOCK s_lock = SRWLOCK_INIT;
	struct Lock
	{
		Lock() { AcquireSRWLockExclusive(&s_lock); }
		~Lock() { ReleaseSRWLockExclusive(&s_lock); }
	};

	bool s_bInit = false;
	double s_dTicksPerMs = 1.0;
	u64 s_moduleBase = 0;
	std::string s_dir;
	int s_iTurn = -1;
	int s_bPartial = 1;
	u64 s_turnT0 = 0;
	u64 s_overhead = 0;      // ticks spent inside the counters this turn
	u64 s_overheadAll = 0;   // the same since the start (windows subtract it)
	u64 s_ev[EV_COUNT];      // events this turn, by type
	u64 s_evAll = 0;         // every event since the start: the reach counter's world epoch
	u64 s_evPlot = 0;        // plot, team and city events (owner, route, improvement, feature, pillage, war, peace, cities)
	u64 s_evUnit = 0;        // unit moves, creations, deaths
	u64 s_evView[MAX_TEAMS]; // revealed* and visibility events of each team
	u64 s_evSeen[MAX_TEAMS]; // unit events on a plot the team sees
	u64 s_evPlot0 = 0, s_evUnit0 = 0, s_evView0[MAX_TEAMS], s_evSeen0[MAX_TEAMS];

	FILE* s_fTurns = NULL;
	FILE* s_fStacks = NULL;
#if defined(VP64_COUNT_TRADEPATH)
	FILE* s_fTrade = NULL;
	FILE* s_fTradeInv = NULL;
#endif
#if defined(VP64_COUNT_REACH)
	FILE* s_fReach = NULL;
	FILE* s_fReachUneq = NULL;
#endif
#if defined(VP64_COUNT_PARALLEL)
	FILE* s_fPar = NULL;
#endif

	std::unordered_map<u64, int> s_stackIds;
	int s_nStacks = 0;

	inline u64 Now0()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return (u64)t.QuadPart;
	}
	inline u64 SM(u64 x)
	{
		x += 0x9e3779b97f4a7c15ULL;
		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
		x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
		return x ^ (x >> 31);
	}
	inline u64 H(u64 h, u64 v) { return SM(h ^ v); }
	inline u64 H2(u64 h, int a, int b) { return H(h, (u64)(unsigned int)a | ((u64)(unsigned int)b << 32)); }
	inline double Ms(u64 ticks) { return (double)ticks / s_dTicksPerMs; }

	FILE* Open(const char* name, const char* header)
	{
		std::string p = s_dir + "\\" + name;
		FILE* f = fopen(p.c_str(), "w");
		if (f)
			fprintf(f, "%s\n", header);
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
		HMODULE hMod = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Init, &hMod);
		s_moduleBase = (u64)(uintptr_t)hMod;
		char buf[MAX_PATH];
		DWORD n = GetEnvironmentVariableA("VP64_COUNT_DIR", buf, MAX_PATH);
		s_dir = (n > 0 && n < MAX_PATH) ? std::string(buf) : std::string(".");
		memset(s_ev, 0, sizeof(s_ev));
		memset(s_evView, 0, sizeof(s_evView));
		memset(s_evSeen, 0, sizeof(s_evSeen));
		memset(s_evView0, 0, sizeof(s_evView0));
		memset(s_evSeen0, 0, sizeof(s_evSeen0));
		s_iTurn = GC.getGame().getGameTurn();
		s_bPartial = 1;
		s_turnT0 = Now0();

		std::string hdr = "turn,partial,wall_ms,counter_ms";
		for (int i = 0; i < EV_COUNT; i++)
			hdr += std::string(",ev_") + EV_NAMES[i];
		hdr += ",reach_distinct_a,reach_distinct_a_plots,reach_distinct_b,reach_distinct_b_plots";
		s_fTurns = Open("VP64CountTurns.csv", hdr.c_str());
		s_fStacks = Open("VP64CountStacks.csv", "stack,rva_frames");
#if defined(VP64_COUNT_TRADEPATH)
		s_fTrade = Open("TradePathCount.csv", "turn,partial,player,reason,last_update,inv_count,inv_stack,cities,eq_paths,eq_info,new,gone,dests,path_nodes,ms_total,ms_cities,ms_eq_paths,ms_eq_info,first_of_turn");
		s_fTradeInv = Open("TradePathInvalidations.csv", "turn,partial,stack,count");
#endif
#if defined(VP64_COUNT_REACH)
		s_fReach = Open("ReachCount.csv", "turn,partial,stack,calls,nested,repeats_a,eq_a,uneq_a,eq_b,ms_all,ms_eq_a,ms_eq_b,ms_nested,plots_all,plots_eq_a");
		s_fReachUneq = Open("ReachUnequal.csv", "turn,stack,x,y,path,flags,player,unit,epoch,plots");
#endif
#if defined(VP64_COUNT_PARALLEL)
		s_fPar = Open("ParallelPremise.csv", "turn,partial,player,kind,first_of_turn,ms,ev_plot,ev_unit,ev_view_team,ev_unit_seen_team,ev_types");
#endif
		FILE* fi = Open("VP64CountInfo.txt", "VP64Count counting build (ticket 4669)");
		if (fi)
		{
			fprintf(fi, "module_base=0x%llx\nqpc_ticks_per_ms=%.3f\nstart_turn=%d\n", s_moduleBase, s_dTicksPerMs, s_iTurn);
#if defined(VP64_COUNT_TRADEPATH)
			fprintf(fi, "define=VP64_COUNT_TRADEPATH\n");
#endif
#if defined(VP64_COUNT_REACH)
			fprintf(fi, "define=VP64_COUNT_REACH\n");
#endif
#if defined(VP64_COUNT_PARALLEL)
			fprintf(fi, "define=VP64_COUNT_PARALLEL\n");
#endif
			fclose(fi);
		}
	}

#if defined(VP64_COUNT_TRADEPATH) || defined(VP64_COUNT_REACH)
	// the stack id of the caller's call chain: frame 0 is the function that called CaptureStack
	__declspec(noinline) int CaptureStack()
	{
		void* fr[NFRAMES];
		memset(fr, 0, sizeof(fr));
		USHORT n = RtlCaptureStackBackTrace(1, NFRAMES, fr, NULL);
		u64 rva[NFRAMES];
		u64 h = 0x5641363443ULL;
		for (int i = 0; i < NFRAMES; i++)
		{
			rva[i] = (i < (int)n && fr[i] != NULL) ? (u64)(uintptr_t)fr[i] - s_moduleBase : 0;
			h = H(h, rva[i]);
		}
		std::unordered_map<u64, int>::const_iterator it = s_stackIds.find(h);
		if (it != s_stackIds.end())
			return it->second;
		int id = s_nStacks++;
		s_stackIds[h] = id;
		if (s_fStacks)
		{
			fprintf(s_fStacks, "%d,", id);
			for (int i = 0; i < NFRAMES; i++)
				fprintf(s_fStacks, i ? " %llx" : "%llx", rva[i]);
			fprintf(s_fStacks, "\n");
		}
		return id;
	}
#endif

#if defined(VP64_COUNT_TRADEPATH) || defined(VP64_COUNT_PARALLEL)
	int TeamOf(int ePlayer)
	{
		return (ePlayer >= 0 && ePlayer < MAX_PLAYERS) ? (int)GET_PLAYER((PlayerTypes)ePlayer).getTeam() : -1;
	}

	// events seen since the turn start by ePlayer's team: plot, unit, view, unit seen; and the by-type list
	struct EventsBefore
	{
		u64 plot, unit, view, seen;
		std::string types;
	};
	EventsBefore Before(int ePlayer)
	{
		EventsBefore e;
		int t = TeamOf(ePlayer);
		e.plot = s_evPlot - s_evPlot0;
		e.unit = s_evUnit - s_evUnit0;
		e.view = (t >= 0 && t < MAX_TEAMS) ? s_evView[t] - s_evView0[t] : 0;
		e.seen = (t >= 0 && t < MAX_TEAMS) ? s_evSeen[t] - s_evSeen0[t] : 0;
		char b[32];
		for (int i = 0; i < EV_COUNT; i++)
		{
			sprintf(b, i ? ";%llu" : "%llu", s_ev[i]);
			e.types += b;
		}
		return e;
	}
#endif

#if defined(VP64_COUNT_TRADEPATH) || defined(VP64_COUNT_PARALLEL)
	struct TradeState
	{
		bool bActive;
		u64 t0, ovh0;
		int iLastUpdate;
		int bFirstOfTurn;
		int iLastRebuildTurn;
		int iParTurn;
		EventsBefore before;
#if defined(VP64_COUNT_TRADEPATH)
		int iInvCount, iInvStack;
		int iCities, iEqPaths, iEqInfo, iNew;
		u64 iDests, iPathNodes;
		u64 tCities, tEqPaths, tEqInfo;
		std::vector<int> prevCities, curCities;
#endif
	};
	TradeState s_trade[MAX_PLAYERS];
	bool s_tradeInit = false;
	void TradeInitOnce()
	{
		if (s_tradeInit)
			return;
		s_tradeInit = true;
		for (int i = 0; i < MAX_PLAYERS; i++)
		{
			TradeState& s = s_trade[i];
			s.bActive = false;
			s.t0 = s.ovh0 = 0;
			s.iLastUpdate = -1;
			s.bFirstOfTurn = 0;
			s.iLastRebuildTurn = -1;
			s.iParTurn = -1;
			s.before.plot = s.before.unit = s.before.view = s.before.seen = 0;
#if defined(VP64_COUNT_TRADEPATH)
			s.iInvCount = 0;
			s.iInvStack = -1;
			s.iCities = s.iEqPaths = s.iEqInfo = s.iNew = 0;
			s.iDests = s.iPathNodes = 0;
			s.tCities = s.tEqPaths = s.tEqInfo = 0;
#endif
		}
	}
#endif

#if defined(VP64_COUNT_TRADEPATH)
	std::unordered_map<u64, std::pair<u64, u64> > s_tradeHash; // (player << 32 | city plot) -> (paths hash, info hash)
	std::unordered_map<int, u64> s_tradeInvAgg;                // this turn: stack -> invalidations
#endif

#if defined(VP64_COUNT_REACH)
	struct ReachAgg
	{
		u64 calls, nested, repA, eqA, uneqA, eqB, tAll, tEqA, tEqB, tNested, plotsAll, plotsEqA;
	};
	std::unordered_map<int, ReachAgg> s_reachAgg;   // this turn, by stack
	std::unordered_map<u64, u64> s_reachA;          // this turn: key with the epoch -> result hash
	std::unordered_map<u64, u64> s_reachB;          // this turn: key without the epoch -> latest result hash
	u64 s_distinctA = 0, s_distinctAPlots = 0, s_distinctB = 0, s_distinctBPlots = 0;
	int s_reachDepth = 0;
	int s_reachUneqWritten = 0;
#endif

#if defined(VP64_COUNT_PARALLEL)
	struct DangerState
	{
		int iDepth;
		int bFirst;
		u64 t0, ovh0;
		EventsBefore before;
	};
	DangerState s_danger[MAX_PLAYERS];
	int s_dangerTurn[MAX_PLAYERS];
	bool s_dangerInit = false;
	void DangerInitOnce()
	{
		if (s_dangerInit)
			return;
		s_dangerInit = true;
		for (int i = 0; i < MAX_PLAYERS; i++)
		{
			s_danger[i].iDepth = 0;
			s_danger[i].bFirst = 0;
			s_danger[i].t0 = s_danger[i].ovh0 = 0;
			s_dangerTurn[i] = -1;
		}
	}
#endif

	void AddOverhead(u64 t0)
	{
		u64 d = Now0() - t0;
		s_overhead += d;
		s_overheadAll += d;
	}

	void FlushTurn(u64 tNow)
	{
		if (s_fTurns)
		{
			fprintf(s_fTurns, "%d,%d,%.3f,%.3f", s_iTurn, s_bPartial, Ms(tNow - s_turnT0), Ms(s_overhead));
			for (int i = 0; i < EV_COUNT; i++)
				fprintf(s_fTurns, ",%llu", s_ev[i]);
#if defined(VP64_COUNT_REACH)
			fprintf(s_fTurns, ",%llu,%llu,%llu,%llu\n", s_distinctA, s_distinctAPlots, s_distinctB, s_distinctBPlots);
#else
			fprintf(s_fTurns, ",0,0,0,0\n");
#endif
			fflush(s_fTurns);
		}
#if defined(VP64_COUNT_TRADEPATH)
		if (s_fTradeInv)
		{
			for (std::unordered_map<int, u64>::const_iterator it = s_tradeInvAgg.begin(); it != s_tradeInvAgg.end(); ++it)
				fprintf(s_fTradeInv, "%d,%d,%d,%llu\n", s_iTurn, s_bPartial, it->first, it->second);
			fflush(s_fTradeInv);
		}
		s_tradeInvAgg.clear();
		if (s_fTrade)
			fflush(s_fTrade);
#endif
#if defined(VP64_COUNT_REACH)
		if (s_fReach)
		{
			for (std::unordered_map<int, ReachAgg>::const_iterator it = s_reachAgg.begin(); it != s_reachAgg.end(); ++it)
			{
				const ReachAgg& a = it->second;
				fprintf(s_fReach, "%d,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%llu,%llu\n", s_iTurn, s_bPartial, it->first,
					a.calls, a.nested, a.repA, a.eqA, a.uneqA, a.eqB, Ms(a.tAll), Ms(a.tEqA), Ms(a.tEqB), Ms(a.tNested), a.plotsAll, a.plotsEqA);
			}
			fflush(s_fReach);
		}
		if (s_fReachUneq)
			fflush(s_fReachUneq);
		s_reachAgg.clear();
		s_reachA.clear();
		s_reachB.clear();
		s_distinctA = s_distinctAPlots = s_distinctB = s_distinctBPlots = 0;
#endif
#if defined(VP64_COUNT_PARALLEL)
		if (s_fPar)
			fflush(s_fPar);
#endif
		if (s_fStacks)
			fflush(s_fStacks);
	}
} // anonymous namespace

u64 Now()
{
	return Now0();
}

void OnEvent(Event e, int eTeam)
{
	Lock l;
	Init();
	s_ev[e]++;
	s_evAll++;
	if (e == EV_REVEALED || e == EV_REVEALED_IMPROVEMENT || e == EV_REVEALED_ROUTE || e == EV_VISIBILITY)
	{
		if (eTeam >= 0 && eTeam < MAX_TEAMS)
			s_evView[eTeam]++;
	}
	else if (e != EV_DANGER_UPDATE)
		s_evPlot++;
}

void OnUnitEvent(Event e, const CvPlot* pA, const CvPlot* pB)
{
	Lock l;
	Init();
	s_ev[e]++;
	s_evAll++;
	s_evUnit++;
	for (int t = 0; t < MAX_CIV_TEAMS; t++)
	{
		if ((pA && pA->isVisible((TeamTypes)t)) || (pB && pB->isVisible((TeamTypes)t)))
			s_evSeen[t]++;
	}
}

void OnTurnStart(int iGameTurn)
{
	Lock l;
	Init();
	u64 t = Now0();
	FlushTurn(t);
	s_iTurn = iGameTurn;
	s_bPartial = 0;
	s_turnT0 = t;
	s_overhead = 0;
	memset(s_ev, 0, sizeof(s_ev));
	s_evPlot0 = s_evPlot;
	s_evUnit0 = s_evUnit;
	memcpy(s_evView0, s_evView, sizeof(s_evView));
	memcpy(s_evSeen0, s_evSeen, sizeof(s_evSeen));
}

#if defined(VP64_COUNT_TRADEPATH) || defined(VP64_COUNT_PARALLEL)
void TradeRebuildBegin(int ePlayer, int iLastUpdate)
{
	if (ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return;
	Lock l;
	Init();
	TradeInitOnce();
	u64 t = Now0();
	TradeState& s = s_trade[ePlayer];
	s.bActive = true;
	s.iLastUpdate = iLastUpdate;
	s.bFirstOfTurn = (s.iParTurn != s_iTurn) ? 1 : 0;
	s.iParTurn = s_iTurn;
	s.before = Before(ePlayer);
#if defined(VP64_COUNT_TRADEPATH)
	s.iCities = s.iEqPaths = s.iEqInfo = s.iNew = 0;
	s.iDests = s.iPathNodes = 0;
	s.tCities = s.tEqPaths = s.tEqInfo = 0;
	s.curCities.clear();
#endif
	AddOverhead(t);
	s.ovh0 = s_overheadAll;
	s.t0 = Now0();
}

void TradeRebuildEnd(int ePlayer)
{
	u64 t1 = Now0();
	if (ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return;
	Lock l;
	Init();
	TradeInitOnce();
	TradeState& s = s_trade[ePlayer];
	if (!s.bActive)
		return;
	s.bActive = false;
	u64 inner = s_overheadAll - s.ovh0;
	u64 ticks = (t1 - s.t0 > inner) ? t1 - s.t0 - inner : 0;
#if defined(VP64_COUNT_TRADEPATH)
	int iGone = 0;
	for (size_t i = 0; i < s.prevCities.size(); i++)
	{
		if (std::find(s.curCities.begin(), s.curCities.end(), s.prevCities[i]) == s.curCities.end())
			iGone++;
	}
	const char* szReason = "turn";
	if (s.iLastUpdate == -1)
		szReason = (s.iLastRebuildTurn == s_iTurn) ? "inv_same_turn" : "invalidated";
	if (s_fTrade)
		fprintf(s_fTrade, "%d,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%d\n", s_iTurn, s_bPartial, ePlayer, szReason,
			s.iLastUpdate, s.iInvCount, s.iInvStack, s.iCities, s.iEqPaths, s.iEqInfo, s.iNew, iGone, s.iDests, s.iPathNodes,
			Ms(ticks), Ms(s.tCities), Ms(s.tEqPaths), Ms(s.tEqInfo), s.bFirstOfTurn);
	s.prevCities.swap(s.curCities);
	s.curCities.clear();
	s.iInvCount = 0;
	s.iInvStack = -1;
#endif
	s.iLastRebuildTurn = s_iTurn;
#if defined(VP64_COUNT_PARALLEL)
	if (s_fPar)
		fprintf(s_fPar, "%d,%d,%d,trade,%d,%.4f,%llu,%llu,%llu,%llu,%s\n", s_iTurn, s_bPartial, ePlayer, s.bFirstOfTurn, Ms(ticks),
			s.before.plot, s.before.unit, s.before.view, s.before.seen, s.before.types.c_str());
#endif
	AddOverhead(t1);
}
#endif

#if defined(VP64_COUNT_TRADEPATH)
void TradeInvalidate(int ePlayer)
{
	if (ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return;
	CvPlayer& kPlayer = GET_PLAYER((PlayerTypes)ePlayer);
	if (!kPlayer.isAlive() || kPlayer.isBarbarian() || kPlayer.isMinorCiv())
		return;
	Lock l;
	Init();
	TradeInitOnce();
	u64 t = Now0();
	int id = CaptureStack();
	s_tradeInvAgg[id]++;
	s_trade[ePlayer].iInvCount++;
	s_trade[ePlayer].iInvStack = id;
	AddOverhead(t);
}

void TradeCity(int ePlayer, int iCityPlot, u64 t0, const std::map<int, SPath>& water, const std::map<int, SPath>& land,
	const std::map<int, STradePathInfo>& waterInfo, const std::map<int, STradePathInfo>& landInfo)
{
	u64 t1 = Now0();
	if (ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return;
	Lock l;
	Init();
	TradeInitOnce();
	TradeState& s = s_trade[ePlayer];
	u64 ticks = t1 - t0;
	u64 hp = 0x7061746873ULL;
	u64 nodes = 0;
	const std::map<int, SPath>* pm[2] = { &water, &land };
	for (int k = 0; k < 2; k++)
	{
		hp = H(hp, 0xABCD0000ULL + (u64)k);
		for (std::map<int, SPath>::const_iterator it = pm[k]->begin(); it != pm[k]->end(); ++it)
		{
			const SPath& p = it->second;
			hp = H2(hp, it->first, p.iTotalCost);
			hp = H2(hp, p.iNormalizedDistanceRaw, p.iTotalTurns);
			hp = H(hp, (u64)p.vPlots.size());
			nodes += (u64)p.vPlots.size();
			for (size_t i = 0; i < p.vPlots.size(); i++)
			{
				const SPathNode& n = p.vPlots[i];
				hp = H(hp, (u64)(unsigned short)n.x | ((u64)(unsigned short)n.y << 16) | ((u64)(unsigned short)n.turns << 32) |
					((u64)(unsigned short)n.moves << 48));
			}
		}
	}
	u64 hi = 0x696e666fULL;
	u64 dests = 0;
	const std::map<int, STradePathInfo>* pi[2] = { &waterInfo, &landInfo };
	for (int k = 0; k < 2; k++)
	{
		hi = H(hi, 0xABCD0000ULL + (u64)k);
		for (std::map<int, STradePathInfo>::const_iterator it = pi[k]->begin(); it != pi[k]->end(); ++it)
		{
			const STradePathInfo& f = it->second;
			hi = H2(hi, it->first, f.iTotalCost);
			hi = H2(hi, f.iNormalizedDistanceRaw, f.iTotalTurns);
			hi = H2(hi, f.iPathLength, f.iDangerScore);
			hi = H2(hi, f.iScoreFromPassingTR, f.iScoreFromTerrain);
			dests++;
		}
	}
	u64 key = ((u64)(unsigned int)ePlayer << 32) | (u64)(unsigned int)iCityPlot;
	std::unordered_map<u64, std::pair<u64, u64> >::iterator it = s_tradeHash.find(key);
	s.iCities++;
	s.tCities += ticks;
	s.iDests += dests;
	s.iPathNodes += nodes;
	if (it == s_tradeHash.end())
	{
		s.iNew++;
		s_tradeHash[key] = std::make_pair(hp, hi);
	}
	else
	{
		if (it->second.first == hp)
		{
			s.iEqPaths++;
			s.tEqPaths += ticks;
		}
		if (it->second.second == hi)
		{
			s.iEqInfo++;
			s.tEqInfo += ticks;
		}
		it->second = std::make_pair(hp, hi);
	}
	s.curCities.push_back(iCityPlot);
	AddOverhead(t1);
}
#endif

#if defined(VP64_COUNT_REACH)
ReachScope::ReachScope(const CvAStar* pFinder, int iX, int iY, const SPathFinderUserData& data)
{
	u64 t = Now0();
	Lock l;
	Init();
	m_bDone = false;
	u64 h = 0x7265616368ULL;
	h = H(h, (u64)(uintptr_t)pFinder);
	h = H2(h, iX, iY);
	h = H2(h, (int)data.ePath, (int)data.eRoute);
	h = H2(h, (int)data.eBuild, (int)data.eRoutePurpose);
	h = H2(h, data.bUseRivers ? 1 : 0, data.iFlags);
	h = H2(h, (int)data.ePlayer, (int)data.eEnemy);
	h = H2(h, data.iUnitID, data.iMaxTurns);
	h = H2(h, data.iMaxNormalizedDistance, data.iMinMovesLeft);
	h = H2(h, data.iStartMoves, (int)data.plotsToIgnoreForZOC.size());
	for (size_t i = 0; i < data.plotsToIgnoreForZOC.size(); i++)
		h = H(h, (u64)(unsigned int)data.plotsToIgnoreForZOC[i]);
	m_keyB = h;
	m_epoch = s_evAll;
	m_keyA = H(h, m_epoch);
	m_iStack = CaptureStack();
	m_iX = iX;
	m_iY = iY;
	m_iPath = (int)data.ePath;
	m_iFlags = data.iFlags;
	m_iPlayer = (int)data.ePlayer;
	m_iUnit = data.iUnitID;
	s_reachDepth++;
	AddOverhead(t);
	m_t0 = Now0();
}

ReachScope::~ReachScope()
{
	if (m_bDone)
		return;
	Lock l;
	s_reachDepth--;
}

void ReachScope::Done(const ReachablePlots& plots)
{
	u64 t1 = Now0();
	Lock l;
	m_bDone = true;
	s_reachDepth--;
	bool bNested = s_reachDepth > 0;
	u64 ticks = t1 - m_t0;
	u64 hr = 0x726573756c74ULL;
	u64 n = (u64)plots.size();
	hr = H(hr, n);
	for (ReachablePlots::const_iterator it = plots.begin(); it != plots.end(); ++it)
	{
		hr = H2(hr, it->iPlotIndex, it->iPathLength);
		hr = H2(hr, it->iMovesLeft, it->iNormalizedDistanceRaw);
	}
	ReachAgg& a = s_reachAgg[m_iStack];
	a.calls++;
	a.tAll += ticks;
	a.plotsAll += n;
	if (bNested)
	{
		a.nested++;
		a.tNested += ticks;
	}
	std::unordered_map<u64, u64>::iterator itA = s_reachA.find(m_keyA);
	if (itA != s_reachA.end())
	{
		a.repA++;
		if (itA->second == hr)
		{
			a.eqA++;
			a.tEqA += ticks;
			a.plotsEqA += n;
		}
		else
		{
			a.uneqA++;
			if (s_fReachUneq && s_reachUneqWritten < 2000)
			{
				s_reachUneqWritten++;
				fprintf(s_fReachUneq, "%d,%d,%d,%d,%d,%d,%d,%d,%llu,%llu\n", s_iTurn, m_iStack, m_iX, m_iY, m_iPath, m_iFlags, m_iPlayer,
					m_iUnit, m_epoch, n);
			}
		}
	}
	else
	{
		s_reachA[m_keyA] = hr;
		s_distinctA++;
		s_distinctAPlots += n;
	}
	std::unordered_map<u64, u64>::iterator itB = s_reachB.find(m_keyB);
	if (itB != s_reachB.end())
	{
		if (itB->second == hr)
		{
			a.eqB++;
			a.tEqB += ticks;
		}
		itB->second = hr;
	}
	else
	{
		s_reachB[m_keyB] = hr;
		s_distinctB++;
		s_distinctBPlots += n;
	}
	AddOverhead(t1);
}
#endif

#if defined(VP64_COUNT_PARALLEL)
void DangerBegin(int ePlayer)
{
	if (ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return;
	u64 t = Now0();
	Lock l;
	Init();
	DangerInitOnce();
	DangerState& s = s_danger[ePlayer];
	if (s.iDepth++ > 0)
	{
		AddOverhead(t);
		return; // nested: counted in the outer call
	}
	s.bFirst = (s_dangerTurn[ePlayer] != s_iTurn) ? 1 : 0;
	s_dangerTurn[ePlayer] = s_iTurn;
	s.before = Before(ePlayer);
	AddOverhead(t);
	s.ovh0 = s_overheadAll;
	s.t0 = Now0();
}

void DangerEnd(int ePlayer)
{
	u64 t1 = Now0();
	if (ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return;
	Lock l;
	Init();
	DangerInitOnce();
	DangerState& s = s_danger[ePlayer];
	if (s.iDepth <= 0 || --s.iDepth > 0)
		return;
	u64 inner = s_overheadAll - s.ovh0;
	u64 ticks = (t1 - s.t0 > inner) ? t1 - s.t0 - inner : 0;
	if (s_fPar)
		fprintf(s_fPar, "%d,%d,%d,danger,%d,%.4f,%llu,%llu,%llu,%llu,%s\n", s_iTurn, s_bPartial, ePlayer, s.bFirst, Ms(ticks),
			s.before.plot, s.before.unit, s.before.view, s.before.seen, s.before.types.c_str());
	AddOverhead(t1);
}
#endif

} // namespace VP64Count
