# Live-game Lua cookbook (Civ 5 + Vox Populi)

Short Lua recipes for setting up and checking test situations in a running game, grouped by what you
want to do. Each one is written for the DLL's external Lua channel (`scripts/vp_lua.py`).

**Status tags.** Every recipe starts as `Status: UNTESTED`. `RAN, EFFECT NOT VERIFIED` means the call ran without error or
assert but the game state made its effect impossible to see; `NOT RUN` means it was deliberately skipped. The method names and argument orders were
checked against the DLL bindings (`CvGameCoreDLL_Expansion2/Lua/CvLua*.cpp`, as of commit `434f30f80` plus the
uncommitted Lua changes, 2026-09-16), and every `UI.*` / `Events.*` / `ControlTypes` / popup name against shipped or
modpack UI Lua or the DLL enum registrations. Type strings such as `UNIT_WARRIOR` were checked against the
modpack's `Civ5DebugDatabase.db` (cache folder, 2026-09-15). Once a recipe works in the game, change its tag to
`Status: TESTED (date, what was checked)`. If it fails, add a `Broken:` line saying what happened.

**Live testing, 2026-09-17** (turn-317 huge-map save, release DLL, human player 2): B1-B4, G1-G4, G10 and P1-P2 were run
in a first session, which stopped at 02:00:24 when a follow-up query froze the game behind a release-build "Assertion
Failed" dialog. A second session (02:45-03:40, no assert, no freeze) ran every Unit, City, Map/plot and UI recipe and
P3-P17 against a spawned test unit, a founded test city and restored test plots. Final tally: **64 TESTED** (C7 and B2
failed as written and were fixed), **3 RAN, EFFECT NOT VERIFIED** (P2: faith is a no-op with religion disabled; P14: the
map was already fully revealed; UI12: nothing was stale), **14 NOT RUN** (G5-G9: turn/autoplay/save; C13, P8-P10: religion
is disabled in this game; P4, P7, P12, P13, P16: irreversible or open a modal popup), **0 UNTESTED**. A third,
adversarial session re-ran U1, U4, U5, U6, U9, U10, U11, C1, C4, C6, C7, C10, P17 and M4 on fresh objects (see
"Verification re-run" in the Findings). See "Findings from live testing 2026-09-17" at the end. Screenshots named below
are in the second session's scratchpad
(`%LOCALAPPDATA%/Temp/claude/C--Users-Art-Documents-GitHub-Community-Patch-DLL/8e0df880-3f58-4534-b8e3-5329f7c12c90/scratchpad/shots/`).
**Observed lines often quote an instrumented run**: the chunk that ran returned extra before/after values next to the
snippet's own, so an Observed tuple can be longer than the snippet's `return`.

Related references: `lua-api.md` has every DLL binding and how it reads each argument. `firetuner.md` §7
has the stock FireTuner panel Lua. `engine-api-observed.md` covers the engine-side `UI` and `Events` APIs.

---

## How to run a recipe

```bash
python .claude/skills/civ5-game-ui/scripts/vp_lua.py "return Game.GetGameTurn()"
python .claude/skills/civ5-game-ui/scripts/vp_lua.py -f recipe.lua          # uses the file's --@state= line
python .claude/skills/civ5-game-ui/scripts/vp_lua.py --state InGame -f recipe.lua
python .claude/skills/civ5-game-ui/scripts/vp_lua.py --states
```

- Every snippet starts with a `--@state=` line. `vp_lua.py -f` uses it as the state when you don't pass
  `--state`. `Main` is the game's global Lua state, and any other name is a UI context's environment
  (`InGame`, `TechTree`, ...). Recipes that call `UI.*`, `Events.*`, `ContextPtr` or `Controls` say which UI state they need.
- A chunk gets three extra names: `vp` (a table that persists across requests for the life of the game
  process), `States()` (the UI state names), and `G` (the real global table). `print` output comes back to you
  and does not go to Lua.log. Error line numbers count the 2 header lines that `vp_lua.py` adds.
- The DLL polls for requests at the top of `CvGame::update`. That means a request is answered only while a
  game is loaded, and **a chunk runs on the game-core thread**. An endless loop freezes the game.

## Conventions used in every recipe

1. **Use `local` for everything.** Globals that a chunk assigns are written into the target state (the
   same as in FireTuner), so they can collide with the UI's own globals.
2. **Look up IDs by Type string and check them:** `local id = assert(GameInfoTypes.UNIT_WARRIOR, "unknown type")`.
   An unknown Type gives `nil`, `assert` turns that into a clean Lua error, and nothing wrong reaches C++.
3. **Pass IDs, never objects, between requests:** `vp.u = { owner = u:GetOwner(), id = u:GetID() }`. A Lua
   Unit/City/Plot object is a raw C++ pointer with no liveness check (`CvLuaScopedInstance.h`), so **using it
   after the unit dies or upgrades, or after the city changes hands, can crash the game.** In a later
   request, look it up again with `Players[owner]:GetUnitByID(id)` or `:GetCityByID(id)`.
4. **Return something you can check.** Results come back rendered as text: tables up to depth 4 and 200
   entries per level.
5. **Don't change a collection while iterating it.** Collect IDs out of `p:Units()`, `p:Cities()` or
   `plot:GetUnit(i)` first, then act on them. For plot unit lists, go backwards.

## Argument pitfalls (verified in the binding source)

The DLL reads arguments in four ways, and each one handles omitted and boolean arguments differently.
`lua-api.md` marks which one each parameter uses.

| How the binding reads the parameter | You omit it | You pass `true`/`false` | You pass `0`/`1` |
|---|---|---|---|
| `lua_toboolean` (and the bool parameters of a `BasicLuaMethod` wrapper, `~X` in lua-api.md) | **false, and the C++ default is NOT applied** | fine | `0` is **true** (Lua truthiness) |
| `lua_tointeger` (and the int/enum parameters of a `BasicLuaMethod` wrapper) | **0, and the C++ default is NOT applied** | **both read as 0, silently** (verified live: `Map.GetPlot(true, true)` is plot 0,0), so `true` for a flag read this way is false | fine |
| `luaL_optint` (`?:int=d` in lua-api.md) | the default `d` | **Lua error "bad argument #N to 'X' (number expected, got boolean)"** (confirmed live 2026-09-17 with `pCity:PushOrder`; a clean Lua error, nothing reaches C++) | fine |
| `luaL_optbool` (`?:bool=d`) | the default `d` | fine | `0` is **true** |

The cases these recipes run into, where omitting an argument does something harmful:

| Call | If you omit the trailing arguments | Write instead |
|---|---|---|
| `pTeam:SetHasTech(tech, true)` | credits **player 0** (`ePlayer`=0), with bFirst and bAnnounce false | `pTeam:SetHasTech(tech, true, pid, false, false)` |
| `pTeamTechs:SetResearchProgress(tech, n)` / `ChangeResearchProgress` | player 0 | `(tech, n, pid)` |
| `pTeam:DeclareWar(t)` | originating player 0 | `pTeam:DeclareWar(t, false, pid)` |
| `pTeam:MakePeace(t)` | bBumpUnits false, originating player 0 | `pTeam:MakePeace(t, true, false, pid)` |
| `pTeam:CanDeclareWar(t)` | asks as player 0 | `pTeam:CanDeclareWar(t, pid)` |
| `Game.SetAIAutoPlay(n)` | returns control as **player 0** | `Game.SetAIAutoPlay(n, returnPid)` (-1 = stay observer) |
| `pPlot:SetImprovementType(imp)` | builder = **player 0**, and some improvements then claim the plot for that player | `pPlot:SetImprovementType(imp, -1, false)` |
| `pPlot:SetRouteType(r)` | builder = player 0 | `pPlot:SetRouteType(r, -1)` |
| `pPlot:SetPlotType(t)` | no recalc, no graphics rebuild, no unit erase | `pPlot:SetPlotType(t, true, true, true)` |
| `pPlot:SetTerrainType(t)` | no recalc, no graphics rebuild | `pPlot:SetTerrainType(t, true, true)` |
| `pPlot:SetImprovementPillaged(b)` / `SetRoutePillaged(b)` | bEvents false | `(b, true)` |
| `pPlot:SetOwner(p)` | acquiring city ID 0 | `pPlot:SetOwner(p, cityID or -1, true)` |
| `pCity:PushOrder(o, d1)` | d2 = 0 (UnitAI 0) | `pCity:PushOrder(o, d1, -1, 0, true, false, 0)` (bSave and bForce are read as numbers: `false` for bForce is a Lua error) |
| `pUnit:Promote(promo)` / `CanPromote(promo)` | leader unit ID 0 | `(promo, -1)` |
| `pTeamTechs:ChangeResearchProgress(tech, n, -1)` | - | never pass -1 as the player: `SetResearchProgressTimes100` has `PRECONDITION(ePlayer >= 0)`, which crashes the game |
| `pCity:SetPopulation(n, false)` / `ChangePopulation(n, false)` | omitted = true (the binding checks `lua_isboolean`) | never pass `false`: the binding ASSERTs on it |
| `pPlot:SetResourceType(r, 0)` / `SetNumResource(0)` on a plot with a resource | - | quantity must be >= 1 while a resource is set (ASSERT); remove with `SetResourceType(-1, 0)` |
| `pPlot:SetResourceType(csLuxury, n)` | bIgnoreMinorCivRestrictions = false | city-state-only luxuries ASSERT unless the 3rd argument is `true` |
| `pMinor:GetMinorCivFriendshipWithMajor(p)` / `ChangeMinorCivFriendshipWithMajor(p, n)` | player 0 | the getter has no range check: `p` must be a major (0 .. MAX_MAJOR_CIVS-1), or a PRECONDITION crashes the game |
| `Game.IsOption(GameInfoTypes.GAMEOPTION_X)` | - | **wrong ID space**: `GameInfoTypes.GAMEOPTION_NO_RELIGION` is 25 (DB row), `GameOptionTypes.GAMEOPTION_NO_RELIGION` is 21 (enum, `NUM_GAMEOPTION_TYPES` = 22). A number >= 22 goes to the hash lookup that ASSERTs. Use the name string, `Game.IsOption("GAMEOPTION_NO_RELIGION")` (the string overload, `CvPreGame::GetGameOption(const char*)`, has no assert path; `true` live), or a range-checked `GameOptionTypes.*` value |

Crashes, as opposed to Lua errors: passing an invalid enum or ID into C++ (an out-of-range tech such as -2 or a stale
number into `SetHasTech` or `IsHasTech`, an invalid type into `InitUnit`, a unit with no upgrade into `Upgrade`) is not
caught by `pcall`. It takes the game down or freezes it (see below). Guard these calls with `assert`. Not every bad value
crashes: `SetHasTech(-1, ...)` returns early (`NO_TECH`), and a `nil` where the binding expects an object (`FoundReligion`'s
holy city) is a clean Lua error, "Instance does not exist." (`CvLuaScopedInstance::GetInstance`).

**Freezes: release-build ASSERT dialogs (seen live 2026-09-17).** The release DLL is built with `VPRELEASE_ERRORMSG`, so
every failed C++ `ASSERT` opens a system-modal "Assertion Failed" `MessageBoxA` on the thread that hit it. From this
channel that is the game-core thread **while it holds the engine's Lua lock**: the chunk never returns (`vp_lua.py`
times out), the UI thread blocks too (a PrintWindow screenshot hung for 25 s), and nothing moves until someone answers
the dialog. **Its default button is Cancel, which exits the game** (`BUILTIN_TRAP`); OK continues and silences that
assert for the session. **A failed `PRECONDITION` (and `VALIDATE_OBJECT`) is worse**: it shows `CvPreconditionDlg` and then
always hits `BUILTIN_TRAP`, so the game dies whatever is answered (`CvGameCoreDLLPCH.h`). Treat every PRECONDITION reachable
with your arguments as a crash. `pcall` does not help. Triggers are the same invalid values as above, plus `NO_*` (-1) and
`NUM_*` members of enum tables: never feed `pairs(SomeTypes)` straight into C++ (see Findings). Check for one with
`CvAssert.log` in the game folder (a new entry) or a visible `#32770` window titled "Assertion Failed" in the game
process.

## General caveats for UI-state recipes

- **Threading (tested 2026-09-17, no problem seen).** UI Lua normally runs on the main thread. Through this channel it runs on the
  game-core thread while holding the engine's Lua lock (see `CvLuaSupport.h`). In the second live session every UI recipe
  below ran from the channel without a crash, hang or assert: `Events.SerialEventGameMessagePopup` (tech tree, policies,
  economic/military overviews, text popup), `Events.SerialEventExitCityScreen`, `Events.GameplayAlertMessage`,
  `Events.SerialEventHexHighlight`/`ClearHexHighlights`, `Events.SearchForPediaEntry`, `UIManager:DequeuePopup`, a screen's
  own `OnClose()`, `UI.LookAt`, `UI.SelectUnit`, `UI.DoSelectCityAtPlot`. `Game.DoControl(ControlTypes.*)` from Main
  also works for the screen and toggle controls (tech tree, policies, yields); only the end-turn controls did nothing.
- **State names.** The UI context names come from `States()`. FireTuner uses `InGame`, `WorldView`, `CityView`
  and `DebugMenu` (see `firetuner.md`). Popup contexts (`TechTree`, `SocialPolicyPopup`, ...) are named after
  their files, as in vox-deorum's `VoxDeorumHumanTrigger.lua` (INFERRED).
- **Refreshes.** Changes made through the DLL usually mark the UI dirty themselves. If a banner or panel still looks stale, run UI12.
- **Human mode.** In autoplay or observer mode the active player is an observer slot. "Active player"
  recipes then act on the observer, not on a civ. Pass a real player ID, or first run
  `game_session.py human-mode on` and reload.

---

## Recipe index

| ID | Recipe | State |
|---|---|---|
| **Channel** | | |
| B1 | List the Lua state names | Main |
| B2 | Which API tables a state can see | any |
| B3 | Keep values between requests | any |
| B4 | Run several steps and report each one | any |
| **Game** | | |
| G1 | Where are we: turn, active player, observer, autoplay | Main |
| G2 | Game settings: speed, difficulty, eras, map size, VP options | Main |
| G3 | GameInfo lookups by Type, ID and filter; DB.Query | Main |
| G4 | List all players | Main |
| G5 | End the turn | Main |
| G6 | AI autoplay for N turns, then return as a player | Main |
| G7 | Toggle observer mode or AI takeover | Main |
| G8 | Change the turn counter | Main |
| G9 | Save the game | Main / InGame |
| G10 | Debug mode (show the whole map) | Main |
| **Units** | | |
| U1 | Spawn a unit of a type at a plot for a player | Main |
| U2 | Find units of a type | Main |
| U3 | Describe one unit | Main |
| U4 | Give or remove a promotion | Main |
| U5 | Set XP, level, promotion-ready | Main |
| U6 | Teleport a unit | Main |
| U7 | Order a move with pathfinding | Main / InGame |
| U8 | Heal or damage a unit | Main |
| U9 | Kill a unit, or clear a plot | Main |
| U10 | Embark or disembark | Main |
| U11 | Upgrade a unit for free | Main |
| U12 | Refill or use up a unit's moves | Main |
| U13 | Create a Great General or Admiral the real way | Main |
| **Cities** | | |
| C1 | Found (plop) a city at a plot | Main |
| C2 | Find a city by name | Main |
| C3 | Describe a city | Main |
| C4 | Add or remove a building | Main |
| C5 | Set population | Main |
| C6 | Add food, production, border culture; faith and instant yields | Main |
| C7 | Change the production target | Main |
| C8 | Finish current production now | Main |
| C9 | Capture or transfer a city | Main |
| C10 | Destroy a city | Main |
| C11 | Heal or damage a city | Main |
| C12 | Expand borders by one plot | Main |
| C13 | Convert a city fully to a religion | Main |
| **Players and teams** | | |
| P1 | Gold | Main |
| P2 | Faith | Main |
| P3 | Culture and free policies | Main |
| P4 | Golden age | Main |
| P5 | Grant or remove a tech; grant every tech up to an era | Main |
| P6 | Set current research and progress | Main |
| P7 | Adopt, grant or revoke a policy; unlock a branch | Main |
| P8 | Found a pantheon | Main |
| P9 | Found a religion | Main |
| P10 | Enhance a religion | Main |
| P11 | Great person points (specialist GPP, general and admiral points) | Main |
| P12 | Spawn a great person (plain, real threshold, or free choice) | Main |
| P13 | Meet, declare war, make peace | Main |
| P14 | Reveal the map for a team | Main |
| P15 | Give or take live vision (fog) | Main |
| P16 | Change a team's era | Main |
| P17 | City-state influence | Main |
| **Map and plots** | | |
| M1 | Describe a plot | Main |
| M2 | Set plot type (flat, hills, mountain, water) and terrain | Main |
| M3 | Set or remove a feature | Main |
| M4 | Set a resource with quantity, or remove it | Main |
| M5 | Set or remove an improvement | Main |
| M6 | Set or remove a route | Main |
| M7 | Pillage or repair an improvement and route | Main |
| M8 | Set or clear a plot's owner | Main |
| M9 | Plots within a radius; find a plot matching a test | Main |
| **UI** | | |
| UI1 | Move the camera to a plot | InGame |
| UI2 | Select a unit | InGame |
| UI3 | Open (and close) the city screen for a city | InGame |
| UI4 | Open the tech tree | Main / InGame |
| UI5 | Open the social policy screen | Main / InGame |
| UI6 | Open other overview screens | InGame |
| UI7 | Close a popup screen | InGame |
| UI8 | Toggle yield icons, resource icons, hex grid | Main / InGame |
| UI9 | Show a text popup or a top-of-screen alert | InGame |
| UI10 | Send a notification | Main |
| UI11 | Highlight hexes, and clear them | InGame |
| UI12 | Force a UI refresh after state edits | InGame |
| UI13 | What is selected, under the mouse, interface mode | InGame |
| UI14 | Reset the interface mode and selection; strategic view | InGame |
| UI15 | Open a Civilopedia entry | InGame |

---

## 0. Channel basics

### B1. List the Lua state names
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
return States()
```
- **Check:** you get a sorted list of names. `InGame` should be in it. Use these names for `--state` and `--@state=`.
- **Observed:** 150 unique names, `InGame`, `WorldView`, `TechTree`, `CityView`, `TextPopup`, `SocialPolicyPopup` all present; sorted byte-wise, so `CBP_IncaFunctions` sorts after `Automation Monitor` and the lower-case mod contexts `asar`, `autoplay`, `util` come last. Neither `Main` nor `Main State` is listed. FireTuner lists 159 (duplicates such as `ConfirmKick` x3 plus `Main State`).
- **Caveats:** `Main` is not in the list because it is the global state itself. Popup contexts appear before they are ever opened (`TextPopup`, `SocialPolicyPopup` were listed in a game where neither had been opened).

### B2. Which API tables a state can see
State: any (run it once in Main and once in InGame) · Status: FAILED as written in UI states, fixed and TESTED 2026-09-17
```lua
--@state=InGame
local getfenv, rawget = getfenv or G.getfenv, rawget or G.rawget   -- UI states have neither
local env = getfenv(1)
local names = { "Game", "Map", "Players", "Teams", "GameInfo", "GameInfoTypes", "GameDefines", "DB",
  "Locale", "UI", "Events", "LuaEvents", "OptionsManager", "UIManager", "ContextPtr", "Controls",
  "InStrategicView", "ToggleStrategicView", "ToHexFromGrid", "Vector2", "Vector4", "include" }
