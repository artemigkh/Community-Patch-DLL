// VP64ProtoHiddenEmbark.inl: the implementation of VP64ProtoHiddenEmbark.h (epic 0437, D165, ticket 4676), included
// at the end of CvUnit.cpp. Evidence only: compiled only with VP64_PROTO_HIDDEN_EMBARK.
//
// State: for each deferred unit (owner, id), the model state the EXE has (true = the embarked model), i.e. the state
// before the first deferred call. A flush always posts GameplayUnitEmbark(unit, isEmbarked()), also when the EXE
// already has that state (an embark and a disembark both out of sight): each such call makes the EXE build the unit's
// model again with the art it has now, which stock did at every deferred call (build 1 skipped it, and a unit whose
// art had changed since the load came into view with its old model: research turn-time-study.md "Embarks the human
// cannot see", 2026-11-17). Nothing is serialised: at a load the EXE gets every unit's true state from
// CvUnit::setupGraphical.
#if defined(VP64_PROTO_HIDDEN_EMBARK)
#include <map>
#include <string>
#include <stdio.h>

namespace VP64HiddenEmbark
{
	enum Reason { R_MOVE, R_REVEAL, R_COMBAT, R_SWEEP, R_COUNT };
	static const char* const REASON_NAMES[R_COUNT] = { "move", "reveal", "combat", "sweep" };

	struct TurnStats
	{
		int calls;              // GameplayUnitEmbark calls from embark()/disembark()
		int postedWatched;      // posted at once: the plot is watched, or the unit's team has a human
		int postedNoGraphics;   // posted at once: graphics not initialised (the stock call; nothing to save)
		int deferred;           // not posted
		int wouldDefer;         // mode off: posted, but "on" would have deferred it
		int flushed[R_COUNT];   // posted later, by what brought the unit into view (sweep = a missed reveal)
		int flushSame[R_COUNT]; // of flushed: the EXE already had the state (posted for the model's rebuild)
		int dropped;            // the unit left the game while deferred
		int pendingMax;
	};

	bool s_bInit = false;
	bool s_bOn = true;
	std::string s_dir;
	FILE* s_fTurns = NULL;
	int s_iTurn = -1;
	TurnStats s_st;
	std::map<unsigned long long, bool> s_pending;   // (owner, id) -> the EXE's model state

	inline unsigned long long Key(int eOwner, int iID) { return ((unsigned long long)(unsigned int)eOwner << 32) | (unsigned long long)(unsigned int)iID; }

	void WriteRow()
	{
		if (!s_fTurns || s_iTurn < 0)
			return;
		fprintf(s_fTurns, "%d,%s,%d,%d,%d,%d,%d", s_iTurn, s_bOn ? "on" : "off", s_st.calls, s_st.postedWatched,
			s_st.postedNoGraphics, s_st.deferred, s_st.wouldDefer);
		for (int i = 0; i < R_COUNT; i++)
			fprintf(s_fTurns, ",%d", s_st.flushed[i]);
		for (int i = 0; i < R_COUNT; i++)
			fprintf(s_fTurns, ",%d", s_st.flushSame[i]);
		fprintf(s_fTurns, ",%d,%d,%d\n", s_st.dropped, s_st.pendingMax, (int)s_pending.size());
		fflush(s_fTurns);
	}

	void Init()
	{
		if (s_bInit)
			return;
		s_bInit = true;
		char buf[MAX_PATH];
		DWORD n = GetEnvironmentVariableA("VP64_PROTO_HIDDEN_EMBARK_DIR", buf, MAX_PATH);
		s_dir = (n > 0 && n < MAX_PATH) ? std::string(buf) : std::string(".");
		n = GetEnvironmentVariableA("VP64_PROTO_HIDDEN_EMBARK_MODE", buf, MAX_PATH);
		std::string mode = (n > 0 && n < MAX_PATH) ? std::string(buf) : std::string("on");
		s_bOn = (mode != "off");
		memset(&s_st, 0, sizeof(s_st));
		std::string hdr = "turn,mode,calls,posted_watched,posted_no_graphics,deferred,would_defer";
		for (int i = 0; i < R_COUNT; i++)
			hdr += std::string(",flushed_") + REASON_NAMES[i];
		for (int i = 0; i < R_COUNT; i++)
			hdr += std::string(",flush_same_") + REASON_NAMES[i];
		hdr += ",dropped,pending_max,pending_end";
		std::string p = s_dir + "\\HiddenEmbark.csv";
		s_fTurns = fopen(p.c_str(), "w");
		if (s_fTurns)
		{
			fprintf(s_fTurns, "%s\n", hdr.c_str());
			fflush(s_fTurns);
		}
		FILE* fi = fopen((s_dir + "\\HiddenEmbarkInfo.txt").c_str(), "w");
		if (fi)
		{
			fprintf(fi, "VP64ProtoHiddenEmbark prototype (D165 rank 2), ticket 4676\nmode=%s\n", s_bOn ? "on" : "off");
			fclose(fi);
		}
	}

