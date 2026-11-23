// VP64SpeedTrade.h: the trade package of the x64 speedup work (c5-64bit-patch, ticket 4808, epics 0504/0562).
// x64 fork only. Seen only inside `#if defined(VP64_SPEED_TRADE) || defined(VP64_SPEED_TRADE_TR7) ||
// defined(VP64_SPEED_TRADE_TR8)`: with every package define off this file is not included and the DLL is the pin's.
//
// The defines (compile time only, no environment switch):
//   VP64_SPEED_TRADE          TR3: a trade-path search (CvPathFinder::GetMultiplePaths for PT_TRADE_WATER / PT_TRADE_LAND
//                             inside CvGameTrade::UpdateTradePathCache on the core thread) is answered from the result
//                             of the same key's last search while everything that search read is unchanged.
//   VP64_SPEED_TRADE_TR4      TR4 (needs VP64_SPEED_TRADE): the per-plot signature narrowed to what the callbacks of the
//                             search's own path type derive from the plot (a water and a land signature).
//   VP64_SPEED_TRADE_TR7      TR7: CvPlayerTrade::UpdateFurthestPossibleTradeRoute walks only the origin city's row of
//                             the trade-path cache (listener and logging fallback to the stock loop).
//   VP64_SPEED_TRADE_TR8      TR8: CvPlayerTrade::GetTradeRouteTurnMod walks only the buildings a city has.
//   VP64_SPEED_TRADE_VERIFY   every fast answer is computed next to the stock one, compared and counted
//                             (VP64SpeedVerify.h); the stock answer is played, so the game is the pin's.
//   VP64_SPEED_TRADE_SPOIL=n  negative control for the verify run: the n-th stored search result is made wrong.
//   VP64_SPEED_TRADE_SPOIL_EVERY=n  negative control for the identity job: every n-th stored result wrong, no verify.
//   VP64_SPEED_TRADE_STATS    per rebuild-scope counters and the store's bytes by part, VP64SpeedTrade.csv in
//                             %VP64_SPEED_DIR%.
//   VP64_SPEED_TRADE_CAP_MB   the store's cap on its counted bytes (default 32).
// Design and the two-thread answers: research/08-performance/speedup-trade.md (c5-64bit-patch).
#pragma once
#ifndef VP64_SPEED_TRADE_H
#define VP64_SPEED_TRADE_H

#if defined(VP64_SPEED_TRADE_VERIFY) || defined(VP64_SPEED_TRADE_SPOIL) || defined(VP64_SPEED_TRADE_SPOIL_EVERY)
#include "VP64SpeedVerify.h"
#endif

namespace VP64Trade
{
	// CvGameTrade::UpdateTradePathCache, around the origin-city loop (core thread only; no-op elsewhere)
	void Begin(PlayerTypes ePlayer);
	void End();
	// CvMap::uninit (every new game, load and exit): empties the store and frees the arrays
	void GameReset();
	// every place the DLL lets go of the game-core lock with the lock held (a Lua hook raise, the tactical AI's
	// hand-over to the ui): another thread may change game state there, so the per-scope memo of plot signatures
	// starts a new generation at the next lookup
	void LockYield();
	// statistics of TR7 and TR8 (written with the scope rows in statistics builds; no-ops otherwise)
	void CountTR7(bool bFast);
	void CountTR8();
}

#endif // VP64_SPEED_TRADE_H
