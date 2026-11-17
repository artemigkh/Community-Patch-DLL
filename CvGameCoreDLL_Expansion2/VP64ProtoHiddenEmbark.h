// VP64ProtoHiddenEmbark.h: rank 2 of the x64 turn-time study's pacing work (epic 0437, D165, ticket 4676): no
// GameplayUnitEmbark for a unit that no watching human can see; the unit's true state is sent when it comes into view.
// Each such call posts six keyed records into the EXE's visual queue that pass one per frame, so an AI turn's hidden
// embarks cost frames that nobody sees. Evidence only: this file and every call site compile only with
// VP64_PROTO_HIDDEN_EMBARK, which no product build defines. Implementation: VP64ProtoHiddenEmbark.inl, included at the
// end of CvUnit.cpp.
//
// "Watching" is the DLL's own rule, CvPlot::isVisibleToWatchingHuman (a living human player's team, or an observer slot
// that is claimed: the autoplay observer), the rule CvUnit::setXY already uses to decide animated moves. A unit whose
// team has a human is never deferred.
//
// Run-time switch VP64_PROTO_HIDDEN_EMBARK_MODE (environment): "on" (default) defers; "off" posts every call as the
// stock DLL does and only counts what "on" would defer. Statistics per game turn go to
// %VP64_PROTO_HIDDEN_EMBARK_DIR%\HiddenEmbark.csv (else the working folder).
#pragma once
#if defined(VP64_PROTO_HIDDEN_EMBARK)

class CvUnit;
class CvPlot;
class CvCombatInfo;

namespace VP64HiddenEmbark
{
	// CvUnit::embark/disembark, after the unit's state changed, at the plot the call is made for: true when the call
	// is deferred (the caller then does not post it)
	bool OnEmbark(CvUnit* pUnit, const CvPlot* pPlot, bool bEmbark);
	// CvUnit::setXY, before the move's visualisation (SetPosition / QueueMoveForVisualization): the unit enters pNewPlot
	void OnMove(CvUnit* pUnit, const CvPlot* pNewPlot);
	// CvPlot::changeVisibilityCount: pPlot turned visible for a team
	void OnPlotVisible(const CvPlot* pPlot);
	// CvCombatInfo::setVisualizeCombat(true): the combat's units are about to be shown
	void OnCombatVisualized(const CvCombatInfo* pInfo);
	// CvUnit::kill: the unit leaves the game (its pending state is dropped)
	void OnUnitGone(const CvUnit* pUnit);
	// CvPlayer::setTurnActive(false): the safety sweep (a unit still pending on a watched plot is a missed reveal:
	// counted and flushed) and the statistics row at a new game turn
	void Sweep();
}

#endif
