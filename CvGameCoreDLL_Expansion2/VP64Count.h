// VP64Count.h: counting builds for a performance study (memory-for-speed candidates of the x64 server).
//
// Three counters, each behind its own define, none of which is set by any product build:
//   VP64_COUNT_TRADEPATH  how often a major rebuilds its trade paths with the same result as its previous rebuild
//   VP64_COUNT_REACH      how often the same reachable set (CvPathFinder::GetPlotsInReach) is asked again unchanged
//   VP64_COUNT_PARALLEL   whether each major's first trade-path rebuild and danger map of a game turn saw any input
//                         change since the game turn started (the premise of building them in parallel at turn start)
// The counters only read game state, hash it and time it with QueryPerformanceCounter; they write CSV files into
// the folder named by the environment variable VP64_COUNT_DIR (else the working folder). Nothing here changes a
// decision of the game. Without any of the three defines this header is empty and every hook in the sources is
// compiled out by its own #if, so the preprocessed sources equal the unchanged ones (the PCH header includes this
// file only under the same #if).
#ifndef VP64COUNT_H
#define VP64COUNT_H

#if defined(VP64_COUNT_TRADEPATH) || defined(VP64_COUNT_REACH) || defined(VP64_COUNT_PARALLEL)
#define VP64_COUNT_ANY 1

#include <map>

class CvPlot;
struct SPath;
struct STradePathInfo;
struct SPathFinderUserData;
class ReachablePlots;
class CvAStar;

namespace VP64Count
{
	typedef unsigned long long u64;

	// input events: counted per game turn; each one also advances the reach counter's world epoch
	enum Event
	{
		EV_ROUTE, EV_ROUTE_PILLAGE, EV_IMPROVEMENT, EV_IMPROVEMENT_PILLAGE, EV_FEATURE, EV_OWNER,
		EV_REVEALED, EV_REVEALED_IMPROVEMENT, EV_REVEALED_ROUTE, EV_VISIBILITY,
		EV_WAR, EV_PEACE, EV_CITY_FOUND, EV_CITY_ACQUIRE, EV_CITY_KILL,
		EV_UNIT_MOVE, EV_UNIT_CREATE, EV_UNIT_DEATH, EV_DANGER_UPDATE,
		EV_COUNT
	};

	u64 Now();
	// eTeam: the team whose view changed (revealed*, visibility), else -1
	void OnEvent(Event e, int eTeam = -1);
	// a unit event, also counted for every team that sees pA or pB (either may be NULL)
	void OnUnitEvent(Event e, const CvPlot* pA, const CvPlot* pB);
	// called right after CvGame::doTurn increments the game turn: closes the previous turn's rows
	void OnTurnStart(int iGameTurn);

#if defined(VP64_COUNT_TRADEPATH) || defined(VP64_COUNT_PARALLEL)
	void TradeRebuildBegin(int ePlayer, int iLastUpdate);
	void TradeRebuildEnd(int ePlayer);
#endif
#if defined(VP64_COUNT_TRADEPATH)
	void TradeInvalidate(int ePlayer);
	void TradeCity(int ePlayer, int iCityPlot, u64 t0, const std::map<int, SPath>& water, const std::map<int, SPath>& land,
		const std::map<int, STradePathInfo>& waterInfo, const std::map<int, STradePathInfo>& landInfo);
#endif
#if defined(VP64_COUNT_REACH)
	// one per CvPathFinder::GetPlotsInReach(int, int, data) call; Done() is called with the result before it returns
	class ReachScope
	{
	public:
		ReachScope(const CvAStar* pFinder, int iX, int iY, const SPathFinderUserData& data);
		~ReachScope();
		void Done(const ReachablePlots& plots);
	private:
		bool m_bDone;
		u64 m_t0;
		u64 m_keyA, m_keyB;
		u64 m_epoch;
		int m_iStack;
		int m_iX, m_iY, m_iPath, m_iFlags, m_iPlayer, m_iUnit;
	};
#endif
#if defined(VP64_COUNT_PARALLEL)
	void DangerBegin(int ePlayer);
	void DangerEnd(int ePlayer);
#endif
}

#endif // any counter
#endif // VP64COUNT_H
