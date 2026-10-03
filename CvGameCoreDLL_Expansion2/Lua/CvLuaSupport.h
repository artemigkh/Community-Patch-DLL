/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
#pragma once
#ifndef CVLUASUPPORT_H
#define CVLUASUPPORT_H

#include "CvLuaArgsHandle.h"

typedef struct lua_State lua_State;
typedef int(*lua_CFunction)(lua_State*);
typedef void(*lua_CVoidFunc)(lua_State*);

namespace LuaSupport {
	
	void NotifyDatabaseCached();
	void NotifyDllUnload(bool bProcessExit);
	// For every metatable whose __gc is DLL code (the metatable at idx)
	void TrackDllFinalizer(lua_State* L, int idx);
	
	// Can be used to invoke lua functions without actually including lua headers
	void LuaPCall(lua_State* L,lua_CFunction f);
	void LuaPCall(lua_State* L,lua_CVoidFunc f);

	// Called to register all game script data into Lua.
	void RegisterScriptData(lua_State* L);
	
	// Equivalent to what include(script) does in lua
	void RunScript(lua_State* L, const char* script);

	// Fire game events
	bool CallHook(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value);
	bool CallTestAll(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value);
	bool CallTestAny(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value);
	bool CallAccumulator(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, int& value);
	bool CallAccumulator(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, float& value);

	// Every hook releases the game core lock, which lets the UI thread run Lua.
	// While a city changes hands it is briefly inconsistent (population 0, then
	// deleted, then a new half-built city), and UI scripts reading it crash.
	// Inside this scope, fire-and-forget hooks raised on the creating thread are
	// queued with copies of their arguments and run in order when the outermost
	// scope ends. Test and accumulator hooks return values and still run at once.
	class DeferredHookScope
	{
	public:
		DeferredHookScope();
		~DeferredHookScope();
		// Run the queued hooks now (outermost scope only); later hooks run at once.
		void Flush();
	private:
		DeferredHookScope(const DeferredHookScope&);
		DeferredHookScope& operator=(const DeferredHookScope&);
		bool m_bActive;
	};
}

#endif //CVLUASUPPORT_H
