local loadOnStart = false;
local saveNameFilter = "";
local modsToEnable = "";
local modsExclusive = false;
local newGameSetup = "";
local loadToken = "";

-- vp-dll-dev: patched MainMenu.
--
-- The five locals above are rewritten in place by the skill's scripts before every
-- launch; keep each of them on its own line, in order, in exactly this form.
--   loadOnStart    - when true, auto-load a save as soon as the menu comes up.
--   saveNameFilter - substring of the save's file name to load. Empty = newest save.
--   modsToEnable   - MODS-folder paradigm only: "<guid>@<version>;..." in activation
--                    order ("<guid>" alone takes the newest installed version). Empty
--                    leaves the mod set alone, which is what the modpack wants.
--   modsExclusive  - also disable every other enabled mod, so a measurement run gets
--                    exactly the listed set and not whatever the player left switched on.
--   newGameSetup   - "key=value;..." describing a brand-new game (see vpDevStartNewGame).
--                    Empty means "do not start one".
--   loadToken      - a value unique to this launch, used to issue the auto-load exactly
--                    once even though this file gets re-executed during the load.
-------------------------------------------------
-- Main Menu
-------------------------------------------------
include( "MPGameDefaults" );

-------------------------------------------------
-- Script Body
-------------------------------------------------
local bHideUITest = true;
local bHideGridExamples = true;
local bHideLoadGame = true;
local bHidePreGame = true;
local fTime = 0;
local i1, i2 = string.find( UI.GetVersionInfo(), " " );
versionNumber = string.sub(UI.GetVersionInfo(), 1, i2-1);
Controls.VersionNumber:SetText(versionNumber);

