#include "CvGameCoreDLLPCH.h"
#include "ShellHelperLoader.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace
{

const char MARKER_PATH[] = "crashlogs\\shell.helper";
const char LOG_PATH[] = "crashlogs\\shell.log";

void AppendToLog(const char* szMessage)
{
	SYSTEMTIME kNow;
	GetLocalTime(&kNow);
	char szLine[1400];
	_snprintf_s(szLine, sizeof(szLine), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d  pid %lu  %s\r\n",
		kNow.wYear, kNow.wMonth, kNow.wDay, kNow.wHour, kNow.wMinute, kNow.wSecond, GetCurrentProcessId(), szMessage);

	HANDLE hLog = CreateFileA(LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hLog == INVALID_HANDLE_VALUE)
		return;
	DWORD dwWritten = 0;
	WriteFile(hLog, szLine, static_cast<DWORD>(strlen(szLine)), &dwWritten, NULL);
	CloseHandle(hLog);
}

bool IsBlank(wchar_t c)
{
	return c == L' ' || c == L'\t' || c == L'"' || c == L'\'';
}

//! The marker's first line as a wide string, trimmed. False if the file cannot be read or is empty.
bool ReadHelperPath(wchar_t* wszOut, int iChars)
{
	HANDLE hFile = CreateFileA(MARKER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;
	char acBytes[2048 + 2];
	DWORD dwRead = 0;
	const BOOL bRead = ReadFile(hFile, acBytes, 2048, &dwRead, NULL);
	CloseHandle(hFile);
	if (!bRead)
		return false;
	acBytes[dwRead] = '\0';
	acBytes[dwRead + 1] = '\0';

	int iLen = 0;
	if (dwRead >= 2 && static_cast<unsigned char>(acBytes[0]) == 0xFF && static_cast<unsigned char>(acBytes[1]) == 0xFE)
	{
		const wchar_t* wsz = reinterpret_cast<const wchar_t*>(acBytes + 2);
		const int iAvailable = static_cast<int>((dwRead - 2) / 2);
		for (; iLen < iAvailable && iLen < iChars - 1; iLen++)
			wszOut[iLen] = wsz[iLen];
	}
	else
	{
		const char* sz = acBytes;
		if (dwRead >= 3 && static_cast<unsigned char>(sz[0]) == 0xEF && static_cast<unsigned char>(sz[1]) == 0xBB && static_cast<unsigned char>(sz[2]) == 0xBF)
			sz += 3;
		iLen = MultiByteToWideChar(CP_UTF8, 0, sz, -1, wszOut, iChars);
		if (iLen <= 0)
			return false;
		iLen--;   // the terminator was counted
	}
	wszOut[iLen] = L'\0';

	// First line only, without blanks or quotes around it.
	for (int i = 0; i < iLen; i++)
	{
		if (wszOut[i] == L'\r' || wszOut[i] == L'\n')
		{
			wszOut[i] = L'\0';
			iLen = i;
			break;
		}
	}
	while (iLen > 0 && IsBlank(wszOut[iLen - 1]))
		wszOut[--iLen] = L'\0';
	int iStart = 0;
	while (iStart < iLen && IsBlank(wszOut[iStart]))
		iStart++;
	if (iStart > 0)
		memmove(wszOut, wszOut + iStart, (iLen - iStart + 1) * sizeof(wchar_t));
	return wszOut[0] != L'\0';
}

}	// namespace

namespace ShellHelperLoader
{

void LoadOnce()
{
	static bool s_bTried = false;
	if (s_bTried)
		return;
	s_bTried = true;

	if (GetFileAttributesA(MARKER_PATH) == INVALID_FILE_ATTRIBUTES)
		return;

	wchar_t wszPath[MAX_PATH * 2];
	if (!ReadHelperPath(wszPath, sizeof(wszPath) / sizeof(wszPath[0])))
	{
		AppendToLog("shell.helper present but holds no path; nothing loaded");
		return;
	}

	char szPath[MAX_PATH * 4];
	if (WideCharToMultiByte(CP_UTF8, 0, wszPath, -1, szPath, sizeof(szPath), NULL, NULL) <= 0)
		strcpy_s(szPath, sizeof(szPath), "(path not printable)");

	char szMessage[MAX_PATH * 4 + 128];
	HMODULE hHelper = LoadLibraryW(wszPath);
	const DWORD dwError = GetLastError();
	if (hHelper != NULL)
		_snprintf_s(szMessage, sizeof(szMessage), _TRUNCATE, "loaded %s at 0x%08lx", szPath, reinterpret_cast<unsigned long>(hHelper));
	else
		_snprintf_s(szMessage, sizeof(szMessage), _TRUNCATE, "FAILED to load %s: GetLastError %lu", szPath, dwError);
	AppendToLog(szMessage);
}

}	// namespace ShellHelperLoader
