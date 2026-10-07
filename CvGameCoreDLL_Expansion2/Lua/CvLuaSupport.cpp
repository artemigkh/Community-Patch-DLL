/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		CvLuaSupport.cpp
//!  \brief     Private implementation of the Gamecore Lua framework.
//!
//!		This file includes methods for registering game data w/ Lua.
//!
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "CvGameCoreDLLPCH.h"
#include "../MemoryDiagnostics.h"
#include "MemoryHooks.h"
#include "../CvGameCoreDLLPCH.h"
#include "../CustomMods.h"
#include "CvLuaSupport.h"
#include "CvLuaEnums.h"
#include "CvLuaFractal.h"
#include "CvLuaGameInfo.h"
#include "CvLuaMap.h"
#include "CvLuaGame.h"
#include "CvLuaPlayer.h"
#include "CvLuaTeam.h"
#include "CvLuaCity.h"
#include "CvLuaPlot.h"
#include "CvLuaUnit.h"
#include "CvLuaTeamTech.h"
#include "CvLuaDeal.h"
#include "CvLuaArea.h"
#include "CvLuaLeague.h"

#pragma warning(disable:4800 ) //forcing value to bool 'true' or 'false'

//------------------------------------------------------------------------------
// Utility methods
//------------------------------------------------------------------------------
bool luaL_optbool(lua_State* L, int idx, bool bdefault)
{
	if(lua_isnoneornil(L, idx))
	{
		return bdefault;
	}
	else
	{
		return (bool)lua_toboolean(L, idx);
	}
}

//------------------------------------------------------------------------------
void LuaSupport::RegisterScriptData(lua_State* L)
{
	//Register some plain datatables
	CvLuaEnums::Register(L);
	CvLuaGameInfo::Register(L);

	//Register static interfaces
	CvLuaFractal::Register(L);
	CvLuaMap::Register(L);
	CvLuaGame::Register(L);

	//Register Players and Teams - those instances already exist
	CvLuaPlayer::Register(L);
	CvLuaTeam::Register(L);

	//Compat for older mods:
	//Additionally put Player and Team directly into the gamecore table again
	lua_getglobal(L,"LuaTypes");
	lua_getfield(L,-1,"Team");
	lua_setglobal(L,"Team");
	lua_getfield(L,-1,"Player");
	lua_setglobal(L,"Player");
	lua_pop(L,1);

	//Register additional lua types, for which instances will only be created on demand
	CvLuaCity::PushTypeTable(L);
	CvLuaPlot::PushTypeTable(L);
	CvLuaUnit::PushTypeTable(L);
	CvLuaTeamTech::PushTypeTable(L);
	CvLuaDeal::PushTypeTable(L);
	CvLuaArea::PushTypeTable(L);
	CvLuaLeague::PushTypeTable(L);
}