-- vp-dll-dev: pick the save to auto-load.
-- Returns the newest save whose file name contains saveNameFilter, or the newest
-- save overall when the filter is empty. UI.SaveFileList already resolves the right
-- save root, so this works for both plain and modpack (ModdedSaves) games.
-- Its third argument selects ONE of the two lists, as the Load Game screen's "show
-- autosaves" checkbox does: true = autosaves only, false = manual saves only. Ask for both.
function getSaveToLoad()
  local fileList = {};
  for _, autos in ipairs({false, true}) do
    local part = {};
    UI.SaveFileList(part, GameTypes.GAME_SINGLE_PLAYER, autos, true);
    for i = 1, #part do table.insert(fileList, part[i]); end
  end

  if (#fileList == 0) then
    print("vp-dll-dev: no save files found");
    return nil;
  end
  print("vp-dll-dev: " .. #fileList .. " saves visible, filter=[" .. saveNameFilter .. "]");

  local best = nil;
  local bestHigh, bestLow = 0, 0;
  for i = 1, #fileList do
    local candidate = fileList[i];
    local matches = (saveNameFilter == "") or (string.find(candidate, saveNameFilter, 1, true) ~= nil);
    if (matches and (PreGame.GetFileHeader(candidate) ~= nil or UI.GetReplayFileHeader(candidate) ~= nil)) then
      local high, low = UI.GetSavedGameModificationTimeRaw(candidate);
      if (best == nil or UI.CompareFileTime(high, low, bestHigh, bestLow) == 1) then
        best = candidate;
        bestHigh = high;
        bestLow = low;
      end
    end
  end

  if (best == nil) then
    print("vp-dll-dev: no save matched filter [" .. saveNameFilter .. "]");
  else
    print("vp-dll-dev: selected save " .. best);
  end
  return best;
end

VPDEV_LOAD_PROPERTY = "vpDevLoadIssued";

-- vp-dll-dev: load the selected save exactly once per process.
--
-- The local below is NOT enough on its own. Activating mods makes the engine swap the UI
-- out when a load starts, and that RE-EXECUTES this file - which resets any local back to
-- its initial value. The result is a second Events.PlayerChoseToLoadGame two seconds into
-- the first load: the engine tears the half-built game down again, and the DLL's guard in
-- CvDllGame::Uninit traps on the way out (illegal instruction, no dialog, back to the main
-- menu). Under the modpack no mods are active, nothing swaps the UI, and the local alone
-- was sufficient - which is exactly why this only appeared in the MODS paradigm.
--
-- So the real guard is engine-side state, which no chunk reload can reset:
--   PreGame.GameStarted()      true as soon as the save's PreGame has been read
--   PreGame.GetLoadFileName()  the file the engine is currently loading
local hasLoaded = false;
function loadSelectedSave()
  if (hasLoaded or not loadOnStart) then
    return;
  end
  -- Has THIS launch already issued its load? The answer has to outlive a re-execution of
  -- this very file, so it is kept in the mods database rather than in any Lua value.
  if (loadToken ~= "" and Modding.GetSystemProperty ~= nil) then
    local seen = Modding.GetSystemProperty(VPDEV_LOAD_PROPERTY);
    if (seen ~= nil and tostring(seen) == loadToken) then
      print("vp-dll-dev: this launch already issued its load - not issuing a second one");
      hasLoaded = true;
      return;
    end
  end
  if (PreGame.GameStarted ~= nil and PreGame.GameStarted()) then
    print("vp-dll-dev: a game is already starting - not issuing a second load");
    hasLoaded = true;
    return;
  end
  if (PreGame.GetLoadFileName ~= nil) then
    local pending = PreGame.GetLoadFileName();
    if (pending ~= nil and pending ~= "") then
      print("vp-dll-dev: a load is already in progress (" .. tostring(pending) ..
            ") - not issuing a second one");
      hasLoaded = true;
      return;
    end
  end
  hasLoaded = true;
  local saveToLoad = getSaveToLoad();
  if (saveToLoad == nil) then
    print("vp-dll-dev: nothing to load");
    return;
  end

  -- The Load Game screen asks this before it enables its Start button, and so must we:
  -- 0 = loadable, 2 = missing DLC, 3 = DLC not purchased, 4 = missing mods,
  -- 5 = incompatible mods (LoadMenu.lua's errorTooltips). Loading anyway does not fail
  -- politely - the engine tears the half-built game down again, and the DLL's guard in
  -- CvDllGame::Uninit traps on the way out (illegal instruction, no dialog).
  local canLoad = 0;
  if (Modding.CanLoadSavedGame ~= nil) then
    canLoad = Modding.CanLoadSavedGame(saveToLoad) or 0;
  end
  local reasons = {};
  reasons[2] = "the save needs DLC that is not installed";
  reasons[3] = "the save needs DLC that is not purchased";
  reasons[4] = "the save needs mods that are not installed";
  reasons[5] = "the save's mods are incompatible with what is active";
  if (canLoad ~= 0) then
    print("vp-dll-dev: REFUSING to load, CanLoadSavedGame = " .. tostring(canLoad) ..
          " (" .. tostring(reasons[canLoad] or "unknown reason") .. ")");
    if (Modding.GetSavedGameRequirements ~= nil) then
      local _, mods = Modding.GetSavedGameRequirements(saveToLoad);
      if (mods ~= nil) then
        for _, m in ipairs(mods) do
          print("vp-dll-dev:   save requires mod " .. tostring(m.Id or m.ModID or m.ID) ..
                " v" .. tostring(m.Version) .. " " .. tostring(m.Name or ""));
        end
      end
    end
    return;
  end

  if (loadToken ~= "" and Modding.SetSystemProperty ~= nil) then
    Modding.SetSystemProperty(VPDEV_LOAD_PROPERTY, loadToken);
  end
  print("vp-dll-dev: loading " .. saveToLoad);
  Events.PlayerChoseToLoadGame(saveToLoad, false);
end

-- vp-dll-dev: running Vox Populi out of the MODS folder instead of as a DLC modpack.
--
-- Nothing can be loaded or started until the mods are enabled AND activated, which a
-- player does by walking Main Menu -> Mods -> Next. This context is the only one that
-- can do the same without a click: Modding, PreGame and GameInfo all work here, while
-- the -Automation Lua state is a bare MainState where even `print` and `pairs` are nil.

local function vpDevSplit(text, sep)
  local out = {};
  for piece in string.gmatch(text or "", "[^" .. sep .. "]+") do
    out[#out + 1] = piece;
  end
  return out;
end

-- Field names differ between the two Modding enumerations: GetActivatedMods yields .ID,
-- GetEnabledModsByActivationOrder yields .ModID. Accept either.
local function vpDevModId(entry)
  return entry.ModID or entry.ID;
end

local function vpDevContains(list, id, version)
  for _, entry in ipairs(list) do
    if (vpDevModId(entry) == id and tonumber(entry.Version) == tonumber(version)) then
      return true;
    end
  end
  return false;
end

local function vpDevWantedMods()
  local wanted = {};
  for _, entry in ipairs(vpDevSplit(modsToEnable, ";")) do
    local id, version = string.match(entry, "^(.-)@(%-?%d+)$");
    if (id == nil) then
      id = entry;
    end
    local v = tonumber(version or "-1") or -1;
    if (v < 0) then
      v = Modding.GetLatestInstalledModVersion(id);
    end
    if (v ~= nil and v ~= -1) then
      wanted[#wanted + 1] = { ModID = id, Version = v };
    else
      print("vp-dll-dev: mod not installed: " .. tostring(id));
    end
  end
  return wanted;
end

-- True when exactly the wanted mods are active. Asked of the engine every time rather
-- than remembered: activating mods swaps the whole UI out, so no state in this file
-- survives it, and the show handler below has to know the answer too.
function vpDevModsAlreadyActive()
  if (modsToEnable == "") then
    return false;
  end
  local wanted = vpDevWantedMods();
  if (#wanted == 0) then
    return false;
  end
  local active = Modding.GetActivatedMods();
  if (active == nil or #active ~= #wanted) then
    return false;
  end
  for _, w in ipairs(wanted) do
    if (not vpDevContains(active, w.ModID, w.Version)) then
      return false;
    end
  end
  return true;
end

-- Returns true when the wanted mods are active and the caller may carry on. A false
-- return means activation was just kicked off; it re-enters through RestoreUI.
local function vpDevEnsureMods()
  if (modsToEnable == "") then
    return true;
  end
  if (vpDevModsAlreadyActive()) then
    print("vp-dll-dev: requested mods are already active");
    return true;
  end
  local wanted = vpDevWantedMods();
  if (#wanted == 0) then
    print("vp-dll-dev: none of the requested mods are installed - not activating anything");
    return false;
  end

  if (modsExclusive) then
    -- Reverse activation order: a dependent mod has to be disabled before the mod it
    -- depends on, or the engine refuses.
    local enabled = Modding.GetEnabledModsByActivationOrder();
    for i = #enabled, 1, -1 do
      local e = enabled[i];
      if (not vpDevContains(wanted, vpDevModId(e), e.Version)) then
        Modding.DisableMod(vpDevModId(e), e.Version);
        print("vp-dll-dev: disabled " .. tostring(vpDevModId(e)) .. "@" .. tostring(e.Version));
      end
    end
  end
  for _, w in ipairs(wanted) do
    Modding.EnableMod(w.ModID, w.Version);
    print("vp-dll-dev: enabled " .. w.ModID .. "@" .. w.Version);
  end

  print("vp-dll-dev: activating " .. #wanted .. " mods - this rebuilds the game database");
  UIManager:SetUICursor( 1 );
  Modding.ActivateEnabledMods();
  UIManager:SetUICursor( 0 );
  -- Activation may have swapped the UI out. Ask for the menu pass again either way,
  -- exactly as the real mods browser does.
  Events.SystemUpdateUI( SystemUpdateUIType.RestoreUI, "MainMenu" );
  return false;
end

-- vp-dll-dev: start a brand-new game from a description, doing what the Advanced Setup
-- screen's own Defaults and Start handlers do. newGameSetup keys, all optional:
--   map=Continents          basename of a map script in GameInfo.MapScripts
--   size=WORLDSIZE_STANDARD ais=7   minors=-1 (-1 = the size's own default)
--   speed=GAMESPEED_STANDARD  era=ERA_ANCIENT  handicap=HANDICAP_PRINCE  maxturns=0
local hasStartedNewGame = false;
function vpDevStartNewGame()
  if (hasStartedNewGame) then
    return;
  end

  local cfg = {};
  for _, piece in ipairs(vpDevSplit(newGameSetup, ";")) do
    local k, v = string.match(piece, "^([^=]+)=(.*)$");
    if (k ~= nil) then
      cfg[k] = v;
    end
  end
  local function opt(key, fallback)
    local v = cfg[key];
    if (v == nil or v == "") then
      return fallback;
    end
    return v;
  end

  local sizeType = opt("size", "WORLDSIZE_STANDARD");
  local world = GameInfo.Worlds[sizeType];
  local mapName = string.lower(opt("map", "Continents"));
  -- Prefer an exact file name: "continents" is also a prefix of other scripts.
  local suffix = "\\" .. mapName .. ".lua";
  local mapScript = nil;
  for row in GameInfo.MapScripts() do
    local f = string.lower(row.FileName or "");
    if (string.sub(f, -string.len(suffix)) == suffix) then
      mapScript = row;
      break;
    end
  end
  if (mapScript == nil) then
    for row in GameInfo.MapScripts() do
      if (string.find(string.lower(row.FileName or ""), mapName, 1, true) ~= nil) then
        mapScript = row;
        break;
      end
    end
  end
  if (world == nil or mapScript == nil) then
    print("vp-dll-dev: cannot start - size [" .. sizeType .. "] or map [" .. mapName .. "] not found");
    return;
  end
  hasStartedNewGame = true;

  PreGame.SetPrivateGame(false);
  PreGame.SetGameType(GameTypes.GAME_SINGLE_PLAYER);
  PreGame.ResetSlots();
  PreGame.ResetGameOptions();
  PreGame.ResetMapOptions();

  -- Quick combat and quick movement are PreGame game options (GAMEOPTION_QUICK_COMBAT /
  -- GAMEOPTION_QUICK_MOVEMENT), not merely the UserSettings.ini mirror, so ResetGameOptions
  -- above has just cleared them however the player had the options screen set. Put them
  -- back from the player's own setting. They matter more than they look: the engine waits
  -- on the combat and movement animations before a turn can end, so leaving them off makes
  -- an unattended autoplay run several times slower for no benefit.
  PreGame.SetQuickCombat(OptionsManager.GetSinglePlayerQuickCombatEnabled());
  PreGame.SetQuickMovement(OptionsManager.GetSinglePlayerQuickMovementEnabled());

  PreGame.SetLoadWBScenario(false);
  PreGame.SetRandomMapScript(false);
  PreGame.SetMapScript(mapScript.FileName);
  PreGame.SetRandomWorldSize(false);
  PreGame.SetWorldSize(world.ID);

  local minors = tonumber(opt("minors", "-1")) or -1;
  if (minors < 0) then
    minors = world.DefaultMinorCivs;
  end
  PreGame.SetNumMinorCivs(minors);

  local speed = GameInfo.GameSpeeds[opt("speed", "GAMESPEED_STANDARD")];
  if (speed ~= nil) then
    PreGame.SetGameSpeed(speed.ID);
  end
  local era = GameInfo.Eras[opt("era", "ERA_ANCIENT")];
  if (era ~= nil) then
    PreGame.SetEra(era.ID);
  end
  local handicap = GameInfo.HandicapInfos[opt("handicap", "HANDICAP_PRINCE")];
  if (handicap ~= nil) then
    PreGame.SetHandicap(0, handicap.ID);
  end
  PreGame.SetMaxTurns(tonumber(opt("maxturns", "0")) or 0);
  for row in GameInfo.Victories() do
    PreGame.SetVictory(row.ID, true);
  end

  -- ResetSlots already made slot 0 the human one; fill the AI slots and close the rest.
  local ais = tonumber(opt("ais", "7")) or 7;
  for i = 0, GameDefines.MAX_MAJOR_CIVS - 1 do
    if (i > 0) then
      if (i <= ais) then
        PreGame.SetSlotStatus(i, SlotStatus.SS_COMPUTER);
      else
        PreGame.SetSlotStatus(i, SlotStatus.SS_CLOSED);
      end
    end
    PreGame.SetCivilization(i, -1);
    PreGame.SetTeam(i, i);
    PreGame.SetLeaderName(i, "");
    PreGame.SetCivilizationDescription(i, "");
    PreGame.SetCivilizationShortDescription(i, "");
    PreGame.SetCivilizationAdjective(i, "");
  end

  -- false, unlike the real Start button: an experiment must not rewrite the settings
  -- the player left sitting in the menu.
  PreGame.SetPersistSettings(false);
  print("vp-dll-dev: starting new game map=" .. mapScript.FileName ..
        " size=" .. sizeType .. " ais=" .. ais .. " minors=" .. minors);
  Events.SerialEventStartGame();
  UIManager:SetUICursor( 1 );
end

-- vp-dll-dev: the single entry point every menu pass goes through.
function vpDevMenuReady()
  if (not vpDevEnsureMods()) then
    return;
  end
  if (newGameSetup ~= "") then
    vpDevStartNewGame();
    return;
  end
  loadSelectedSave();
end

function ShowHideHandler( bIsHide, bIsInit )
    if( not bIsHide ) then
        Controls.Civ5Logo:SetTexture( "CivilzationV_Logo.dds" );
        
        -- This is a catch all to ensure that mods are not activated at this point in the UI.
        -- Also, since certain maps and settings will only be available in either the modding or multiplayer
        -- screen, we want to ensure that "safe" settings are loaded that can be used for either SP, MP or Mods.
        -- Activating the DLC (there doesn't have to be any) will make sure no mods are active and all the user's
        -- purchased content is available
        if (not ContextPtr:IsHotLoad()) then
			UIManager:SetUICursor( 1 );
			-- vp-dll-dev: ActivateDLC() deactivates every mod, which would undo our own
			-- activation on the very next menu pass. Skip it once the wanted set is live.
			if (not vpDevModsAlreadyActive()) then
				Modding.ActivateDLC();
			end
			PreGame.LoadPreGameSettings();
			UIManager:SetUICursor( 0 );
			
			-- Send out an event to continue on, as the ActivateDLC may have swapped out the UI	
			Events.SystemUpdateUI( SystemUpdateUIType.RestoreUI, "MainMenu" );
		end
    else
        Controls.Civ5Logo:UnloadTexture();
    end
end
ContextPtr:SetShowHideHandler( ShowHideHandler );

-------------------------------------------------
-- Event Handler: ConnectedToNetworkHost
-------------------------------------------------

-------------------------------------------------
-- StartGame Button Handler
-------------------------------------------------
function SinglePlayerClick()
	UIManager:QueuePopup( Controls.SinglePlayerScreen, PopupPriority.SinglePlayerScreen );
end
Controls.SinglePlayerButton:RegisterCallback( Mouse.eLClick, SinglePlayerClick );

-------------------------------------------------
-- Multiplayer Button Handler
-------------------------------------------------
function MultiplayerClick()
    UIManager:QueuePopup( Controls.MultiplayerSelectScreen, PopupPriority.MultiplayerSelectScreen );
end
Controls.MultiplayerButton:RegisterCallback( Mouse.eLClick, MultiplayerClick );


-------------------------------------------------
-- Mods button handler
-------------------------------------------------
function ModsButtonClick()
    UIManager:QueuePopup( Controls.ModsEULAScreen, PopupPriority.ModsEULAScreen );
end
Controls.ModsButton:RegisterCallback( Mouse.eLClick, ModsButtonClick );


-------------------------------------------------
-- UITest Button Handler
-------------------------------------------------
--[[
function UITestRClick()
    bHideUITest = not bHideUITest;
    Controls.UITestScreen:SetHide( bHideUITest );
end
Controls.OptionsButton:RegisterCallback( Mouse.eRClick, UITestRClick );
--]]


-------------------------------------------------
-- Options Button Handler
-------------------------------------------------
function OptionsClick()
    UIManager:QueuePopup( Controls.OptionsMenu_FrontEnd, PopupPriority.OptionsMenu );
end
Controls.OptionsButton:RegisterCallback( Mouse.eLClick, OptionsClick );


-------------------------------------------------
-- Hall Of Fame Button Handler
-------------------------------------------------
function OtherClick()
    UIManager:QueuePopup( Controls.Other, PopupPriority.OtherMenu );
end
Controls.OtherButton:RegisterCallback( Mouse.eLClick, OtherClick );


-------------------------------------------------
-- Exit Button Handler
-------------------------------------------------
function OnExitGame()
	Events.UserRequestClose();
end
Controls.ExitButton:RegisterCallback( Mouse.eLClick, OnExitGame );


----------------------------------------------------------------        
----------------------------------------------------------------
Steam.SetOverlayNotificationPosition( "bottom_left" );

-------------------------------------------------
-- Event Handler: MultiplayerGameLaunched
-------------------------------------------------
function OnGameLaunched()

	UIManager:DequeuePopup( ContextPtr );

end
Events.MultiplayerGameLaunched.Add( OnGameLaunched );


-- Returns -1 if time1 < time2, 0 if equal, 1 if time1 > time 2
function CompareTime(time1, time2)
	
	--First, convert the table into a single numerical value
	-- YYYYMMDDHH
	function convert(t)
		local r = 0;
		if(t.year ~= nil) then
			r = r + t.year * 1000000
		end
		
		if(t.month ~= nil) then
			r = r + t.month * 10000
		end
		
		if(t.day ~= nil) then
			r = r + t.day * 100
		end
		
		if(t.hour ~= nil) then
			r = r + t.hour;
		end
		
		return r;
	end
	
	local ct1 = convert(time1);
	local ct2 = convert(time2);
	
	if(ct1 < ct2) then
		return -1;
	elseif(ct1 > ct2) then
		return 1;
	else
		return 0;
	end
end

function DisplayDLCButtons()
	local ButtonsDisplayUntil = {};
	
	if (Controls.MapPack2PromoButton ~= nil) then
		ButtonsDisplayUntil[Controls.MapPack2PromoButton] = {
			start = {
				month = 10,
				day = 2,
				year = 2013,
				hour = 12,
			},
			
			stop = {
				year = 2013,
				month = 11, 
				day = 4,
				hour = 12,
			},		
			
			customurl = "http://store.steampowered.com/app/235584/",
		}
	end
	
	if (Controls.MapPack3PromoButton ~= nil) then
		ButtonsDisplayUntil[Controls.MapPack3PromoButton] = {
			start = {
				month = 11,
				day = 12,
				year = 2013,
				hour = 17,
			},
			
			stop = {
				year = 2013,
				month = 11, 
				day = 25,
				hour = 12,
			},
			
			customurl = "http://store.steampowered.com/app/235585/",
		}
	end
	
	if (Controls.AcePatrolPromoButton ~= nil) then
		ButtonsDisplayUntil[Controls.AcePatrolPromoButton] = {
			start = {
				month = 11,
				day = 5,
				year = 2013,
				hour = 12,
			},
			
			stop = {
				year = 2013,
				month = 11, 
				day = 11,
				hour = 12,
			},
			
			customurl = "http://store.steampowered.com/app/244090/",
		}
	end	
	
	local currentDate = os.date("!*t");

	for k,v in pairs(ButtonsDisplayUntil) do
		local bShow = false;
		
		if(CompareTime(currentDate, v.start) >= 0 and CompareTime(v.stop, currentDate) >= 0) then
			bShow = true;
		end
		
		
		k:SetHide(not bShow);
		
		k:RegisterCallback(Mouse.eLClick, function()
			if(v.customurl == nil) then
				Steam.ActivateGameOverlayToStore();
			else
				Steam.ActivateGameOverlayToWebPage(v.customurl);
			end
		end);
	end
end

DisplayDLCButtons();

----------------------------------------------------------------        
function OnExpansionRulesSwitch()
	UIManager:QueuePopup( Controls.PremiumContentScreen, PopupPriority.OtherMenu );
end		
Controls.ExpansionRulesSwitch:RegisterCallback(Mouse.eLClick, OnExpansionRulesSwitch);

-------------------------------------------------------------------------------
function OnSystemUpdateUI( type, tag  )
    if( type == SystemUpdateUIType.RestoreUI) then
		if (tag == "MainMenu") then
			-- vp-dll-dev: the menu is up and DLC/mods are active - safe to act now.
			vpDevMenuReady();

			-- Look for any cached invite
			UI:CheckForCommandLineInvitation();
			
			if (Network.IsDedicatedServer()) then
					ResetMultiplayerOptions(); 
			    UIManager:QueuePopup( ContextPtr:LookUpControl( "DedicatedServerScreen" ), PopupPriority.LobbyScreen );
			end
		elseif (tag == "StagingRoom") then
			if (UIManager:GetVisibleNamedContext("StagingRoom") == nil) then
				UIManager:QueuePopup( Controls.StagingRoomScreen, PopupPriority.StagingScreen );
			end
		elseif (tag == "ScenariosMenuReset") then			
			local pScenarioScreen = ContextPtr:LookUpControl( "SinglePlayerScreen/ScenariosScreen" );
			if (pScenarioScreen ~= nil) then
				if (pScenarioScreen:IsHidden()) then						
					UIManager:QueuePopup( pScenarioScreen, PopupPriority.GameSetupScreen );
				end
			end
		elseif (tag == "ModsBrowserReset") then
			local pModsMenu = ContextPtr:LookUpControl("ModsEULAScreen/ModsBrowser" );
			if(pModsMenu ~= nil) then
				if(pModsMenu:IsHidden()) then
					UIManager:QueuePopup(pModsMenu, PopupPriority.ModsBrowserScreen);
				end
			end 
		elseif (tag == "ModsMenu" ) then
			local pModsMenu = ContextPtr:LookUpControl("ModsEULAScreen/ModsBrowser/ModsMenu" );
			if(pModsMenu ~= nil) then
				if(pModsMenu:IsHidden()) then
					UIManager:QueuePopup(pModsMenu, PopupPriority.ModsMenuScreen);
				end
			end 
	    end
	elseif (type == SystemUpdateUIType.ReloadUI) then
		-- vp-dll-dev: the UI was swapped out (DLC/mods activating). Try again here.
		vpDevMenuReady();
	end
end

Events.SystemUpdateUI.Add( OnSystemUpdateUI );

-------------------------------------------------------------------------------
if(UI.IsTouchScreenEnabled()) then
	function OnTouchHelpButton()
		Controls.TouchControlsMenu:SetHide( false );
	end		
	Controls.TouchHelpButton:RegisterCallback( Mouse.eLClick, OnTouchHelpButton );
	Controls.TouchHelpButton:SetHide(false);
	OnTouchHelpButton();
else
	Controls.TouchHelpButton:SetHide(true);
	
end