local out = {}
for _, n in ipairs(names) do
  out[n] = type(env[n]) .. ((rawget(G, n) ~= nil) and " (global)" or "")
end
return out
```
- **Check:** `Game`, `Players` and `GameInfo` are tables in both states. In InGame, `UI`, `Events` and `ContextPtr` are present.
- **Broken (original):** in InGame, WorldView and TechTree the first line failed with `attempt to call global 'getfenv' (a nil value)`, then `rawget` the same way. UI states have no `getfenv`, `setfenv`, `rawget`, `rawset`, `rawequal`, `load`, `loadfile`, `dofile`, `require`, `module`, `io` or `package`; reach them through `G`.
- **Observed (fixed):** Main: `Game`, `Map`, `Players`, `Teams`, `GameInfoTypes`, `Events`, `LuaEvents`, `include` tables/functions; `GameInfo`, `GameDefines`, `DB`, `Locale`, `UI`, `OptionsManager`, `ToHexFromGrid`, `InStrategicView`, `ToggleStrategicView` are real globals; `ContextPtr`, `Controls`, `UIManager`, `Vector2`, `Vector4` are nil. InGame, WorldView and TechTree: all 22 names present (`Vector2`/`Vector4` included). The raw `_G` (`--state _G`, FireTuner's `Main State`) has no `Game`, `Players`, `Teams`, `Map`, `GameInfoTypes`, `Events`.
- **Caveats:** This is the quickest way to find out whether a recipe can run in Main or needs a UI state.

### B3. Keep values between requests
State: any · Status: TESTED 2026-09-17
```lua
--@state=Main
vp.hits = (vp.hits or 0) + 1
return vp.hits
```
- **Check:** the result goes up by one on every run.
- **Observed:** `1`, `2` in Main, then `3` from `--state InGame`: one `vp` table is shared by all states.
- **Caveats:** Store IDs in `vp`, not objects (see Conventions). According to `vp_lua.py`, `vp` lasts for the game process. Whether it survives loading a different save is UNVERIFIED.

### B4. Run several steps and report each one
State: any · Status: TESTED 2026-09-17
```lua
--@state=Main
local report = {}
local function step(name, fn)
  local ok, res = pcall(fn)
  report[#report + 1] = name .. ": " .. (ok and ("ok " .. tostring(res)) or ("ERROR " .. tostring(res)))
end
step("turn", function() return Game.GetGameTurn() end)
step("bad player", function() return Players[9999]:GetName() end)
return report
```
- **Check:** one line per step. An error in one step doesn't hide the others.
- **Observed:** `{ "turn: ok 317", "bad player: ERROR luaexec:10: attempt to index field '?' (a nil value)" }`.
- **Caveats:** `pcall` catches only Lua errors, not C++ crashes (see Argument pitfalls).

---

## 1. Game

### G1. Where are we: turn, active player, observer, autoplay
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local pid = Game.GetActivePlayer()
local p = Players[pid]
return {
  turn = Game.GetGameTurn(), year = Game.GetGameTurnYear(), elapsed = Game.GetElapsedGameTurns(),
  activePlayer = pid, activeTeam = Game.GetActiveTeam(), name = p:GetName(),
  civ = p:GetCivilizationShortDescription(), isObserver = p:IsObserver(), isHuman = p:IsHuman(),
  turnActive = p:IsTurnActive(), observerUIOverride = Game.GetObserverUIOverridePlayer(),
  aiAutoPlay = Game.GetAIAutoPlay(), paused = Game.IsPaused(),
  endTurnBlocking = p:GetEndTurnBlockingType(),   -- compare with EndTurnBlockingTypes.*
}
```
- **Check:** the turn matches the top panel (the result header's `turn=` shows it too). `isObserver=true` means you are in autoplay or observer mode.
- **Observed:** `{ turn = 317, year = 1894, elapsed = 67, activePlayer = 2, activeTeam = 2, name = "Shaka", civ = "The Zulus", isObserver = false, isHuman = true, turnActive = true, observerUIOverride = -1, aiAutoPlay = 0, paused = false, endTurnBlocking = 2 }`; 2 is `ENDTURN_BLOCKING_PRODUCTION` (the screen showed CHOOSE PRODUCTION). Turn and year match the top panel.
- **Caveats:** none known.

### G2. Game settings: speed, difficulty, eras, map size, VP options
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local function T(tbl, id) local r = tbl[id]; return r and r.Type or tostring(id) end
local w, h = Map.GetGridSize()
return {
  speed = T(GameInfo.GameSpeeds, Game.GetGameSpeedType()),
  handicap = T(GameInfo.HandicapInfos, Game.GetHandicapType()),
  startEra = T(GameInfo.Eras, Game.GetStartEra()),
  gameEra = T(GameInfo.Eras, Game.GetCurrentEra()),
  worldSize = T(GameInfo.Worlds, Map.GetWorldSize()),
  grid = w .. "x" .. h, numPlots = Map.GetNumPlots(), maxTurns = Game.GetMaxTurns(),
  balanceVP = Game.GetCustomModOption("BALANCE_VP"),   -- any CustomModOptions Name
}
```
- **Check:** `speed` and `worldSize` match the game setup. `balanceVP` is 1 in the modpack DB.
- **Observed:** `{ speed = "GAMESPEED_STANDARD", handicap = "HANDICAP_DEITY", startEra = "ERA_INDUSTRIAL", gameEra = "ERA_POSTMODERN", worldSize = "WORLDSIZE_HUGE", grid = "128x80", numPlots = 10240, maxTurns = 250, balanceVP = 1 }`. `Game.GetCurrentEra()` (6, POSTMODERN) is not the human's era (`Players[2]:GetCurrentEra()` = 7, ERA_FUTURE). `maxTurns` counts from `Game.GetStartTurn()` (250), not from 0.
- **Caveats:** `Game.GetHandicapType()` is the game-level handicap, not necessarily the human's.

### G3. GameInfo lookups by Type, ID and filter; DB.Query
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local id  = assert(GameInfoTypes.UNIT_WARRIOR, "unknown type")  -- Type -> ID
local row = GameInfo.Units["UNIT_WARRIOR"]                        -- Type -> row
local back = GameInfo.Units[id].Type                              -- ID -> row
local melee = {}
for r in GameInfo.Units{ CombatClass = "UNITCOMBAT_MELEE" } do melee[#melee + 1] = r.Type end
local cheap = {}
for r in DB.Query("SELECT Type, Cost FROM Buildings WHERE Cost > 0 ORDER BY Cost LIMIT 5") do
  cheap[#cheap + 1] = r.Type .. "=" .. r.Cost
end
return id, row.Combat, row.Moves, back, #melee, cheap, Locale.ConvertTextKey(row.Description)
```
- **Check:** `back == "UNIT_WARRIOR"`, and the combat strength matches the Civilopedia.
- **Observed:** `83, 8, 2, "UNIT_WARRIOR", 30, { "BUILDING_HEROIC_EPIC=60", "BUILDING_PARTHENON=60", "BUILDING_SCRIVENERS_OFFICE=60", "BUILDING_FORNIX=60", "BUILDING_LONGHOUSE=65" }, "Warrior"`. `DB` and `Locale` exist in Main (B2).
- **Caveats:** `DB` and `Locale` are real globals, so this works in every state.

### G4. List all players
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local rows = {}
for i = 0, GameDefines.MAX_PLAYERS - 1 do
  local p = Players[i]
  if p and p:IsEverAlive() then
    rows[#rows + 1] = string.format("%d team=%d %s alive=%s human=%s minor=%s barb=%s cities=%d units=%d",
      i, p:GetTeam(), p:GetCivilizationShortDescription(), tostring(p:IsAlive()), tostring(p:IsHuman()),
      tostring(p:IsMinorCiv()), tostring(p:IsBarbarian()), p:GetNumCities(), p:GetNumUnits())
  end
end
return rows
```
- **Check:** majors come first, then city-states, then the barbarians (`GameDefines.BARBARIAN_PLAYER`).
- **Observed:** `"0 team=0 Carthage alive=true human=false minor=false barb=false cities=10 units=195"`, ..., `"2 team=2 The Zulus alive=true human=true ... cities=12 units=95"`, `"7 team=7 The Shoshone alive=false ..."` (ever alive, now dead), then city-states from ID 22 (`Tyre`).
- **Caveats:** none known.

### G5. End the turn
State: Main · Status: NOT RUN 2026-09-17 (the live test was not allowed to end the turn; SKILL.md records that `DoControl(CONTROL_ENDTURN/FORCEENDTURN)` from the channel did nothing in an earlier session)
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
vp.endTurnFrom = Game.GetGameTurn()
local blocking = p:GetEndTurnBlockingType()     -- EndTurnBlockingTypes.NO_ENDTURN_BLOCKING_TYPE is free to go
Game.DoControl(ControlTypes.CONTROL_ENDTURN)    -- CONTROL_FORCEENDTURN = Shift+Enter
return vp.endTurnFrom, blocking
```
- **Check:** a few seconds later, run `return Game.GetGameTurn(), vp.endTurnFrom`. The turn should be one higher.
- **Caveats:**
  - Human mode only.
  - `DoControl` silently does nothing when `CvGame::canDoControl` or `doControl` refuses. `CONTROL_ENDTURN` needs the UI's `canEndTurn`, all AI civs processed, and no focused text box.
  - `CONTROL_FORCEENDTURN` goes through only when the blocking type is none or `ENDTURN_BLOCKING_UNITS`. Settle production, research or policy choices first, or use the GUI (`civ_ui.py key "{ENTER}"`).
  - The idiom comes from ActionInfoPanel.lua (shipped and CP override). Calling it from inside `CvGame::update` is UNVERIFIED.

### G6. AI autoplay for N turns, then return as a player
State: Main · Status: NOT RUN 2026-09-17 (plays turns; not allowed in the live test)
```lua
--@state=Main
local turns = 5
local returnAs = Game.GetActivePlayer()      -- a player ID, or -1 to stay an observer afterwards
vp.humanSeat = returnAs
Game.SetAIAutoPlay(turns, returnAs)          -- ALWAYS pass the second argument
return Game.GetAIAutoPlay(), Game.GetActivePlayer()
```
To stop immediately and take a seat back: `Game.SetAIAutoPlay(0, vp.humanSeat)`.
- **Check:** `GetAIAutoPlay()` counts down each turn. The active player becomes an observer slot and, when the count reaches 0, switches back to `returnAs`.
- **Caveats:**
  - Without the second argument you return as player 0. The stock FireTuner integer control has this bug (see `firetuner.md` §7.2).
  - Starting autoplay needs a free or closed major slot for the observer. If there is none, it silently resets to 0 (`CvGame::setAIAutoPlay`).
  - Save the human's player ID before starting, because `GetActivePlayer()` changes.
  - The modpack's autoplay mod re-arms `SetAIAutoPlay(1,-1)` on every load unless human mode is on.

### G7. Toggle observer mode or AI takeover
State: Main · Status: NOT RUN 2026-09-17 (starts autoplay; not allowed in the live test)
```lua
--@state=Main
Game.DoControl(ControlTypes.CONTROL_TOGGLE_OBSERVER_MODE)   -- or ControlTypes.CONTROL_TOGGLE_AI_TAKEOVER
return Game.GetAIAutoPlay(), Game.GetObserverUIOverridePlayer(), Players[Game.GetActivePlayer()]:IsObserver()
```
- **Check:** run it again to toggle back, then check G1.
- **Caveats:**
  - Single-player only.
  - OBSERVER_MODE is ignored while an observer UI override is set.
  - AI_TAKEOVER pins the UI to your civ (`SetObserverUIOverridePlayer`) and autoplays for up to `MAX_TURNS_OBSERVER_MODE` turns, or effectively forever when that define is 0 or less.

### G8. Change the turn counter
State: Main · Status: NOT RUN 2026-09-17 (a set-and-restore variant was refused by the session's permission check)
```lua
--@state=Main
local old = Game.GetGameTurn()
Game.SetGameTurn(old + 10)
return old, Game.GetGameTurn()
```
- **Check:** the top panel turn changes.
- **Caveats:** This changes only the counter and the UI dirty bits (`CvGame::setGameTurn`). Nothing is simulated, so anything keyed by turn (deal durations, cooldowns, WLTKD, per-turn logs) can go wrong. Use a throwaway save.

### G9. Save the game
State: Main (quick save) or InGame (named) · Status: NOT RUN 2026-09-17 (saving was not allowed in the live test)
```lua
--@state=Main
Game.DoControl(ControlTypes.CONTROL_QUICK_SAVE)
return Game.GetGameTurn()
```
```lua
--@state=InGame
UI.SaveGame("vp_test_t" .. Game.GetGameTurn())
return "saving"
```
- **Check:** a new file appears in the saves folder. Where modpack games save is UNVERIFIED; look for the newest `.Civ5Save` under `My Games\Sid Meier's Civilization 5\Saves`.
- **Caveats:** Quick save overwrites the quicksave slot. Saving is asynchronous, so don't kill the game until the file stops growing. Killing it mid-save corrupts the save.

### G10. Debug mode (show the whole map)
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
Game.SetDebugMode(not Game.IsDebugMode())
return Game.IsDebugMode()
```
- **Check:** take a screenshot. The fog is gone for display.
- **Observed:** `false` -> `true` (0.33 s wall on the 128x80 map) -> `false`. On screen the visible change was the plot tooltip, which gained a debug line `n:4729 x:121 y:36 hex x:103 hex y:36`; the fog looked the same because this team already had all 10,240 plots revealed.
- **Caveats:**
  - This changes display only: `toggleDebugMode` updates visibility and dirty bits, but plots do not become revealed to the team. For a real reveal, use P14 or P15.
  - Other debug-gated UI changes too. For example, EUI's city banners let you click any city.
  - Toggle it off before taking screenshots that should look like normal play.

---

## 2. Units

To look up a unit saved by an earlier recipe: `local u = Players[vp.u.owner]:GetUnitByID(vp.u.id)`.
VP quirk: `GetUnitByID(n)` with `n < 1000` and no such ID falls back to treating `n` as a list index
(`CvLuaPlayer::lGetUnitByID`), so a stale small number can return a different unit. Real IDs are larger.
`GetCityByID` has the same fallback, and a `nil` ID reads as 0, which returns the player's *first* unit or city.

### U1. Spawn a unit of a type at a plot for a player
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local pid  = Game.GetActivePlayer()        -- any player ID
local x, y = 10, 12
local unitType = assert(GameInfoTypes.UNIT_WARRIOR, "unknown unit type")
local plot = assert(Map.GetPlot(x, y), "no such plot")
local u = Players[pid]:InitUnit(unitType, x, y)   -- optional: unitAI (GameInfoTypes.UNITAI_DEFENSE), DirectionTypes.*, bHistoric
-- u:JumpToNearestValidPlot()                     -- if the plot may be illegal for it
vp.u = { owner = pid, id = u:GetID() }
return vp.u, u:GetX(), u:GetY(), plot:GetNumUnits()
```
- **Check:** `Players[pid]:GetUnitByID(vp.u.id)` is not nil, the unit flag shows on a screenshot, and `plot:GetNumUnits()` went up.
- **Observed:** player 2, UNIT_WARRIOR at the empty hill 38,21 next to Ulundi: `{ id = 7850, owner = 2 }, 38, 21`, plot units 0 -> 1, player units 95 -> 96, name "Warrior", UnitAI 3. It arrived with the player's free promotions (Nationalism, Embarkation, Imperialism, Brute Force, BT movement; 3 moves). Re-run unmodified (verifier, same plot): `{ id = 7855, owner = 2 }, 38, 21, 1`, units 95 -> 96, UnitAI 3, 180 moves.
- **Caveats:**
  - `InitUnit` never returns nil (`CvPlayer::initUnit` always returns the new unit; an invalid type is a PRECONDITION crash, not nil), so the old `if not u` guard was dead code. Guard the type with `assert` instead.
  - No legality checks: a land unit on water, two combat units on one plot and a unit on an enemy plot are all possible. `JumpToNearestValidPlot()` fixes this afterwards.
  - Check the plot with `Map.GetPlot` first, because out-of-range coordinates reach C++.
  - This is the stock WorldView "unit plopper" idiom. The unit counts toward supply.

### U2. Find units of a type
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local unitType = assert(GameInfoTypes.UNIT_WARRIOR)
local onlyPlayer = nil                  -- a player ID, or nil for everyone
local rows = {}
for i = 0, GameDefines.MAX_PLAYERS - 1 do
  local p = Players[i]
  if p:IsAlive() and (onlyPlayer == nil or onlyPlayer == i) then
    for u in p:Units() do
      if u:GetUnitType() == unitType then
        rows[#rows + 1] = string.format("owner=%d id=%d at %d,%d hp=%d xp=%d lvl=%d",
          i, u:GetID(), u:GetX(), u:GetY(), u:GetCurrHitPoints(), u:GetExperience(), u:GetLevel())
      end
    end
  end
end
return #rows, rows
```
- **Check:** the count matches the military overview.
- **Observed:** with the U1 warrior (after U5/U4): `1, { "owner=2 id=7850 at 38,21 hp=100 xp=100 lvl=2" }` - the test warrior was the only UNIT_WARRIOR alive on the turn-317 map.
- **Caveats:** only 200 rows are rendered. Don't kill or upgrade inside the `Units()` loop; collect IDs first.

### U3. Describe one unit
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local owner, id = vp.u.owner, vp.u.id     -- or literal numbers
local u = Players[owner]:GetUnitByID(id)
if not u then return "no such unit" end
local promos = {}
for row in GameInfo.UnitPromotions() do
  if u:IsHasPromotion(row.ID) then promos[#promos + 1] = row.Type end
end
local D = GameDefines.MOVE_DENOMINATOR
return {
  type = GameInfo.Units[u:GetUnitType()].Type, name = u:GetName(), x = u:GetX(), y = u:GetY(),
  hp = u:GetCurrHitPoints(), maxHp = u:GetMaxHitPoints(), damage = u:GetDamage(),
  moves = u:MovesLeft() / D, maxMoves = u:MaxMoves() / D,
  xp = u:GetExperience(), level = u:GetLevel(), xpForNext = u:ExperienceNeeded(),
  promotionReady = u:IsPromotionReady(), embarked = u:IsEmbarked(), promotions = promos,
}
```
- **Check:** the values match the unit panel.
- **Observed:** `{ type = "UNIT_WARRIOR", name = "Warrior", x = 38, y = 21, hp = 100, maxHp = 100, damage = 0, moves = 3, maxMoves = 3, xp = 0, level = 1, xpForNext = 8, promotionReady = false, embarked = false, promotions = { "PROMOTION_SHOCK_1", "PROMOTION_NATIONALISM", "PROMOTION_EMBARKATION", "PROMOTION_IMPERIALISM", "PROMOTION_BRUTE_FORCE", "PROMOTION_BT_MOVEMENT" } }` (Shock I from U4). Looping `IsHasPromotion` over every `GameInfo.UnitPromotions()` row is safe (valid IDs only).
- **Caveats:** moves are stored in 60ths (`MOVE_DENOMINATOR` is 60 in the DB).

### U4. Give or remove a promotion
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id), "no such unit")
local promo = assert(GameInfoTypes.PROMOTION_SHOCK_1, "unknown promotion")
u:SetHasPromotion(promo, true)       -- false removes it
return u:IsHasPromotion(promo)
```
Or go through the real "choose a promotion" path, which checks prerequisites and uses up the promotion-ready state:
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id))
local promo = assert(GameInfoTypes.PROMOTION_SHOCK_1)
local can = u:CanPromote(promo, -1)
if can then u:Promote(promo, -1) end       -- Promote also raises the level by 1
return can, u:IsHasPromotion(promo), u:GetLevel()
```
- **Check:** the promotion icon appears on the unit panel (select the unit with UI2).
- **Observed:** first form: `PROMOTION_SHOCK_1` = 36, has before `false`, after `true`, level stays 1; `SetHasPromotion(promo, false)` -> `false`. Second form (after U5 made the unit promotion-ready): `CanPromote` `true`, has `true`, level 1 -> 2, still promotion-ready (100 XP is enough for the next level too), gold +0, **player culture +12** (VP level-up instant yield). Both forms re-run unmodified on a fresh warrior (verifier): identical results, culture and `GetJONSCultureEverGenerated()` +12, XP needed for level 3 = 23.
- **Caveats:**
  - `SetHasPromotion` skips most checks: prerequisites, CannotBeChosen and domain. It silently refuses blocked promotions, embarkation promotions for units that cannot embark, and weaker plague versions. It returns early for -1 and for IDs past the last promotion, but any other negative ID reaches `CvUnitPromotions::HasPromotion`'s PRECONDITION (crash); `IsHasPromotion` has no early return, so **`IsHasPromotion(-1)` crashes too**. Removing a promotion may not undo one-time effects.
  - `Promote` is the real path: it raises the level, heals a little (VP), fires the level-up instant yields for the owner (not undoable) and re-tests promotion-readiness from the XP (`testPromotionReady`).

### U5. Set XP, level, promotion-ready
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id))
u:SetExperience(100)                 -- whole XP; u:ChangeExperience(20) adds
-- u:SetLevel(4)
u:SetPromotionReady(u:GetExperience() >= u:ExperienceNeeded())
return u:GetExperience(), u:GetLevel(), u:ExperienceNeeded(), u:IsPromotionReady()
```
- **Check:** the XP bar in the unit panel, and the promotion button when ready.
- **Observed:** on the level-1 test warrior: `100, 1, 8, true` (XP, level unchanged, XP needed, promotion-ready). The unit flag then showed the promotion arrow.
- **Caveats:**
  - XP and level are separate counters. Setting XP does not change the level, and `Promote` adds a level (`CvUnit::promote`).
  - `ExperienceNeeded()` depends on the current level. IGE's hard-coded XP table is vanilla, not VP, so don't copy it.
  - AI units use up promotion-ready on their own turn.

### U6. Teleport a unit
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id))
local x, y = 11, 12
assert(Map.GetPlot(x, y), "no such plot")
u:SetXY(x, y)
-- u:JumpToNearestValidPlot()
return u:GetX(), u:GetY()
```
- **Check:** the position, and the flag in a screenshot.
- **Observed:** test warrior 38,21 -> empty hill 39,20: `38, 21, 39, 20` from an instrumented run (old position added); the snippet as written returns `39, 20` (re-run unmodified by the verifier). New plot units 1, old plot units 0, moves left unchanged (180 = 3 moves), not embarked. No movement cost.
- **Caveats:**
  - No movement cost, no zone of control and no legality check. This is the idiom IGE uses.
  - **Embark state is updated** (source and live, 2026-09-17): `CvUnit::setXY` sets the embarked flag when a unit that can embark lands on water and clears it on land. Moving the warrior onto water made `IsEmbarked()` true; back onto land, false.
  - Don't drop a unit onto a foreign city or an enemy unit.
  - Optional arguments `(x, y, bGroup, bUpdate, bShow, bCheckPlotVisible)` are read with `luaL_optint`, so pass 0 or 1 for them.

