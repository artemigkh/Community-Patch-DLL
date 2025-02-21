-- Player Bonuses
ALTER TABLE HandicapInfos ADD MapPlacementPriority integer DEFAULT 3;
ALTER TABLE HandicapInfos ADD StartingGold integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HappinessDefaultCapital integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD EmpireSizeUnhappinessMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD MaintenanceFreeUnits integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD UnitSupplyBase integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD UnitSupplyPerCity integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD UnitSupplyPopulationPercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD UnitSupplyPerEraFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD UnitSupplyPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD UnitSupplyBonusPercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD StartingUnitMultiplier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD WorkRateModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD UnitUpgradePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD UnitUpgradePerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD GrowthPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD GrowthPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD ResearchPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD TechCatchUpMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD PolicyPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD PolicyCatchUpMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD ProphetPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD GreatPeoplePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD GoldenAgePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CivilianPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CivilianPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD TrainPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD TrainPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD WorldTrainPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD ConstructPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD ConstructPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD WorldConstructPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CreatePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CreatePerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD WorldCreatePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD ProcessBonus integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD ProcessPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD FreeXP integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD FreeXPPercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD FreeXPPercentVSHuman integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CombatBonus integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD ResistanceCap integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD VisionBonus integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD SpySecurityModifier integer DEFAULT 0;
-- VP Difficulty Bonus
ALTER TABLE HandicapInfos ADD DifficultyBonusTurnInterval integer DEFAULT 0;

-- AI Bonuses
ALTER TABLE HandicapInfos ADD AIStartingGold integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIStartingPolicyPoints integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIAdvancedStartPointsMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIHappinessDefault integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIHappinessDefaultCapital integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIExtraHappinessPerLuxury integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIEmpireSizeUnhappinessMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIPopulationUnhappinessMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIFreeCulturePerTurn integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIMaintenanceFreeUnits integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIUnitSupplyBase integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIUnitSupplyPerCity integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIUnitSupplyPopulationPercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIUnitSupplyPerEraFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIUnitSupplyPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIUnitSupplyBonusPercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIImprovementCostPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD AIUnitUpgradePerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIGrowthPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIResearchPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD AIResearchPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AITechCatchUpMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIPolicyPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD AIPolicyPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIPolicyCatchUpMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIProphetPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD AIGreatPeoplePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD AIGoldenAgePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD AICivilianPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD AICivilianPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AITrainPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIConstructPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AICreatePerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIProcessBonus integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIProcessPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIFreeXPPercentVSHuman integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AICombatBonus integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIResistanceCap integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIVisionBonus integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AISpySecurityModifier integer DEFAULT 0;
-- VP Difficulty Bonus
ALTER TABLE HandicapInfos ADD AIDifficultyBonusTurnInterval integer DEFAULT 0;

-- City-States
ALTER TABLE HandicapInfos ADD StartingCityStateWorkerUnits integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD StartingCityStateDefenseUnits integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateUnitSupplyBase integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateUnitSupplyPerCity integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateUnitSupplyPopulationPercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateUnitSupplyPerEraFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateUnitSupplyPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateUnitSupplyBonusPercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateWorkRateModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateGrowthPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CityStateGrowthPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateCivilianPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CityStateCivilianPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateTrainPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CityStateTrainPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateConstructPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CityStateConstructPerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateCreatePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD CityStateCreatePerEraModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateFreeXP integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateFreeXPPercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateCombatBonus integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD CityStateVisionBonus integer DEFAULT 0;

-- Barbarians
ALTER TABLE HandicapInfos ADD BonusVSBarbarians integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIBonusVSBarbarians integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD BarbarianCampGold integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIBarbarianCampGold integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD BarbarianSpawnDelay integer DEFAULT 0;

-- AI Behavior Modifiers
-- Weighted Randomized Choices
ALTER TABLE HandicapInfos ADD CityProductionChoiceCutoffThreshold integer DEFAULT 90;
ALTER TABLE HandicapInfos ADD TechChoiceCutoffThreshold integer DEFAULT 90;
ALTER TABLE HandicapInfos ADD PolicyChoiceCutoffThreshold integer DEFAULT 90;
ALTER TABLE HandicapInfos ADD BeliefChoiceCutoffThreshold integer DEFAULT 90;
-- Tactical AI
ALTER TABLE HandicapInfos ADD TacticalSimMaxCompletedPositions integer DEFAULT 512;
ALTER TABLE HandicapInfos ADD TacticalSimMaxBranches integer DEFAULT 4;
ALTER TABLE HandicapInfos ADD TacticalSimMaxChoicesPerUnit integer DEFAULT 4;
-- Diplomacy AI
ALTER TABLE HandicapInfos ADD LandDisputePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD WonderDisputePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD MinorCivDisputePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD VictoryDisputePercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD VictoryDisputeMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD VictoryBlockPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD VictoryBlockMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD WonderBlockPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD WonderBlockMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD TechBlockPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD TechBlockMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD PolicyBlockPercent integer DEFAULT 100;
ALTER TABLE HandicapInfos ADD PolicyBlockMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD PeaceTreatyDampenerTurns integer DEFAULT 20;
ALTER TABLE HandicapInfos ADD AggressionIncrease integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanStrengthPerceptionMod integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanTradeModifier integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanOpinionChange integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanWarApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanWarApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanHostileApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanHostileApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanDeceptiveApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanDeceptiveApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanGuardedApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanGuardedApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanAfraidApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanAfraidApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanNeutralApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanNeutralApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanFriendlyApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD HumanFriendlyApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIOpinionChange integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIWarApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIWarApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIHostileApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIHostileApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIDeceptiveApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIDeceptiveApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIGuardedApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIGuardedApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIAfraidApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIAfraidApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AINeutralApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AINeutralApproachChangePercent integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIFriendlyApproachChangeFlat integer DEFAULT 0;
ALTER TABLE HandicapInfos ADD AIFriendlyApproachChangePercent integer DEFAULT 0;
