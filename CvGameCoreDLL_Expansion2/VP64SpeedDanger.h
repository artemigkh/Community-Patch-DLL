// VP64SpeedDanger.h: the danger package of the speedup epic (c5-64bit-patch epic 0504, ticket 4807), x64 fork only.
// Seen only with VP64_SPEED_DANGER (CvGameCoreDLLPCH.h includes it inside that #if); no product build defines it.
//
// The enemy-reach cache: CvDangerPlots::UpdateDangerSingleUnit asks every enemy unit's reach this turn
// (TacticalAIHelpers::GetAllPlotsInReachThisTurn with the unit's own maxMoves, max turns 1); the question carries
// nothing of the observer, so every observer of one unit asks the same one. VP64Danger::Reach keeps the answer per unit
// for the game turn and returns it while nothing the search read has changed: the unit (a snapshot compared by value),
// its team and player (epochs bumped by the setters of techs, war, borders, policies, buildings, traits), and every plot
// the search closed plus one ring (two for a unit that can embark; for an airlift search two rings around every airlift
// city), each with a stamp per plot and per plot and team, bumped by the setters of what a search reads there. The
// design, every input with its writers, and the two-thread answers: research/08-performance/speedup-danger.md of the
// c5-64bit-patch repository.
//
// Defines: VP64_SPEED_DANGER (the cache); VP64_SPEED_DANGER_VERIFY (every hit searches again, compares, logs every
// difference through VP64SpeedVerify.h and plays the fresh result); VP64_SPEED_DANGER_STATS (per-turn counters in
// %VP64_SPEED_DIR%\VP64DangerStats.csv; implied by VERIFY and by the spoil defines); VP64_SPEED_DANGER_SPOIL=<n> and
// VP64_SPEED_DANGER_SPOIL_EVERY=<n> (the negative controls: the kept result of the n-th, or every n-th, hit made wrong
// in the cache before it is used).
#pragma once
#if defined(VP64_SPEED_DANGER)

class CvUnit;
class CvPlot;

namespace VP64Danger
{
	// a change on one plot that a unit's reach may read (owner, owning city, city, plot type, terrain, feature,
	// improvement and pillage, route and pillage, the units on it)
	void PlotEvent(const CvPlot* pPlot);
	// the same for a plot and its six neighbours (a river crossing is read from both sides)
	void PlotAndAdjacentEvent(const CvPlot* pPlot);
	// a change of what one team knows or sees of a plot (visibility on/off, revealed, revealed route, improvement and
	// owner, invisible-unit visibility)
	void TeamPlotEvent(const CvPlot* pPlot, int eTeam);
	// a change of the unit itself (promotion, embark, transport, death): its plot changes (every region holds its unit's
	// own plot, so the unit's entry and every entry that read that plot miss)
	void UnitEvent(const CvUnit* pUnit);
	// a unit moves from pOld to pNew
	void UnitMove(const CvPlot* pOld, const CvPlot* pNew);
	// a change of a team's or a player's own state that its units' reach may read; a player event moves its team too
	void TeamEvent(int eTeam);
	void PlayerEvent(int ePlayer);
	// what entering a player's (team's) territory means changed: every plot it owns changes
	void PlayerPlotsEvent(int ePlayer);
	void TeamPlotsEvent(int eTeam);
	// a new game turn (CvGame::doTurn): the cache is emptied; caching starts at the first one after a reset
	void TurnStart(int iTurn);
	// the map goes (CvMap::uninit: every load, new game and exit): the cache and the stamp arrays go, caching stops
	void GameReset();
	// the cached replacement of GetAllPlotsInReachThisTurn(pUnit, pUnit->plot(), iFlags, 0, pUnit->maxMoves(), zoc)
	ReachablePlots Reach(const CvUnit* pUnit, int iFlags, const PlotIndexContainer& plotsToIgnoreForZOC);

	// scope guards for setters that do more than their member write: the event at the start and again at every exit
	class PlotScope
	{
	public:
		explicit PlotScope(const CvPlot* pPlot);
		~PlotScope();
	private:
		int m_iPlot;
	};
	class PlayerScope
	{
	public:
		explicit PlayerScope(int ePlayer);
		~PlayerScope();
	private:
		int m_ePlayer;
	};
	class TeamScope
	{
	public:
		explicit TeamScope(int eTeam);
		~TeamScope();
	private:
		int m_eTeam;
	};
}

#endif