	// a new game turn: the row of the turn before, then fresh counters
	void TurnCheck()
	{
		Init();
		const int iTurn = GC.getGame().getGameTurn();
		if (iTurn == s_iTurn)
			return;
		WriteRow();
		memset(&s_st, 0, sizeof(s_st));
		s_iTurn = iTurn;
	}

	bool Watched(const CvUnit* pUnit, const CvPlot* pPlot)
	{
		if (GET_TEAM(pUnit->getTeam()).isHuman())
			return true;
		return pPlot != NULL && pPlot->isVisibleToWatchingHuman();
	}

	void Post(CvUnit* pUnit, bool bEmbarked)
	{
		CvInterfacePtr<ICvUnit1> pDllUnit(new CvDllUnit(pUnit));
		gDLL->GameplayUnitEmbark(pDllUnit.get(), bEmbarked);
	}

	// posts the unit's true state (always: the EXE builds the model again, as at each call stock posted); the entry goes
	void Flush(CvUnit* pUnit, std::map<unsigned long long, bool>::iterator it, Reason eReason)
	{
		const bool bExe = it->second;
		s_pending.erase(it);
		Post(pUnit, pUnit->isEmbarked());
		s_st.flushed[eReason]++;
		if (pUnit->isEmbarked() == bExe)
			s_st.flushSame[eReason]++;
	}

	void FlushIfPending(CvUnit* pUnit, Reason eReason)
	{
		if (pUnit == NULL || s_pending.empty())
			return;
		std::map<unsigned long long, bool>::iterator it = s_pending.find(Key(pUnit->getOwner(), pUnit->GetID()));
		if (it != s_pending.end())
			Flush(pUnit, it, eReason);
	}

	bool OnEmbark(CvUnit* pUnit, const CvPlot* pPlot, bool bEmbark)
	{
		TurnCheck();
		s_st.calls++;
		if (!GC.IsGraphicsInitialized())
		{
			s_st.postedNoGraphics++;
			return false;
		}
		const unsigned long long k = Key(pUnit->getOwner(), pUnit->GetID());
		std::map<unsigned long long, bool>::iterator it = s_pending.find(k);
		if (Watched(pUnit, pPlot))
		{
			if (it != s_pending.end())
				s_pending.erase(it);   // the call about to be posted carries the true state
			s_st.postedWatched++;
			return false;
		}
		if (!s_bOn)
		{
			s_st.wouldDefer++;
			return false;
		}
		if (it == s_pending.end())
			s_pending[k] = !bEmbark;   // the EXE keeps the model of the state before this change
		s_st.deferred++;
		if ((int)s_pending.size() > s_st.pendingMax)
			s_st.pendingMax = (int)s_pending.size();
		return true;
	}

	void OnMove(CvUnit* pUnit, const CvPlot* pNewPlot)
	{
		if (s_pending.empty() || pUnit == NULL)
			return;
		std::map<unsigned long long, bool>::iterator it = s_pending.find(Key(pUnit->getOwner(), pUnit->GetID()));
		if (it != s_pending.end() && Watched(pUnit, pNewPlot))
			Flush(pUnit, it, R_MOVE);
	}

	void OnPlotVisible(const CvPlot* pPlot)
	{
		if (s_pending.empty() || pPlot == NULL || pPlot->getNumUnits() == 0 || !pPlot->isVisibleToWatchingHuman())
			return;
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
		if (s_pending.empty() || pInfo == NULL)
			return;
		FlushIfPending(pInfo->getUnit(BATTLE_UNIT_ATTACKER), R_COMBAT);
		FlushIfPending(pInfo->getUnit(BATTLE_UNIT_DEFENDER), R_COMBAT);
		FlushIfPending(pInfo->getUnit(BATTLE_UNIT_INTERCEPTOR), R_COMBAT);
	}

	void OnUnitGone(const CvUnit* pUnit)
	{
		if (s_pending.empty() || pUnit == NULL)
			return;
		std::map<unsigned long long, bool>::iterator it = s_pending.find(Key(pUnit->getOwner(), pUnit->GetID()));
		if (it != s_pending.end())
		{
			s_pending.erase(it);
			s_st.dropped++;
		}
	}

	void Sweep()
	{
		TurnCheck();
		if (s_pending.empty())
			return;
		std::map<unsigned long long, bool>::iterator it = s_pending.begin();
		while (it != s_pending.end())
		{
			const int eOwner = (int)(it->first >> 32);
			const int iID = (int)(unsigned int)(it->first & 0xFFFFFFFFull);
			CvUnit* pUnit = (eOwner >= 0 && eOwner < MAX_PLAYERS) ? GET_PLAYER((PlayerTypes)eOwner).getUnit(iID) : NULL;
			std::map<unsigned long long, bool>::iterator cur = it++;
			if (pUnit == NULL)
			{
				s_pending.erase(cur);
				s_st.dropped++;
			}
			else if (Watched(pUnit, pUnit->plot()))
				Flush(pUnit, cur, R_SWEEP);
		}
	}
}
#endif
