#pragma once

#ifndef CV_OUT_OF_PROCESS_INTERFACE_H
#define CV_OUT_OF_PROCESS_INTERFACE_H

// The version of the contract between this DLL and the 64-bit host that loads it in its own process (the build with
// VP_OUT_OF_PROCESS). The host is built against this header and refuses to compile when the number differs from the
// one it expects; at run time it reads the number through the export DllGetOutOfProcessInterfaceVersion and refuses
// to load a DLL with another number.
//
// Raise it by one whenever a change on the DLL side would make a host built for the old number misbehave:
// - the export DllGetGameContext or any interface the host calls through (the ICv* classes and their virtual tables);
// - the functions this DLL imports from the game database and Lua libraries, whose import slots the host rewrites;
// - the shape of a failed check (the call before the ud2) that the host decodes into its error report;
// - what the DLL does under VP_OUT_OF_PROCESS, and the widths of what it writes into saves.
// A change on the host's side alone does not raise it. The 32-bit build does not use this header.
#define VP_OUT_OF_PROCESS_INTERFACE_VERSION 1

#endif // CV_OUT_OF_PROCESS_INTERFACE_H
