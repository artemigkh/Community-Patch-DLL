// VP64HiddenEmbark.h: no GameplayUnitEmbark for a unit that no watching human can see; the unit's true state is sent
// when it comes into view. Each embark or disembark call makes the EXE post six records into its visual queue, which
// pass one per frame, so the hidden embarks of an AI turn cost frames that nobody sees.
//
// Compiled only with VP64_HIDDEN_EMBARK; without the define the DLL is unchanged. The implementation is
// VP64HiddenEmbark.inl, included at the end of CvUnit.cpp.
//
// "Watching" is the DLL's own rule, CvPlot::isVisibleToWatchingHuman (a living human player's team, or an assigned
// observer slot), the rule CvUnit::setXY already uses to choose animated moves. A unit whose team has a human is never
// held back. The rule is the same on every machine of a multiplayer game, and nothing here is game state: the side table
// only decides which calls go to the EXE, is never read by game logic and is never saved.
//
// Run-time switch, first match wins:
//   1. VP64HiddenEmbarkControl(set = 0 or 1, ...), the export below (the 64-bit host calls it from its options file);
//   2. the environment variable VP64_HIDDEN_EMBARK = 0/off or 1/on (hosts that call no export);
//   3. VP64_HIDDEN_EMBARK_DEFAULT (0 unless the build defines it).
// Tests: VP64_HIDDEN_EMBARK_DIR = a folder for the per-turn counters (HiddenEmbark.csv) and, with
// VP64_HIDDEN_EMBARK_TRACE = 1, the facing trace (HiddenEmbarkFacing.csv); VP64_HIDDEN_EMBARK_FACING = none / call
// chooses how a flushed unit's facing is shown (see the .inl).
#pragma once
#if defined(VP64_HIDDEN_EMBARK)

#ifndef VP64_HIDDEN_EMBARK_DEFAULT
#define VP64_HIDDEN_EMBARK_DEFAULT 0
#endif

class CvUnit;
class CvPlot;
class CvCombatInfo;
class ICvUnit1;

// The counters since the DLL was loaded, as VP64HiddenEmbarkControl returns them. iSize is set by the caller (bytes it
// can take); the DLL fills at most that many and writes the size it filled.
struct VP64HiddenEmbarkStats
{
	int iSize;
	int iVersion;            // VP64_HIDDEN_EMBARK_VERSION
	int iEnabled;            // 1 = calls are held back
	int iSource;             // 1 = the export, 2 = the environment, 3 = the compiled default
	int iFacingMode;         // 0 = none, 1 = the facing of the last held-back call
	int iReserved;
	long long iCalls;            // GameplayUnitEmbark calls from embark()/disembark()
	long long iPostedWatched;    // posted at once: the plot is watched, or the unit's team has a human
	long long iPostedNoGraphics; // posted at once: graphics not initialised
	long long iDeferred;         // held back
	long long iWouldDefer;       // switched off: posted, but "on" would have held it back
	long long aiFlushed[4];      // posted later: move, reveal, combat, sweep (sweep = a missed reveal)
	long long aiFlushSame[4];    // of those: the EXE already had the state (posted for the model's rebuild)
	long long iDropped;          // the unit left the game while held back
	long long iCleared;          // entries dropped by a new game or a load
	long long iPendingMax;
	long long iPendingNow;
	long long iOtherThread;      // hook entries from a thread other than the first (safe under the lock; counted)
	long long iFacingOverrides;  // flushes that set a facing override
};

#define VP64_HIDDEN_EMBARK_VERSION 1

namespace VP64HiddenEmbark
{
	// CvUnit::embark/disembark, after the unit's state changed: posts the call, or holds it back
	void Embark(CvUnit* pUnit, const CvPlot* pPlot, bool bEmbark, ICvUnit1* pDllUnit);
	// CvUnit::setXY, before the move's visualisation: the unit enters pNewPlot
	void OnMove(CvUnit* pUnit, const CvPlot* pNewPlot);
	// CvPlot::changeVisibilityCount: pPlot turned visible for a team
	void OnPlotVisible(const CvPlot* pPlot);
	// CvCombatInfo::setVisualizeCombat(true): the combat's units are about to be shown
	void OnCombatVisualized(const CvCombatInfo* pInfo);
	// CvUnit::kill: the unit leaves the game
	void OnUnitGone(const CvUnit* pUnit);
	// CvPlayer::setTurnActive(false): the safety sweep (a unit still held back on a watched plot is a missed reveal:
	// counted and posted) and the statistics row at a new game turn
	void Sweep();
	// CvGame::uninit (a new game or a load): every entry goes; setupGraphical posts every unit's true state at a load
	void Clear();
	// CvDllUnit::GetFacingDirection(false): the facing the EXE is told
	int VisualFacing(const CvUnit* pUnit, int iFacing);
}

extern "C" __declspec(dllexport) int VP64HiddenEmbarkControl(int iSet, VP64HiddenEmbarkStats* pOut);

#endif
