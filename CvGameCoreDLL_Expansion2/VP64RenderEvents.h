#pragma once

#ifndef VP64_RENDER_EVENTS_H
#define VP64_RENDER_EVENTS_H

// Messages from the game DLL to a renderer in the same process (the 64-bit out-of-process build).
//
// A message reports a state change that has already happened, for display only. A send site sits after the change it
// reports, passes values (never pointers), computes its arguments without side effects (members and plain getters),
// never waits and reads no answer, so the game plays the same with and without the messages.
//
// The host offers one function, exported by its EXE under the name "VP64RenderEvent"; the DLL looks it up once while it
// loads (VP64RenderEvents.inl). Where the host has no such export, every send does nothing. Everything below the kinds is
// compiled only when the build defines VP64_RENDER_EVENTS; without it the macros expand to nothing.
//
// Kinds: numbers 1000 to 1999. A new kind is added at the end with the meaning of each value; a number is never reused
// and a kind's values never change meaning. The host checks these numbers against its own copy when it is built.
// Version 1: kinds 1000 and 1001. Version 2: adds 1002.
#define VP64_RENDER_EVENTS_VERSION 2
#define VP64_RENDER_EVENTS_FIRST 1000

// A unit's activity (ActivityTypes) changed. plot = the unit's plot index (-1 without a plot), a0 = owner, a1 = unit
// id, a2 = the new activity, a3 = the old one. a3 == a2: the activity as it is when the unit is set up for display (a
// load or a creation), not a change.
#define VP64_RENDER_EVENT_UNIT_ACTIVITY 1000
// A unit was loaded onto or unloaded from a transport (a carrier, a transport ship). plot as above, a0 = owner, a1 =
// unit id, a2 = the transport's unit id (-1: no longer cargo), a3 = the transport's owner (-1: none). Also sent when the
// unit is set up for display while it is cargo.
#define VP64_RENDER_EVENT_UNIT_CARRIER 1001
// A plot's type (PlotTypes) or terrain (TerrainTypes) changed in a way that does not reach the host through the
// plot's layout update: a terrain change, and a plot type change made without bRebuildGraphics. Sent only once the
// graphics are initialised. plot = the plot index, a0 = the new plot type, a1 = the new terrain, a2 = the old plot type,
// a3 = the old terrain. One change can send it twice for the same plot (setPlotType and setTerrainType call each other
// when water turns to land or back).
#define VP64_RENDER_EVENT_PLOT_CHANGED 1002

#define VP64_RENDER_EVENTS_LAST 1999

#if defined(VP64_RENDER_EVENTS)
typedef void (__cdecl* VP64RenderEventFn)(int iKind, int iPlot, int iA0, int iA1, int iA2, int iA3);
// The host's function, or NULL when the host has none (VP64RenderEvents.inl).
extern VP64RenderEventFn g_pfnVP64RenderEvent;
#define VP64_RENDER_EVENTS_ACTIVE() (g_pfnVP64RenderEvent != NULL)
#define VP64_RENDER_EVENT(kind, plot, a0, a1, a2, a3) \
	do { if (g_pfnVP64RenderEvent != NULL) g_pfnVP64RenderEvent((kind), (plot), (a0), (a1), (a2), (a3)); } while (0)
#else
#define VP64_RENDER_EVENTS_ACTIVE() (false)
#define VP64_RENDER_EVENT(kind, plot, a0, a1, a2, a3) do { } while (0)
#endif

#endif // VP64_RENDER_EVENTS_H