### U7. Order a move with pathfinding
State: Main (any unit) · Status: TESTED 2026-09-17 (both forms)
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id))
local x, y = 14, 12
u:PushMission(MissionTypes.MISSION_MOVE_TO, x, y)
return u:GetX(), u:GetY(), u:MovesLeft() / GameDefines.MOVE_DENOMINATOR
```
The human-UI path acts on the current selection. It is the network-safe message the shipped popups send:
```lua
--@state=InGame
local u = assert(Players[Game.GetActivePlayer()]:GetUnitByID(vp.u.id))
local msg = assert(GameMessageTypes.GAMEMESSAGE_PUSH_MISSION)   -- a nil here reads as 0, a message type that ASSERTs
UI.SelectUnit(u)
Game.SelectionListGameNetMessage(msg, assert(MissionTypes.MISSION_MOVE_TO), 14, 12, 0, false, false)
return "sent"
```
- **Check:** in the next request the position has changed, or the unit shows a path.
- **Observed:** first form, warrior at 39,20 -> 37,21 (distance 2): the returned position was already `37, 21` with 1.75 moves left - `PushMission` moved it **inside the same call**. Second form (spearman at 37,21, moves refilled, target 38,21 hill; the `assert` lines were added afterwards by the verifier, values checked live: `GAMEMESSAGE_PUSH_MISSION` = 35, `MISSION_MOVE_TO` = 0): the instrumented chunk returned `"sent", 37, 21, 180` (position and moves added); the next request read `38, 21, 60` - the net message is processed **after** the chunk returns.
- **Caveats:** `PushMission` moves right away (seen live); the net-message form is asynchronous and acts on whatever unit is selected, so select first (it needs the active player's unit, not busy). AI units re-plan on their turn and may ignore the order. The second form comes from `MinorCivEnterTerritoryPopup.lua`; `CvGame::selectionListGameNetMessage` only ASSERTs for message types other than DO_COMMAND / PUSH_MISSION / AUTO_MISSION / SWAP_UNITS.

### U8. Heal or damage a unit
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id))
u:SetDamage(0)              -- full heal; u:ChangeDamage(30) takes 30 HP
return u:GetCurrHitPoints(), u:GetMaxHitPoints()
```
- **Check:** the health bar.
- **Observed:** `ChangeDamage(30)` -> 70 HP; `SetDamage(0)` -> `100, 100, 0` (HP, max HP, damage).
- **Caveats:** `CvUnit::setDamage` clamps to 0 .. max HP; damage equal to max HP kills the unit (source, not run), after which the object is dead. Max HP can differ from `GameDefines.MAX_HIT_POINTS` (100) because of promotions. An omitted player argument is `NO_PLAYER` here (the binding checks `lua_isnil`), not player 0.

### U9. Kill a unit, or clear a plot
State: Main · Status: TESTED 2026-09-17 (both forms)
```lua
--@state=Main
local u = Players[vp.u.owner]:GetUnitByID(vp.u.id)
if not u then return "already gone" end
u:Kill(true, -1)            -- delayed death, as the stock WorldView de-plopper; (false, -1) = immediate
vp.u = nil
return "killed"
```
```lua
--@state=Main
local plot = assert(Map.GetPlot(10, 12))
local n = plot:GetNumUnits()
for i = n - 1, 0, -1 do
  local u = plot:GetUnit(i)
  if u then u:Kill(true, -1) end
end
return n
```
- **Check:** in the next request `GetUnitByID` returns nil and `plot:GetNumUnits()` is 0.
- **Observed:** second form on the city plot holding only the U13 Great General: `1, 1` (units before, units still counted right after `Kill(true, -1)`); the next request read 0 units and `GetUnitByID` nil. First form on the test spearman: `"killed"`; next request `GetUnitByID` nil, plot units 0, player units back to 95.
- **Caveats:** Never use the object after `Kill`. The second argument is the killer player (credit), and -1 means none. Delayed death finishes inside the game's update.