void LuaSupport::InitLuaFramework()
{
	// Before anything else Lua-related: the allocator wrapper only measures what happens
	// after it is installed, so it goes in at the earliest point Lua is known to exist.
	MemoryDiagnostics::InstallLuaAllocHook();

	ICvEngineScriptSystem1* pkScriptSystem = gDLL->GetScriptSystem();
	lua_State* L = pkScriptSystem->CreateLuaThread("VP_LUAAPI");

	lua_pushvalue(L,LUA_REGISTRYINDEX);
	lua_setglobal(L,"REGISTRY");

	const char* luaCommand = ""

"local G = REGISTRY._LOADED._G ;\n"
"local Threads = G.Threads ;\n"
"local IncludeFileList = G.IncludeFileList ;\n"
"local string_find = string.find ;\n"
"local debug_getinfo = debug.getinfo ;\n"
"\n"
"local assert = assert ;\n"
"local coroutine = coroutine ;\n"
"local error = error ;\n"
"local getfenv = G.getfenv ;\n"
"local ipairs = ipairs ;\n"
"local pairs = pairs ;\n"
"local pcall = pcall ;\n"
"local print = print ;\n"
"local rawget = G.rawget ;\n"
"local rawset = G.rawset ;\n"
"local select = select ;\n"
"local setfenv = G.setfenv ;\n"
"local setmetatable = setmetatable ;\n"
"local type = type ;\n"
"local _ENV = getfenv(0) ;\n"
"\n"
"Threads[coroutine.running()] = {} ; -- Prevent my global env from being cleared out by the engine!\n"
"\n"
"local function sandboxedCall( func, ... ) setfenv( 0, getfenv(0), setfenv( 0, getfenv(func) ), pcall( func, ... ) ) end;\n"
"local newThreadCallback = nil ;\n"
"local function setNewThreadCallback( func ) newThreadCallback = ( type(func) == 'function' and func ) or error('Not a function!') end;\n"
"local function setfenv2( func, env, ... ) local ret = getfenv(func) ; setfenv(func, env) ; return ret, ... end;\n"
"local function callWithEnv( func, env, ... )\n"
"	assert( type(func) == 'function', 'First argument to callWithEnv must be a function!' ) ;\n"
"	assert( type(env) == 'table', 'Second argument to callWithEnv must be a table!' ) ;\n"
"	return select( 2, assert( select( 2, setfenv2( func, setfenv2( func, env ), pcall(func, ...) ) ) ) ) ;\n"
"end;\n"
"local function getSource( func )\n"
"	assert( type(func) == 'function', 'Argument must be a function!') ;\n"
"	return debug_getinfo(func).source ;\n"
"end ;\n"
"\n"
"local function getThreadEnvByName( name )\n"
"	for k,v in pairs(Threads) do\n"
"		if v.StateName == name then\n"
"			return v ;\n"
"		end\n"
"	end\n"
"end;\n"
"\n"
"local doesIncludeExist = function( file )\n"
"	local idx = 0-#file ;\n"
"	for _,val in ipairs(IncludeFileList) do\n"
"		if string_find(val,file,idx) then\n"
"			return val ;\n"
"		end\n"
"	end\n"
"	return false ;\n"
"end;\n"
"\n"
"local function copyTableEntries( to, from )\n"
"	for k,v in pairs(from) do \n"
"		to[k] = v ;\n"
"	end\n"
"end;\n"
"\n"
"local extraApi = {\n"
"	setNewThreadCallback = setNewThreadCallback ;\n"
"	rawset = rawset ;\n"
"	rawget = rawget ;\n"
"	newproxy = G.newproxy ;\n"
"	settenv = function( env,... ) local ret = getfenv(0); setfenv(0, env); return ret,... end ;\n"
"	gettenv = function() return getfenv(0) end ;\n"
"	doesIncludeExist = doesIncludeExist ;\n"
"	callWithEnv = callWithEnv ;\n"
"	getSource = getSource ;\n"
"	firaxisGameInfo = G.GameInfo ;\n"
"};\n"
"\n"
"if G.jit then\n"
"	extraApi.jit = true ;\n"
"	extraApi.bit = G.require('bit');\n"
"end\n"
"\n"
"local baseApi = nil ;\n"
"local loaderCoroutine = coroutine.create( function( env )\n"
"	while true do\n"
"		print('################################################################################');\n"
"		print('(Re-)Loading api context!');\n"
"		setfenv(0,env);\n"
"		print('<-Switching context! ContextPtr:', env.ContextPtr);\n"
"		if env.VPUI_loader then\n"
"			print('Releasing previous api context! ContextPtr:', env.VPUI_loader);\n"
"			env.ContextPtr:ReleaseChild(env.VPUI_loader);\n"
"		end\n"
"		local status, result = pcall( env.ContextPtr.LoadNewContext, env.ContextPtr, 'VPUI_loader' );\n"
"		setfenv(0,_ENV);\n"
"		if status then\n"
"			print('New api context created! ContextPtr:' , result or 'NIL?');\n"
"		else\n"
"			print('Failed to create API context! Error-message:');\n"
"			print(result);\n"
"			result = false ;\n"
"		end\n"
"		env.VPUI_loader = result ;\n"
"		print('################################################################################');\n"
"		env = coroutine.yield();\n"
"	end\n"
"end);\n"
"\n"
"local function getBaseApi( reload ) \n"
"	if reload or (not baseApi) then\n"
"		newThreadCallback = nil ;\n"
"		local ttenv = getThreadEnvByName('ToolTips') ;\n"
"		baseApi = { quicktraceback = ttenv.quicktraceback } ;\n"
"		coroutine.resume( loaderCoroutine , ttenv );\n"
"	end\n"
"	return baseApi;\n"
"end;\n"
"\n"
"local GAMECORE_EXPOSED_TO_API = false ;\n"
"local GAMECORE_DELETE_NEXT_CHECK = false ;\n"
"local function validateGamecoreAndApiStatus( delete_next_time )\n"
"	if GAMECORE_DELETE_NEXT_CHECK then\n"
"		G.GameCore = nil ;\n"
"		baseApi = nil ;\n"
"		newThreadCallback = nil ;\n"
"	elseif not GAMECORE_EXPOSED_TO_API then\n"
"		baseApi = nil ;\n"
"		newThreadCallback = nil ;\n"
"	end\n"
"	GAMECORE_EXPOSED_TO_API = true ;\n"
"	GAMECORE_DELETE_NEXT_CHECK = delete_next_time ;\n"
"end;\n"
"\n"
"local function reset()\n"
"	baseApi = nil ;\n"
"	newThreadCallback = nil ;\n"
"	if GAMECORE_EXPOSED_TO_API then\n"
"		G.GameCore = nil ;\n"
"		GAMECORE_EXPOSED_TO_API = false ;\n"
"		GAMECORE_DELETE_NEXT_CHECK = false ;\n"
"	end\n"
"end;\n"
"\n"
"local function pushApi( to )\n"
"	copyTableEntries( to, getBaseApi() );\n"
"	to._ENV = to ;\n"
"	if newThreadCallback then sandboxedCall( newThreadCallback, to ) end;\n"
"end;\n"
"\n"
"local IMPORT_BLACKLIST = {\n"
"	StateName = true ;\n"
"	Events = true ;\n"
"	LuaEvents = true ;\n"
"	GameEvents = true ;\n"
"	Controls = true ;\n"
"	ContextPtr = true ;\n"
"};\n"
"\n"
"local registrationListenerVPUILoader = { __newindex = function( to, key, value )\n"
"	if key == 'PreGame' then\n"
"		setmetatable(to,nil);\n"
"		copyTableEntries(to, extraApi);\n"
"		to.exportedObjects = baseApi ;\n"
"		to.GameCore = G.GameCore ;\n"
"		to._ENV = to ;\n"
"	end\n"
"	rawset(to, key, value);\n"
"	if not IMPORT_BLACKLIST[key] then \n"
"		baseApi[key] = value;\n"
"	end\n"
"end;};\n"
"\n"
"local registrationListenerFinishBeforeControls = { __newindex = function( to, key, value )\n"
"	if key == 'Controls' and not(G.GameCore and G.GameCore.Controls == value) then\n"
"		setmetatable(to,nil);\n"
"		pushApi(to);\n"
"	end\n"
"	rawset(to, key, value);\n"
"end;};\n"
"\n"
"local registrationListenerWaitForPreGame = { __newindex = function( to, key, value )\n"
"	if key == 'PreGame' then\n"
"		setmetatable(to, registrationListenerFinishBeforeControls);\n"
"	end\n"
"end;};\n"
"\n"
"local registrationListenerFinishAfterArtInfo = { __newindex = function( to, key, value )\n"
"	if key == 'ArtInfo' and not(G.GameCore and G.GameCore.ArtInfo == value) then\n"
"		setmetatable(to,nil);\n"
"		pushApi(to);\n"
"	else\n"
"		rawset(to, key, value);\n"
"	end\n"
"end;};\n"
"\n"
"local registrationListenerPotentiallyMatchmakingLib = { __newindex = function( to, key, value )\n"
"	if key == 'Matchmaking' and not(G.GameCore and G.GameCore.Matchmaking == value) then\n"
"		setmetatable(to, registrationListenerWaitForPreGame);\n"
"	else\n"
"		setmetatable(to, registrationListenerFinishAfterArtInfo);\n"
"	end\n"
"end;};\n"
"\n"
"local registrationListenerWaitForEvents = { __newindex = function( to, key, value )\n"
"	if key == 'LuaEvents' or key == 'Events' then\n"
"		rawset(to, key, value);\n"
"	elseif key == 'UI' then\n"
"		setmetatable(to, registrationListenerPotentiallyMatchmakingLib);\n"
"	end\n"
"end;};\n"
"\n"
"local registrationListenerCheckoutType = { __newindex = function( to, key, value )\n"
"	local stn = to.StateName ;\n"
"	setmetatable(to,nil);\n"
"	if stn == nil then -- ParseMapScript or GameCore creation - Have yet to figure out how to handle them efficiently\n"
"		print('VP_LUAAPI: Ignoring exe thread!');\n"
"		rawset(to, key, value);\n"
"	elseif stn == 'VP_LUAAPI' then\n"
"		print('VP_LUAAPI: Ignoring another me?');\n"
"		reset();\n"
"		rawset(to,key,value);\n"
"	elseif stn == 'ToolTips' then\n"
"		print('VP_LUAAPI: Ignoring ToolTips context!');\n"
"		reset();\n"
"		rawset(to, key, value);\n"
"	elseif stn == 'VPUI_loader' then\n"
"		setmetatable(to, registrationListenerVPUILoader);\n"
"		to[key]=value; --don't use rawset here!\n"
"	else\n"
"		if stn == 'Map Script' or stn == 'InGame' then\n"
"			validateGamecoreAndApiStatus(true);\n"
"		elseif stn == 'LoadScreen' or stn == 'WaitingForPlayers' then\n"
"			validateGamecoreAndApiStatus(false);\n"
"		elseif stn == 'FrontEnd' then\n"
"			reset();\n"
"		end\n"
"		setmetatable(to, registrationListenerWaitForEvents);\n"
"	end\n"
"end;};\n"
"\n"
"if getmetatable(Threads) then\n"
"	print('Warning: Metatable for Threads already present!');\n"
"end;\n"
"\n"
"setmetatable(Threads, { __newindex = function( to, thread, env )\n"
"	rawset(to, thread, env);\n"
"	if doesIncludeExist('VPUI_core.lua') then\n"
"		setmetatable(env, registrationListenerCheckoutType);\n"
"	else\n"
"		print('VP_LUAAPI: Shutting down!');\n"
"		setmetatable(to, nil);\n"
"	end\n"
"end;});\n"
"\n"
"print('Embedded script ran without errors!');\n";

	luaL_dostring(L,luaCommand);
	
	pkScriptSystem->FreeLuaThread(L);
}

