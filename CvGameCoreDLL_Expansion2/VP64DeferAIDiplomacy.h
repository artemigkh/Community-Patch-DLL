// VP64DeferAIDiplomacy.h: deferred AI diplomacy for single player and hot seat. Every contact an AI starts toward a
// human (offers, demands, threats, peace offers, requests for help, denouncements, war declarations, comments) waits in
// that human's diplomacy request queue instead of opening a leader screen during the AI's turn. The AI decides and
// acts in its own turn as before and treats a waiting contact as unanswered; no AI turn waits for a human answer. At
// the start of the human's turn the waiting screens open one after another, in the order they arrived, whenever no
// modal window is up, with the re-check that network multiplayer games already use (a deal that is still valid and
// acceptable is shown, otherwise rebalanced, a peace offer is asked again, or the deal is withdrawn with a notice).
//
// Compiled only with VP64_DEFER_AI_DIPLOMACY; without the define the DLL is unchanged. The implementation is
// VP64DeferAIDiplomacy.inl, included at the end of CvDiplomacyRequests.cpp. Network multiplayer and play by email are
// never affected: they keep their own behaviour whatever the switch says.
//
// Run-time switch, first match wins:
//   1. VP64DeferAIDiplomacyControl(set = 0 or 1, ...), the export below (a host calls it after loading the DLL and
//      before a game starts or loads; the value is read when an AI turn runs, so set it only between games);
//   2. the environment variable VP64_DEFER_AI_DIPLOMACY = 0/off or 1/on (hosts that call no export);
//   3. VP64_DEFER_AI_DIPLOMACY_DEFAULT (0 unless the build defines it).
// The switch is not saved. A save made while contacts wait keeps them (the request queue and the proposed deals are
// already part of the save); they open at that human's turn start whatever the switch says when the save is loaded.
// Tests: VP64_DEFER_AI_DIPLOMACY_DIR = a folder; at unload the DLL writes DeferAIDiplomacy.txt there (the switch, its
// source and the counters below).
#pragma once
#if defined(VP64_DEFER_AI_DIPLOMACY)

#ifndef VP64_DEFER_AI_DIPLOMACY_DEFAULT
#define VP64_DEFER_AI_DIPLOMACY_DEFAULT 0
#endif

#define VP64_DEFER_AI_DIPLOMACY_VERSION 1

class CvDeal;

// The counters since the DLL was loaded, as VP64DeferAIDiplomacyControl returns them. iSize is set by the caller
// (bytes it can take); the DLL fills at most that many and writes the size it filled.
struct VP64DeferAIDiplomacyStats
{
	int iSize;
	int iVersion;               // VP64_DEFER_AI_DIPLOMACY_VERSION
	int iEnabled;               // 1 = the switch is on
	int iSource;                // 1 = the export, 2 = the environment, 3 = the compiled default, 0 = not read yet
	int iDeferredNow;           // 1 = in effect in the current game (switch on, single player or hot seat)
	int iReserved;
	long long iQueued;          // contacts put in a queue in place of a leader screen
	long long iShown;           // queued contacts opened by the turn-start drain
	long long iWithdrawn;       // queued contacts the drain dropped (sender gone, or the deal no longer possible)
	long long iShownAsStock;    // requests without a stored deal, shown the way the stock queue shows them
	long long iBlocksSkipped;   // AI turns the stock code would have held on a leader screen
	long long iHandoversSkipped;// turn handovers the stock code would have held on a leader screen
};

namespace VP64DeferAIDiplomacy
{
	// The switch
	bool IsOn();
	// The switch is on and the game is single player or hot seat (not network multiplayer, not play by email)
	bool IsDeferred();
	// A contact from eFrom (an AI) to eTo (a human) goes to eTo's queue: IsDeferred(), and it is not eTo's own turn as
	// the active player (a reply to something the human does in their own turn holds up nothing and is shown at once)
	bool QueueFor(PlayerTypes eFrom, PlayerTypes eTo);
	// Puts the contact in eTo's queue with its deal (an empty deal for a contact without one) stored as a proposed deal,
	// in the same order as the request, so the drain can pair them
	void Queue(PlayerTypes eFrom, PlayerTypes eTo, CvDeal* pkDeal, DiploUIStateTypes eDiploType, const char* pszMessage, LeaderheadAnimationTypes eAnimationType, int iExtraGameData);
	// A leader screen from eFrom to eTo that the stock code opens directly: queued when QueueFor(), else opened
	void LeaderMessage(PlayerTypes eFrom, PlayerTypes eTo, DiploUIStateTypes eDiploType, const char* pszMessage, LeaderheadAnimationTypes eAnimationType, int iExtraGameData = -1);
	// End of an AI's diplomacy turn: cancel its proposed deals with AI players only (those with humans wait in a queue)
	void CancelProposedDealsWithAIs(PlayerTypes eCancelPlayer);
	// CvPlayer::doTurn: true (and counted) when the stock code would hold this AI turn and the switch says it must not
	bool SkipBlock();
	// CvGame::CheckPlayerTurnDeactivate: true (and counted) when the stock code would hold the turn handover
	bool SkipHandover();
	// Counters of CvDiplomacyRequests::VP64Drain
	void CountDrain(int iShown, int iWithdrawn, int iShownAsStock);
}

extern "C" __declspec(dllexport) int VP64DeferAIDiplomacyControl(int iSet, VP64DeferAIDiplomacyStats* pOut);

#endif
