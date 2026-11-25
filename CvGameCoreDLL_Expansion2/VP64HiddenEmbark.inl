// VP64HiddenEmbark.inl: the implementation of VP64HiddenEmbark.h, included at the end of CvUnit.cpp. Compiled only with
// VP64_HIDDEN_EMBARK.
//
// State: for each held-back unit (owner, id), the model state the EXE has (true = the embarked model) and the unit's
// facing at its last held-back call. A flush always posts GameplayUnitEmbark(unit, isEmbarked()), also when the EXE
// already has that state (an embark and a disembark both out of sight): the call makes the EXE build the model again
// with the unit's current art, as stock did at each held-back call.
//
// Facing (VP64_HIDDEN_EMBARK_FACING): "call" (default) makes CvDllUnit::GetFacingDirection(false), which only the EXE
// calls, answer the facing of the unit's last held-back call from its flush until the unit's next move, embark call or
// death, so the rebuilt model faces the way stock's last rebuild faced; "none" shows the unit's current facing. Facing
// is not game state for sight (CvUnit::getFacingDirection(true) is always NO_DIRECTION), and the DLL's own code never
// calls the wrapper, so neither mode changes the game.
//
// Threads: the hooks run on the game-core thread; the EXE may call GetFacingDirection from its own thread (the 64-bit
// host serves it on another thread). One critical section guards the tables; the override lookup takes it only when
// an override exists.
#if defined(VP64_HIDDEN_EMBARK)
#include <map>
#include <string>
#include <stdio.h>

namespace VP64HiddenEmbark
{
	enum Reason { R_MOVE, R_REVEAL, R_COMBAT, R_SWEEP, R_COUNT };
	static const char* const REASON_NAMES[R_COUNT] = { "move", "reveal", "combat", "sweep" };

	struct Entry
	{
		bool bExeEmbarked;   // the model the EXE has
		int iFacing;         // the unit's facing at its last held-back call
	};

	struct Lock
	{
		CRITICAL_SECTION cs;
		Lock() { InitializeCriticalSection(&cs); }   // never deleted: hooks may run during static destruction
	};
	static Lock s_lock;
	struct Guard
	{
		Guard() { EnterCriticalSection(&s_lock.cs); }
		~Guard() { LeaveCriticalSection(&s_lock.cs); }
	};

	static bool s_bInit = false;
	static bool s_bOn = false;
	static int s_iSource = 3;
	static int s_iFacingMode = 1;
	static bool s_bTrace = false;
	static std::string s_dir;
	static FILE* s_fTurns = NULL;
	static FILE* s_fTrace = NULL;
	static int s_iTurn = -1;
	static DWORD s_dwThread = 0;
	static long long s_iSeq = 0;
	static int s_iInPost = 0;                              // > 0 while a GameplayUnitEmbark call is being made
	static VP64HiddenEmbarkStats s_tot;                    // since load
	static VP64HiddenEmbarkStats s_turn;                   // this game turn (the CSV row)
	static std::map<unsigned long long, Entry> s_pending;  // (owner, id) -> entry
	static std::map<unsigned long long, int> s_facing;     // (owner, id) -> the facing the EXE is told
	static volatile LONG s_lFacingCount = 0;               // s_facing.size(), read without the lock
	static std::map<unsigned long long, bool> s_traced;    // units with an embark event (the trace's filter)
	// Destroyed before the tables above (reverse order of construction in this file): CvGame::uninit may run at the
	// DLL's static destruction, after which every hook does nothing.
	static bool s_bDead = false;
	struct DeadMark { ~DeadMark() { s_bDead = true; } };
	static DeadMark s_deadMark;

	inline unsigned long long Key(int eOwner, int iID) { return ((unsigned long long)(unsigned int)eOwner << 32) | (unsigned long long)(unsigned int)iID; }