//------------------------------------------------------------------------------
void LuaSupport::DumpCallStack(lua_State* L, FILogFile* pLog)
{
	CvString szTemp;
    lua_Debug entry;
    int depth = 0; 

	while (lua_getstack(L, depth, &entry))
	{
		szTemp.Format("%s (%d): %s\n", entry.source ? entry.source : "?", entry.currentline, entry.name ? entry.name : "?");
		depth++;

		if (pLog)
			pLog->Msg(szTemp.c_str());
		else
			OutputDebugString(szTemp.c_str());
	}
}

//------------------------------------------------------------------------------
bool LuaSupport::CallHook(ICvEngineScriptSystem1* pkScriptSystem, const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	MEMHOOK_SCOPE(MEMTAG_LUA);
	if (MOD_API_DISABLE_LUA_HOOKS)
		return false;

	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	bool bResult = pkScriptSystem->CallHook(szName, args, value);
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
bool LuaSupport::CallTestAll(ICvEngineScriptSystem1* pkScriptSystem, const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	if (MOD_API_DISABLE_LUA_HOOKS)
		return false;

	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	bool bResult = pkScriptSystem->CallTestAll(szName, args, value);
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
bool LuaSupport::CallTestAny(ICvEngineScriptSystem1* pkScriptSystem, const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	if (MOD_API_DISABLE_LUA_HOOKS)
		return false;

	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	bool bResult = pkScriptSystem->CallTestAny(szName, args, value);
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
bool LuaSupport::CallAccumulator(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, int& value)
{
	if (MOD_API_DISABLE_LUA_HOOKS)
		return false;

	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	bool bResult = pkScriptSystem->CallAccumulator(szName, args, value);
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
bool LuaSupport::CallAccumulator(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, float& value)
{
	if (MOD_API_DISABLE_LUA_HOOKS)
		return false;

	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	bool bResult = pkScriptSystem->CallAccumulator(szName, args, value);
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
//	External Lua execution - see CvLuaSupport.h for the protocol
//------------------------------------------------------------------------------
namespace
{

const char* const LUAEXEC_REQUEST_EVENT = "Local\\VPLuaExec";
const char* const LUAEXEC_DONE_EVENT = "Local\\VPLuaExecDone";
const wchar_t* const LUAEXEC_REQUEST_FILE = L"luaexec_request.lua";
const wchar_t* const LUAEXEC_RESULT_FILE = L"luaexec_result.txt";
const wchar_t* const LUAEXEC_ENABLE_FILE = L"luaexec.enabled";
const DWORD LUAEXEC_MAX_REQUEST_BYTES = 1024 * 1024;
const DWORD LUAEXEC_ENABLE_RECHECK_MS = 2000;

//! The Lua half, compiled per request and called with the registry. It returns the function that runs
//! one chunk: find the target environment by StateName, give the chunk its own environment that reads
//! and writes through to the target (so globals persist there, as they do in FireTuner's console) but
//! captures print, run it under xpcall with a traceback, and render every return value as text - all
//! under size caps, because this runs inside a process that is short of address space. Everything is
//! reached through the real global table, because this thread's own environment is wrapped by
//! VP_LUAAPI's registration listeners. Tested outside the game against the game's own lua51_Win32.dll.
const char* const LUAEXEC_RUNNER =
"local REG, T = ...\n"
"local G = REG._LOADED._G\n"
"local pairs, ipairs, type, tostring, select, pcall = G.pairs, G.ipairs, G.type, G.tostring, G.select, G.pcall\n"
"local rawget, rawset, setfenv, loadstring, xpcall = G.rawget, G.rawset, G.setfenv, G.loadstring, G.xpcall\n"
"local setmetatable = G.setmetatable\n"
"local getmetatable = (G.debug and G.debug.getmetatable) or G.getmetatable\n"
"local tconcat, tsort = G.table.concat, G.table.sort\n"
"local sformat, smatch = G.string.format, G.string.match\n"
"local traceback = G.debug and G.debug.traceback\n"
"\n"
"-- Caps, so that returning _G or printing in a loop cannot freeze the game or exhaust its address space.\n"
"local RESULT_LIMIT = 1048576\n"
"local PRINT_LIMIT = 262144\n"
"\n"
"local persist = rawget(REG, 'VP_LUAEXEC_PERSIST')\n"
"if type(persist) ~= 'table' then persist = {} ; rawset(REG, 'VP_LUAEXEC_PERSIST', persist) end\n"
"\n"
"-- G.Threads maps each Lua thread to its environment. If it is a proxy, its contents sit behind __index.\n"
"local function threadEnvs()\n"
"\tlocal list = {}\n"
"\tlocal threads = G.Threads\n"
"\tif type(threads) == 'table' then\n"
"\t\tfor _, env in pairs(threads) do list[#list + 1] = env end\n"
"\t\tif #list == 0 then\n"
"\t\t\tlocal mt = getmetatable(threads)\n"
"\t\t\tlocal backing = type(mt) == 'table' and rawget(mt, '__index')\n"
"\t\t\tif type(backing) == 'table' then\n"
"\t\t\t\tfor _, env in pairs(backing) do list[#list + 1] = env end\n"
"\t\t\tend\n"
"\t\tend\n"
"\tend\n"
"\treturn list\n"
"end\n"
"\n"
"local function stateNameOf(env)\n"
"\tif type(env) ~= 'table' then return nil end\n"
"\tlocal name = rawget(env, 'StateName')\n"
"\tif name == nil then\n"
"\t\tlocal ok, value = pcall(function() return env.StateName end)\n"
"\t\tif ok then name = value end\n"
"\tend\n"
"\treturn name\n"
"end\n"
"\n"
"local function stateNames()\n"
"\tlocal names, seen = {}, {}\n"
"\tfor _, env in ipairs(threadEnvs()) do\n"
"\t\tlocal name = stateNameOf(env)\n"
"\t\tif name ~= nil then\n"
"\t\t\tname = tostring(name)\n"
"\t\t\tif not seen[name] then seen[name] = true ; names[#names + 1] = name end\n"
"\t\tend\n"
"\tend\n"
"\ttsort(names)\n"
"\treturn names\n"
"end\n"
"\n"
"local function findState(name)\n"
"\tfor _, env in ipairs(threadEnvs()) do\n"
"\t\tif stateNameOf(env) == name then return env end\n"
"\tend\n"
"end\n"
"\n"
"-- The game-core API (Game, Players, Map...) is registered into thread environments, not into _G, so\n"
"-- \"Main\" means the first environment that can see it: this thread's own, then _G, then InGame, then any.\n"
"local function seesGame(env)\n"
"\tif type(env) ~= 'table' then return false end\n"
"\tlocal ok, game = pcall(function() return env.Game end)\n"
"\treturn ok and type(game) == 'table'\n"
"end\n"
"\n"
"local function resolveMain()\n"
"\tif seesGame(T) then return T, 'thread' end\n"
"\tif seesGame(G) then return G, '_G' end\n"
"\tlocal inGame = findState('InGame')\n"
"\tif seesGame(inGame) then return inGame, 'InGame' end\n"
"\tfor _, env in ipairs(threadEnvs()) do\n"
"\t\tif type(env) == 'table' and type(rawget(env, 'Game')) == 'table' then\n"
"\t\t\treturn env, tostring(stateNameOf(env))\n"
"\t\tend\n"
"\tend\n"
"\treturn G, '_G'\n"
"end\n"
"\n"
"local function newRenderer(limit)\n"
"\tlocal used = 0\n"
"\tlocal function render(v, depth, seen)\n"
"\t\tif used > limit then return '<truncated>' end\n"
"\t\tlocal t = type(v)\n"
"\t\tif t == 'table' and not seen[v] and depth > 0 then\n"
"\t\t\tseen[v] = true\n"
"\t\t\tlocal parts = {}\n"
"\t\t\tfor k, val in pairs(v) do\n"
"\t\t\t\tif #parts >= 200 or used > limit then parts[#parts + 1] = '...' ; break end\n"
"\t\t\t\tlocal key = (type(k) == 'string' and smatch(k, '^[%a_][%w_]*$')) and k or ('[' .. render(k, 0, seen) .. ']')\n"
"\t\t\t\tused = used + #key + 5\n"
"\t\t\t\tparts[#parts + 1] = key .. ' = ' .. render(val, depth - 1, seen)\n"
"\t\t\tend\n"
"\t\t\tseen[v] = nil\n"
"\t\t\treturn '{ ' .. tconcat(parts, ', ') .. ' }'\n"
"\t\tend\n"
"\t\tlocal s\n"
"\t\tif t == 'string' then s = sformat('%q', v)\n"
"\t\telseif t == 'table' then s = seen[v] and '<cycle>' or '{...}'\n"
"\t\telse s = tostring(v) end\n"
"\t\tif type(s) ~= 'string' then s = '<' .. t .. '>' end\n"
"\t\tused = used + #s\n"
"\t\treturn s\n"
"\tend\n"
"\treturn render\n"
"end\n"
"\n"
"local function pack(...) return select('#', ...), { ... } end\n"
"\n"
"return function(code, stateName)\n"
"\tlocal base, envName\n"
"\tif stateName == nil or stateName == '' or stateName == 'Main' or stateName == 'Main State' then\n"
"\t\tbase, envName = resolveMain()\n"
"\telseif stateName == '_G' then\n"
"\t\tbase, envName = G, '_G'\n"
"\telse\n"
"\t\tbase, envName = findState(stateName), stateName\n"
"\t\tif not base then\n"
"\t\t\treturn 'ok=0\\tenv=none\\n--- error ---\\nno Lua state named ' .. stateName .. '\\nstates: ' .. tconcat(stateNames(), ', ') .. '\\n'\n"
"\t\tend\n"
"\tend\n"
"\tlocal chunk, err = loadstring(code, '=luaexec')\n"
"\tif not chunk then return 'ok=0\\tenv=' .. envName .. '\\n--- error ---\\n' .. tostring(err) .. '\\n' end\n"
"\n"
"\tlocal printed, printedBytes, printTruncated = {}, 0, false\n"
"\tlocal env = setmetatable({}, { __index = base, __newindex = base })\n"
"\trawset(env, 'print', function(...)\n"
"\t\tif printedBytes > PRINT_LIMIT then printTruncated = true ; return end\n"
"\t\tlocal p = {}\n"
"\t\tfor i = 1, select('#', ...) do\n"
"\t\t\tlocal ok, text = pcall(tostring, (select(i, ...)))\n"
"\t\t\tp[i] = (ok and type(text) == 'string') and text or '<unprintable>'\n"
"\t\tend\n"
"\t\tlocal line = tconcat(p, '\\t')\n"
"\t\tprintedBytes = printedBytes + #line + 1\n"
"\t\tprinted[#printed + 1] = line\n"
"\tend)\n"
"\trawset(env, 'vp', persist)\n"
"\trawset(env, 'States', stateNames)\n"
"\trawset(env, 'G', G)\n"
"\tsetfenv(chunk, env)\n"
"\n"
"\tlocal n, r = pack(xpcall(chunk, function(e)\n"
"\t\tlocal ok, text = pcall(tostring, e)\n"
"\t\ttext = (ok and type(text) == 'string') and text or '<unprintable error>'\n"
"\t\treturn traceback and traceback(text, 2) or text\n"
"\tend))\n"
"\n"
"\tlocal out = {}\n"
"\tif r[1] then\n"
"\t\tout[1] = 'ok=1\\tnret=' .. (n - 1) .. '\\tenv=' .. envName\n"
"\t\tlocal render = newRenderer(RESULT_LIMIT)\n"
"\t\tfor i = 2, n do\n"
"\t\t\tlocal ok, text = pcall(render, r[i], 4, {})\n"
"\t\t\tif not (ok and type(text) == 'string') then\n"
"\t\t\t\ttext = '<render error: ' .. ((type(text) == 'string') and text or '?') .. '>'\n"
"\t\t\tend\n"
"\t\t\tout[#out + 1] = '[' .. (i - 1) .. '] ' .. text\n"
"\t\tend\n"
"\telse\n"
"\t\tout[1] = 'ok=0\\tenv=' .. envName\n"
"\t\tout[2] = '--- error ---'\n"
"\t\tout[3] = (type(r[2]) == 'string') and r[2] or '<unprintable error>'\n"
"\tend\n"
"\tif #printed > 0 or printTruncated then\n"
"\t\tout[#out + 1] = '--- print ---'\n"
"\t\tfor _, line in ipairs(printed) do out[#out + 1] = line end\n"
"\t\tif printTruncated then out[#out + 1] = '<print output truncated at ' .. PRINT_LIMIT .. ' bytes>' end\n"
"\tend\n"
"\treturn tconcat(out, '\\n') .. '\\n'\n"
"end\n";

struct ExternalLuaRequest
{
	const char* pCode;
	size_t uiCodeBytes;
	const char* szState;
	std::string strResult;
	bool bRan;
};

//! Runs inside ICvEngineScriptSystem1::CallCFunction, which holds the engine's Lua lock: the UI runs Lua
//! on the main thread while CvGame::update runs on the game core thread, so calling lua_pcall directly
//! from update() would race it.
int RunExternalLua(lua_State* L)
{
	ExternalLuaRequest* pRequest = static_cast<ExternalLuaRequest*>(lua_touserdata(L, 1));
	const int iTop = lua_gettop(L);

	if (luaL_loadbuffer(L, LUAEXEC_RUNNER, strlen(LUAEXEC_RUNNER), "=VP_LUAEXEC") == 0)
	{
		lua_pushvalue(L, LUA_REGISTRYINDEX);
		lua_pushvalue(L, LUA_GLOBALSINDEX);
		if (lua_pcall(L, 2, 1, 0) == 0 && lua_isfunction(L, -1))
		{
			lua_pushlstring(L, pRequest->pCode, pRequest->uiCodeBytes);
			lua_pushstring(L, pRequest->szState);
			if (lua_pcall(L, 2, 1, 0) == 0 && lua_type(L, -1) == LUA_TSTRING)
			{
				size_t uiLength = 0;
				const char* szResult = lua_tolstring(L, -1, &uiLength);
				pRequest->strResult.assign(szResult, uiLength);
				pRequest->bRan = true;
			}
		}
	}

	if (!pRequest->bRan)
	{
		pRequest->strResult = "ok=0\n--- error ---\nVP_LUAEXEC runner failed: ";
		pRequest->strResult += (lua_type(L, -1) == LUA_TSTRING) ? lua_tostring(L, -1) : "no message";
		pRequest->strResult += "\n";
	}

	lua_settop(L, iTop);
	return 0;
}

//! Opt-in. VP_LUAEXEC=1 enables and VP_LUAEXEC=0 disables for the life of the process; otherwise the
//! channel stays off until luaexec.enabled exists in the cache folder, checked every couple of seconds,
//! so a session can be switched on without restarting the game. A release build never runs outside
//! code by accident.
bool IsExternalLuaEnabled(const wchar_t* wszFolder)
{
	static int s_iState = -1;   // -1 undecided, 0 off for good, 1 on
	static DWORD s_dwLastCheck = 0;
	if (s_iState >= 0)
		return s_iState == 1;

	if (s_dwLastCheck == 0)
	{
		char szValue[8];
		const DWORD dwLength = GetEnvironmentVariableA("VP_LUAEXEC", szValue, sizeof(szValue));
		if (dwLength > 0 && dwLength < sizeof(szValue))
		{
			s_iState = (szValue[0] == '0') ? 0 : 1;
			return s_iState == 1;
		}
	}

	const DWORD dwNow = GetTickCount();
	if (s_dwLastCheck != 0 && dwNow - s_dwLastCheck < LUAEXEC_ENABLE_RECHECK_MS)
		return false;
	s_dwLastCheck = (dwNow == 0) ? 1 : dwNow;

	wchar_t wszEnable[MAX_PATH];
	_snwprintf_s(wszEnable, MAX_PATH, _TRUNCATE, L"%s%s", wszFolder, LUAEXEC_ENABLE_FILE);
	if (GetFileAttributesW(wszEnable) != INVALID_FILE_ATTRIBUTES)
		s_iState = 1;
	return s_iState == 1;
}

}

void LuaSupport::PollExternalLuaRequest()
{
	// The cache folder path is UTF-8. It does not change during a session, so convert it once.
	static wchar_t s_wszFolder[MAX_PATH] = { 0 };
	static bool s_bHaveFolder = false;
	if (!s_bHaveFolder)
	{
		s_bHaveFolder = true;
		const char* szFolder = gDLL->GetCacheFolderPath();
		if (szFolder == NULL || MultiByteToWideChar(CP_UTF8, 0, szFolder, -1, s_wszFolder, MAX_PATH) == 0)
			s_wszFolder[0] = L'\0';
	}

	if (!IsExternalLuaEnabled(s_wszFolder))
		return;

	static HANDLE s_hRequest = NULL;
	static HANDLE s_hDone = NULL;
	if (s_hRequest == NULL)
	{
		// Created rather than opened, so either side may start first.
		s_hRequest = CreateEventA(NULL, FALSE, FALSE, LUAEXEC_REQUEST_EVENT);
		s_hDone = CreateEventA(NULL, FALSE, FALSE, LUAEXEC_DONE_EVENT);
		if (s_hRequest == NULL)
			return;
	}

	if (WaitForSingleObject(s_hRequest, 0) != WAIT_OBJECT_0)
		return;
	// Decisive if a client created the name first as a manual-reset event.
	ResetEvent(s_hRequest);

	wchar_t wszRequestPath[MAX_PATH];
	wchar_t wszResultPath[MAX_PATH];
	_snwprintf_s(wszRequestPath, MAX_PATH, _TRUNCATE, L"%s%s", s_wszFolder, LUAEXEC_REQUEST_FILE);
	_snwprintf_s(wszResultPath, MAX_PATH, _TRUNCATE, L"%s%s", s_wszFolder, LUAEXEC_RESULT_FILE);

	// Read the chunk. Delete-on-close removes exactly the file that was read, never a newer request
	// written after it. No file means a stray signal: answer nothing rather than overwrite a result.
	HANDLE hFile = CreateFileW(wszRequestPath, GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	std::string strRequest;
	bool bHaveRequest = false;
	const DWORD dwSize = GetFileSize(hFile, NULL);
	if (dwSize != INVALID_FILE_SIZE && dwSize <= LUAEXEC_MAX_REQUEST_BYTES)
	{
		strRequest.resize(dwSize);
		DWORD dwRead = 0;
		bHaveRequest = (dwSize == 0) || (ReadFile(hFile, &strRequest[0], dwSize, &dwRead, NULL) && dwRead == dwSize);
	}
	CloseHandle(hFile);

	// Header lines are ordinary Lua comments, so they stay in the chunk and line numbers stay true.
	size_t uiBody = 0;
	if (strRequest.size() >= 3 && static_cast<unsigned char>(strRequest[0]) == 0xEF
		&& static_cast<unsigned char>(strRequest[1]) == 0xBB && static_cast<unsigned char>(strRequest[2]) == 0xBF)
		uiBody = 3;
	std::string strId;
	std::string strState;
	for (size_t uiLine = uiBody; uiLine < strRequest.size() && strRequest.compare(uiLine, 3, "--@") == 0; )
	{
		size_t uiEnd = strRequest.find('\n', uiLine);
		const size_t uiStop = (uiEnd == std::string::npos) ? strRequest.size() : uiEnd;
		std::string strHeader = strRequest.substr(uiLine + 3, uiStop - uiLine - 3);
		if (!strHeader.empty() && strHeader[strHeader.size() - 1] == '\r')
			strHeader.erase(strHeader.size() - 1);
		if (strHeader.compare(0, 3, "id=") == 0)
			strId = strHeader.substr(3);
		else if (strHeader.compare(0, 6, "state=") == 0)
			strState = strHeader.substr(6);
		if (uiEnd == std::string::npos)
			break;
		uiLine = uiEnd + 1;
	}

	ExternalLuaRequest kRequest;
	kRequest.pCode = strRequest.c_str() + uiBody;
	kRequest.uiCodeBytes = strRequest.size() - uiBody;
	kRequest.szState = strState.c_str();
	kRequest.bRan = false;

	const DWORD dwStart = GetTickCount();
	ICvEngineScriptSystem1* pkScriptSystem = gDLL->GetScriptSystem();
	if (!bHaveRequest)
	{
		kRequest.strResult = "ok=0\n--- error ---\nluaexec_request.lua could not be read (or is larger than 1 MB)\n";
	}
	else if (pkScriptSystem == NULL)
	{
		kRequest.strResult = "ok=0\n--- error ---\nno script system\n";
	}
	else
	{
		// The same lock dance as CallHook: the main thread may hold the Lua lock while it waits for the
		// game core lock, so holding the game core lock here would deadlock.
		const bool bHadLock = gDLL->HasGameCoreLock();
		if (bHadLock)
			gDLL->ReleaseGameCoreLock();

		// NEVER CreateLuaThread/FreeLuaThread here. Both add or remove an entry in the engine's thread
		// table outside the Lua lock, and the main thread walks that table with lua_next: the first
		// version of this channel did exactly that per request and killed the game with an unprotected
		// "invalid key to 'next'" on its second request (dump CvMiniDump_20260917_002305). The memory
		// probe's thread is created once, on the main thread at database load, and never freed.
		lua_State* L = MemoryDiagnostics::GetProbeLuaState();
		if (L != NULL)
		{
			const bool bCalled = pkScriptSystem->CallCFunction(L, RunExternalLua, &kRequest);
			if (!bCalled && kRequest.strResult.empty())
				kRequest.strResult = "ok=0\n--- error ---\nCallCFunction failed before the runner produced a result\n";
		}
		else
		{
			kRequest.strResult = "ok=0\n--- error ---\nno persistent Lua thread (MemoryDiagnostics probe) to run on\n";
		}

		if (bHadLock)
			gDLL->GetGameCoreLock();
	}
	if (kRequest.strResult.empty())
		kRequest.strResult = "ok=0\n--- error ---\nno result\n";

	char szNumbers[64];
	_snprintf_s(szNumbers, sizeof(szNumbers), _TRUNCATE, "\tms=%lu\tturn=%d\n",
		static_cast<unsigned long>(GetTickCount() - dwStart), GC.getGame().getGameTurn());
	std::string strOut = "id=" + strId + "\tstate=" + (strState.empty() ? std::string("Main") : strState) + szNumbers;
	strOut += kRequest.strResult;

	// Written in full before done is signalled; the client re-reads until the id and status line appear.
	HANDLE hResult = CreateFileW(wszResultPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hResult != INVALID_HANDLE_VALUE)
	{
		DWORD dwWritten = 0;
		WriteFile(hResult, strOut.data(), static_cast<DWORD>(strOut.size()), &dwWritten, NULL);
		CloseHandle(hResult);
	}

	if (s_hDone != NULL)
		SetEvent(s_hDone);
}