### U10. Embark or disembark
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id))
local x, y = 41, 22                  -- an empty water plot
local p = assert(Map.GetPlot(x, y))
assert(p:IsWater() and p:GetNumUnits() == 0, "target not empty water")
u:SetXY(x, y)                        -- U6 onto water: setXY already sets the embarked flag
local ok = u:Embark()                -- no argument: fires the embark graphics at the unit's own plot
return ok, u:IsEmbarked()
-- disembark: u:SetXY(landX, landY) clears the flag again
```
- **Check:** `IsEmbarked()`, and the boat model in a screenshot.
- **Observed:** warrior 37,21 -> water 41,22: embarked `false` before, `true` right after `SetXY`, `Embark()` returned `true`, still `true`. Screenshot `02c_crop.png`: the unit flag above embarked boat models. `SetXY` back to the land plot 37,21 -> `false`. Re-run unmodified on a fresh warrior (verifier, from 39,20): `true, true`, boat models under the flag, `SetXY(39, 20)` -> `false`.
- **Caveats:**
  - `CvUnit::embark(plot)` removes and re-adds sight **at the plot it is given**, not at the unit's plot. `u:Embark(otherPlot)` therefore corrupts the visibility counts of both places; call it without an argument.
  - It does not check that the plot is water or that the team can embark, so move the unit onto water first (U6).
  - `SetEmbarked(false)` only flips the flag (no sight update, no graphics). Moving the unit back onto land with `SetXY` is the clean disembark.
  - This is the idiom of the stock "Plop Unit Embarked" option.

### U11. Upgrade a unit for free
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id))
local nextType = u:GetUpgradeUnitType()
if nextType == -1 then return "no upgrade path" end      -- Upgrade() with no path reaches C++ with -1
local canNow = u:CanUpgradeRightNow()
local newUnit = u:Upgrade(true)                          -- or u:UpgradeTo(assert(GameInfoTypes.UNIT_PIKEMAN), true)
if not newUnit then return "upgrade failed", canNow end
vp.u = { owner = newUnit:GetOwner(), id = newUnit:GetID() }
return GameInfo.Units[newUnit:GetUnitType()].Type, vp.u, canNow
```
- **Check:** the new type and ID, and the XP and promotions carried over (U3).
- **Observed:** level-2 warrior with Shock I and 100 XP: `"UNIT_SPEARMAN", { id = 7852, owner = 2 }, true` (canNow); the new unit kept 100 XP, level 2 and Shock I; gold unchanged (99151); old ID 7850 returned nil afterwards. The spearman came out with 0 moves and **was selected** in the UI (`DoUpgradeTo` calls `selectUnit` for the active player's units). Re-run unmodified (verifier) on a fresh level-2 warrior that had been teleported but not moved: `"UNIT_SPEARMAN", { id = 7856, owner = 2 }, true`, XP 100, level 2, Shock I kept, gold unchanged, selected - but **180 moves (3)**, not 0.
- **Caveats:**
  - `DoUpgradeTo` does no validity check of its own; only `bFree=false` charges gold. `upgradePrice` ASSERTs on an invalid type, so never let -1 reach `Upgrade`/`UpgradeTo`.
  - The old object is dead afterwards and the new unit has a new ID.
  - **Moves after upgrading** (source, confirmed by the two runs): `CvUnit::convert` copies the old unit's moves and calls `finishMoves()` if the old unit had moved this turn; the new type's `MoveAfterUpgrade` column (true for UNIT_SPEARMAN) decides whether it may keep them. The first run's warrior had moved with U7, so 0; `SetXY` (U6) does not count as a move.
  - `CanUpgradeRightNow(bTestVisible)` reads its argument with `luaL_optint`, so pass 0 or 1, or nothing.

### U12. Refill or use up a unit's moves
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local u = assert(Players[vp.u.owner]:GetUnitByID(vp.u.id))
u:SetMoves(u:MaxMoves())        -- values in 60ths
-- u:FinishMoves()              -- zero them
return u:MovesLeft() / GameDefines.MOVE_DENOMINATOR
```
- **Check:** moves in the unit panel.
- **Observed:** `MOVE_DENOMINATOR` = 60; moves 3 -> `FinishMoves()` 0 -> `SetMoves(MaxMoves())` 3 (`MaxMoves()` = 180).
- **Caveats:** none known.

### U13. Create a Great General or Admiral the real way
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local pid = Game.GetActivePlayer()
local cap = assert(Players[pid]:GetCapitalCity(), "no capital")
Players[pid]:CreateGreatGeneral(assert(GameInfoTypes.UNIT_GREAT_GENERAL), cap:GetX(), cap:GetY(), 0)  -- last: bIsFree as 0/1
return Players[pid]:GetGreatGeneralsCreated(false)          -- (bExcludeFree), a BasicLuaMethod bool
```
- **Check:** a general appears at the capital, and the "created" count and threshold go up.
- **Observed:** player 2, at the test city's plot 78,32: generals created 5 -> 6, `GreatGeneralThreshold()` 1200 -> 1400, `GetGreatGeneralsThresholdModifier()` 500 -> 600 (+50 own, +50 again from the same-team loop), plot units 0 -> 1 (GG ID 7853), and a "Great Person" notification. No popup. **These counters are permanent** - there is no Lua setter to undo them; use a throwaway save.
- **Caveats:**
  - Unlike a plain `InitUnit`, `CvPlayer::createGreatGeneral` applies the spawn bonus, counts toward the next threshold, and moves the unit off water.
  - `bIsFree` is read with `luaL_optint`, so pass 0 or 1.
  - **Not for admirals** (source): `CvPlayer::createGreatGeneral` always bumps the *general* counters and threshold, whatever unit type it is given. The C++ `createGreatAdmiral` is reachable from Lua only as `pCity:CreateGreatAdmiral(UNIT_GREAT_ADMIRAL, bIsFree)` (a city method, not run).

---

## 3. Cities

### C1. Found (plop) a city at a plot
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local pid  = Game.GetActivePlayer()
local x, y = 20, 14
local plot = assert(Map.GetPlot(x, y), "no such plot")
if plot:IsCity() then return "already a city here" end   -- InitCity ASSERTs on an existing city
local legal = Players[pid]:CanFound(x, y)          -- normal founding rules, for information only
local city = Players[pid]:InitCity(x, y)           -- optional: bBumpUnits(0/1), bInitialFounding(0/1), religion
if not city then return "InitCity returned nil", legal end
vp.city = { owner = pid, id = city:GetID() }
return vp.city, city:GetName(), legal
```
- **Check:** the city banner appears, and `Players[pid]:GetCityByID(vp.city.id)` works.
- **Observed:** player 2 on the unowned one-tile plains island 78,32 (13 plots from the nearest city): `{ id = 7851, owner = 2 }, "Khangela", false`; cities 12 -> 13, population 3, plot owner 2, next policy cost 15465 -> 16126. Screenshot `01_ui1_city.png` (after UI1): banner KHANGELA, pop 3, borders drawn. Re-run unmodified on the same plot (verifier): `{ id = 7857, owner = 2 }, "Khangela", false`, the same counts and cost, banner and borders in a screenshot; the player's culture, lifetime culture and gold did not change at the founding.
- **Caveats:**
  - `InitCity` ignores distance, terrain and happiness rules. `CanFound` shows what the rules would say - but **`CanFound` is false on every plot while the empire is very unhappy** (`IsEmpireVeryUnhappy()`, true for this human): 0 of the 355 unowned revealed land plots passed. To find a site, filter plots yourself (land, not mountain, not `IsImpassable(team)`, unowned, no units, distance to the nearest city > 3, no adjacent owned plot).
  - The Lua binding recomputes the next policy cost; killing or giving the city away does not, so the cost stays too high until the next turn. `p:SetNumFreePolicies(p:GetNumFreePolicies())` forces the recompute without changing anything (restored 16126 -> 15465 live).
  - `Players[pid]:Found(x, y)` is the settler-style path (`foundCity`) if you want the real founding logic. It is UNTESTED here.
  - IGE fires `Events.SerialEventGameDataDirty()` afterwards (UI12).

### C2. Find a city by name
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local needle = string.lower("rome")
local hits = {}
for i = 0, GameDefines.MAX_PLAYERS - 1 do
  local p = Players[i]
  if p:IsAlive() then
    for c in p:Cities() do
      if string.find(string.lower(c:GetName()), needle, 1, true) then
        hits[#hits + 1] = { owner = i, id = c:GetID(), name = c:GetName(), x = c:GetX(), y = c:GetY() }
      end
    end
  end
end
if hits[1] then vp.city = { owner = hits[1].owner, id = hits[1].id } end
return hits
```
- **Check:** the name and coordinates match the banner.
- **Observed:** needle "khangela": `{ { owner = 2, id = 7851, name = "Khangela", x = 78, y = 32 } }`. `GetName()` returns the localized display name; `GetNameKey()` returns `"TXT_KEY_CITY_NAME_KHANGELA"`.
- **Caveats:** `GetName()` returned the localized name for a DB-named city (live). A player-renamed city may differ; `c:GetNameKey()` gives the TXT_KEY.

### C3. Describe a city
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id), "no such city")
local blds = {}
for b in GameInfo.Buildings() do
  local n = c:GetNumRealBuilding(b.ID)
  if n > 0 then blds[#blds + 1] = b.Type .. (n > 1 and ("x" .. n) or "") end
end
return {
  name = c:GetName(), owner = c:GetOwner(), x = c:GetX(), y = c:GetY(), pop = c:GetPopulation(),
  food = c:GetFood() .. "/" .. c:GrowthThreshold(), production = c:GetProduction() .. "/" .. c:GetProductionNeeded(),
  producing = c:GetProductionNameKey(), culture = c:GetJONSCultureStored() .. "/" .. c:GetJONSCultureThreshold(),
  hp = c:GetMaxHitPoints() - c:GetDamage(), capital = c:IsCapital(), puppet = c:IsPuppet(),
  religion = c:GetReligiousMajority(), buildings = blds,
}
```
- **Check:** the values match the city screen.
- **Observed:** fresh test city with the C4 monument: `{ name = "Khangela", owner = 2, x = 78, y = 32, pop = 3, food = "0/32", production = "0/2147483647", producing = "", culture = "0/20", hp = 274, capital = false, puppet = false, religion = -1, buildings = { "BUILDING_MONUMENT" } }`. With nothing in production `GetProductionNeeded()` is INT_MAX (2147483647).
- **Caveats:** `GetNumRealBuilding` doesn't count free buildings (`GetNumFreeBuilding`).

### C4. Add or remove a building
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
local b = assert(GameInfoTypes.BUILDING_MONUMENT, "unknown building")
local before = c:GetNumRealBuilding(b)
c:SetNumRealBuilding(b, 1)            -- 0 removes; 3rd arg bNoBonus defaults to TRUE
-- c:SetNumRealBuilding(b, 1, false)  -- also grant the first-construction instant yields / wonder historic event
return before, c:GetNumRealBuilding(b), c:IsHasBuilding(b)
```
- **Check:** the building appears in the city screen, and the yields change.
- **Observed:** `BUILDING_MONUMENT` on the test city: `0, 1, true`; `c:CanConstruct(b, 0, 1)` was then `false` (already built) and base culture per turn 9. `SetNumRealBuilding(b, 0)`: `0, false`, culture per turn back to 7. `SetNumRealBuilding` hands back its last argument (`0`), not a result. Re-run unmodified on a fresh test city (verifier): `0, 1, true`, `CanConstruct` false, culture per turn 9 with the monument and 7 after removing it, and the removal call returned exactly one value, `0`.
- **Caveats:**
  - No `CanConstruct` check: prerequisites, uniques and one-per-world wonders are all bypassed. `c:CanConstruct(b, 0, 1)` shows the rules (flags as 0/1).
  - With `bNoBonus` true (the Lua default), `CvCity::processBuilding` skips the construction instant yields and the wonder historic event.
  - IGE follows this with the CITY_UPDATE_TYPE_BANNER / PRODUCTION dirty events (UI12).

### C5. Set population
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
c:SetPopulation(10, true)            -- or c:ChangePopulation(2, true)
return c:GetPopulation()
```
- **Check:** the size on the banner, and the citizens reassigned.
- **Observed:** test city `SetPopulation(10, true)`: 3 -> 10, food box unchanged (0), HP 274 -> 330. `ChangePopulation(-1, true)`: 10 -> 9.
- **Caveats:** Keep the second argument `true` (or omit it: the binding treats a non-boolean as `true`). `false` triggers the binding's ASSERT, which freezes the game from this channel. Food in the box is unchanged. Lowering population runs `ASSERT(iUnassignedWorkers >= -iPopChange)` after removing citizens; small steps (`ChangePopulation(-1, true)`, the starvation path) are the well-trodden way down.

### C6. Add food, production, border culture; faith and instant yields
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
local p = Players[c:GetOwner()]
c:ChangeFood(50)
c:ChangeProduction(100)
c:ChangeJONSCultureStored(30)                       -- towards the next border plot
p:ChangeFaith(100)                                  -- faith is per player
p:DoInstantYield(YieldTypes.YIELD_GOLD, 200, false, c:GetID())   -- VP instant yield credited to this city
return c:GetFood(), c:GetProduction(), c:GetJONSCultureStored(), p:GetFaith(), p:GetGold()
```
- **Check:** the city screen and the top panel.
- **Observed:** test city with an empty production queue, before `{ food 0, production 0, culture 0, faith 0, gold 99151 }`, after `{ 50, 0, 30, 0, 99351 }`: food +50, **production unchanged** (it went to `GetOverflowProduction()` = 100), culture +30, faith +0 (religion is disabled in this game), gold +200 exactly. The 200 gold was taken back with `ChangeGold(-200)`. Re-run unmodified on a fresh test city (verifier): `50, 0, 30, 0, 99351`, `GetOverflowProduction()` 100, `GetProductionNeeded()` 2147483647.
- **Caveats:**
  - `ChangeFood` above the growth threshold grows the city at its next turn, not immediately (UNVERIFIED).
  - `ChangeProduction` with nothing in production goes into overflow production (`CvCity::changeProduction`), which is applied at turn processing; `GetProduction()` shows 0. Set a target first (C7) to see it.
  - `ChangeFaith` does nothing in a game with `GAMEOPTION_NO_RELIGION` (see P2).
  - `DoInstantYield(yield, amount, bSuppressNotification, cityID)` uses `INSTANT_YIELD_TYPE_LUA`. 200 gold arrived unscaled (live). It shows up in the player's instant-yield history (`GetInstantYieldHistoryTooltip(1)`).

### C7. Change the production target
State: Main · Status: FAILED as written 2026-09-17, fixed and TESTED 2026-09-17
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
local unit = assert(GameInfoTypes.UNIT_WORKER)
if not c:CanTrain(unit, 0, 0) then return "cannot train it here" end
-- args: order, data1, data2(-1 = default UnitAI), bSave(0/1), bPop(clear queue first), bAppend, bForce(0/1)
c:PushOrder(assert(OrderTypes.ORDER_TRAIN), unit, -1, 0, true, false, 0)
-- buildings: c:PushOrder(OrderTypes.ORDER_CONSTRUCT, GameInfoTypes.BUILDING_LIBRARY, -1, 0, true, false, 0)
-- process:   OrderTypes.ORDER_MAINTAIN with GameInfoTypes.PROCESS_WEALTH
return c:GetProductionNameKey(), c:GetOrderQueueLength()
```
- **Check:** `GetProductionNameKey()` is the new item, and the banner shows it.
- **Broken (original):** `c:PushOrder(OrderTypes.ORDER_TRAIN, unit, -1, false, true, false, false)` raised `bad argument #7 to 'PushOrder' (number expected, got boolean)`: the last argument (bForce) is read with `luaL_optint`. Nothing reached C++. The original target `UNIT_ARCHER` was also not trainable in this future-era city (`CanTrain` false).
- **Observed (fixed):** test city: `CanTrain(UNIT_ARCHER)` false, `CanTrain(UNIT_WORKER)` true, 21 unit types trainable; queue 0 -> `"TXT_KEY_UNIT_WORKER"`, length 1, production 0/26 (the C6 overflow 100 stays in `GetOverflowProduction()` until turn processing). Screenshot `16_ui3_cityscreen.png` shows "Worker, 1 turn". Re-run unmodified (verifier): `"TXT_KEY_UNIT_WORKER", 1`, production 0/26, overflow 100; the old boolean form inside `pcall` again gave "bad argument #7 to 'PushOrder' (number expected, got boolean)" with the queue unchanged.
- **Caveats:**
  - `CvCity::pushOrder` rejects items the city can't build (`canTrain`/`canConstruct`), leaving the queue empty after the pop. Check with `c:CanTrain(unit, 0, 0)` first, with flags as 0/1.
  - The 7th Lua argument is `bForce` (C++ `bRush`); bSave is read with `lua_tointeger` (a boolean reads as 0) and bForce with `luaL_optint` (a boolean is an error). Pass numbers for both.
  - The shipped UI uses `Game.CityPushOrder(city, order, id, bAlt, bShift, bCtrl)`, which works only on the owner's active turn.

### C8. Finish current production now
State: Main · Status: TESTED 2026-09-17 (value only: no turn was ended, so the completion was not seen)
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
if not c:IsProduction() then return "nothing in production" end
c:SetProduction(c:GetProductionNeeded())
return c:GetProductionNameKey(), c:GetProduction(), c:GetProductionNeeded()
```
- **Check:** the item completes when the city next processes production (end of turn), not instantly.
- **Observed:** worker order from C7: `"TXT_KEY_UNIT_WORKER", 26, 26`.
- **Caveats:** This is the idiom of IGE's "hurry production" button. For an instant building, use C4 instead.

### C9. Capture or transfer a city
State: Main · Status: TESTED 2026-09-17 (gift to a city-state)
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
local newOwner = 1
local x, y = c:GetX(), c:GetY()
Players[newOwner]:AcquireCity(c, false, true)     -- (city, bConquest, bGift); (c, true, false) = conquest
local nc = Map.GetPlot(x, y):GetPlotCity()        -- the old object is dead: re-find by plot
if not nc then return "city gone (razed?)" end
vp.city = { owner = nc:GetOwner(), id = nc:GetID() }
return vp.city, nc:GetName()
```
- **Check:** the banner colour, and `nc:GetOwner() == newOwner`.
- **Observed:** test city (pop 9, player 2) given to Warsaw (player 37, allied with player 2) with `AcquireCity(c, false, true)`: binding returned nothing (`#r = 0`), new `{ id = 7854, owner = 37 }`, name kept, pop 9, not puppet; player 2 cities 13 -> 12, Warsaw 1 -> 2. Screenshot `31_c9_crop.png`: city-state banner and dashed city-state border. A gifted city is annexed by a minor (`AI_conquerCity` -> `DoAnnex`); no popup for the human.
- **Caveats:**
  - `acquireCity` destroys the city and creates a new one with a new ID. The binding returns nothing.
  - Conquest can trigger a capture popup, razing or puppeting decisions for a human.
  - Transferring the last city of a player can kill that player.

### C10. Destroy a city
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
c:Kill()
vp.city = nil
return "killed"
```
- **Check:** the plot's `GetPlotCity()` is nil.
- **Observed:** the Warsaw-owned test city: `GetPlotCity()` nil at once, Warsaw cities 2 -> 1, plot owner -1, no units; the plot was left with `IMPROVEMENT_CITY_RUINS` and `ROUTE_RAILROAD` (removed afterwards with `SetImprovementType(-1, -1, false)` and `SetRouteType(-1, -1)`). No ghost banner (screenshot `33_after_kill.png`); the surrounding plots lost their owner. One screenshot taken 2 s after the kill caught a frame with the whole HUD missing; the next one was normal. Re-run unmodified by the verifier on a city **still owned by the human** (no gift first): same result - plot city nil, owner -1, no owned plot within 3, cities 13 -> 12, CITY_RUINS + RAILROAD left (removed the same way), no ghost banner; the next policy cost stayed 16126 until `SetNumFreePolicies(GetNumFreePolicies())` put it back to 15465. The order queue (a worker) was no problem: `PreKill` clears it before its `PRECONDITION(!isProduction())`.
- **Caveats:**
  - Units on the plot, the owner's capital status, and wonders are all affected. Killing a capital: UNVERIFIED.
  - IGE also fires `Events.SerialEventCityDestroyed(ToHexFromGrid(Vector2(x, y)), owner, id, -1)` in its UI state to clear the banner and art. Try that in InGame if a ghost banner stays.

### C11. Heal or damage a city
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
c:SetDamage(0)                        -- or c:ChangeDamage(100)
return c:GetDamage(), c:GetMaxHitPoints()
```
- **Check:** the banner's health bar.
- **Observed:** `0, 100, 0, 322` (damage before, after `ChangeDamage(100)`, after `SetDamage(0)`, max HP).
- **Caveats:** none known.

### C12. Expand borders by one plot
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
local before = c:GetJONSCultureLevel()
c:DoJONSCultureLevelIncrease()
return before, c:GetJONSCultureLevel()
```
- **Check:** a new plot is owned. IGE also refreshes fog on newly revealed plots (`plot:UpdateFog()`).
- **Observed:** `Game.GetCustomModOption("UI_CITY_EXPANSION")` = 0; culture level 0 -> 1, stored culture 30 -> 10 (the overflow above the threshold 20), owned plots within 5 of the city 7 -> 8 (acquired immediately).
- **Caveats:** This is the idiom of IGE's "expand borders" button. Which plot gets picked follows the normal border-growth choice. With the `UI_CITY_EXPANSION` mod option on, a human city instead gets a "choose a tile" notification and the level is raised only when the tile is picked (`CvCity::DoJONSCultureLevelIncrease`).

### C13. Convert a city fully to a religion
State: Main · Status: NOT RUN 2026-09-17 (religion is disabled in this game: `Game.IsOption(GameOptionTypes.GAMEOPTION_NO_RELIGION)` = true, and nobody has founded one)
```lua
--@state=Main
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
local religion = Players[c:GetOwner()]:GetReligionCreatedByPlayer()   -- or a GameInfoTypes.RELIGION_* ID
assert(religion and religion > 0, "no religion to adopt")
c:AdoptReligionFully(religion)
return c:GetReligiousMajority()
```
- **Check:** the religion icon on the banner, and the religion tooltip.
- **Caveats:** The religion must already be founded (P9). This is the idiom of IGE's "max followers" button.

---

## 4. Players and teams

### P1. Gold
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
p:ChangeGold(1000)            -- or p:SetGold(5000)
return p:GetGold()
```
- **Check:** the top panel.
- **Observed:** gold 99151 -> `100151`; the clean-up `p:ChangeGold(-1000)` returned `-1000` (its own argument, as the caveat says) and gold read 99151 again.
- **Caveats:** `SetGold` and `ChangeGold` return their argument, not a result (`return 1` with nothing pushed).

### P2. Faith
State: Main · Status: RAN, EFFECT NOT VERIFIED 2026-09-17 (ran without error, but faith cannot change in this game: religion is disabled, confirmed. That faith actually goes up in a religion game is still unproven)
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
p:ChangeFaith(500)            -- or p:SetFaith(1000)
return p:GetFaith()
```
- **Check:** the top panel.
- **Observed:** faith 0 -> `0` after `ChangeFaith(500)`. `CvPlayer::SetFaithTimes100` returns early when `GAMEOPTION_NO_RELIGION` is set, and this game's top panel shows no faith counter (EUI hides it under that option), so the option is the likely cause; the query meant to confirm it is the one that froze the game (Findings). Confirmed in the second session with a range-guarded check: `GameOptionTypes.GAMEOPTION_NO_RELIGION` = 21, `NUM_GAMEOPTION_TYPES` = 22, `Game.IsOption(21)` = `true`. P8-P10 and C13 cannot work in such a game either.
- **Caveats:** check the option by name or with the range-checked enum, never with `GameInfoTypes.GAMEOPTION_NO_RELIGION` (= 25, a DB row ID; `IsOption(25)` takes the hash-lookup path that ASSERTs). The name form has no assert path at all (`CvPreGame::GetGameOption(const char*)` compares names, then reads the `GameOptions` default); both returned `true` live:
  ```lua
  return Game.IsOption("GAMEOPTION_NO_RELIGION")
  ```
  ```lua
  local v, n = GameOptionTypes.GAMEOPTION_NO_RELIGION, GameOptionTypes.NUM_GAMEOPTION_TYPES
  return (v and n and v >= 0 and v < n) and Game.IsOption(v)
  ```

### P3. Culture and free policies
State: Main · Status: TESTED 2026-09-17 (the culture line; the free-policy line was NOT RUN because it cannot be undone, see Caveats)
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
p:ChangeJONSCulture(p:GetNextPolicyCost())     -- exactly one policy's worth; or p:SetJONSCulture(n)
p:ChangeNumFreePolicies(1)                     -- a free pick, no culture needed
return p:GetJONSCulture(), p:GetNextPolicyCost(), p:GetNumFreePolicies()
```
- **Check:** the culture meter. A free policy shows the choose-policy notification for a human.
- **Observed:** player 2: culture 14732, next policy cost 16126 (raised by the C1 test city); `ChangeJONSCulture(16126)` -> 30858, then `ChangeJONSCulture(-16126)` -> 14732. `GetJONSCultureEverGenerated()` stayed +16126 after the undo. The call hands back its argument (16126). End-turn blocking stayed 2 (production).
- **Caveats:**
  - Free tenets are separate: `ChangeNumFreeTenets(n)`.
  - Not undoable exactly (source, 2026-09-17): `SetNumFreePolicies` also raises the lifetime `NumFreePoliciesEver` counter, which discounts the next policy cost, and `ChangeNumFreePolicies(-1)` does not lower it; there is no Lua setter for it. A positive change also adds a "free policy" notification for a human.
  - Raising culture is not fully undoable either: `JONSCultureEverGenerated` keeps the increase (live). When culture reaches the cost on the human's turn, `CheckPolicy` adds the "enough culture for a policy" notification (a notification, not a popup).
  - `p:SetNumFreePolicies(p:GetNumFreePolicies())` changes nothing but recomputes the next policy cost (useful after C1/C9/C10).

### P4. Golden age
State: Main · Status: NOT RUN 2026-09-17 for `ChangeGoldenAgeTurns` (cannot be undone, see Caveats); the meter line was TESTED
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
p:ChangeGoldenAgeTurns(10)                                    -- starts one if none is running
-- p:ChangeGoldenAgeProgressMeter(p:GetGoldenAgeProgressThreshold())  -- fill the meter instead
return p:IsGoldenAge(), p:GetGoldenAgeTurns(), p:GetGoldenAgeProgressMeter(), p:GetGoldenAgeProgressThreshold()
```
- **Check:** the golden age indicator.
- **Observed (meter only):** player 2 already in a golden age (6 turns): `GetGoldenAgeProgressMeterTimes100()` 52700 -> `ChangeGoldenAgeProgressMeter(100)` 62700 -> `(-100)` 52700; the meter moves even during a golden age in this game. `GetGoldenAgeLength(10)` = 10, threshold 7260.
- **Caveats:**
  - The binding passes the turns through `getGoldenAgeLength(turns)`, which applies game-speed and player modifiers. The actual turn count can differ from 10, so read it back.
  - **A negative count does not shorten it** (source, 2026-09-17): `CvGame::goldenAgeLength` replaces any negative manual length with `GOLDEN_AGE_LENGTH` (10), so `ChangeGoldenAgeTurns(-10)` *adds* a default-length golden age. There is no `SetGoldenAgeTurns` binding, so a test extension cannot be undone.
  - A second optional argument, `bFree`, is read with `luaL_optbool`.
  - The meter is undoable: `ChangeGoldenAgeProgressMeter(-n)`, or `SetGoldenAgeProgressMeter(n)` (whole points only, so it drops the hundredths). `ChangeGoldenAgeProgressMeter` is ignored under `GAMEOPTION_NO_HAPPINESS`, and during a golden age when `BALANCE_NO_GAP_DURING_GA` is on.
  - A full meter pops the golden age at turn processing (UNVERIFIED).

### P5. Grant or remove a tech; grant every tech up to an era
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local pid = Game.GetActivePlayer()
local team = Teams[Players[pid]:GetTeam()]
local tech = assert(GameInfoTypes.TECH_BRONZE_WORKING, "unknown tech")
team:SetHasTech(tech, true, pid, false, false)     -- (tech, bNewValue, player credited, bFirst, bAnnounce)
return team:IsHasTech(tech)
```
Every tech up to and including an era (the idiom of the FireTuner "Active Player" panel, with the VP argument fix):
```lua
--@state=Main
local pid = Game.GetActivePlayer()
local team = Teams[Players[pid]:GetTeam()]
local maxEra = assert(GameInfoTypes.ERA_CLASSICAL)
local n = 0
for t in GameInfo.Technologies() do
  if GameInfoTypes[t.Era] <= maxEra and not team:IsHasTech(t.ID) then
    team:SetHasTech(t.ID, true, pid, false, false)
    n = n + 1
  end
end
return n
```
- **Check:** the tech tree (UI4) and the newly unlocked build items.
- **Observed:** team 2 lacked 12 techs. `SetHasTech(TECH_ECOLOGY, true, 2, false, false)` -> `IsHasTech` true (era, science rate and current research unchanged); `SetHasTech(TECH_ECOLOGY, false, 2, false, false)` -> false and `CanResearch` true again; the tech tree showed Alternative Energy unresearched afterwards. **Despite bAnnounce = false, a modal "You Have Researched A New Technology! Alternative Energy" popup opened** (screenshot `13_p15_before_half.png`); it was closed with UI7 (`TechAwardPopup`). The era-loop form returned `0` (every Classical tech already known).
- **Caveats:**
  - Removing: `SetHasTech(tech, false, pid, false, false)` (ran cleanly live). Unlocked items already in queues or on the map are not taken back (UNVERIFIED).
  - Granting to the active team opens the BUTTONPOPUP_TECH_AWARD popup unless the player's "no reward popups" option is set (`CvTeam::setHasTech`; bAnnounce does not control it), and runs `DoDifficultyBonus(RESEARCHED_TECH)` and tech instant yields. Close the popup with UI7 using `TechAwardPopup` (its Continue button is exactly `UIManager:DequeuePopup`).
  - A player of -1 is replaced by the team leader; an omitted player is 0 (player 0).
  - `pTeam:GetTeamTechs():SetHasTech(id, b)` flips only the bit and applies no tech effects, so don't use it for tests.
  - Use a throwaway save for removals.

### P6. Set current research and progress
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local pid = Game.GetActivePlayer()
local p = Players[pid]
local tech = assert(GameInfoTypes.TECH_WRITING)
if not p:CanResearch(tech) then return "cannot research now (prereqs?)" end
p:PushResearch(tech, true)                               -- true = clear the queue first
local tt = Teams[p:GetTeam()]:GetTeamTechs()
tt:ChangeResearchProgress(tech, 50, pid)                 -- pass the player!
return p:GetCurrentResearch(), tt:GetResearchProgress(tech), p:GetResearchTurnsLeft(tech, true)
```
- **Check:** the research meter on the top panel.
- **Observed:** player 2 researching TECH_SATELLITES (71, progress 6131, queue length 1). `CanResearch(TECH_ECOLOGY)` true; `PushResearch(tech, true)` -> `true`, current research 64, progress 0 -> 50, turns left 6, queue length 1. Undone with `ChangeResearchProgress(tech, -50, 2)` and `PushResearch(71, true)`: research 71, its progress still 6131, Ecology 0.
- **Caveats:**
  - Progress at or above the cost completes the tech at the next research step (UNVERIFIED).
  - **Never pass -1 as the player** to `ChangeResearchProgress`/`SetResearchProgress`: `CvTeamTechs::SetResearchProgressTimes100` has `PRECONDITION(ePlayer >= 0)`, a crash.
  - `PushResearch(tech, true)` replaces the whole queue; note `GetCurrentResearch()` first to put it back. Per-tech progress is kept when the target changes.
  - `GetResearchTurnsLeft(tech, bOverflow)` reads its second argument with `lua_toboolean`, so pass true/false. It controls whether overflow research counts.

### P7. Adopt, grant or revoke a policy; unlock a branch
State: Main · Status: NOT RUN 2026-09-17 (adopting or granting a policy on the live human player cannot be undone: `RevokePolicy` may not reverse one-time effects and the free-policy counter is permanent, see P3)
The legitimate way: pay with a free policy, which runs the full `doAdoptPolicy` logic, including unlocking the branch.
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
local policy = assert(GameInfoTypes.POLICY_TRADITION, "unknown policy")
p:ChangeNumFreePolicies(1)
local can = p:CanAdoptPolicy(policy, true)     -- (policy, bIgnoreCost)
if can then p:DoAdoptPolicy(policy) end
return can, p:HasPolicy(policy), p:GetNumFreePolicies()
```
Forcing it without the rules:
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
local branch = assert(GameInfoTypes.POLICY_BRANCH_LIBERTY)
p:SetPolicyBranchUnlocked(branch, true, false)                       -- (branch, bUnlocked, bRevolution)
p:GrantPolicy(assert(GameInfoTypes[GameInfo.PolicyBranchTypes[branch].FreePolicy]), true)   -- the opener, free
-- p:RevokePolicy(GameInfoTypes.POLICY_LIBERTY)
return p:IsPolicyBranchUnlocked(branch), p:HasPolicy(GameInfoTypes.POLICY_LIBERTY)
```
- **Check:** the social policy screen (UI5).
- **Observed (read-only part):** looping `HasPolicy` / `CanAdoptPolicy(id, true)` over every `GameInfo.Policies()` row was safe; player 2 could adopt 5 (ideology tenets such as POLICY_HERO_OF_THE_PEOPLE, POLICY_SOCIALIST_REALISM).
- **Caveats:**
  - `DoAdoptPolicy` silently does nothing if `canAdoptPolicy` fails: prerequisites, a blocked branch or the era requirement. Without free policies it spends culture.
  - Branch openers are `PolicyBranchTypes.FreePolicy` (for example POLICY_LIBERTY). Ideology branches have no opener in the DB.
  - `GrantPolicy`, `RevokePolicy` and `SetHasPolicy` skip the rules. Revoking may not undo one-time effects.

### P8. Found a pantheon
State: Main · Status: NOT RUN 2026-09-17 (religion is disabled in this game, see P2)
```lua
--@state=Main
local pid = Game.GetActivePlayer()
local p = Players[pid]
if p:HasCreatedPantheon() then return "already has one", p:GetBeliefInPantheon() end
local belief = Game.GetAvailablePantheonBeliefs(pid)[1]     -- or GameInfoTypes.BELIEF_GODDESS_HUNT
assert(belief, "no pantheon belief available")
Game.FoundPantheon(pid, belief)
return p:HasCreatedPantheon(), GameInfo.Beliefs[p:GetBeliefInPantheon()].Type
```
- **Check:** the religion overview (UI6), and the pantheon icon on cities.
- **Caveats:**
  - `CvGameReligions::FoundPantheon` does not check for an existing pantheon, so calling it twice adds a second one. Keep the guard.
  - No faith is spent.
  - The UI path is `Network.SendFoundPantheon` (`IGE_ChoosePantheonPopup.lua` uses `Game.FoundPantheon`).

### P9. Found a religion
State: Main · Status: NOT RUN 2026-09-17 (religion is disabled in this game, see P2)
```lua
--@state=Main
local pid = Game.GetActivePlayer()
local p = Players[pid]
if p:HasCreatedReligion() then return "already founded", p:GetReligionCreatedByPlayer() end
local holy = assert(p:GetCapitalCity(), "needs a holy city (nil crashes)")
local taken = {}
for i = 0, GameDefines.MAX_CIV_PLAYERS - 1 do
  local o = Players[i]
  if o:IsEverAlive() and o:HasCreatedReligion() then taken[o:GetReligionCreatedByPlayer()] = true end
end
local religion
for r in GameInfo.Religions() do
  if r.Type ~= "RELIGION_PANTHEON" and not taken[r.ID] then religion = r.ID; break end
end
assert(religion, "no religion left to found")
local founder  = assert(Game.GetAvailableFounderBeliefs(pid, religion)[1], "no founder belief")
local follower = assert(Game.GetAvailableFollowerBeliefs(pid, religion)[1], "no follower belief")
local pantheon = -1
if not p:HasCreatedPantheon() then pantheon = Game.GetAvailablePantheonBeliefs(pid)[1] or -1 end
local bonus = -1          -- VP bonus slot for civs with p:IsTraitBonusReligiousBelief(): Game.GetAvailableBonusBeliefs(pid, religion)[1]
-- argument order as VP's ChooseReligionPopup: founder, follower, pantheon-if-none, bonus
Game.FoundReligion(pid, religion, nil, founder, follower, pantheon, bonus, holy)
return GameInfo.Religions[p:GetReligionCreatedByPlayer()].Type, holy:GetName(), Game.GetBeliefsInReligion(religion)
```
- **Check:** the religion overview and the holy city icon.
- **Caveats:**
  - No faith is spent and no prophet is used up.
  - `nil` for the custom name keeps the default name.
  - Belief IDs are read with `luaL_checkint`, so pass -1 for an empty slot, never nil.
  - Some religions may be restricted (`LocalReligion`, civ-specific). The first free row might not be a valid choice (UNVERIFIED).

### P10. Enhance a religion
State: Main · Status: NOT RUN 2026-09-17 (religion is disabled in this game, see P2)
```lua
--@state=Main
local pid = Game.GetActivePlayer()
local p = Players[pid]
local religion = p:GetReligionCreatedByPlayer()
assert(religion and religion > 0, "found a religion first (P9)")
if p:HasEnhancedReligion() then return "already enhanced" end
local follower2 = assert(Game.GetAvailableFollowerBeliefs(pid, religion)[1])
local enhancer  = assert(Game.GetAvailableEnhancerBeliefs(pid, religion)[1])
Game.EnhanceReligion(pid, religion, follower2, enhancer, true)   -- last: bNotify
return p:HasEnhancedReligion(), Game.GetBeliefsInReligion(religion)
```
- **Check:** the religion overview lists 2 more beliefs.
- **Caveats:** The argument order follows VP's popup (`Network.SendEnhanceReligion(..., FOLLOWER2, ENHANCER)`).

### P11. Great person points (specialist GPP, general and admiral points)
State: Main · Status: TESTED 2026-09-17 (run on the test city instead of the capital, with a spawn guard and an undo)
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
local c = assert(p:GetCapitalCity())
local spec = assert(GameInfoTypes.SPECIALIST_SCIENTIST)
local class = assert(GameInfoTypes[GameInfo.Specialists[spec].GreatPeopleUnitClass])
local n = 50
-- spawn guards: reaching a threshold spawns a great person at once (permanent, and a modal popup for a human)
assert(c:GetSpecialistGreatPersonProgress(spec) + n < c:GetSpecialistUpgradeThreshold(class), "would spawn a great person")
assert(p:GetCombatExperienceTimes100() + 100 * n < 100 * p:GreatGeneralThreshold(), "would spawn a Great General")
assert(p:GetNavalCombatExperienceTimes100() + 100 * n < 100 * p:GreatAdmiralThreshold(), "would spawn a Great Admiral")
local before = c:GetSpecialistGreatPersonProgress(spec)
c:ChangeSpecialistGreatPersonProgressTimes100(spec, 100 * n)    -- +50 GPP
p:ChangeCombatExperience(n)                                       -- Great General points
p:ChangeNavalCombatExperience(n)                                  -- Great Admiral points
return before, c:GetSpecialistGreatPersonProgress(spec), p:GetCombatExperience(), p:GreatGeneralThreshold()
```
- **Check:** the great people panel in the city screen and the GP list.
- **Observed:** test city, scientist threshold 320, progress 0 -> 50; player 2 combat XP (Times100) 30964 -> 35964, naval 3990 -> 8990 (thresholds 1400 and 200, so nothing spawned). The same calls with negative amounts put all three back exactly (0, 30964, 3990). The verifier then added the three `assert` guard lines and ran the snippet exactly as written on the capital (Ulundi): `141, 191, 359, 1400` (scientist progress before and after, general points, threshold), no unit spawned; undone with the same calls at -50 (141, 30964, 3990).
- **Caveats:** The Lua binding calls the C++ with `bCheckForSpawn=true`. **If progress reaches the threshold, the great person spawns immediately** (see P12). General and admiral points spawn through their own threshold logic (`setCombatExperienceTimes100`: `>= threshold * 100`). Keep the guards. Negative changes undo them: `changeCombatExperienceTimes100` clamps at 0, but a *positive* change is first scaled by the state religion's Great General rate belief and a negative one is not, so the undo is exact only without such a belief. `SetCombatExperience(n)` / `SetNavalCombatExperience(n)` also exist; a negative `n` hits `ASSERT(iExperienceTimes100 >= 0)`.

### P12. Spawn a great person (plain, real threshold, or free choice)
State: Main · Status: NOT RUN 2026-09-17 (for the active player both the threshold path and the free-choice path open a modal popup that needs a click, and the great-person counters cannot be undone)
Through the real threshold path, which gives a proper GP birth with counters and cost increase:
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
local c = assert(p:GetCapitalCity())
local spec = assert(GameInfoTypes.SPECIALIST_ENGINEER)
local class = assert(GameInfoTypes[GameInfo.Specialists[spec].GreatPeopleUnitClass])
local need = c:GetSpecialistUpgradeThreshold(class) - c:GetSpecialistGreatPersonProgress(spec)
local before = p:GetNumUnits()
c:ChangeSpecialistGreatPersonProgressTimes100(spec, 100 * math.max(need, 1))
return need, p:GetNumUnits() - before
```
Other ways:
- A plain spawn without GP bookkeeping: `Players[pid]:InitUnit(GameInfoTypes.UNIT_SCIENTIST, x, y)` (U1).
- A free choice popup for a human: `Players[pid]:ChangeNumFreeGreatPeople(1)`.
- Generals and admirals: U13.
- **Check:** a new GP unit next to the city, and the unit count delta is 1.
- **Caveats:**
  - Minor civs never spawn this way.
  - For the active player, `CvCityCitizens::DoSpawnGreatPerson` adds a `BUTTONPOPUP_GREAT_PERSON_REWARD` popup (modal) plus a notification, and `incrementGreatPersonCount` raises the next threshold for good (source, 2026-09-17). Close the popup through its own context (UI7 pattern) or test on an AI player.
  - `SPECIALIST_CIVIL_SERVANT` maps to `UNITCLASS_GREAT_DIPLOMAT`.
  - A plain-spawned prophet has no religion.

### P13. Meet, declare war, make peace
State: Main · Status: NOT RUN 2026-09-17 (a war declaration between live civs cannot be undone: diplomatic memory, defensive pacts and the peace-treaty lock stay; use a throwaway save)
```lua
--@state=Main
local a, b = 0, 1                                   -- player IDs
local ta, tb = Players[a]:GetTeam(), Players[b]:GetTeam()
local teamA = Teams[ta]
if not teamA:IsHasMet(tb) then teamA:Meet(tb, true) end      -- true = suppress messages
teamA:DeclareWar(tb, false, a)                      -- (team, bDefensivePact, originatingPlayer)
-- teamA:MakePeace(tb, true, false, a)              -- (team, bBumpUnits, bSuppressNotification, originatingPlayer)
return teamA:IsHasMet(tb), teamA:IsAtWar(tb), Players[a]:IsAtWarWith(b)
```
- **Check:** the diplomacy list shows war or peace.
- **Caveats:**
  - These calls bypass `CanDeclareWar`, peace treaties and the diplomacy AI. A defensive pact may drag in others (`DoDeclareWar`).
  - War on a vassal is redirected to its master.
  - Always pass the originating player (see Argument pitfalls).

### P14. Reveal the map for a team
State: Main · Status: RAN, EFFECT NOT VERIFIED 2026-09-17 (ran without error or assert, but a no-op in this save: every plot was already revealed)
```lua
--@state=Main
local team = Game.GetActiveTeam()
for i = 0, Map.GetNumPlots() - 1 do
  local plot = Map.GetPlotByIndex(i)
  plot:SetRevealed(team, true)
end
Map.UpdateDeferredFog()
return Map.GetNumPlots()
```
- **Check:** the whole map is drawn (still under fog). Take a screenshot.
- **Observed:** team 2 had all 10240 plots revealed before and after: `10240` returned, counted revealed 10240 -> 10240, no error or assert from 10240 `SetRevealed` calls plus `Map.UpdateDeferredFog()`. The visible effect could not be checked in this save.
- **Caveats:**
  - Revealed does not mean visible: units and cities outside vision are still hidden (see P15).
  - This is the "Reveal Terrain" action of the FireTuner Map panel.
  - IGE adds `plot:UpdateFog()` per plot and `UI:RequestMinimapBroadcast()` (InGame) if the minimap lags.

### P15. Give or take live vision (fog)
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local team = Game.GetActiveTeam()
local cx, cy, radius = 20, 14, 4           -- radius = -1 means the whole map
vp.vision = vp.vision or {}
for i = 0, Map.GetNumPlots() - 1 do
  local plot = Map.GetPlotByIndex(i)
  if radius < 0 or Map.PlotDistance(cx, cy, plot:GetX(), plot:GetY()) <= radius then
    plot:ChangeVisibilityCount(team, 1, -1, true, true)   -- (team, change, invisibleType, bInformExploration, bAlwaysSeeInvisible)
    plot:SetRevealed(team, true)
    vp.vision[#vp.vision + 1] = i
  end
end
return #vp.vision
```
To undo, take the same plots back with -1:
```lua
--@state=Main
local team = Game.GetActiveTeam()
for _, i in ipairs(vp.vision or {}) do Map.GetPlotByIndex(i):ChangeVisibilityCount(team, -1, -1, true, true) end
local n = #(vp.vision or {}); vp.vision = nil
return n
```
- **Check:** enemy units become visible inside the area; `plot:IsVisible(team)`.
- **Observed:** centre Carthage 108,56 (not visible to team 2), radius 2: `19` plots, `IsVisible(2)` true, 2 units on the city plot. Screenshot `15_p15_vision.png`: the fog over Carthage lifted and its unit flags appeared (compare `14_ui7_closed.png`). The undo returned `19` but `IsVisible(2)` stayed **true**: see Caveats.
- **Caveats:**
  - Visibility is a reference count. Always undo exactly what you added, or the plots stay visible for good.
  - **Delayed visibility** (live and source, 2026-09-17): with the `CORE_DELAYED_VISIBILITY` mod option on (it is, value 1), `IsVisible` and `GetVisibilityCount` report the highest count reached **this turn** (`m_aiVisibilityCountThisTurnMax`), so plots stay visible until the next turn even after a correct undo. The count itself never goes below 0.
  - This is the idiom of VP's InGame.lua debug key and the FireTuner "Reveal/Refresh Map" action.

### P16. Change a team's era
State: Main · Status: NOT RUN 2026-09-17 (era-change effects on the live human team cannot be undone, and moving an era backwards is unverified)
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
local team = Teams[p:GetTeam()]
team:SetCurrentEra(assert(GameInfoTypes.ERA_MEDIEVAL))
return GameInfo.Eras[p:GetCurrentEra()].Type
```
- **Check:** the era shown in tooltips and the era-dependent costs.
- **Caveats:**
  - `CvTeam::SetCurrentEra` runs the era-change effects (era-based bonuses, possibly the "first to reach the era" handling) but grants no techs. The tech tree and the era now disagree.
  - Moving the era backwards: UNVERIFIED.

### P17. City-state influence
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local major, minor = Game.GetActivePlayer(), 30        -- minor = a city-state player ID (see G4)
assert(major >= 0 and major < GameDefines.MAX_MAJOR_CIVS, "not a major civ")   -- the getter crashes otherwise
assert(Players[minor] and Players[minor]:IsMinorCiv(), "not a city-state")
Players[minor]:ChangeMinorCivFriendshipWithMajor(major, 60)
return Players[minor]:GetMinorCivFriendshipWithMajor(major)
```
- **Check:** the city-state diplomacy popup, and the ally or friend status.
- **Observed:** Warsaw (player 37, ally 2, influence 285): +60 -> 345, ally still 2; -60 -> 285. The change call hands back its last argument (60). Re-run by the verifier with `minor = 37`, first as the tester left it and again after adding the two guard lines (`GameDefines.MAX_MAJOR_CIVS` = 22 live): `345` both times, undone with `-60` -> 285, ally still 2.
- **Caveats:** Majors must be `0 <= id < MAX_MAJOR_CIVS`: the change call ignores other IDs, but **the getter `GetMinorCivFriendshipWithMajor` does not check** and reaches a PRECONDITION (crash). The getter returns the *effective* influence. Pick a city-state where +/- the amount does not cross the friend/ally thresholds if you need to undo cleanly. This is the idiom of IGE's "make ally" button.

---

## 5. Map and plots

### M1. Describe a plot
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local x, y = 20, 14
local plot = assert(Map.GetPlot(x, y), "no such plot")
local team = Game.GetActiveTeam()
local function T(tbl, id) local r = (id and id >= 0) and tbl[id] or nil; return r and r.Type or "none" end
local owner, owningCityID = plot:GetOwner()          -- VP returns two values
local units = {}
for i = 0, plot:GetNumUnits() - 1 do
  local u = plot:GetUnit(i)
  if u then units[#units + 1] = u:GetOwner() .. ":" .. GameInfo.Units[u:GetUnitType()].Type .. "#" .. u:GetID() end
end
local city = plot:GetPlotCity()
return {
  index = plot:GetPlotIndex(), plotType = plot:GetPlotType(), terrain = T(GameInfo.Terrains, plot:GetTerrainType()),
  feature = T(GameInfo.Features, plot:GetFeatureType()),
  resource = T(GameInfo.Resources, plot:GetResourceType()), resourceQty = plot:GetNumResource(),
  improvement = T(GameInfo.Improvements, plot:GetImprovementType()), impPillaged = plot:IsImprovementPillaged(),
  route = T(GameInfo.Routes, plot:GetRouteType()), routePillaged = plot:IsRoutePillaged(),
  owner = owner, owningCityID = owningCityID, city = city and city:GetName() or nil,
  water = plot:IsWater(), hills = plot:IsHills(), mountain = plot:IsMountain(), river = plot:IsRiver(),
  revealed = plot:IsRevealed(team), visible = plot:IsVisible(team), units = units,
}
```
- **Check:** compare with the plot tooltip (mouse over it with `civ_ui.py move`).
- **Observed:** the unowned test plot 119,33 (before M2-M7 and again after restoring it, identical): `{ index = 4343, plotType = 2, terrain = "TERRAIN_GRASS", feature = "none", resource = "none", resourceQty = 0, improvement = "none", impPillaged = false, route = "none", routePillaged = false, owner = -1, owningCityID = -1, water = false, hills = false, mountain = false, river = false, revealed = true, visible = true, units = {} }`.
- **Caveats:** `PlotTypes`: 0 = PLOT_MOUNTAIN, 1 = PLOT_HILLS, 2 = PLOT_LAND, 3 = PLOT_OCEAN (from `CvGameCoreDLLUtil/include/CvEnums.h`). Compare with `PlotTypes.*` names rather than numbers.

### M2. Set plot type (flat, hills, mountain, water) and terrain
State: Main · Status: TESTED 2026-09-17 (land <-> hills only; the water change was not run)
```lua
--@state=Main
local plot = assert(Map.GetPlot(20, 14))
-- a nil type would read as 0 = PLOT_MOUNTAIN / the first terrain, so check both
plot:SetPlotType(assert(PlotTypes.PLOT_HILLS), true, true, true)          -- (type, bRecalc, bRebuildGraphics, bEraseUnitsIfWater)
plot:SetTerrainType(assert(TerrainTypes.TERRAIN_PLAINS), true, true)       -- (terrain, bRecalc, bRebuildGraphics)
-- to water: SetPlotType(PlotTypes.PLOT_OCEAN, true, true, true); SetTerrainType(TerrainTypes.TERRAIN_COAST, true, true)
return plot:GetPlotType(), GameInfo.Terrains[plot:GetTerrainType()].Type
```
- **Check:** the redrawn tile in a screenshot, and M1.
- **Observed:** 119,33 flat grass -> `1, "TERRAIN_PLAINS"`, `IsHills()` true, production yield 0 -> 2, area unchanged (40). Screenshot `06_m2_hills_crop.png`: **the yield icons updated (2 production) but the 3D tile did not change** - still flat grassland art. Restored with `SetPlotType(PLOT_LAND, true, true, true)` + `SetTerrainType(TERRAIN_GRASS, true, true)`: `2, "TERRAIN_GRASS"`, food 2, production 0.
- **Caveats:**
  - Hills and mountains are plot types, not terrains. Don't set `TERRAIN_HILL` or `TERRAIN_MOUNTAIN` (graphical-only terrains).
  - **Terrain art does not redraw live** for plot-type/terrain changes (features, resources and improvements do, see M3-M5). Expect the 3D map to be stale until a reload.
  - **Don't call `Map.RecalculateAreas()` in a running game** (the earlier version of this recipe did). It runs `calculateAreas()`, which rebuilds every area with new IDs while cities, AI operations and caches still hold the old ones. VP's `setPlotType` with bRecalculate = true already updates the areas locally on a land <-> water change (it ASSERTs "This should be logically impossible" / "All adjacent plots are invalid???" in impossible neighbour layouts).
  - A land-to-water change erases units and features that are no longer valid. Cities on the plot: don't.
  - `SetPlotType` is a `BasicLuaMethod`: omitted flags become `false` (no recalc, no graphics rebuild). `SetTerrainType` reads its flags with `lua_toboolean`.

### M3. Set or remove a feature
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local plot = assert(Map.GetPlot(20, 14))
local f = assert(GameInfoTypes.FEATURE_FOREST)
local ok = plot:CanHaveFeature(f)
if ok then plot:SetFeatureType(f) end      -- plot:SetFeatureType(-1) removes
return ok, plot:GetFeatureType()
```
- **Check:** the forest model, and M1.
- **Observed:** 119,33: `CanHaveFeature(FEATURE_FOREST)` true, feature -1 -> 5, production 0 -> 1. Screenshot `07_m3_forest_crop.png`: forest model drawn at once. `SetFeatureType(-1)` -> -1.
- **Caveats:** `SetFeatureType` itself checks nothing except the ID range (an ID below -1 or past the last feature ASSERTs). Natural wonders are features that span several plots, so don't plop them this way.

### M4. Set a resource with quantity, or remove it
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local plot = assert(Map.GetPlot(20, 14))
local r = assert(GameInfoTypes.RESOURCE_IRON)
local ok = plot:CanHaveResource(r)                  -- optional 2nd: bIgnoreLatitude (bool)
plot:SetResourceType(r, 4)                          -- (resource, quantity); SetResourceType(-1, 0) removes
-- plot:SetNumResource(6)                           -- change the quantity only
return ok, plot:GetResourceType(), plot:GetNumResource()
```
- **Check:** the resource icon (UI8 turns on resource icons), and the owner's resource count on the top panel.
- **Observed:** 119,33 (flat grass): `false, 0, 4` - `CanHaveResource(RESOURCE_IRON)` false, set anyway, iron = ID 0, quantity 4. Screenshot `08_m4_iron_crop.png`: iron rock model drawn at once. `SetResourceType(-1, 0)` -> `-1, 0`. Re-run unmodified on the restored plot (verifier): `false, 0, 4`, iron rocks drawn at once, removed -> `-1, 0`, food 2 / production 0 again.
- **Caveats:**
  - The resource shows only once the viewer's team has the reveal tech.
  - **Asserts** (source): quantity must be >= 1 while a resource is set (`SetResourceType(r, 0)` or `SetNumResource(0)` ASSERT); city-state-only luxuries ASSERT unless the 3rd argument `bIgnoreMinorCivRestrictions` is `true` (a `BasicLuaMethod` argument, so omitted = false).
  - `SetResourceType` with the resource the plot already has does nothing, not even the quantity (`CvPlot::setResourceType` only acts when the type changes): use `SetNumResource(n)` (n >= 1) for that.
  - For owned plots, IGE re-applies ownership afterwards (`SetOwner(-1)` then the old owner again) so the owner's totals update. Do the same (M8) if the count looks wrong.

### M5. Set or remove an improvement
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local plot = assert(Map.GetPlot(20, 14))
local imp = assert(GameInfoTypes.IMPROVEMENT_FARM)
local ok = plot:CanHaveImprovement(imp, -1)         -- (improvement, player or -1)
plot:SetImprovementType(imp, -1, false)             -- (improvement, builder, bGiftFromMajor); ALWAYS pass the builder
-- plot:SetImprovementType(-1, -1, false)           -- remove
return ok, plot:GetImprovementType()
```
- **Check:** the improvement model, and the plot's yield.
- **Observed:** 119,33: `true, 3, 3, -1, 7` - can have a farm, IMPROVEMENT_FARM = 3 set, owner still -1 with builder -1, food 2 -> 7. Screenshot `09_m5_farm_crop.png`: future-era farm model and 7-food icon at once. Removed with `SetImprovementType(-1, -1, false)`.
- **Caveats:**
  - With the builder omitted, player 0 counts as the builder. For some improvements `CvPlot::setImprovementType` then **takes ownership of the plot for player 0**.
  - Pass the plot owner as the builder only when you want builder effects: happiness on construction, the ownership grab, archaeology data.
  - The stock WorldView improvement plopper has this bug under VP.

### M6. Set or remove a route
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local plot = assert(Map.GetPlot(20, 14))
plot:SetRouteType(assert(GameInfoTypes.ROUTE_ROAD), -1)   -- (route, builder); SetRouteType(-1, -1) removes
return plot:GetRouteType()
```
- **Check:** the road model, and M1.
- **Observed:** 119,33: `0` (ROUTE_ROAD = 0). `SetRouteType(-1, -1)` -> -1.
- **Caveats:** A builder of -1 means no player pays maintenance or gets credit (`SetPlayerResponsibleForRoute`). Pass a player ID for realistic upkeep.

### M7. Pillage or repair an improvement and route
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local plot = assert(Map.GetPlot(20, 14))
plot:SetImprovementPillaged(true, true)     -- (bPillaged, bEvents); false repairs
plot:SetRoutePillaged(true, true)
return plot:IsImprovementPillaged(), plot:IsRoutePillaged()
```
- **Check:** the pillaged art and the plot's yield.
- **Observed:** farm + road on 119,33: `true, true`, improvement and route still 3 and 0, food 7 -> 2, player gold unchanged. Screenshot `10_m7_pillaged_crop.png`: smoke and pillaged art at once. `(false, true)` for both -> `false, false`, food back to 7.
- **Caveats:** No gold or heal is granted to anyone, unlike a unit pillaging. Improvements with `DestroyedWhenPillaged` are not removed by this call (UNVERIFIED).

### M8. Set or clear a plot's owner
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local plot = assert(Map.GetPlot(21, 14))
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
plot:SetOwner(-1, -1, true)                        -- clear first, as IGE does
plot:SetOwner(c:GetOwner(), c:GetID(), true)       -- (player, acquiringCityID, bCheckUnits)
return plot:GetOwner()
```
- **Check:** the border line, and M1's `owner` and `owningCityID`.
- **Observed:** unowned water plot 76,32 at distance 2 from the test city: owner `{ -1, -1 }` -> `{ 2, 7851 }` (player, owning city ID). Screenshot `12_m8_owner_crop.png`: the border bulged to include it. `SetOwner(-1, -1, true)` -> `-1, -1`.
- **Caveats:**
  - Plots far from the city may be owned but unworkable.
  - `bCheckUnits` bumps foreign units out.
  - IGE also calls `plot:UpdateFog()` and fires `Events.HexFOWStateChanged` for newly revealed neighbours.

### M9. Plots within a radius; find a plot matching a test
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local cx, cy, range = 20, 14, 3
local team = Game.GetActiveTeam()
local found = {}
for dx = -range, range do
  for dy = -range, range do
    local plot = Map.PlotXYWithRangeCheck(cx, cy, dx, dy, range)
    if plot and not plot:IsWater() and not plot:IsCity() and not plot:IsMountain()
       and plot:GetNumUnits() == 0 and plot:GetOwner() == -1 then
      found[#found + 1] = plot:GetX() .. "," .. plot:GetY()
    end
  end
end
return #found, found
```
- **Check:** feed a result into M1 to confirm.
- **Observed:** around Ulundi 37,20, range 3, with the owner test changed to `== 2` (everything near a capital is owned): 23 plots such as `"34,20"`, `"37,21"`, `"40,20"`; the test unit's plot 38,21 was correctly left out. `Map.PlotDirection(37, 20, d)` for d = 0..5: `37,21  38,20  37,19  36,19  36,20  36,21`.
- **Caveats:**
  - `PlotXYWithRangeCheck` returns nil outside the hex range or the map. This is the shipped InGame.lua idiom.
  - Neighbours only: `Map.PlotDirection(x, y, dir)` with `dir` 0..5.
  - `plot:GetOwner()` returns 2 values, and the comparison uses the first.

---

## 6. UI

All of these need a UI state. Check the state name with B1.

### UI1. Move the camera to a plot
State: InGame · Status: TESTED 2026-09-17
```lua
--@state=InGame
local plot = assert(Map.GetPlot(20, 14))
UI.LookAt(plot, 0)                 -- 2nd arg: zoom mode (LuaCATS: 1 zooms out)
return "looking at " .. plot:GetX() .. "," .. plot:GetY()
```
- **Check:** take a screenshot; the plot is at the centre.
- **Observed:** `"looking at 78,32"`; 2 s later the test city was at the screen centre (screenshot `01_ui1_city.png`; before: `00_start.png` centred elsewhere). Used ten more times in the session, always centred within 2 s.
- **Caveats:** The camera moves smoothly, so wait about 1 s before the screenshot. This idiom appears in ActionInfoPanel.lua, CityList.lua and ProductionPopup.lua.

### UI2. Select a unit
State: InGame · Status: TESTED 2026-09-17
```lua
--@state=InGame
local u = assert(Players[Game.GetActivePlayer()]:GetUnitByID(vp.u.id), "no such unit of the active player")
UI.LookAt(u:GetPlot(), 0)
UI.SelectUnit(u)
local sel = UI.GetHeadSelectedUnit()
return sel and sel:GetID()
```
- **Check:** `GetHeadSelectedUnit()` returns the ID (it may lag one request), and the unit panel shows it.
- **Observed:** test spearman: returned `7852` in the same request (no lag); UI13 then reported `unit = "2:7852 UNIT_SPEARMAN"`. Screenshot `04_ui2_selected.png`: camera on it, selection ring, unit panel SPEARMAN 0/3 moves.
- **Caveats:** Only the active player's units can be selected. The idiom comes from ActionInfoPanel.lua. `UI.ClearSelectionList()` deselects.

### UI3. Open (and close) the city screen for a city
State: InGame · Status: TESTED 2026-09-17 (own-city branch and the close event)
```lua
--@state=InGame
local pid = Game.GetActivePlayer()
local c = assert(Players[vp.city.owner]:GetCityByID(vp.city.id))
local plot = c:Plot()
if c:GetOwner() == pid then
  UI.DoSelectCityAtPlot(plot)                            -- own city
else
  UI.SelectCity(c); UI.LookAt(plot, 0); UI.SetCityScreenUp(true)   -- EUI banner path (observer/debug mode)
end
return "requested"
```
To close it:
```lua
--@state=InGame
Events.SerialEventExitCityScreen()
return UI.IsCityScreenUp()
```
- **Check:** `UI.IsCityScreenUp()` in a later request, and a screenshot.
- **Observed:** own test city: `"requested"`; next request `UI.IsCityScreenUp()` true, head selected city "Khangela". Screenshot `16_ui3_cityscreen.png`: KHANGELA city screen. Close event: returned `true` (still up inside the same request), next request `false`, no selected city; screenshot `17_ui3_closed.png`: map view, camera back on the selected unit.
- **Caveats:**
  - EUI uses the foreign-city branch only for observers (without a UI override) or in debug mode. For a normal human looking at a foreign major civ, it may do nothing (UNVERIFIED).
  - For a foreign city while not observing, try G10 (debug mode) first.
  - Idioms from CityBannerManager.lua (EUI compat) and CityView.lua.

### UI4. Open the tech tree
State: Main (engine queue, preferred) · Status: TESTED 2026-09-17 (both forms)
```lua
--@state=Main
Game.DoControl(ControlTypes.CONTROL_TECH_CHOOSER)    -- what F6 does: AddPopup(BUTTONPOPUP_TECH_TREE)
return "queued"
```
State: InGame (direct event, as the EUI top panel does it) · Status: TESTED 2026-09-17
```lua
--@state=InGame
local pid = Game.GetActivePlayer()
local observer = Players[pid]:IsObserver() and 1 or 0
local who = Game.GetObserverUIOverridePlayer(); if who < 0 then who = pid end
Events.SerialEventGameMessagePopup{ Type = ButtonPopupTypes.BUTTONPOPUP_TECH_TREE, Data1 = 1, Data2 = -1, Data4 = observer, Data5 = who }
return "sent"
```
- **Check:** a screenshot. In InGame, `local c = ContextPtr:LookUpControl("/InGame/TechTree"); return c and not c:IsHidden()` (path confirmed live).
- **Observed:** Main `DoControl(CONTROL_TECH_CHOOSER)`: `"queued"`; 2 s later `/InGame/TechTree` shown and `UI.IsPopupUp()` true; screenshot `18_ui4_techtree.png`. The InGame event closed it (`false, false`), opened it again (`true, true`, screenshot `19_ui4_event_open.png`) and closed it again - the toggle works both ways. `CloseTechTree` is not reachable from the `TechTree` state (nil there), so toggle with the event.
- **Caveats:**
  - VP's TechTree handler toggles: sending the event while the tree is open closes it.
  - Always pass the `Data*` fields, because the handler compares `Data4 > 0` (a nil there would raise inside the UI).
  - Observers need `Data4=1` and `Data5` set to the player to show.
  - `DoControl` needs `canDoControl`, which requires that no text box has focus.

### UI5. Open the social policy screen
State: Main · Status: TESTED 2026-09-17 (both forms, and `OnClose()`)
```lua
--@state=Main
Game.DoControl(ControlTypes.CONTROL_POLICIES_SCREEN)
return "queued"
```
State: InGame · Status: TESTED 2026-09-17
```lua
--@state=InGame
local pid = Game.GetActivePlayer()
Events.SerialEventGameMessagePopup{ Type = ButtonPopupTypes.BUTTONPOPUP_CHOOSEPOLICY, Data1 = 1,
  Data3 = Players[pid]:IsObserver() and 1 or 0, Data4 = pid }
return "sent"
```
- **Check:** a screenshot. To close it, run `OnClose()` in state `SocialPolicyPopup` (a global function in the EUI compat file), or use UI7.
- **Observed:** Main form: `/InGame/SocialPolicyPopup` shown, `IsPopupUp()` true; screenshot `20_ui5_policies.png` (it opened on the Ideological Tenets tab, "Zulu Order"). `OnClose()` in state `SocialPolicyPopup` returned `true` (hidden) and the next request read `false, false`. The InGame event form opened it the same way (`true, true`) and `OnClose()` closed it.
- **Caveats:** With `Data1 = 1` the handler toggles: it closes the screen if open and queues it if hidden. `Data3 > 0` means "view another player (espionage)", showing player `Data4`.

### UI6. Open other overview screens
State: InGame · Status: TESTED 2026-09-17 (ECONOMIC and MILITARY)
```lua
--@state=InGame
local which = "RELIGION"   -- ECONOMIC, MILITARY, DIPLOMATIC, RELIGION, CULTURE, TRADE_ROUTE, LEAGUE, ESPIONAGE, VICTORY, DEMOGRAPHICS
local types = {
  ECONOMIC = ButtonPopupTypes.BUTTONPOPUP_ECONOMIC_OVERVIEW, MILITARY = ButtonPopupTypes.BUTTONPOPUP_MILITARY_OVERVIEW,
  DIPLOMATIC = ButtonPopupTypes.BUTTONPOPUP_DIPLOMATIC_OVERVIEW, RELIGION = ButtonPopupTypes.BUTTONPOPUP_RELIGION_OVERVIEW,
  CULTURE = ButtonPopupTypes.BUTTONPOPUP_CULTURE_OVERVIEW, TRADE_ROUTE = ButtonPopupTypes.BUTTONPOPUP_TRADE_ROUTE_OVERVIEW,
  LEAGUE = ButtonPopupTypes.BUTTONPOPUP_LEAGUE_OVERVIEW, ESPIONAGE = ButtonPopupTypes.BUTTONPOPUP_ESPIONAGE_OVERVIEW,
  VICTORY = ButtonPopupTypes.BUTTONPOPUP_VICTORY_INFO, DEMOGRAPHICS = ButtonPopupTypes.BUTTONPOPUP_DEMOGRAPHICS,
}
local info = { Type = assert(types[which]), Data1 = 1 }
if which == "CULTURE" then info.Data2 = 4 end       -- as vox-deorum passes it
Events.SerialEventGameMessagePopup(info)
return which
```
- **Check:** a screenshot.
- **Observed:** ECONOMIC: `/InGame/EconomicOverview` shown, screenshot `21_ui6_economic.png` (city list included the test city); closed with UI7. MILITARY: shown, screenshot `22_ui6_military.png`; closed with `OnClose()` in state `MilitaryOverview` (`true`, then `IsPopupUp()` false).
- **Caveats:**
  - The table comes from vox-deorum's `VoxDeorumHumanTrigger.lua`, which targets this same EUI plus VP UI. All enum names are registered in `CvLuaEnums.cpp`.
  - From Main, `Game.DoControl(ControlTypes.CONTROL_DOMESTIC_SCREEN / CONTROL_MILITARY_SCREEN / CONTROL_FOREIGN_SCREEN / CONTROL_VICTORY_SCREEN / CONTROL_INFO / CONTROL_RELIGION_OVERVIEW / CONTROL_ESPIONAGE_OVERVIEW)` covers some of these.

### UI7. Close a popup screen
State: InGame · Status: TESTED 2026-09-17
```lua
--@state=InGame
local name = "TechTree"      -- context under /InGame: TechTree, SocialPolicyPopup, EconomicOverview, MilitaryOverview,
                             -- ReligionOverview, CultureOverview, TradeRouteOverview, EspionageOverview,
                             -- DiploRelationships, VictoryProgress, LeagueOverview, Demographics, CivilopediaScreen
local ctx = ContextPtr:LookUpControl("/InGame/" .. name)
if not ctx then return "no context /InGame/" .. name end
local wasOpen = not ctx:IsHidden()
UIManager:DequeuePopup(ctx)
return wasOpen, ctx:IsHidden()
```
- **Check:** `IsHidden()` is true, and a screenshot.
- **Observed:** all of `TechAwardPopup`, `TechTree`, `SocialPolicyPopup`, `EconomicOverview`, `MilitaryOverview`, `ReligionOverview`, `CivilopediaScreen` resolve under `/InGame/`. `TechAwardPopup` (opened by P5): `true, true` (was open, now hidden), screenshot `14_ui7_closed.png`; `EconomicOverview`: `true, true`, then `UI.IsPopupUp()` false.
- **Caveats:**
  - Dequeuing skips the screen's own close function, but for most screens that function *is* `UIManager:DequeuePopup(ContextPtr)` and the bookkeeping (turn-timer semaphore, `SerialEventGameMessagePopupProcessed`) lives in the ShowHide handler, which still runs: TechAwardPopup, TextPopup, EconomicOverview, MilitaryOverview, VictoryProgress, Demographics, SocialPolicyPopup (source). Exceptions: VP's TechTree (`CloseTechTree` does the bookkeeping itself: toggle with the UI4 event) and CultureOverview (`OnClose` sends PopupProcessed itself). Where a global close function exists, prefer calling it in the screen's own state: `OnClose()` in `SocialPolicyPopup`, `MilitaryOverview`, `EconomicOverview`, `CivilopediaScreen`; `OnCloseButtonClicked()` in `TextPopup`.
  - The context paths were confirmed live for the names listed in the snippet that were tried (see Observed).

### UI8. Toggle yield icons, resource icons, hex grid
State: Main (toggles, what the hotkeys do) · Status: TESTED 2026-09-17
```lua
--@state=Main
Game.DoControl(ControlTypes.CONTROL_YIELDS)          -- toggle yield icons
-- Game.DoControl(ControlTypes.CONTROL_RESOURCE_ALL) -- toggle resource icons
return "toggled"
```
State: InGame (set explicitly and persist, as the minimap checkboxes do) · Status: TESTED 2026-09-17 without the `CommitGameOptions` line (see Observed below)
```lua
--@state=InGame
local on = true
UI.SetYieldVisibleMode(on);    OptionsManager.SetYieldOn_Cached(on)
UI.SetResourceVisibleMode(on); OptionsManager.SetResourceOn_Cached(on)
OptionsManager.CommitGameOptions(PreGame.IsHotSeatGame())
-- UI.ToggleGridVisibleMode()                        -- hex grid (the G key in InGame.lua)
return OptionsManager.GetYieldOn(), OptionsManager.GetResourceOn()
```
- **Check:** a screenshot.
- **Observed:** yield icons on at start (`OptionsManager.GetYieldOn()` true). `DoControl(CONTROL_YIELDS)` -> icons gone (screenshot `23_ui8_yields_off.png`) and `GetYieldOn()` **false**; a second toggle -> true again. InGame form, run **without** `CommitGameOptions` (it would write the user's options): `UI.SetResourceVisibleMode(true)` + `SetResourceOn_Cached(true)` -> resource icons drawn (screenshot `24_ui8_resources_on.png`) but `OptionsManager.GetResourceOn()` stayed `false`; set back to false afterwards.
- **Caveats:**
  - The DoControl toggle **does** change what `OptionsManager.GetYieldOn()` returns (live), so it can be read back. Whether the minimap checkbox follows was not checked.
  - `SetResourceOn_Cached` does not change `GetResourceOn()` (live): the getter reads the committed value. `CommitGameOptions` persists the player's game options, so leave it out for throwaway tests and put the display back yourself.
  - The explicit form is from `MiniMapPanel.lua` (CP override). The minimap checkbox itself does not update until that panel refreshes.

### UI9. Show a text popup or a top-of-screen alert
State: InGame · Status: TESTED 2026-09-17 (both)
```lua
--@state=InGame
Events.SerialEventGameMessagePopup{ Type = ButtonPopupTypes.BUTTONPOPUP_TEXT, Data1 = 800, Option1 = true, Text = "Test popup from vp_lua" }
-- Events.GameplayAlertMessage("Test alert from vp_lua")   -- fading text near the top (InGame.lua listener)
return "shown"
```
- **Check:** a screenshot.
- **Observed:** text popup: `"shown"`, `TextPopup` context visible, `IsPopupUp()` true; screenshot `25_ui9_textpopup.png` ("Test popup from vp_lua" with OK, map dimmed). Closed with `OnCloseButtonClicked()` in state `TextPopup` -> hidden, `IsPopupUp()` false. Alert: `Events.GameplayAlertMessage("Test alert from vp_lua")` -> the text near the top of the map (screenshot `26_ui9_alert_top.png`).
- **Caveats:** The text popup is modal: close it with Esc, its OK button, or `OnCloseButtonClicked()` run in state `TextPopup` (no input needed). `Data1` is the wrap width. Idioms from IGE_Window.lua and VP's InGame.lua.

### UI10. Send a notification
State: Main · Status: TESTED 2026-09-17
```lua
--@state=Main
local p = Players[Game.GetActivePlayer()]
local x, y = 20, 14                 -- plot the notification jumps to, or -1, -1
local id = p:AddNotification(NotificationTypes.NOTIFICATION_GENERIC, "Detailed text (tooltip)", "Summary", x, y, -1)
return id
```
- **Check:** the id is at least 0 and the notification icon appears on the right-hand panel.
- **Observed:** player 2, plot 78,32: id `62`, notifications 62 -> 63; screenshot `27_ui10_right.png`: a "!" notification with its summary slid out on the right. Removed with `p:DismissNotification(62, false)`; the entry stays in the list with `GetNotificationDismissed(i)` true (count still 63).
- **Caveats:**
  - It is displayed only when the recipient is the active player. In observer mode it goes to the civ and is not shown (see vox-deorum `post-notification.lua`).
  - `NotificationTypes` values are string hashes, so use the names.
  - Arguments are (type, text, summary, x, y, data1[, data2]).
  - To take it away again: `p:DismissNotification(id, false)` with the id the add returned.

### UI11. Highlight hexes, and clear them
State: InGame · Status: TESTED 2026-09-17
```lua
--@state=InGame
local cx, cy, range = 20, 14, 2
local n = 0
for dx = -range, range do
  for dy = -range, range do
    local plot = Map.PlotXYWithRangeCheck(cx, cy, dx, dy, range)
    if plot then
      Events.SerialEventHexHighlight(ToHexFromGrid(Vector2(plot:GetX(), plot:GetY())), true, Vector4(1.0, 0.0, 0.0, 1.0))
      n = n + 1
    end
  end
end
return n
```
To clear: `Events.ClearHexHighlights()`.
- **Check:** red hexes in a screenshot.
- **Observed:** centre Ulundi 37,20, range 2: `19`; screenshot `28_ui11_highlight.png`: 19 red hex outlines around the capital. `Events.ClearHexHighlights()` removed them (`29_ui11_cleared.png`).
- **Caveats:**
  - `Vector2` and `Vector4` come from `include("FLuaVector")`, which InGame.lua includes. Other states may not have them (B2).
  - The UI itself clears highlights on selection changes, so take the screenshot right away.

### UI12. Force a UI refresh after state edits
State: InGame · Status: RAN, EFFECT NOT VERIFIED 2026-09-17 (ran without error; nothing was stale, so there was no visible change to confirm)
```lua
--@state=InGame
local c = Players[vp.city.owner]:GetCityByID(vp.city.id)
if c then
  Events.SpecificCityInfoDirty(c:GetOwner(), c:GetID(), CityUpdateTypes.CITY_UPDATE_TYPE_BANNER)
  Events.SpecificCityInfoDirty(c:GetOwner(), c:GetID(), CityUpdateTypes.CITY_UPDATE_TYPE_PRODUCTION)
end
Events.SerialEventCityInfoDirty()
Events.SerialEventGameDataDirty()
UI.SetDirty(InterfaceDirtyBits.GameData_DIRTY_BIT, true)
return "dirtied"
```
- **Check:** banners, the top panel and the unit panel show the new values.
- **Observed:** `"dirtied"`. `InterfaceDirtyBits.GameData_DIRTY_BIT` = 11, `CityUpdateTypes.CITY_UPDATE_TYPE_BANNER` = 0, `CITY_UPDATE_TYPE_PRODUCTION` = 2. In practice every edit in this session (buildings, population, plots, ownership, units) showed up on screen without it.
- **Caveats:**
  - This is the idiom of IGE's `InvalidateCity` plus InGame.lua's `SetDirty`.
  - Fog changes use `Events.HexFOWStateChanged(ToHexFromGrid(Vector2(x, y)), true, false)` per plot, then `UI:RequestMinimapBroadcast()`.

### UI13. What is selected, under the mouse, interface mode
State: InGame · Status: TESTED 2026-09-17
```lua
--@state=InGame
local u, c = UI.GetHeadSelectedUnit(), UI.GetHeadSelectedCity()
local mx, my = UI.GetMouseOverHex()
return {
  unit = u and (u:GetOwner() .. ":" .. u:GetID() .. " " .. GameInfo.Units[u:GetUnitType()].Type) or nil,
  city = c and c:GetName() or nil, mouseHex = mx and (mx .. "," .. my) or nil,
  interfaceMode = UI.GetInterfaceMode(), cityScreenUp = UI.IsCityScreenUp(),
  strategicView = InStrategicView(), popupUp = UI.IsPopupUp(),
}
```
- **Check:** move the mouse with `civ_ui.py move X Y`, then read `mouseHex`. This maps screen pixels to plots.
- **Observed:** `{ unit = "2:7852 UNIT_SPEARMAN", mouseHex = "44,19", interfaceMode = 1, cityScreenUp = false, strategicView = false, popupUp = false }` (city nil). `interfaceMode` 1 = `InterfaceModeTypes.INTERFACEMODE_SELECTION`. `mouseHex` changes whenever the camera moves under a parked mouse (it read "6,1" after the camera jumped to the Arctic).
- **Caveats:** `UI.GetMouseOverHex()` returns the grid x, y (shipped usage: `Map.GetPlot(UI.GetMouseOverHex())`). Compare `interfaceMode` with `InterfaceModeTypes.*`.

### UI14. Reset the interface mode and selection; strategic view
State: InGame · Status: TESTED 2026-09-17 (strategic view toggle not run)
```lua
--@state=InGame
UI.SetInterfaceMode(InterfaceModeTypes.INTERFACEMODE_SELECTION)   -- leave move/attack/range-strike targeting
UI.ClearSelectionList()
-- ToggleStrategicView()                                         -- flip 2D strategic view (WorldView.lua idiom)
return UI.GetInterfaceMode(), InStrategicView()
```
- **Check:** normal cursor; nothing selected only if no unit needs orders.
- **Observed:** `1, false` and no head unit inside the request - but the next screenshot (`03_ui14_cleared.png`) showed an **Ironclad selected and the camera moved to it** in the Arctic, and UI13 reported `unit = "2:4049 UNIT_IRONCLAD"`: on the human's turn the UI auto-cycles to the next unit that needs orders as soon as the selection is empty.
- **Caveats:** "Nothing selected" does not last on a human turn with units awaiting orders: the engine selects the next one and moves the camera (live). Re-select with UI2 or move the camera with UI1 afterwards.

### UI15. Open a Civilopedia entry
State: InGame · Status: TESTED 2026-09-17
```lua
--@state=InGame
Events.SearchForPediaEntry(Locale.ConvertTextKey(GameInfo.Units.UNIT_WARRIOR.Description))
return "opened"
```
- **Check:** a screenshot.
- **Observed:** `"opened"`; `CivilopediaScreen` context visible, screenshot `30_ui15_pedia.png`: the Warrior page. Closed with `OnClose()` in state `CivilopediaScreen` -> hidden, `IsPopupUp()` false.
- **Caveats:** The search string is a displayed name. `""` opens the pedia home, as vox-deorum does. Close it with Esc, `OnClose()` in state `CivilopediaScreen` (also unloads the portrait texture), or UI7.

---

## Brainstorm backlog (not written up yet)

Ideas for later recipes. The API names given here exist in the bindings, but nothing below has been
worked out or tested.

- Units: transfer a unit to another player (`newOwnerUnit:Convert(oldUnit, false, true)` after `InitUnit`); rename (`SetName`); fortify, sleep or automate (`PushMission(MissionTypes.MISSION_FORTIFY)`, `DoCommand`); set UnitAI (`SetUnitAIType`); load onto a carrier or transport (`GetTransportUnit`); spawn a religious unit with a religion (IGE buys it with faith through `Game.CityPurchaseUnit`); one of each unit type (FireTuner Map panel).
- Cities: puppet, occupied or never-lost flags (`SetPuppet`, `SetOccupied`, `SetNeverLost`, IGE idioms); WLTKD (`SetWeLoveTheKingDayCounter`); rename (`SetName`); force a worked plot (`AlterWorkingPlot`); AI city focus (`Network.SendSetCityAIFocus`, FireTuner); buy a plot; add a great work (`Game.CreateGreatWork` plus `SetBuildingGreatWork`); city events (VP event choices); the resistance counter.
- Players: kill a player (`KillUnits` plus `KillCities`, IGE); happiness (`SetHappiness`); anarchy turns (`SetAnarchyNumTurns`); the ideology or free tenet flow; DoF and denounce (`DoForceDoF`, `DoForceDenounce`, IGE); open borders (`pTeam:SetOpenBorders`); vassalage (`DoBecomeVassal`); tourism and influence; spies (`GetEspionageSpies`); trade routes; the World Congress (`Game.GetActiveLeague`); switch the active seat (`Game.ChangeActivePlayer`).
- Map: rivers (`SetWOfRiver`, `SetNWOfRiver`, `SetNEOfRiver`, IGE_API_Rivers.lua); natural wonders; goody huts and barbarian camps (`IMPROVEMENT_GOODY_HUT`, `IMPROVEMENT_BARBARIAN_CAMP`); archaeology sites; set a plot's extra yield (`Game.SetPlotExtraYield`); nuke a plot (`NukeExplosion`).
- Game: pause and unpause (`Game.SetPausePlayer`); victory (`Game.SetWinner`, FireTuner); game options (`Game.SetOption`); a quick "tail Lua.log" helper from Python; a "wait until turn N" loop in Python around G1.
- UI: open the diplomacy screen with a leader; the production popup for a city (`BUTTONPOPUP_CHOOSEPRODUCTION`); the list of open popups (`UI.GetPopupTypeCount`, `UI.GetPopupTypeByIndex`, `UI.IsPopupTypeOpen` from FireTuner's "Game UI" panel); the notification log (`CONTROL_TURN_LOG`); screenshot helpers that position the camera, open a screen and shoot.

## Sources mined

- DLL bindings: `CvGameCoreDLL_Expansion2/Lua/CvLua{Game,Map,Player,Team,TeamTech,City,Unit,Plot,Enums,Support}.cpp`, the C++ signatures in `CvPlayer.h`, `CvTeam.h`, `CvCity.h`, `CvUnit.h`, `CvPlot.h` and `CvGame.h`, and the behaviour in `CvGame.cpp` (doControl, setAIAutoPlay), `CvReligionClasses.cpp`, `CvCity.cpp` (pushOrder, processBuilding), `CvCityCitizens.cpp` and `CvUnit.cpp`.
- Shipped UI: `Assets/UI/InGame/{InGame,WorldView/WorldView,WorldView/ActionInfoPanel,WorldView/MiniMapPanel,CityList,TopPanel,TechPopup,PopupsGeneric/*}.lua`.
- Modpack UI: `Assets/DLC/VP_MODPACK/UI/{InGame,MiniMapPanel}.lua`, and `Mods/(1) Community Patch/Core Files/Overrides/{ActionInfoPanel,ChooseReligionPopup,MiniMapPanel}.lua`, and `Mods/(3a) VP - EUI Compatibility Files/{ImprovedTechTree/TechTree,ImprovedTopPanel/TopPanel,LUA/SocialPolicyPopup,LUA/CityBannerManager}.lua`.
- FireTuner panels: `Sid Meier's Civilization V/Debug/*.ltp` (Active Player, Game, Map, Selected City, Selected Unit, Game UI).
- IGE: `My Games/Sid Meier's Civilization 5/MODS/InGame Editor+/` (Panels/*, IGE_API_*, IGE_Controller_Fog, BulkUI/IGE_Choose*Popup).
- vox-deorum: `civ5-mod/UI/VoxDeorumHumanTrigger.lua` (screen popups and context paths), `mcp-server/lua/post-notification.lua`, `civ5-dll/LuaCATS/UI.d.lua`.
- Type strings: `My Games/.../cache/Civ5DebugDatabase.db` (modpack content, 2026-09-15).

## Findings from live testing 2026-09-17

Game: the turn-317 (logged turn 67) 128x80 save, VP release DLL, human-controlled player 2 (Shaka), FireTuner connected.
Requests went one at a time through `vp_lua.py`; no keys or clicks were sent to the game in either session.

### What ran

- **Session 1 (to 02:00):** TESTED B1, B3, B4, G1-G4, G10, P1; B2 failed and was fixed; P2 ran with no effect. NOT RUN
  G5-G9 (end turn, autoplay, save; G8 was refused by the permission check). Stopped by the freeze below; nothing had
  been spawned, and gold, faith and debug mode were back at their starting values.
- **Session 2 (02:45-03:40; CvAssert.log stayed at 11,078 bytes, no dialog, no freeze):** TESTED U1-U13, C1-C12
  (C7 failed as written and was fixed), M1-M9, UI1-UI11, UI13-UI15, P3, P5, P6, P11, P15, P17. RAN, EFFECT NOT VERIFIED:
  P2 (religion disabled, cause confirmed), P14 (map already revealed), UI12 (nothing stale).
  NOT RUN: C13, P8-P10 (religion disabled), P4 (turns line), P7, P12, P13, P16 (irreversible or modal, see each recipe).
  Test objects: warrior 7850 (upgraded to spearman 7852) spawned at 38,21; Great General 7853; city Khangela (7851,
  later 7854 under Warsaw) founded on the one-tile island 78,32; plot 119,33 edited and restored; plot 76,32 owner set
  and cleared.
- **Left in the game after session 2:** no test unit, city, building or plot change (units 95, cities 12; plots
  119,33, 78,32 and 76,32 match their recorded originals; the ruins and railroad left by C10 were removed; the next
  policy cost was recomputed to 15,465). Permanent side effects on player 2 with no Lua undo: Great Generals created
  5 -> 6 and GG threshold modifier 500 -> 600 (U13); `JONSCultureEverGenerated` +16,126 (P3); this turn's instant yields
  from the founding, the level-up and the tech grant (history for the turn: food +140, science +875, culture +1,291;
  culture read 15,432 at the end against 14,732 mid-session); notifications for the Great General and "enough culture";
  Warsaw briefly owned a second city. Plots around Carthage stay visible until the next turn (P15). Research, gold,
  faith, golden-age meter, combat XP, Warsaw influence and the yield/resource icons were put back.
- **Verification re-run (session 3, adversarial, one request at a time, no keys or clicks; CvAssert.log still 11,078
  bytes, no dialog):** re-ran unmodified, apart from coordinates and player IDs, U1 (warrior 7855 at 38,21), U5, U4 (both
  forms), U6 (to 39,20), U10 (onto water 41,22, boat models in a screenshot), U11 (spearman 7856), U9 (first form), C1
  (Khangela 7857 on 78,32), C4, C6, C7, C10 (killed while still the human's city), P17 (Warsaw) and M4 (119,33); UI1 was used
  to aim screenshots. Every return value matched its Observed line except two, now corrected: U6's snippet returns only the
  new position, and U11's upgraded unit keeps its moves unless the old unit had moved. Guard lines were then added to U7,
  P11, P17 and M2; P11 and P17 were re-run with them. Clean-up checked against a before-snapshot: player 2 back to 95 units,
  12 cities, gold 99,151, policy cost 15,465, combat XP 30,964, Warsaw 285; plots 38,21, 39,20, 41,22, 78,32 (ruins and
  railroad removed again) and 119,33 as before. Left behind: culture and lifetime culture +12 from the U4 level-up, and
  the camera and unit selection moved.

### Asserts and freezes

**The freeze (session 1, 02:00:24).** The request was a diagnostic, not a recipe:
```lua
local o = {}; for k, v in pairs(GameOptionTypes) do if Game.IsOption(v) then o[#o+1] = k end end
return GameOptionTypes.GAMEOPTION_NO_RELIGION, Game.IsOption(GameOptionTypes.GAMEOPTION_NO_RELIGION), o, ...
```
`GameOptionTypes` also holds `NO_GAMEOPTION` (-1) and `NUM_GAMEOPTION_TYPES`. For a number outside
`0 .. NUM_GAMEOPTION_TYPES-1`, `CvPreGame::GetGameOption` treats it as a hash and calls
`CvGlobals::getInfoTypeForHash`, whose `ASSERT(uiHash==0, "Could not find resource hash")` (CvGlobals.cpp:7402) fired.
In this release build that is a system-modal "Assertion Failed" message box on the game-core thread, raised while the
channel held the Lua lock: `vp_lua.py` got no answer in 15 s, a PrintWindow screenshot hung for 25 s, and the game
stayed frozen with the dialog up. By the source, OK is safe here: the lookup returns -1, the caller checks `>= 0`, and
the chunk then finishes. Cancel (the default button, so also Enter) exits the game.
- Safe form (run in session 2): `local v, n = GameOptionTypes.GAMEOPTION_NO_RELIGION, GameOptionTypes.NUM_GAMEOPTION_TYPES;
  return (v >= 0 and v < n) and Game.IsOption(v)` -> `v` = 21, `n` = 22, `true`. Simpler and assert-free (session 3):
  `Game.IsOption("GAMEOPTION_NO_RELIGION")` -> `true`; the string overload never reaches the hash lookup.
- **`GameInfoTypes.GAMEOPTION_*` is the wrong ID space for `Game.IsOption`**: `GameInfoTypes.GAMEOPTION_NO_RELIGION` = 25
  (DB row) against enum 21. 25 >= 22 takes the same hash path and would have frozen the game again.
- General rule: filter out `NO_*` and `NUM_*` whenever a loop over an enum table feeds C++, and don't mix DB row IDs
  (`GameInfoTypes`) with DLL enums (`*Types` tables): they agree for most tables but not for game options.
- **ASSERT vs PRECONDITION.** `ASSERT` shows "Assertion Failed" (OK continues, Cancel exits). `PRECONDITION` and
  `VALIDATE_OBJECT` show `CvPreconditionDlg` and then always hit `BUILTIN_TRAP`: the game dies whatever is answered.
- Asserts reachable from recipe calls (read in the source during session 2; none was triggered):

  | Call | Kind | Trigger |
  |---|---|---|
  | `pCity:SetPopulation(n, false)`, `ChangePopulation(n, false)` | ASSERT (binding) | `false` as the 2nd argument |
  | `pTeamTechs:ChangeResearchProgress(t, n, -1)` | PRECONDITION | player -1 |
  | `pMinor:GetMinorCivFriendshipWithMajor(p)` | PRECONDITION | p outside 0 .. MAX_MAJOR_CIVS-1 |
  | `pPlot:SetResourceType(r, 0)`, `SetNumResource(0)` with a resource set | ASSERT | quantity 0 while a resource is set |
  | `pPlot:SetResourceType(csLuxury, n)` | ASSERT | 3rd argument not `true` |
  | `pPlot:SetFeatureType(f)` | ASSERT | f < -1 or past the last feature |
  | `pPlot:IsRevealed(t)`, `IsImpassable(t)`, `SetRevealed(t, ...)`, `ChangeVisibilityCount(t, ...)` | PRECONDITION | team < 0 (other than `IsImpassable`'s -1) or too large |
  | `pUnit:IsHasPromotion(p)` | PRECONDITION | p < 0 (including -1) or past the last promotion |
  | `pTeam:IsHasTech(t)`, `pTeam:SetHasTech(t, ...)` | PRECONDITION | t < -1 or past the last tech (-1 is a no-op / `true`) |
  | `pPlayer:SetCombatExperience(n)`, `SetNavalCombatExperience(n)` | ASSERT | n < 0 (the `Change*` forms clamp at 0) |
  | `Game.SelectionListGameNetMessage(nil, ...)` | ASSERT | a nil message type reads as 0 (see the U7 guard) |
  | `pPlayer:InitUnit(t, ...)`, `pUnit:Upgrade()` / `UpgradeTo(t)` | PRECONDITION / ASSERT | invalid unit type, or no upgrade path (-1) |
  | `pPlayer:InitCity(x, y)` | ASSERT | a city already on the plot |
  | `pCity:SetNumRealBuilding(b, n)`, `GetNumRealBuilding(b)` | PRECONDITION | b past the last building (-1 is filtered by the binding) |
  | `pTeam:SetHasTech(t, b, p, ...)` | PRECONDITION | p >= MAX_PLAYERS (-1 is replaced by the team leader) |
  | `Game.SelectionListGameNetMessage(m, ...)` | ASSERT | m other than DO_COMMAND / PUSH_MISSION / AUTO_MISSION / SWAP_UNITS |

- Before a risky call, note the size of `C:/Program Files (x86)/Steam/steamapps/common/Sid Meier's Civilization V/CvAssert.log`
  (the game keeps it open, so `tail` fails with "Device or resource busy"; `stat`/`ls` work), and afterwards or on any
  timeout run `scripts/dismiss_assert.py --check` (read-only). A size change or a dialog means stop.

### Lua states and what they see

- `States()` returns 150 unique names; FireTuner's combo shows 159 (duplicates, plus `Main State`). Popup contexts
  that were never opened are already listed.
- `Main` (the channel default, `env=thread`) sees `Game`, `Map`, `Players`, `Teams`, `GameInfoTypes`, `Events`,
  `LuaEvents`, `UI` and `include`, but not `ContextPtr`, `Controls`, `UIManager`, `Vector2` or `Vector4`.
- UI states (InGame, WorldView, TechTree) see all of those, **but lack `getfenv`, `setfenv`, `rawget`, `rawset`,
  `rawequal`, `load`, `loadfile`, `dofile`, `require`, `module`, `io` and `package`**. Use `G.rawget` and the like.
- The raw global table (`--state _G`) is FireTuner's `Main State`: `GameInfo`, `GameDefines`, `DB`, `Locale`, `UI`,
  `OptionsManager` and `collectgarbage` are there; `Game`, `Players`, `Teams`, `Map`, `PreGame`, `GameInfoTypes`,
  `Events` and `LuaEvents` are not. In FireTuner, `print(Game.GetGameTurn())` in `Main State` fails with
  "attempt to index global 'Game' (a nil value)"; pick `InGame` there (see `firetuner.md` section 5a).
- `vp` is one table shared by every state (B3).
- A screen's own global functions are reachable by running in its state: `OnClose` in `SocialPolicyPopup`,
  `MilitaryOverview` and `CivilopediaScreen`; `OnCloseButtonClicked` in `TextPopup`. A function the file declared
  `local` first (VP's `CloseTechTree`) reads as nil there.
- Every popup context tried resolves as `/InGame/<Name>` from InGame: TechAwardPopup, TechTree, SocialPolicyPopup,
  EconomicOverview, MilitaryOverview, ReligionOverview, CivilopediaScreen.

### UI calls that work from the channel (session 2)

- **Camera and selection (InGame):** `UI.LookAt(plot, 0)` (centred within 2 s), `UI.SelectUnit(u)` (head unit readable
  in the same request), `UI.DoSelectCityAtPlot(plot)` (city screen up by the next request), `UI.ClearSelectionList()`
  (the UI at once auto-selects the next unit needing orders and moves the camera there).
- **Engine popup queue from Main:** `Game.DoControl(ControlTypes.CONTROL_TECH_CHOOSER / CONTROL_POLICIES_SCREEN /
  CONTROL_YIELDS)` all worked. Only the end-turn controls are known to do nothing.
- **Events from InGame:** `SerialEventGameMessagePopup` (tech tree toggle, policies, economic and military overviews,
  text popup), `SerialEventExitCityScreen`, `GameplayAlertMessage`, `SerialEventHexHighlight` / `ClearHexHighlights`,
  `SearchForPediaEntry`. No crash, hang or assert, although the listeners ran on the game-core thread.
- **Closing:** `UIManager:DequeuePopup(ctx)` (UI7) and the screens' own close functions both closed screens cleanly, and
  `UI.IsPopupUp()` went false.
- **Popups raised by game-state edits** also need closing through the channel: a tech grant to the active team opens
  the tech-award popup (P5, seen); a threshold great person opens the GP reward popup (P12, source).
- **What redraws live:** unit flags, embark boats, city banners and borders, features, resources, improvements,
  pillage art, fog, yield and resource icons. **Not** the 3D terrain after `SetPlotType`/`SetTerrainType` (M2).
- One PrintWindow capture taken 2 s after `City:Kill()` had no HUD at all; the next capture was normal. Take a second
  screenshot before concluding the UI is broken.

### Argument traps confirmed live

- `luaL_optint` rejects booleans with a clean Lua error: `pCity:PushOrder(..., false)` -> "bad argument #7 to
  'PushOrder' (number expected, got boolean)" (the number counts arguments after `self`).
- `lua_tointeger` does not: a boolean silently reads as 0 (`Map.GetPlot(true, true)` is plot 0,0), so `true` passed for
  a flag read as an integer (`PushOrder`'s bSave) is false.
- `pUnit:PushMission` moves the unit inside the call; `Game.SelectionListGameNetMessage` acts after the chunk returns.
- `pCity:ChangeProduction` with an empty queue goes to overflow production; `GetProductionNeeded()` is 2147483647 then.
- `pPlayer:CanFound(x, y)` is false everywhere while `IsEmpireVeryUnhappy()` (this human: 0 of 355 candidate plots).
- `pPlayer:InitUnit` never returns nil; `pUnit:Upgrade(true)` returns the new unit and selects it for the active player.
- `pUnit:SetXY` updates the embarked flag by itself; `pUnit:Embark(plot)` with a foreign plot would corrupt visibility
  counts (source).
- `pCity:GetName()` is the localized name, `GetNameKey()` the TXT_KEY.
- `IsVisible`/`GetVisibilityCount` report this turn's maximum under `CORE_DELAYED_VISIBILITY` (on in this game).
- `OptionsManager.GetYieldOn()` follows `DoControl(CONTROL_YIELDS)`; `GetResourceOn()` ignores `SetResourceOn_Cached`.
- Setters that hand back their last argument instead of a result: `SetNumRealBuilding`, `ChangeJONSCulture`,
  `ChangeMinorCivFriendshipWithMajor`. `AcquireCity` returns nothing.

### Changes that cannot be undone from Lua

- Great General counters and threshold modifier (U13); great-person counters (P12).
- `NumFreePoliciesEver` (any positive `ChangeNumFreePolicies`, P3/P7) and `JONSCultureEverGenerated` (P3).
- Golden-age turns: a negative change adds a default golden age (P4).
- Level-up, founding and tech instant yields (U4, C1, P5), which land in the player's totals at once.
- Delayed visibility until the next turn (P15).
- Exact undos that do exist: `ChangeGold(-n)`, `ChangeJONSCulture(-n)`, `ChangeGoldenAgeProgressMeter(-n)`,
  `ChangeResearchProgress(t, -n, pid)` plus `PushResearch(old, true)`, `ChangeSpecialistGreatPersonProgressTimes100(s, -n)`,
  `ChangeCombatExperience(-n)`, `ChangeNavalCombatExperience(-n)` (exact only without a state-religion Great General rate belief), `ChangeMinorCivFriendshipWithMajor(p, -n)`,
  `SetHasTech(t, false, pid, false, false)`, `DismissNotification(id, false)`, and
  `p:SetNumFreePolicies(p:GetNumFreePolicies())` to recompute the policy cost.

### Other observations

- `Game.GetCurrentEra()` is the game's era (POSTMODERN here), not the human's (`Players[2]:GetCurrentEra()` = FUTURE);
  `Game.GetMaxTurns()` (250) counts from `Game.GetStartTurn()` (250), not from turn 0.
- The whole map (10,240 plots) was already revealed for team 2, so P14 is a no-op in this save and debug mode (G10)
  changed nothing visible except the debug line in the plot tooltip.
- `EndTurnBlockingTypes` values seen: 2 = `ENDTURN_BLOCKING_PRODUCTION` on the human's turn.
- Round trip through the channel: `ms=0` in the game and 0.03-0.06 s `elapsed_s` for small chunks, 0.2-0.3 s wall per
  `vp_lua.py` call including Python start-up. Whole-map loops (10,240 plots, a few calls each) answered well inside the
  15 s timeout.
- Quiet test sites in this save: the unowned one-tile island 78,32 and the two-tile island 119,33, 13 and 9 plots from
  the nearest city.