	static bool EnvValue(const char* szName, std::string& out)
	{
		char buf[MAX_PATH];
		DWORD n = GetEnvironmentVariableA(szName, buf, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return false;
		out = buf;
		return true;
	}

	static void WriteRow()
	{
		if (!s_fTurns || s_iTurn < 0)
			return;
		fprintf(s_fTurns, "%d,%s,%lld,%lld,%lld,%lld,%lld", s_iTurn, s_bOn ? "on" : "off", s_turn.iCalls, s_turn.iPostedWatched,
			s_turn.iPostedNoGraphics, s_turn.iDeferred, s_turn.iWouldDefer);
		for (int i = 0; i < R_COUNT; i++)
			fprintf(s_fTurns, ",%lld", s_turn.aiFlushed[i]);
		for (int i = 0; i < R_COUNT; i++)
			fprintf(s_fTurns, ",%lld", s_turn.aiFlushSame[i]);
		fprintf(s_fTurns, ",%lld,%lld,%d,%lld,%lld,%lld\n", s_turn.iDropped, s_turn.iPendingMax, (int)s_pending.size(),
			s_turn.iCleared, s_turn.iOtherThread, s_turn.iFacingOverrides);
		fflush(s_fTurns);
	}

	// Under the lock. Reads the environment once (the export may have set the switch before).
	static void Init()
	{
		if (s_bInit)
			return;
		s_bInit = true;
		s_dwThread = GetCurrentThreadId();
		std::string v;
		if (s_iSource != 1)
		{
			if (EnvValue("VP64_HIDDEN_EMBARK", v) && (v == "1" || v == "on" || v == "0" || v == "off"))
			{
				s_bOn = (v == "1" || v == "on");
				s_iSource = 2;
			}
			else
			{
				s_bOn = (VP64_HIDDEN_EMBARK_DEFAULT != 0);
				s_iSource = 3;
			}
		}
		if (EnvValue("VP64_HIDDEN_EMBARK_FACING", v))
			s_iFacingMode = (v == "none") ? 0 : 1;
		if (EnvValue("VP64_HIDDEN_EMBARK_DIR", v))
		{
			s_dir = v;
			s_fTurns = fopen((s_dir + "\\HiddenEmbark.csv").c_str(), "w");
			if (s_fTurns)
			{
				std::string hdr = "turn,mode,calls,posted_watched,posted_no_graphics,deferred,would_defer";
				for (int i = 0; i < R_COUNT; i++)
					hdr += std::string(",flushed_") + REASON_NAMES[i];
				for (int i = 0; i < R_COUNT; i++)
					hdr += std::string(",flush_same_") + REASON_NAMES[i];
				hdr += ",dropped,pending_max,pending_end,cleared,other_thread,facing_overrides";
				fprintf(s_fTurns, "%s\n", hdr.c_str());
				fflush(s_fTurns);
			}
			FILE* fi = fopen((s_dir + "\\HiddenEmbarkInfo.txt").c_str(), "w");
			if (fi)
			{
				fprintf(fi, "VP64HiddenEmbark version %d\nmode=%s\nsource=%d\nfacing=%s\n", VP64_HIDDEN_EMBARK_VERSION,
					s_bOn ? "on" : "off", s_iSource, s_iFacingMode ? "call" : "none");
				fclose(fi);
			}
			if (EnvValue("VP64_HIDDEN_EMBARK_TRACE", v) && v == "1")
			{
				s_fTrace = fopen((s_dir + "\\HiddenEmbarkFacing.csv").c_str(), "w");
				if (s_fTrace)
				{
					fprintf(s_fTrace, "seq,turn,event,owner,id,embarked,facing,in_post,thread\n");
					fflush(s_fTrace);
					s_bTrace = true;
				}
			}
		}
	}

	static void Trace(const char* szEvent, int eOwner, int iID, int iEmbarked, int iFacing)
	{
		if (!s_bTrace)
			return;
		s_traced[Key(eOwner, iID)] = true;
		fprintf(s_fTrace, "%lld,%d,%s,%d,%d,%d,%d,%d,%lu\n", ++s_iSeq, s_iTurn, szEvent, eOwner, iID, iEmbarked, iFacing,
			s_iInPost, (unsigned long)GetCurrentThreadId());
	}

	static void Count(long long VP64HiddenEmbarkStats::* pField)
	{
		s_tot.*pField += 1;
		s_turn.*pField += 1;
	}

	// Under the lock: every hook's entry
	static void Enter()
	{
		Init();
		if (GetCurrentThreadId() != s_dwThread)
			Count(&VP64HiddenEmbarkStats::iOtherThread);
		const int iTurn = GC.getGame().getGameTurn();
		if (iTurn != s_iTurn)
		{
			WriteRow();
			const int iSize = s_turn.iSize;
			memset(&s_turn, 0, sizeof(s_turn));
			s_turn.iSize = iSize;
			s_iTurn = iTurn;
		}
	}

	static void NotePending()
	{
		const long long n = (long long)s_pending.size();
		if (n > s_tot.iPendingMax)
			s_tot.iPendingMax = n;
		if (n > s_turn.iPendingMax)
			s_turn.iPendingMax = n;
	}

	static void DropFacing(unsigned long long k)
	{
		std::map<unsigned long long, int>::iterator it = s_facing.find(k);
		if (it != s_facing.end())
		{
			s_facing.erase(it);
			InterlockedExchange(&s_lFacingCount, (LONG)s_facing.size());
		}
	}

	static void Post(CvUnit* pUnit, bool bEmbarked, ICvUnit1* pDllUnit)
	{
		s_iInPost++;
		Trace("post", pUnit->getOwner(), pUnit->GetID(), bEmbarked ? 1 : 0, pUnit->getFacingDirection(false));
		if (pDllUnit)
			gDLL->GameplayUnitEmbark(pDllUnit, bEmbarked);
		else
		{
			CvInterfacePtr<ICvUnit1> pWrap(new CvDllUnit(pUnit));
			gDLL->GameplayUnitEmbark(pWrap.get(), bEmbarked);
		}
		s_iInPost--;
	}

	// posts the unit's true state; the entry goes
	static void Flush(CvUnit* pUnit, std::map<unsigned long long, Entry>::iterator it, Reason eReason)
	{
		const unsigned long long k = it->first;
		const Entry e = it->second;
		s_pending.erase(it);
		if (s_iFacingMode == 1 && e.iFacing != (int)pUnit->getFacingDirection(false))
		{
			s_facing[k] = e.iFacing;
			InterlockedExchange(&s_lFacingCount, (LONG)s_facing.size());
			Count(&VP64HiddenEmbarkStats::iFacingOverrides);
		}
		Trace(REASON_NAMES[eReason], pUnit->getOwner(), pUnit->GetID(), pUnit->isEmbarked() ? 1 : 0, e.iFacing);
		Post(pUnit, pUnit->isEmbarked(), NULL);
		s_tot.aiFlushed[eReason]++;
		s_turn.aiFlushed[eReason]++;
		if (pUnit->isEmbarked() == e.bExeEmbarked)
		{
			s_tot.aiFlushSame[eReason]++;
			s_turn.aiFlushSame[eReason]++;
		}
	}

	static void FlushIfPending(CvUnit* pUnit, Reason eReason)
	{
		if (pUnit == NULL || s_pending.empty())
			return;
		std::map<unsigned long long, Entry>::iterator it = s_pending.find(Key(pUnit->getOwner(), pUnit->GetID()));
		if (it != s_pending.end())
			Flush(pUnit, it, eReason);
	}

	static bool Watched(const CvUnit* pUnit, const CvPlot* pPlot)
	{
		if (GET_TEAM(pUnit->getTeam()).isHuman())
			return true;
		return pPlot != NULL && pPlot->isVisibleToWatchingHuman();
	}

	void Embark(CvUnit* pUnit, const CvPlot* pPlot, bool bEmbark, ICvUnit1* pDllUnit)
	{
		Guard g;
		Enter();
		Count(&VP64HiddenEmbarkStats::iCalls);
		const unsigned long long k = Key(pUnit->getOwner(), pUnit->GetID());
		DropFacing(k);   // a new call: stock would rebuild the model now
		if (!GC.IsGraphicsInitialized())
		{
			Count(&VP64HiddenEmbarkStats::iPostedNoGraphics);
			Post(pUnit, bEmbark, pDllUnit);
			return;
		}
		std::map<unsigned long long, Entry>::iterator it = s_pending.find(k);
		if (Watched(pUnit, pPlot))
		{
			if (it != s_pending.end())
				s_pending.erase(it);   // the call about to be posted carries the true state
			Count(&VP64HiddenEmbarkStats::iPostedWatched);
			Post(pUnit, bEmbark, pDllUnit);
			return;
		}
		if (!s_bOn)
		{
			Count(&VP64HiddenEmbarkStats::iWouldDefer);
			Post(pUnit, bEmbark, pDllUnit);
			return;
		}
		Entry& e = s_pending[k];
		if (it == s_pending.end())
			e.bExeEmbarked = !bEmbark;   // the EXE keeps the model of the state before this change
		e.iFacing = (int)pUnit->getFacingDirection(false);
		Count(&VP64HiddenEmbarkStats::iDeferred);
		NotePending();
		Trace("defer", pUnit->getOwner(), pUnit->GetID(), bEmbark ? 1 : 0, e.iFacing);
	}

	void OnMove(CvUnit* pUnit, const CvPlot* pNewPlot)
	{
		if (pUnit == NULL)
			return;
		Guard g;
		if (!s_bInit)
			return;   // nothing held back yet
		const unsigned long long k = Key(pUnit->getOwner(), pUnit->GetID());
		DropFacing(k);   // a move: the EXE gets the unit's current facing again
		if (s_pending.empty())
			return;
		std::map<unsigned long long, Entry>::iterator it = s_pending.find(k);
		if (it != s_pending.end() && Watched(pUnit, pNewPlot))
		{
			Enter();
			Flush(pUnit, it, R_MOVE);
		}
	}

	void OnPlotVisible(const CvPlot* pPlot)
	{
		if (pPlot == NULL)
			return;
		Guard g;
		if (s_pending.empty() || pPlot->getNumUnits() == 0 || !pPlot->isVisibleToWatchingHuman())
			return;
		Enter();
		const IDInfo* pUnitNode = pPlot->headUnitNode();
		while (pUnitNode != NULL)
		{
			CvUnit* pLoopUnit = GetPlayerUnit(*pUnitNode);
			pUnitNode = pPlot->nextUnitNode(pUnitNode);
			FlushIfPending(pLoopUnit, R_REVEAL);
		}
	}

	void OnCombatVisualized(const CvCombatInfo* pInfo)
	{
		if (pInfo == NULL)
			return;
		Guard g;
		if (s_pending.empty())
			return;
		Enter();
		FlushIfPending(pInfo->getUnit(BATTLE_UNIT_ATTACKER), R_COMBAT);
		FlushIfPending(pInfo->getUnit(BATTLE_UNIT_DEFENDER), R_COMBAT);
		FlushIfPending(pInfo->getUnit(BATTLE_UNIT_INTERCEPTOR), R_COMBAT);
	}

	void OnUnitGone(const CvUnit* pUnit)
	{
		if (pUnit == NULL || s_bDead)
			return;
		Guard g;
		if (!s_bInit)
			return;
		const unsigned long long k = Key(pUnit->getOwner(), pUnit->GetID());
		DropFacing(k);
		std::map<unsigned long long, Entry>::iterator it = s_pending.find(k);
		if (it != s_pending.end())
		{
			Enter();
			s_pending.erase(it);
			Count(&VP64HiddenEmbarkStats::iDropped);
		}
	}

	void Sweep()
	{
		if (s_bDead)
			return;
		Guard g;
		Enter();
		if (s_pending.empty())
			return;
		std::map<unsigned long long, Entry>::iterator it = s_pending.begin();
		while (it != s_pending.end())
		{
			const int eOwner = (int)(it->first >> 32);
			const int iID = (int)(unsigned int)(it->first & 0xFFFFFFFFull);
			CvUnit* pUnit = (eOwner >= 0 && eOwner < MAX_PLAYERS) ? GET_PLAYER((PlayerTypes)eOwner).getUnit(iID) : NULL;
			std::map<unsigned long long, Entry>::iterator cur = it++;
			if (pUnit == NULL)
			{
				s_pending.erase(cur);
				Count(&VP64HiddenEmbarkStats::iDropped);
			}
			else if (Watched(pUnit, pUnit->plot()))
				Flush(pUnit, cur, R_SWEEP);   // a missed reveal: counted in aiFlushed[R_SWEEP]
		}
	}

	void Clear()
	{
		if (s_bDead)
			return;
		Guard g;
		if (!s_bInit)
			return;
		const long long n = (long long)s_pending.size();
		s_tot.iCleared += n;
		s_turn.iCleared += n;
		s_pending.clear();
		s_facing.clear();
		s_traced.clear();
		InterlockedExchange(&s_lFacingCount, 0);
	}

	int VisualFacing(const CvUnit* pUnit, int iFacing)
	{
		if ((s_lFacingCount == 0 && !s_bTrace) || s_bDead)
			return iFacing;
		Guard g;
		const unsigned long long k = Key(pUnit->getOwner(), pUnit->GetID());
		int iOut = iFacing;
		std::map<unsigned long long, int>::const_iterator it = s_facing.find(k);
		if (it != s_facing.end())
			iOut = it->second;
		if (s_bTrace && s_traced.find(k) != s_traced.end())
			Trace(it != s_facing.end() ? "read_override" : "read", pUnit->getOwner(), pUnit->GetID(),
				pUnit->isEmbarked() ? 1 : 0, iOut);
		return iOut;
	}
}

extern "C" __declspec(dllexport) int VP64HiddenEmbarkControl(int iSet, VP64HiddenEmbarkStats* pOut)
{
	using namespace VP64HiddenEmbark;
	// TryEnter: the host reads the counters while it writes its report, which may be on a crash path where the game
	// thread stopped inside a hook; then nothing is filled (iSize 0) rather than waiting forever
	if (!TryEnterCriticalSection(&s_lock.cs))
	{
		if (pOut != NULL)
			pOut->iSize = 0;
		return VP64_HIDDEN_EMBARK_VERSION;
	}
	struct Leave { ~Leave() { LeaveCriticalSection(&s_lock.cs); } } leave;
	if (iSet == 0 || iSet == 1)
	{
		s_bOn = (iSet == 1);
		s_iSource = 1;
	}
	if (pOut != NULL && pOut->iSize > 0)
	{
		VP64HiddenEmbarkStats st = s_tot;
		st.iSize = (int)sizeof(VP64HiddenEmbarkStats);
		st.iVersion = VP64_HIDDEN_EMBARK_VERSION;
		st.iEnabled = s_bOn ? 1 : 0;
		st.iSource = s_bInit || s_iSource == 1 ? s_iSource : 0;
		st.iFacingMode = s_iFacingMode;
		st.iPendingNow = (long long)s_pending.size();
		const int iCopy = pOut->iSize < (int)sizeof(st) ? pOut->iSize : (int)sizeof(st);
		memcpy(pOut, &st, iCopy);
		pOut->iSize = iCopy;
	}
	return VP64_HIDDEN_EMBARK_VERSION;
}
#endif
