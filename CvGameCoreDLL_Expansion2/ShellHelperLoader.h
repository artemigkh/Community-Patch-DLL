/*	-------------------------------------------------------------------------------------------------------
	ShellHelperLoader - load one developer-chosen DLL into the game process, opt-in.

	If crashlogs\shell.helper exists in the install folder (the folder crashes.log is in), its first line
	is taken as the absolute path of a DLL, which is loaded with LoadLibraryW once per process. The
	result - the module's address, or the GetLastError code - is appended to crashlogs\shell.log.
	Nothing else: the helper's own DllMain does whatever it is for. The file may be UTF-8 (with or
	without a BOM) or UTF-16 LE with a BOM; surrounding blanks and quotes are ignored.

	Development tooling only. Whoever can write to the install folder can already replace this DLL.
	------------------------------------------------------------------------------------------------------- */

#pragma once

#ifndef CIV5_SHELL_HELPER_LOADER_H
#define CIV5_SHELL_HELPER_LOADER_H

namespace ShellHelperLoader
{
//! Loads the helper named by crashlogs\shell.helper, if that file exists. Only the first call does
//! anything. Not from DllMain (LoadLibrary under the loader lock); DllGetGameContext is the place.
void LoadOnce();
}

#endif // CIV5_SHELL_HELPER_LOADER_H
