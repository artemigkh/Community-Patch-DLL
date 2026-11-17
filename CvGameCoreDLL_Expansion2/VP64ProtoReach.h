// VP64ProtoReach.h: prototype (b) of the x64 turn-time study (epic 0437, ticket 4674): a per-turn cache of the danger
// maps' enemy reach (CvDangerPlots::UpdateDangerSingleUnit -> TacticalAIHelpers::GetAllPlotsInReachThisTurn), keyed
// by the unit and the full SPathFinderUserData, invalidated per unit and per region (the plots the search touched plus
// one ring, two for a unit that can embark), with per-team, per-player and global epochs for the inputs that are not
// tied to a plot. Evidence only: this file and every call site compile only with VP64_PROTO_REACH, which no product
// build defines. Implementation: VP64ProtoReach.inl, included at the end of CvDangerPlots.cpp.
//
// Run-time switch VP64_PROTO_REACH_MODE (environment): "on" (default) uses the cache; "verify" computes every hit
// again, compares the two results field by field, records any difference and plays the fresh result (so a verify run
// plays the uncached game); "off" calls the pathfinder directly (only the counters run). Statistics per game turn go to
// %VP64_PROTO_REACH_DIR%\ReachCache.csv (else the working folder), differences to ReachCacheMismatch.csv.
#pragma once
#if defined(VP64_PROTO_REACH)

class CvUnit;
class CvPlot;

namespace VP64Reach
{
	enum GlobalReason
	{
		GR_MINOR_ALLY,
		GR_BORDER_OBSTACLE,
		GR_MINOR_FRIENDS,
		GR_MINOR_INTRUDE,
		GR_COUNT
	};

	// a change on one plot that any unit's reach may read (owner, route, improvement, feature, terrain, units, cities)
	void PlotEvent(const CvPlot* pPlot);
	void PlotXYEvent(int iX, int iY);
	// a change of what one team knows of a plot (visibility on/off, revealed, revealed route or improvement,
	// invisible-unit visibility)
	void TeamPlotEvent(const CvPlot* pPlot, int eTeam);
	// a change of the unit itself (promotion, embark, transport, death): its own entry goes, its plot changes for others
	void UnitEvent(const CvUnit* pUnit);
	// a unit moves from pOld to pNew
	void UnitMove(const CvPlot* pOld, const CvPlot* pNew);
	// a change of a team's or a player's own state that its units' reach may read (techs, war, open borders, vassals,
	// policies, buildings)
	void TeamEvent(int eTeam);
	void PlayerEvent(int ePlayer);
	// a change that any unit's reach may read anywhere (counted by reason; no caller after build 3)
	void GlobalEvent(int eReason);
	// a change of what entering one player's territory means (a minor's ally, friends, intrusion; a border obstacle):
	// every plot the player owns changes (counted under eReason)
	void PlayerPlotsEvent(int ePlayer, int eReason);
	void TeamPlotsEvent(int eTeam, int eReason);
	// a new game turn: the cache is emptied and the statistics row of the turn before is written
	void TurnStart(int iTurn);
	// the cached replacement of GetAllPlotsInReachThisTurn(pUnit, pUnit->plot(), iFlags, 0, pUnit->maxMoves(), zoc)
	ReachablePlots DangerReach(const CvUnit* pUnit, int iFlags, const PlotIndexContainer& plotsToIgnoreForZOC);
}

#endif
