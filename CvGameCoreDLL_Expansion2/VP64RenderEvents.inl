// Included once, at the end of CvGameCoreDLL.cpp, only when the build defines VP64_RENDER_EVENTS (VP64RenderEvents.h).
#include "VP64RenderEvents.h"

VP64RenderEventFn g_pfnVP64RenderEvent = NULL;

namespace
{
// Looks the host's function up once, while the DLL is loaded (a static initialiser runs before DllMain's body).
// GetModuleHandle and GetProcAddress are safe under the loader lock. A host EXE without the export leaves the pointer
// NULL, and every send does nothing.
struct VP64RenderEventsResolve
{
	VP64RenderEventsResolve()
	{
		FARPROC pProc = GetProcAddress(GetModuleHandleW(NULL), "VP64RenderEvent");
		g_pfnVP64RenderEvent = reinterpret_cast<VP64RenderEventFn>(reinterpret_cast<void*>(pProc));
	}
};
VP64RenderEventsResolve s_kVP64RenderEventsResolve;
}

// The host reads this after it loads the DLL, to know that the DLL sends render messages and which kinds. Exported
// here rather than in CvGameCoreDLL.def, which the 32-bit build shares.
extern "C" __declspec(dllexport) unsigned int DllGetRenderEventsVersion()
{
	return VP64_RENDER_EVENTS_VERSION;
}
