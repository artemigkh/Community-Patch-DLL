/*
	-------------------------------------------------------------------------------------------------------
	SqliteLoggerRegistrations - shared table registration helpers for stats.db tables.

	Keep SQLite table registration helpers in this header so any gameplay file that needs to write to an
	existing table can include one import and reuse the same one-time registration logic. This avoids
	duplicating table schemas across translation units and makes multi-file logging changes safer.
	------------------------------------------------------------------------------------------------------- */

#pragma once

#include "SqliteLogger.h"

inline void RegisterTechChoicesTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Technology", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Action", Database::COLTYPE_TEXT));
		GET_SQLITE_LOGGER().RegisterTable("TechChoices", kColumns);
	}
}

inline void RegisterReligionChoicesTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Action", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Belief", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("BeliefType", Database::COLTYPE_TEXT));
		GET_SQLITE_LOGGER().RegisterTable("ReligionChoices", kColumns);
	}
}

inline void RegisterPolicyChoicesTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Era", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Branch", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Policy", Database::COLTYPE_TEXT));
		GET_SQLITE_LOGGER().RegisterTable("PolicyChoices", kColumns);
	}
}

inline void RegisterMilitarySummaryTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Era", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Cities", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Settlers", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("LandUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("LandArmySize", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("RecLandOffensive", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("NavalUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("NavalArmySize", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("RecNavyOffensive", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MeleeUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MobileUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ReconUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ArcherUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("SiegeUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("SkirmisherUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("AllLandRanged", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("AntiAirUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MeleeNavalUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("RangedNavalUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Submarines", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Carriers", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("TotalAirUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("BomberUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("FighterUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Nukes", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Missiles", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("RecTotal", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("TotalMilitaryUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("SupplyLimit", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OutOfSupply", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("WarWearinessSupplyReduction", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("NoSupplyUnits", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("WarCount", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MostEndangeredCity", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Danger", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MilitarySummary", kColumns);
	}
}

inline void RegisterWorldStateLogTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("GsConquest", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("GsSpaceship", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("GsDiplo", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("GsCulture", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OpinionAlly", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OpinionFriend", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OpinionFavorable", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OpinionNeutral", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OpinionCompetitor", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OpinionEnemy", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OpinionUnforgivable", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ApproachWar", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ApproachHostile", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ApproachDeceptive", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ApproachGuarded", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ApproachAfraid", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ApproachFriendly", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ApproachNeutral", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MinorIgnore", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MinorProtective", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MinorConquest", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MinorBully", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("BasicNeedsMedian", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("GoldMedian", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ScienceMedian", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("CultureMedian", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("WorldStateLog", kColumns);
	}
}

inline void RegisterGameResultTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Score", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("VictoryType", Database::COLTYPE_TEXT));
		GET_SQLITE_LOGGER().RegisterTable("GameResult", kColumns);
	}
}

// Per-turn snapshot of which era each civ is in, one row per civ per turn.
inline void RegisterCivTurnEraTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("era", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("civ_turn_era", kColumns);
	}
}

inline void RegisterMapPlotsStateTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("plotX", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("plotY", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("cityName", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("owner", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("routeType", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MapPlotsState", kColumns);
	}
}

// Per-turn snapshot of every unit owned by each major civ, one row per unit.
inline void RegisterMapUnitsStateTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("owner", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("plotX", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("plotY", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("unitID", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("unitName", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("unitMaxHP", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("unitCurrHP", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MapUnitsState", kColumns);
	}
}

// Long-format per-building yield breakdown, one row per (city, constructed building, yield).
// BaseYieldTimes100   = the building's own intrinsic flat yield (incl. population/era/per-tile and
//                       similar self-scalers) PLUS the extra yields the building grants to the tiles
//                       this city is actually working (resource/luxury/terrain/feature/improvement/
//                       sea/lake/river/city-connection plot yields).
// BonusYieldTimes100  = the extra flat yield layered on top of the base by external sources such as
//                       beliefs, policies, traits, corporations, leagues, events and other buildings.
// Percentage-based modifiers and instant/event yields are intentionally excluded (tracked elsewhere).
inline void RegisterBuildingYieldsTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Era", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("City", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Building", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Yield", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("BaseYieldTimes100", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("BonusYieldTimes100", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("BuildingYields", kColumns);
	}
}

// Long-format per-belief yield breakdown, one row per (civ, belief, source, yield) per turn.
inline void RegisterReligionBeliefYieldsTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Era", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Belief", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("BeliefType", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("IsReligionOwner", Database::COLTYPE_BOOL));
		kColumns.push_back(ColumnDef("Source", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Yield", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("YieldTimes100", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("ReligionBeliefYields", kColumns);
	}
}

// Per-turn overview of each civ's controlled building inventory (one row per building type).
inline void RegisterBuildingsOverviewTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Era", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Building", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Count", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("BuildingsOverview", kColumns);
	}
}

// Long-format instant-yield breakdown, one row per (building, city, yield) at the moment an
// instant yield fires (citizen born, building constructed, policy unlocked, tile pillaged, etc.).
// EventType is a short label derived from the instant-yield source table (Birth, PolicyUnlock,
// Pillage, ...). YieldTimes100 is the building's attributed share of the granted yield, multiplied
// by 100 so that fractional per-building contributions are preserved.
inline void RegisterBuildingInstantYieldsTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Era", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("City", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Building", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("EventType", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Yield", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("YieldTimes100", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("BuildingInstantYields", kColumns);
	}
}

// Long-format difficulty-bonus yield breakdown, one row per (civ, trigger, yield) each time an AI player receives an AI difficulty bonus
inline void RegisterHandicapYieldsTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Era", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("CityCount", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Trigger", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Yield", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("YieldTimes100", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("HandicapYields", kColumns);
	}
}

// Long-format instant-yield breakdown, one row per (civ, instant-yield type, yield) at the moment an instant yield fires.
inline void RegisterInstantYieldsTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Era", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Type", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Yield", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("YieldTimes100", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("InstantYields", kColumns);
	}
}

//	--------------------------------------------------------------------------------
//	Memory diagnostics (see MemoryDiagnostics.h). These tables exist to tell exhaustion apart from
//	fragmentation in the 32-bit address space, and to attribute fragmentation to an allocation
//	pattern. All sizes are kilobytes unless a column says otherwise.

// One row per turn: totals from a VirtualQuery walk of the whole user address range. The "Low"
// columns cover only addresses below 2GB, which is all a non-large-address-aware host can use.
// LargestFreeKB is the number that predicts an allocation failure; FreeKB alone does not.
inline void RegisterMemAddressSpaceTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("CommittedKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ReservedKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("FreeKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("LargestFreeKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("CommittedLowKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ReservedLowKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("FreeLowKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("LargestFreeLowKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("ImageKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("MappedKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("PrivateKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("TotalRegions", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("FreeRegions", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("CommittedRegions", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemAddressSpace", kColumns);
	}
}

// One row per size bucket per turn: how the free address space is distributed. A rising count of
// small buckets while LargestFreeKB falls is the signature of fragmentation rather than exhaustion.
// BucketMaxKB is the exclusive upper bound of the bucket; 0 marks the unbounded top bucket.
inline void RegisterMemFreeBlockHistogramTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("BucketMaxKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Blocks", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("TotalKB", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemFreeBlockHistogram", kColumns);
	}
}

// One row per heap-walk turn: in-use vs free-list bytes across every process heap. FreeKB here is
// memory the process has committed but is not using - intra-heap fragmentation, which the address
// space walk cannot see. WalkMs records what the measurement itself cost.
inline void RegisterMemHeapSummaryTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Heaps", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("BusyBlocks", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("BusyKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("FreeBlocks", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("FreeKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("OverheadKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("UncommittedKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("WalkMs", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemHeapSummary", kColumns);
	}
}

// One row per allocation size class per heap-walk turn. A large Blocks count in a small size class
// means many small, long-lived, scattered allocations - the pattern that fragments a heap fastest.
// BucketMaxBytes is the exclusive upper bound; 0 marks the unbounded top bucket.
inline void RegisterMemHeapSizeClassTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("BucketMaxBytes", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Blocks", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("TotalKB", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemHeapSizeClass", kColumns);
	}
}

// One row per surviving-or-dead civ per turn: the replay history each player still holds. Entries
// is the exact number of retained (dataset, turn) samples, each of which is one separately
// allocated red-black tree node. EntryBytes is that node's measured heap cost on this build, so
// Entries * EntryBytes is a measurement rather than an estimate.
inline void RegisterMemReplayDataTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Civ", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("PlayerId", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Datasets", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Entries", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("EntryBytes", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("TotalKB", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemReplayData", kColumns);
	}
}

// One row per turn per large committed address-space region, biggest first. The heap accounts for
// only part of committed memory; direct VirtualAlloc, thread stacks and mapped images make up the
// rest, and this is where that rest becomes visible. BaseHigh is the region's base address shifted
// right by 16 (so it fits an INT), which is enough to recognise the same region across turns.
inline void RegisterMemRegionsTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Rank", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("BaseHigh", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("SizeKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("RegionType", Database::COLTYPE_TEXT));
		GET_SQLITE_LOGGER().RegisterTable("MemRegions", kColumns);
	}
}

// One row per turn per individual large heap allocation, biggest first. This is what turns "the
// 256KB+ size class holds 1.7 GB" into a list of concrete allocation sizes that can be matched
// against the structures in MemGameState.
inline void RegisterMemTopBlocksTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Rank", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Bytes", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemTopBlocks", kColumns);
	}
}

// One row per turn per game subsystem: the analytic census of what the game data *is*, derived from
// public counts and sizeof. Sizes exclude per-block heap headers and spare container capacity, so
// the gap between SUM(SizeKB) and MemHeapSummary.BusyKB is the memory this model does not explain -
// which is the number that says whether the model can be trusted. Items and UnitBytes are carried
// so a wrong assumption shows up as an implausible per-item size instead of a distorted total.
inline void RegisterMemGameStateTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Subsystem", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Detail", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("SizeKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Items", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("UnitBytes", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemGameState", kColumns);
	}
}

// One row per turn of game-entity counts, so memory growth can be correlated with what the game
// actually contains rather than only with the turn number. Deal counts are participation slots:
// the per-player accessors are all that is exposed, so a two-party deal counts once per side.
inline void RegisterMemEntityCountsTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("NumPlots", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("GridWidth", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("GridHeight", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("AlivePlayers", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("AliveMajors", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Units", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Cities", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("PlotsOwned", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("CurrentDealSlots", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("HistoricDealSlots", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemEntityCounts", kColumns);
	}
}

// Static structure sizes, written once per game. Every derived figure in MemGameState is some count
// multiplied by one of these, so recording them makes those figures checkable after the fact and
// makes a layout change between builds visible instead of silently shifting every total.
inline void RegisterMemSizeofTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("TypeName", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("Bytes", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemSizeof", kColumns);
	}
}

// One row per turn per distinct *exact* allocation size inside the leak window (128KB-2MB), largest
// total first. The size-class histogram narrowed the leak to a 256KB-wide bucket; this narrows it to
// a single number. NewThisTurn counts blocks of that size which were absent at the previous heap
// walk, so the leaking size is the one where NewThisTurn stays positive turn after turn.
inline void RegisterMemBlockSizesTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Rank", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("SizeBytes", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("Blocks", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("NewThisTurn", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("TotalKB", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemBlockSizes", kColumns);
	}
}

// A sample of the blocks that appeared since the previous heap walk, with their first four 32-bit
// words as hex. Content identifies the type where size alone cannot: values inside the plot array
// (see MemAnchors) mean an array of CvPlot*, small magnitudes mean integer data, and values in the
// heap's own range mean pointers to something else.
inline void RegisterMemNewBlocksTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Sample", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("SizeBytes", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("HeadWords", Database::COLTYPE_TEXT));
		GET_SQLITE_LOGGER().RegisterTable("MemNewBlocks", kColumns);
	}
}

// One row per turn summarising the leak window: how many blocks live in it, how many appeared this
// turn, and how many bytes each represents. NewBlocks x their size is the per-turn leak rate, direct.
// HadPrevious is false on the first walk of a session, where "new" has no meaning.
inline void RegisterMemBlockWindowTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("WindowBlocks", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("WindowKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("NewBlocks", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("NewKB", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("DistinctSizes", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("SizeOverflow", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("HadPrevious", Database::COLTYPE_BOOL));
		GET_SQLITE_LOGGER().RegisterTable("MemBlockWindow", kColumns);
	}
}

// Addresses of a few known objects, logged each turn because they move across a reload. Purely so a
// pointer value captured in MemNewBlocks can be attributed to a structure offline. AddrHigh is the
// address shifted right by 4 to fit an INT; multiply by 16 to recover it.
inline void RegisterMemAnchorsTable()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool bRegistered = false;
	if (!bRegistered)
	{
		bRegistered = true;
		TableDef kColumns;
		kColumns.push_back(ColumnDef("Anchor", Database::COLTYPE_TEXT));
		kColumns.push_back(ColumnDef("AddrHigh", Database::COLTYPE_INT));
		kColumns.push_back(ColumnDef("SizeKB", Database::COLTYPE_INT));
		GET_SQLITE_LOGGER().RegisterTable("MemAnchors", kColumns);
	}
}
