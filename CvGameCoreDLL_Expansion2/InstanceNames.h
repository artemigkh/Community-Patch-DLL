/*	-------------------------------------------------------------------------------------------------------
	InstanceNames - per-instance names for the instruments' kernel objects.

	Several game processes can run side by side on one machine (one per test instance). The instruments
	talk to the outside through named kernel objects in the session namespace, and with fixed names two
	processes would answer each other's requests. With the environment variable VP_INSTANCE=<id> set for
	the process, every fixed instrument name gets the suffix "_<id>":

		Local\VPLuaExec          ->  Local\VPLuaExec_<id>          (Lua channel request event)
		Local\VPLuaExecDone      ->  Local\VPLuaExecDone_<id>      (Lua channel done event)
		Local\VPMemSnapshot      ->  Local\VPMemSnapshot_<id>      (memory snapshot request event)
		Local\VPMemSnapshotDone  ->  Local\VPMemSnapshotDone_<id>  (memory snapshot done event)
		TurnByTurn               ->  TurnByTurn_<id>               (EXTERNAL_PAUSING mutex)

	<id> must match [A-Za-z0-9_-]{1,32}. Unset keeps the legacy names exactly, so existing tools work
	unchanged; any other value (empty, too long, another character) is rejected and the legacy names are
	used, with a line saying so in the log. The variable is read once per load of this DLL (first call,
	normally from DllGetGameContext), and the effective names are logged once to crashlogs\instruments.log
	(relative to the install folder, like crashlogs\crashes.log; no folder, no log) and to the debugger.

	The harness client (c5-64bit-patch tools/harness/lua.py) builds the same names; its own client mutex
	Local\VPLuaExecClient_<id> lives only on the client side.
	------------------------------------------------------------------------------------------------------- */

#pragma once

#ifndef CIV5_INSTANCE_NAMES_H
#define CIV5_INSTANCE_NAMES_H

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace InstanceNames
{

const DWORD MAX_ID_CHARS = 32;

struct Names
{
	char szId[MAX_ID_CHARS + 1];		// "" for the legacy names
	char szLuaExecRequest[64];
	char szLuaExecDone[64];
	char szMemSnapshotRequest[64];
	char szMemSnapshotDone[64];
	char szTurnByTurn[64];
};

inline bool IsValidId(const char* sz, DWORD dwLength)
{
	if (dwLength < 1 || dwLength > MAX_ID_CHARS)
		return false;
	for (DWORD i = 0; i < dwLength; i++)
	{
		const char c = sz[i];
		const bool bOk = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!bOk)
			return false;
	}
	return true;
}

inline void AppendToLog(const char* szMessage)
{
	SYSTEMTIME kNow;
	GetLocalTime(&kNow);
	char szLine[1024];
	_snprintf_s(szLine, sizeof(szLine), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d  pid %lu  %s\r\n",
		kNow.wYear, kNow.wMonth, kNow.wDay, kNow.wHour, kNow.wMinute, kNow.wSecond, GetCurrentProcessId(), szMessage);
	OutputDebugStringA(szLine);

	HANDLE hLog = CreateFileA("crashlogs\\instruments.log", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hLog == INVALID_HANDLE_VALUE)
		return;
	DWORD dwWritten = 0;
	WriteFile(hLog, szLine, static_cast<DWORD>(strlen(szLine)), &dwWritten, NULL);
	CloseHandle(hLog);
}

inline void Resolve(Names& kNames)
{
	char szValue[MAX_ID_CHARS + 2];		// one more than allowed, to tell "too long" from "exactly 32"
	SetLastError(ERROR_SUCCESS);
	const DWORD dwLength = GetEnvironmentVariableA("VP_INSTANCE", szValue, sizeof(szValue));
	const DWORD dwError = GetLastError();

	char szWhy[96] = "";
	kNames.szId[0] = '\0';
	if (dwLength == 0 && dwError == ERROR_ENVVAR_NOT_FOUND)
		strcpy_s(szWhy, sizeof(szWhy), "VP_INSTANCE unset");
	else if (dwLength >= sizeof(szValue))
		_snprintf_s(szWhy, sizeof(szWhy), _TRUNCATE, "VP_INSTANCE REJECTED (%lu chars; must be 1-32 of [A-Za-z0-9_-])", dwLength - 1);
	else if (!IsValidId(szValue, dwLength))
		_snprintf_s(szWhy, sizeof(szWhy), _TRUNCATE, "VP_INSTANCE REJECTED (%lu chars; must be 1-32 of [A-Za-z0-9_-])", dwLength);
	else
	{
		memcpy(kNames.szId, szValue, dwLength);
		kNames.szId[dwLength] = '\0';
	}

	const char* szSuffix = (kNames.szId[0] != '\0') ? "_" : "";
	_snprintf_s(kNames.szLuaExecRequest, sizeof(kNames.szLuaExecRequest), _TRUNCATE, "Local\\VPLuaExec%s%s", szSuffix, kNames.szId);
	_snprintf_s(kNames.szLuaExecDone, sizeof(kNames.szLuaExecDone), _TRUNCATE, "Local\\VPLuaExecDone%s%s", szSuffix, kNames.szId);
	_snprintf_s(kNames.szMemSnapshotRequest, sizeof(kNames.szMemSnapshotRequest), _TRUNCATE, "Local\\VPMemSnapshot%s%s", szSuffix, kNames.szId);
	_snprintf_s(kNames.szMemSnapshotDone, sizeof(kNames.szMemSnapshotDone), _TRUNCATE, "Local\\VPMemSnapshotDone%s%s", szSuffix, kNames.szId);
	_snprintf_s(kNames.szTurnByTurn, sizeof(kNames.szTurnByTurn), _TRUNCATE, "TurnByTurn%s%s", szSuffix, kNames.szId);

	char szMessage[640];
	if (kNames.szId[0] != '\0')
		_snprintf_s(szMessage, sizeof(szMessage), _TRUNCATE, "instance names: VP_INSTANCE=%s;", kNames.szId);
	else
		_snprintf_s(szMessage, sizeof(szMessage), _TRUNCATE, "instance names: %s, legacy names;", szWhy);
	const size_t uiUsed = strlen(szMessage);
	_snprintf_s(szMessage + uiUsed, sizeof(szMessage) - uiUsed, _TRUNCATE, " %s %s %s %s %s",
		kNames.szLuaExecRequest, kNames.szLuaExecDone, kNames.szMemSnapshotRequest, kNames.szMemSnapshotDone, kNames.szTurnByTurn);
	AppendToLog(szMessage);
}

//! The effective names. The first call reads VP_INSTANCE and logs; later calls return the same names.
//! Safe from any thread: a racing caller waits for the first one to finish.
inline const Names& Get()
{
	static Names s_kNames;				// zero-initialised, no constructor: no hidden init guard
	static volatile LONG s_lState = 0;	// 0 not started, 1 resolving, 2 done
	if (s_lState == 2)
		return s_kNames;
	if (InterlockedCompareExchange(&s_lState, 1, 0) == 0)
	{
		Resolve(s_kNames);
		InterlockedExchange(&s_lState, 2);
	}
	else
	{
		while (s_lState != 2)
			Sleep(0);
	}
	return s_kNames;
}

}	// namespace InstanceNames

#endif // CIV5_INSTANCE_NAMES_H
