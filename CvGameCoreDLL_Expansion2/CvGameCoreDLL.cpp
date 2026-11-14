/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvGlobals.h"
#include "ICvDLLUserInterface.h"
#include "Win32/FDebugHelper.h"
#include "CvDllContext.h"
#include "MemoryImports.h"
#include "EngineQueueGuard.h"
#include "EngineAllocTrace.h"
#include "ShellHelperLoader.h"
#include "ViewMaskSync.h"
#include "InstanceNames.h"

// must be included after all other headers
#include "LintFree.h"

//------------------------------------------------------------------------------
extern "C" ICvGameContext1* DllGetGameContext()
{
	// The engine's first call into this DLL, and the earliest point outside the loader lock - which
	// is what MemoryImports needs, since enumerating modules under it can deadlock. Everything the
	// process has allocated up to here is recorded as pre-existing, i.e. the EXE side's.
	MemoryImports::Install();
	// Well before any save is loaded, which is when the EXE's UI message queue can overrun.
	EngineQueueGuard::Install();
	// Opt-in diagnostics (crashlogs\allocTrace.enabled): chains in front of the EXE's allocator import
	// slots, so it must come after MemoryImports, whose thunks it forwards to.
	EngineAllocTrace::Install();
	// Opt-in (crashlogs\shell.helper): load one developer helper DLL into the process.
	ShellHelperLoader::LoadOnce();
	// Reads VP_INSTANCE once and logs the instruments' kernel-object names (crashlogs\instruments.log).
	InstanceNames::Get();
	// Opt-in (crashlogs\viewmask.enabled): the EXE's per-view system mask honours [DisableSystems] (ViewMaskSync.h).
	ViewMaskSync::Install();

	return CvDllGameContext::GetSingleton();
}
//------------------------------------------------------------------------------
BOOL APIENTRY DllMain(HANDLE hModule,
                      DWORD  ul_reason_for_call,
                      LPVOID)
{
	switch(ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		// The DLL is being loaded into the virtual address space of the current process as a result of the process starting up
		OutputDebugString("DLL_PROCESS_ATTACH\n");
		FDebugHelper::GetInstance().LoadSymbols((HMODULE)hModule);
		// set timer precision
		MMRESULT iTimeSet = timeBeginPeriod(1);		// set timeGetTime and sleep resolution to 1 ms, otherwise it's 10-16ms
		DEBUG_VARIABLE(iTimeSet);
		ASSERT(iTimeSet==TIMERR_NOERROR, "failed setting timer resolution to 1 ms");
		CvDllGameContext::InitializeSingleton();
	}
	break;
	case DLL_THREAD_ATTACH:
		OutputDebugString("DLL_THREAD_ATTACH\n");
		break;
	case DLL_THREAD_DETACH:
		OutputDebugString("DLL_THREAD_DETACH\n");
		break;
	case DLL_PROCESS_DETACH:
		OutputDebugString("DLL_PROCESS_DETACH\n");
		EngineAllocTrace::Uninstall();
		timeEndPeriod(1);
		CvDllGameContext::DestroySingleton();
		GC.setDLLIFace(NULL);
		break;
	}

	return TRUE;	// success
}
