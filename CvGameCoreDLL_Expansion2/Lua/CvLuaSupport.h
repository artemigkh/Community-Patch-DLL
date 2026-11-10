/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		CvLuaSupport.h
//!  \brief     Public header of the Gamecore Lua framework.
//!
//!		This file includes all initial dependencies to adding Lua support to
//!		Gamecore.
//!
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef CVLUASUPPORT_H
#define CVLUASUPPORT_H

// Standard Lua includes
extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
};

// Fireworks Lua utilities
#include <FireWorks/FLua/include/FLua.h>

// Utilities
#include "CvLuaArgsHandle.h"
class ICvEngineScriptSystem1;
class ICvEngineScriptSystemArgs1;

namespace LuaSupport
{

//!	Called to register all game script data into Lua.
void RegisterScriptData(lua_State* L);
//! Setup hooks into the script system
void InitLuaFramework();
//! Dump the Lua callstack to the output stream
void DumpCallStack(lua_State* L, FILogFile* pLog);

bool CallHook(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value);
bool CallTestAll(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value);
bool CallTestAny(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value);
bool CallAccumulator(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, int& value);
bool CallAccumulator(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, float& value);

//	External Lua execution, for development tooling (.claude/skills/civ5-game-ui/scripts/vp_lua.py).
//
//	Runs a Lua chunk supplied by an outside process, in the game's global state or in any UI context's
//	environment chosen by its StateName (InGame, TechTree, ...), and returns the results and anything it
//	printed. It is FireTuner's console without the GUI. Protocol, in the session namespace:
//	  1. write the chunk to luaexec_request.lua in the cache folder (beside stats.db); optional header
//	     lines at the top: "--@id=<token>" (echoed back) and "--@state=<StateName>" (default: Main)
//	  2. set the event Local\VPLuaExec
//	  3. wait on Local\VPLuaExecDone, then read luaexec_result.txt; its first line echoes the id
//	Runs from the top of CvGame::update, so it works whenever a game is loaded - on the player's turn,
//	with any screen open - and never in the front end.
//	OPT-IN: off until luaexec.enabled exists in the cache folder (checked every ~2 s, so it can be switched
//	on mid-session) or VP_LUAEXEC=1 is in the game's environment; VP_LUAEXEC=0 forces it off. Results and
//	captured print output are capped (1 MB / 256 KB) so a careless `return _G` cannot exhaust the game.
void PollExternalLuaRequest();

}

extern bool luaL_optbool(lua_State* L, int idx, bool bdefault);

#endif //CVLUASUPPORT_H
