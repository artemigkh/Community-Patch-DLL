#include "CvGameCoreDLLPCH.h"
#include "EngineAllocTrace.h"

#include <windows.h>
#include <intrin.h>
#include <string.h>

/*	-------------------------------------------------------------------------------------------------------
	Implementation constraints (see EngineAllocTrace.h for the why):

	  * The thunks run inside the EXE's allocation paths on every thread. They never allocate from the
	    heap and never call back into the EXE; text is formatted by hand into a VirtualAlloc'd buffer and
	    written with WriteFile.
	  * The one lock (a critical section) is a leaf: nothing that takes another lock is called while it
	    is held, and the next allocator in the chain is always called with it released.
	  * Everything is read relative to the EXE's run-time base; the EXE has been seen relocated.
	------------------------------------------------------------------------------------------------------- */

namespace
{

//	----------------------------------------------------------------------------------------------
//	The EXE builds this knows. The two allocators are byte-identical in all three; only where they
//	sit differs. RVAs, i.e. offsets from the module base.
//	----------------------------------------------------------------------------------------------

struct ExeBuild
{
	DWORD dwTimestamp;
	const char* szName;
	DWORD dwNew;           //!< operator new(size, count, file, line, tag, tag2)
	DWORD dwAligned;       //!< aligned new(size, align, file, line, tag, tag2) - FFastVector's FMALLOCALIGNED
	DWORD dwAlignedInner;  //!< its (size, align) helper, called when align != 0
};

const ExeBuild EXE_BUILDS[] =
{
	{ 0x546CD0A8, "DX11",   0x42C090, 0x413A80, 0x413AB0 },
	{ 0x546CCB59, "DX9",    0x41CAE0, 0x41CB80, 0x41CBB0 },
	{ 0x546CD5F8, "Tablet", 0x414CA0, 0x414A10, 0x414A40 },
};

//! operator new: if (!size) size = 1; try the small-block allocator; else malloc(size).
const char NEW_PATTERN[] =
	"56 8b 74 24 08 57 85 f6 75 05 be 01 00 00 00 6a 01 e8 ?? ?? ?? ?? 8b f8 83 c4 04 85 ff 74 1f "
	"80 bf b8 00 00 00 00 75 16 8b c6 e8 ?? ?? ?? ?? 85 c0 74 0b 8b c6 e8 ?? ?? ?? ?? 85 c0 75 0a "
	"56 ff 15 ?? ?? ?? ?? 83 c4 04 5f 5e c3";
const DWORD NEW_MALLOC_SLOT = 65;     //!< operand of "call [malloc]"
const DWORD NEW_RET_MALLOC = 69;      //!< return address of that call

//! aligned new: align ? inner(size, align) : _aligned_malloc(size, 0)
const char ALIGNED_PATTERN[] =
	"8b 44 24 08 85 c0 74 0f 50 8b 44 24 08 50 e8 ?? ?? ?? ?? 83 c4 08 c3 8b 4c 24 04 6a 00 51 "
	"ff 15 ?? ?? ?? ?? 83 c4 08 c3";
const DWORD ALIGNED_CALL_INNER = 15;  //!< rel32 of "call inner"
const DWORD ALIGNED_RET_INNER = 19;
const DWORD ALIGNED_AMALLOC_SLOT = 32;
const DWORD ALIGNED_RET_AMALLOC = 36;

//! inner: try the small-block allocator; else _aligned_malloc(size, align). Pushes ebx, ebp, esi, edi.
const char INNER_PATTERN[] =
	"53 8b 5c 24 0c 55 8b 6c 24 0c 8d 43 ff 56 57 85 c3 75 37 f7 d0 8d 74 2b ff 6a 01 23 f0 e8 ?? ?? ?? ?? "
	"8b f8 83 c4 04 85 ff 74 1f 80 bf b8 00 00 00 00 75 16 8b c6 e8 ?? ?? ?? ?? 85 c0 74 0b 8b c6 "
	"e8 ?? ?? ?? ?? 85 c0 75 0b 53 55 ff 15 ?? ?? ?? ?? 83 c4 08 5f 5e 5d 5b c3";
const DWORD INNER_AMALLOC_SLOT = 78;
const DWORD INNER_RET_AMALLOC = 82;

//	----------------------------------------------------------------------------------------------
//	Settings and state. Written by Install before any slot is patched.
//	----------------------------------------------------------------------------------------------

const DWORD DEFAULT_THRESHOLD = 1024 * 1024;
const DWORD MIN_THRESHOLD = 4096;
const DWORD MAX_FRAMES = 16;          //!< EXE code addresses kept from the stack scan
const DWORD SCAN_WORDS = 384;         //!< how far up the stack to look for them
const DWORD MAX_CODE_RANGES = 8;
const DWORD OUT_BYTES = 64 * 1024;
const DWORD FLUSH_MS = 1000;

typedef void* (__cdecl* MallocFn)(size_t);
typedef void* (__cdecl* ReallocFn)(void*, size_t);
typedef void (__cdecl* FreeFn)(void*);
typedef void* (__cdecl* AlignedMallocFn)(size_t, size_t);

MallocFn g_pfnMalloc = NULL;
ReallocFn g_pfnRealloc = NULL;
FreeFn g_pfnFree = NULL;
AlignedMallocFn g_pfnAlignedMalloc = NULL;
FreeFn g_pfnAlignedFree = NULL;

enum SlotKind
{
	SLOT_MALLOC,
	SLOT_ALIGNED_MALLOC,
	SLOT_REALLOC,
	SLOT_FREE,
	SLOT_ALIGNED_FREE,
	NUM_SLOTS
};

const char* const SLOT_IMPORT_NAMES[NUM_SLOTS] = { "malloc", "_aligned_malloc", "realloc", "free", "_aligned_free" };

DWORD* g_apSlot[NUM_SLOTS];           //!< the EXE's import slots
DWORD g_adwThunk[NUM_SLOTS];          //!< what was put in them
DWORD g_adwPrevious[NUM_SLOTS];       //!< what was there before (MemoryImports' thunk, or msvcr90)

const ExeBuild* g_pBuild = NULL;
DWORD g_dwBase = 0;
DWORD g_dwImageSize = 0;
DWORD g_dwThreshold = DEFAULT_THRESHOLD;
DWORD g_dwRetNewMalloc = 0;           //!< absolute return addresses inside the allocators
DWORD g_dwRetInnerAligned = 0;
DWORD g_dwRetAlignedInner = 0;
DWORD g_dwRetAlignedDirect = 0;

struct CodeRange
{
	DWORD dwBegin;
	DWORD dwBytes;
};
CodeRange g_aCode[MAX_CODE_RANGES];
DWORD g_uiCodeRanges = 0;

bool g_bInstalled = false;
CRITICAL_SECTION g_kLock;

HANDLE g_hLog = INVALID_HANDLE_VALUE;
char* g_pOut = NULL;
DWORD g_dwOutUsed = 0;
DWORD g_dwLastFlush = 0;

volatile LONG g_lSeq = 0;
int g_iLastTurn = -1;

#ifdef ENGINE_ALLOC_TRACE_SELFTEST
//! The offline self-test (experiments/shell-gates/alloc-trace/selftest) maps the real EXE file into a
//! test process and points this at it. Never defined in the game build.
HMODULE g_hSelfTestExe = NULL;
#endif

HMODULE GetExeModule()
{
#ifdef ENGINE_ALLOC_TRACE_SELFTEST
	if (g_hSelfTestExe != NULL)
		return g_hSelfTestExe;
#endif
	return GetModuleHandleA(NULL);
}

//	----------------------------------------------------------------------------------------------
//	Tracked blocks: a fixed table of BUCKETS x WAYS pointers, readable without the lock. Only the
//	thread that owns a block frees it, so a lock-free probe that finds no match is a true miss.
//	----------------------------------------------------------------------------------------------

const DWORD TRACK_BUCKET_BITS = 12;
const DWORD TRACK_BUCKETS = 1u << TRACK_BUCKET_BITS;
const DWORD TRACK_WAYS = 8;
const DWORD TRACK_ENTRIES = TRACK_BUCKETS * TRACK_WAYS;

struct TrackInfo
{
	DWORD dwSize;
	DWORD dwSeq;
	DWORD dwTick;
	DWORD dwCaller;
};

volatile LONG* g_alTrackPtr = NULL;   //!< 0 = empty
TrackInfo* g_aTrackInfo = NULL;
volatile LONG g_lTracked = 0;
DWORD g_dwTrackedBytes = 0;           //!< under the lock
DWORD g_dwUntracked = 0;              //!< blocks that found their bucket full; their frees are not logged

inline DWORD TrackBucket(DWORD dwPtr)
{
	return ((dwPtr * 0x9E3779B1u) >> (32 - TRACK_BUCKET_BITS)) * TRACK_WAYS;
}

//! Entry index of a tracked pointer, or -1. No lock needed.
int FindTracked(DWORD dwPtr)
{
	const DWORD dwFirst = TrackBucket(dwPtr);
	for (DWORD i = 0; i < TRACK_WAYS; i++)
	{
		if (static_cast<DWORD>(g_alTrackPtr[dwFirst + i]) == dwPtr)
			return static_cast<int>(dwFirst + i);
	}
	return -1;
}

//! Under the lock. False when the bucket is full.
bool AddTracked(DWORD dwPtr, const TrackInfo& kInfo)
{
	const DWORD dwFirst = TrackBucket(dwPtr);
	for (DWORD i = 0; i < TRACK_WAYS; i++)
	{
		if (g_alTrackPtr[dwFirst + i] == 0)
		{
			g_aTrackInfo[dwFirst + i] = kInfo;
			InterlockedExchange(&g_alTrackPtr[dwFirst + i], static_cast<LONG>(dwPtr));
			InterlockedIncrement(&g_lTracked);
			g_dwTrackedBytes += kInfo.dwSize;
			return true;
		}
	}
	g_dwUntracked++;
	return false;
}

//! Under the lock. True, with the block's details, if it was tracked.
bool RemoveTracked(DWORD dwPtr, TrackInfo& kOut)
{
	const int iEntry = FindTracked(dwPtr);
	if (iEntry < 0)
		return false;
	kOut = g_aTrackInfo[iEntry];
	InterlockedExchange(&g_alTrackPtr[iEntry], 0);
	InterlockedDecrement(&g_lTracked);
	g_dwTrackedBytes -= kOut.dwSize;
	return true;
}

//	----------------------------------------------------------------------------------------------
//	Text, without the CRT
//	----------------------------------------------------------------------------------------------

struct Line
{
	char ac[1024];
	DWORD dwLen;

	Line() : dwLen(0) {}

	void Char(char c)
	{
		if (dwLen < sizeof(ac))
			ac[dwLen++] = c;
	}
	void Str(const char* sz)
	{
		while (*sz != '\0')
			Char(*sz++);
	}
	void Dec(DWORD dw)
	{
		char acDigits[12];
		int i = 0;
		do
		{
			acDigits[i++] = static_cast<char>('0' + dw % 10);
			dw /= 10;
		} while (dw != 0);
		while (i > 0)
			Char(acDigits[--i]);
	}
	void Int(int i)
	{
		if (i < 0)
		{
			Char('-');
			Dec(static_cast<DWORD>(-(i + 1)) + 1);
		}
		else
			Dec(static_cast<DWORD>(i));
	}
	void Hex(DWORD dw, int iMinDigits = 1)
	{
		static const char HEX_DIGITS[] = "0123456789abcdef";
		Str("0x");
		int iDigits = 8;
		while (iDigits > iMinDigits && ((dw >> ((iDigits - 1) * 4)) & 15) == 0)
			iDigits--;
		for (int i = iDigits - 1; i >= 0; i--)
			Char(HEX_DIGITS[(dw >> (i * 4)) & 15]);
	}
	//! "EXE+0x1b73ed" for an address inside the EXE image, the bare address otherwise.
	void Code(DWORD dwAddress)
	{
		if (dwAddress - g_dwBase < g_dwImageSize)
		{
			Str("EXE+");
			Hex(dwAddress - g_dwBase);
		}
		else
			Hex(dwAddress, 8);
	}
	void Key(const char* szKey)
	{
		Char(' ');
		Str(szKey);
		Char('=');
	}
	void End()
	{
		if (dwLen > sizeof(ac) - 2)
			dwLen = sizeof(ac) - 2;
		ac[dwLen++] = '\r';
		ac[dwLen++] = '\n';
	}
};

void FormatNow(Line& kLine)
{
	SYSTEMTIME kNow;
	GetLocalTime(&kNow);
	kLine.Dec(kNow.wYear);
	kLine.Char('-');
	if (kNow.wMonth < 10) kLine.Char('0');
	kLine.Dec(kNow.wMonth);
	kLine.Char('-');
	if (kNow.wDay < 10) kLine.Char('0');
	kLine.Dec(kNow.wDay);
	kLine.Char(' ');
	if (kNow.wHour < 10) kLine.Char('0');
	kLine.Dec(kNow.wHour);
	kLine.Char(':');
	if (kNow.wMinute < 10) kLine.Char('0');
	kLine.Dec(kNow.wMinute);
	kLine.Char(':');
	if (kNow.wSecond < 10) kLine.Char('0');
	kLine.Dec(kNow.wSecond);
}

//! Under the lock.
void FlushLocked()
{
	if (g_dwOutUsed != 0 && g_hLog != INVALID_HANDLE_VALUE)
	{
		DWORD dwWritten = 0;
		WriteFile(g_hLog, g_pOut, g_dwOutUsed, &dwWritten, NULL);
	}
	g_dwOutUsed = 0;
	g_dwLastFlush = GetTickCount();
}

//! Under the lock.
void WriteLocked(const Line& kLine, bool bFlushNow)
{
	if (g_pOut == NULL)
		return;
	if (g_dwOutUsed + kLine.dwLen > OUT_BYTES)
		FlushLocked();
	memcpy(g_pOut + g_dwOutUsed, kLine.ac, kLine.dwLen);
	g_dwOutUsed += kLine.dwLen;
	if (bFlushNow || g_dwOutUsed > OUT_BYTES / 2 || GetTickCount() - g_dwLastFlush > FLUSH_MS)
		FlushLocked();
}

//	----------------------------------------------------------------------------------------------
//	Who asked: decoded from the allocator's frame where possible, plus EXE code on the stack
//	----------------------------------------------------------------------------------------------

enum Api
{
	API_MALLOC,         //!< an EXE caller of malloc
	API_NEW,            //!< the engine's operator new
	API_ALIGNED_MALLOC, //!< an EXE caller of _aligned_malloc
	API_ALIGNED_NEW,    //!< FFastVector's aligned new
	API_REALLOC,
	API_FREE,
	API_ALIGNED_FREE,
};

const char* const API_NAMES[] = { "malloc", "new", "amalloc", "anew", "realloc", "free", "afree" };

struct CallInfo
{
	Api eApi;
	DWORD dwCaller;
	bool bArgs;           //!< the allocator's arguments below were read from its frame
	DWORD dwArg2;         //!< count for new, alignment for anew/amalloc
	DWORD dwFile;
	DWORD dwLine;
	DWORD dwTag;
	DWORD dwTag2;
	DWORD dwFrames;
	DWORD adwFrames[MAX_FRAMES];
};

bool IsExeCode(DWORD dwAddress)
{
	for (DWORD i = 0; i < g_uiCodeRanges; i++)
	{
		if (dwAddress - g_aCode[i].dwBegin < g_aCode[i].dwBytes)
			return true;
	}
	return false;
}

//! The EXE is built without frame pointers, so no stack walk: keep the words that point into its
//! code. Some are stale leftovers of earlier calls; whoever reads the log has to judge.
void ScanStack(const DWORD* pFrom, CallInfo& kInfo)
{
	const DWORD* pStackEnd = reinterpret_cast<const DWORD*>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
	kInfo.dwFrames = 0;
	for (DWORD i = 0; i < SCAN_WORDS && pFrom + i < pStackEnd && kInfo.dwFrames < MAX_FRAMES; i++)
	{
		if (IsExeCode(pFrom[i]))
			kInfo.adwFrames[kInfo.dwFrames++] = pFrom[i];
	}
}

//! pRet is the thunk's own return-address slot; pRet[1..] are its arguments.
void DescribeCall(const DWORD* pRet, Api eDirectApi, CallInfo& kInfo)
{
	const DWORD* pArgs = NULL;   // the allocator's (size, arg2, file, line, tag, tag2), if found
	const DWORD* pScan = pRet + 1;
	kInfo.eApi = eDirectApi;
	kInfo.dwCaller = pRet[0];
	kInfo.bArgs = false;

	if (eDirectApi == API_MALLOC && pRet[0] == g_dwRetNewMalloc)
	{
		// [ret][size][edi][esi][caller][size count file line tag tag2]
		kInfo.eApi = API_NEW;
		kInfo.dwCaller = pRet[4];
		pArgs = pRet + 5;
	}
	else if (eDirectApi == API_ALIGNED_MALLOC && pRet[0] == g_dwRetInnerAligned)
	{
		// [ret][size align][edi esi ebp ebx][ret into aligned new][size align][caller][size align file line tag tag2]
		kInfo.eApi = API_ALIGNED_NEW;
		if (pRet[7] == g_dwRetAlignedInner)
		{
			kInfo.dwCaller = pRet[10];
			pArgs = pRet + 11;
		}
		else
		{
			kInfo.dwCaller = pRet[7];   // someone else calls the helper; no file or line to read
			pScan = pRet + 8;
		}
	}
	else if (eDirectApi == API_ALIGNED_MALLOC && pRet[0] == g_dwRetAlignedDirect)
	{
		// [ret][size 0][caller][size align file line tag tag2]
		kInfo.eApi = API_ALIGNED_NEW;
		kInfo.dwCaller = pRet[3];
		pArgs = pRet + 4;
	}

	if (pArgs != NULL)
	{
		kInfo.bArgs = true;
		kInfo.dwArg2 = pArgs[1];
		kInfo.dwFile = pArgs[2];
		kInfo.dwLine = pArgs[3];
		kInfo.dwTag = pArgs[4];
		kInfo.dwTag2 = pArgs[5];
		pScan = pArgs + 6;
	}
	ScanStack(pScan, kInfo);
}

//! The allocator's file argument, if it really is a source path in the EXE image. 176 call sites of
//! operator new pass only the size, so whatever is in the slot then is the caller's own stack.
void AppendFile(Line& kLine, DWORD dwFile)
{
	if (dwFile - g_dwBase >= g_dwImageSize)
		return;
	const char* sz = reinterpret_cast<const char*>(dwFile);
	DWORD dwLen = 0;
	bool bSlash = false;
	while (dwLen < 200 && dwFile + dwLen - g_dwBase < g_dwImageSize && sz[dwLen] != '\0')
	{
		const char c = sz[dwLen];
		if (c < 0x20 || c > 0x7e)
			return;
		if (c == '\\' || c == '/')
			bSlash = true;
		dwLen++;
	}
	if (dwLen < 3 || dwLen >= 200 || !bSlash)
		return;

	kLine.Key("file");
	for (DWORD i = 0; i < dwLen; i++)
		kLine.Char(sz[i] == ' ' ? '_' : sz[i]);
}

void AppendCall(Line& kLine, const CallInfo& kInfo, bool bArgs)
{
	kLine.Key("caller");
	kLine.Code(kInfo.dwCaller);
	if (bArgs && kInfo.bArgs)
	{
		const DWORD dwBefore = kLine.dwLen;
		AppendFile(kLine, kInfo.dwFile);
		if (kLine.dwLen != dwBefore)
		{
			kLine.Key("line");
			kLine.Dec(kInfo.dwLine);
			kLine.Key("tag");
			kLine.Hex(kInfo.dwTag);
			kLine.Key("tag2");
			kLine.Hex(kInfo.dwTag2);
		}
		kLine.Key(kInfo.eApi == API_NEW ? "count" : "align");
		kLine.Dec(kInfo.dwArg2);
	}
	kLine.Key("stack");
	for (DWORD i = 0; i < kInfo.dwFrames; i++)
	{
		if (i != 0)
			kLine.Char(',');
		kLine.Code(kInfo.adwFrames[i]);
	}
	if (kInfo.dwFrames == 0)
		kLine.Char('-');
}

void BeginEvent(Line& kLine, char cKind, DWORD dwSeq, Api eApi)
{
	kLine.Char(cKind);
	kLine.Key("seq");
	kLine.Dec(dwSeq);
	kLine.Key("tick");
	kLine.Dec(GetTickCount());
	kLine.Key("tid");
	kLine.Dec(GetCurrentThreadId());
	kLine.Key("api");
	kLine.Str(API_NAMES[eApi]);
}

//! A new block (or a failed request): log it and start tracking it.
void RecordAllocation(const CallInfo& kInfo, DWORD dwSize, void* pBlock)
{
	const DWORD dwSeq = static_cast<DWORD>(InterlockedIncrement(&g_lSeq));
	const DWORD dwPtr = reinterpret_cast<DWORD>(pBlock);
	Line kLine;
	BeginEvent(kLine, 'A', dwSeq, kInfo.eApi);
	kLine.Key("size");
	kLine.Dec(dwSize);
	kLine.Key("ptr");
	kLine.Hex(dwPtr, 8);
	if (pBlock == NULL)
		kLine.Str(" fail=1");
	AppendCall(kLine, kInfo, true);

	TrackInfo kTrack;
	kTrack.dwSize = dwSize;
	kTrack.dwSeq = dwSeq;
	kTrack.dwTick = GetTickCount();
	kTrack.dwCaller = kInfo.dwCaller;

	EnterCriticalSection(&g_kLock);
	if (pBlock != NULL && !AddTracked(dwPtr, kTrack))
		kLine.Str(" untracked=1");   // never written: the line is ended below
	kLine.End();
	WriteLocked(kLine, pBlock == NULL);   // a failure may be the last thing this process does
	LeaveCriticalSection(&g_kLock);
}

//	----------------------------------------------------------------------------------------------
//	The thunks. Each forwards to whatever the slot held before (MemoryImports' thunk, or msvcr90).
//	----------------------------------------------------------------------------------------------

__declspec(noinline) void* __cdecl TraceMalloc(size_t uiSize)
{
	if (uiSize < g_dwThreshold)
	{
		void* pBlock = g_pfnMalloc(uiSize);
		if (pBlock != NULL || uiSize == 0)
			return pBlock;
		CallInfo kInfo;
		DescribeCall(static_cast<const DWORD*>(_AddressOfReturnAddress()), API_MALLOC, kInfo);
		RecordAllocation(kInfo, static_cast<DWORD>(uiSize), NULL);
		return NULL;
	}

	// Read the frame before the call: the callee owns its argument slots.
	CallInfo kInfo;
	DescribeCall(static_cast<const DWORD*>(_AddressOfReturnAddress()), API_MALLOC, kInfo);
	void* pBlock = g_pfnMalloc(uiSize);
	RecordAllocation(kInfo, static_cast<DWORD>(uiSize), pBlock);
	return pBlock;
}

__declspec(noinline) void* __cdecl TraceAlignedMalloc(size_t uiSize, size_t uiAlign)
{
	if (uiSize < g_dwThreshold)
	{
		void* pBlock = g_pfnAlignedMalloc(uiSize, uiAlign);
		if (pBlock != NULL || uiSize == 0)
			return pBlock;
		CallInfo kInfo;
		DescribeCall(static_cast<const DWORD*>(_AddressOfReturnAddress()), API_ALIGNED_MALLOC, kInfo);
		RecordAllocation(kInfo, static_cast<DWORD>(uiSize), NULL);
		return NULL;
	}

	CallInfo kInfo;
	DescribeCall(static_cast<const DWORD*>(_AddressOfReturnAddress()), API_ALIGNED_MALLOC, kInfo);
	void* pBlock = g_pfnAlignedMalloc(uiSize, uiAlign);
	RecordAllocation(kInfo, static_cast<DWORD>(uiSize), pBlock);
	return pBlock;
}

//! Shared by both frees. Takes the block out of the table BEFORE it is released, so the address cannot
//! be handed to another thread and tracked again before this entry is gone.
void RecordFree(void* pBlock, Api eApi, const DWORD* pRet)
{
	const DWORD dwPtr = reinterpret_cast<DWORD>(pBlock);
	if (pBlock == NULL || g_lTracked == 0 || FindTracked(dwPtr) < 0)
		return;

	CallInfo kInfo;
	DescribeCall(pRet, eApi, kInfo);

	EnterCriticalSection(&g_kLock);
	TrackInfo kTrack;
	if (RemoveTracked(dwPtr, kTrack))
	{
		Line kLine;
		BeginEvent(kLine, 'F', kTrack.dwSeq, eApi);
		kLine.Key("size");
		kLine.Dec(kTrack.dwSize);
		kLine.Key("ptr");
		kLine.Hex(dwPtr, 8);
		kLine.Key("age");
		kLine.Dec(GetTickCount() - kTrack.dwTick);
		AppendCall(kLine, kInfo, false);
		kLine.End();
		WriteLocked(kLine, false);
	}
	LeaveCriticalSection(&g_kLock);
}

__declspec(noinline) void __cdecl TraceFree(void* pBlock)
{
	RecordFree(pBlock, API_FREE, static_cast<const DWORD*>(_AddressOfReturnAddress()));
	g_pfnFree(pBlock);
}

__declspec(noinline) void __cdecl TraceAlignedFree(void* pBlock)
{
	RecordFree(pBlock, API_ALIGNED_FREE, static_cast<const DWORD*>(_AddressOfReturnAddress()));
	g_pfnAlignedFree(pBlock);
}

__declspec(noinline) void* __cdecl TraceRealloc(void* pOld, size_t uiSize)
{
	const DWORD dwOld = reinterpret_cast<DWORD>(pOld);
	const bool bOldTracked = pOld != NULL && g_lTracked != 0 && FindTracked(dwOld) >= 0;
	if (!bOldTracked && uiSize < g_dwThreshold)
		return g_pfnRealloc(pOld, uiSize);

	CallInfo kInfo;
	DescribeCall(static_cast<const DWORD*>(_AddressOfReturnAddress()), API_REALLOC, kInfo);

	// Out of the table first, for the same reason as a free; put back if the realloc fails.
	TrackInfo kOld;
	bool bHadOld = false;
	if (bOldTracked)
	{
		EnterCriticalSection(&g_kLock);
		bHadOld = RemoveTracked(dwOld, kOld);
		LeaveCriticalSection(&g_kLock);
	}

	void* pNew = g_pfnRealloc(pOld, uiSize);
	const DWORD dwNew = reinterpret_cast<DWORD>(pNew);
	const bool bFailed = (pNew == NULL && uiSize != 0);
	const DWORD dwSeq = static_cast<DWORD>(InterlockedIncrement(&g_lSeq));

	Line kLine;
	BeginEvent(kLine, 'R', dwSeq, API_REALLOC);
	kLine.Key("size");
	kLine.Dec(static_cast<DWORD>(uiSize));
	kLine.Key("ptr");
	kLine.Hex(dwNew, 8);
	kLine.Key("old");
	kLine.Hex(dwOld, 8);
	if (bHadOld)
	{
		kLine.Key("oldseq");
		kLine.Dec(kOld.dwSeq);
		kLine.Key("oldsize");
		kLine.Dec(kOld.dwSize);
	}
	if (bFailed)
		kLine.Str(" fail=1");
	AppendCall(kLine, kInfo, false);

	EnterCriticalSection(&g_kLock);
	if (bFailed && bHadOld)
		AddTracked(dwOld, kOld);   // the old block is still there
	else if (pNew != NULL && uiSize >= g_dwThreshold)
	{
		TrackInfo kNew;
		kNew.dwSize = static_cast<DWORD>(uiSize);
		kNew.dwSeq = dwSeq;
		kNew.dwTick = GetTickCount();
		kNew.dwCaller = kInfo.dwCaller;
		if (!AddTracked(dwNew, kNew))
			kLine.Str(" untracked=1");
	}
	kLine.End();
	WriteLocked(kLine, bFailed);
	LeaveCriticalSection(&g_kLock);
	return pNew;
}

//	----------------------------------------------------------------------------------------------
//	Verification and install
//	----------------------------------------------------------------------------------------------

int HexNibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

//! True if the bytes at pCode match the pattern; "??" matches anything.
bool MatchesPattern(const unsigned char* pCode, const char* szPattern)
{
	for (const char* p = szPattern; *p != '\0';)
	{
		if (*p == ' ')
		{
			p++;
			continue;
		}
		if (p[1] == '\0')
			return false;
		if (p[0] != '?' || p[1] != '?')
		{
			const int iHigh = HexNibble(p[0]);
			const int iLow = HexNibble(p[1]);
			if (iHigh < 0 || iLow < 0 || *pCode != static_cast<unsigned char>(iHigh * 16 + iLow))
				return false;
		}
		pCode++;
		p += 2;
	}
	return true;
}

//! The EXE's MSVCR90 import slot for one function, or NULL.
DWORD* FindImportSlot(const IMAGE_NT_HEADERS* pNt, const char* szFunction)
{
	const IMAGE_DATA_DIRECTORY& kDir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (kDir.VirtualAddress == 0 || kDir.Size == 0)
		return NULL;

	char* pBase = reinterpret_cast<char*>(g_dwBase);
	const IMAGE_IMPORT_DESCRIPTOR* pImport = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(pBase + kDir.VirtualAddress);
	for (; pImport->Name != 0; pImport++)
	{
		if (_stricmp(pBase + pImport->Name, "MSVCR90.dll") != 0 || pImport->OriginalFirstThunk == 0 || pImport->FirstThunk == 0)
			continue;

		const IMAGE_THUNK_DATA* pName = reinterpret_cast<const IMAGE_THUNK_DATA*>(pBase + pImport->OriginalFirstThunk);
		IMAGE_THUNK_DATA* pIat = reinterpret_cast<IMAGE_THUNK_DATA*>(pBase + pImport->FirstThunk);
		for (; pName->u1.AddressOfData != 0; pName++, pIat++)
		{
			if (IMAGE_SNAP_BY_ORDINAL(pName->u1.Ordinal))
				continue;
			const IMAGE_IMPORT_BY_NAME* pByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(pBase + pName->u1.AddressOfData);
			if (strcmp(reinterpret_cast<const char*>(pByName->Name), szFunction) == 0)
				return reinterpret_cast<DWORD*>(&pIat->u1.Function);
		}
	}
	return NULL;
}

DWORD ReadDword(DWORD dwRva)
{
	return *reinterpret_cast<const DWORD*>(g_dwBase + dwRva);
}

//! Checks everything the thunks rely on. NULL if they may be installed, or the reason not.
const char* VerifyExe()
{
	const IMAGE_DOS_HEADER* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_dwBase);
	if (pDos == NULL || pDos->e_magic != IMAGE_DOS_SIGNATURE)
		return "off: no EXE image";
	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_dwBase + pDos->e_lfanew);
	if (pNt->Signature != IMAGE_NT_SIGNATURE)
		return "off: no EXE image";
	g_dwImageSize = pNt->OptionalHeader.SizeOfImage;

	for (size_t i = 0; i < sizeof(EXE_BUILDS) / sizeof(EXE_BUILDS[0]); i++)
	{
		if (EXE_BUILDS[i].dwTimestamp == pNt->FileHeader.TimeDateStamp)
			g_pBuild = &EXE_BUILDS[i];
	}
	if (g_pBuild == NULL)
		return "off: not an EXE build this was written for (timestamp)";

	const DWORD dwLimit = g_dwImageSize - 128;
	if (g_pBuild->dwNew > dwLimit || g_pBuild->dwAligned > dwLimit || g_pBuild->dwAlignedInner > dwLimit)
		return "off: image smaller than expected";
	if (!MatchesPattern(reinterpret_cast<const unsigned char*>(g_dwBase + g_pBuild->dwNew), NEW_PATTERN)
		|| !MatchesPattern(reinterpret_cast<const unsigned char*>(g_dwBase + g_pBuild->dwAligned), ALIGNED_PATTERN)
		|| !MatchesPattern(reinterpret_cast<const unsigned char*>(g_dwBase + g_pBuild->dwAlignedInner), INNER_PATTERN))
		return "off: EXE allocator code differs from the known layout";

	// The relative call from aligned new must land on the helper.
	const DWORD dwCallTarget = g_pBuild->dwAligned + ALIGNED_RET_INNER + ReadDword(g_pBuild->dwAligned + ALIGNED_CALL_INNER);
	if (dwCallTarget != g_pBuild->dwAlignedInner)
		return "off: aligned new does not call the expected helper";

	for (int i = 0; i < NUM_SLOTS; i++)
	{
		g_apSlot[i] = FindImportSlot(pNt, SLOT_IMPORT_NAMES[i]);
		if (g_apSlot[i] == NULL)
			return "off: an MSVCR90 allocator import is missing";
	}
	// The allocators must call through exactly these slots (relocated absolute operands).
	if (ReadDword(g_pBuild->dwNew + NEW_MALLOC_SLOT) != reinterpret_cast<DWORD>(g_apSlot[SLOT_MALLOC])
		|| ReadDword(g_pBuild->dwAligned + ALIGNED_AMALLOC_SLOT) != reinterpret_cast<DWORD>(g_apSlot[SLOT_ALIGNED_MALLOC])
		|| ReadDword(g_pBuild->dwAlignedInner + INNER_AMALLOC_SLOT) != reinterpret_cast<DWORD>(g_apSlot[SLOT_ALIGNED_MALLOC]))
		return "off: allocators do not call the import slots found by name";

	// Code ranges for the stack scan.
	const IMAGE_SECTION_HEADER* pSection = IMAGE_FIRST_SECTION(pNt);
	for (WORD i = 0; i < pNt->FileHeader.NumberOfSections && g_uiCodeRanges < MAX_CODE_RANGES; i++, pSection++)
	{
		if ((pSection->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || pSection->Misc.VirtualSize == 0)
			continue;
		g_aCode[g_uiCodeRanges].dwBegin = g_dwBase + pSection->VirtualAddress;
		g_aCode[g_uiCodeRanges].dwBytes = pSection->Misc.VirtualSize;
		g_uiCodeRanges++;
	}

	g_dwRetNewMalloc = g_dwBase + g_pBuild->dwNew + NEW_RET_MALLOC;
	g_dwRetInnerAligned = g_dwBase + g_pBuild->dwAlignedInner + INNER_RET_AMALLOC;
	g_dwRetAlignedInner = g_dwBase + g_pBuild->dwAligned + ALIGNED_RET_INNER;
	g_dwRetAlignedDirect = g_dwBase + g_pBuild->dwAligned + ALIGNED_RET_AMALLOC;
	return NULL;
}

//! "1048576", "1M", "512K", "0x100000"; 0 if there is no number.
DWORD ParseSize(const char* sz)
{
	while (*sz != '\0' && (*sz < '0' || *sz > '9'))
		sz++;
	DWORD dwValue = 0;
	if (sz[0] == '0' && (sz[1] == 'x' || sz[1] == 'X'))
	{
		sz += 2;
		while (HexNibble(*sz) >= 0)
			dwValue = dwValue * 16 + static_cast<DWORD>(HexNibble(*sz++));
	}
	else
	{
		while (*sz >= '0' && *sz <= '9')
			dwValue = dwValue * 10 + static_cast<DWORD>(*sz++ - '0');
	}
	while (*sz == ' ')
		sz++;
	if (*sz == 'k' || *sz == 'K')
		dwValue *= 1024;
	else if (*sz == 'm' || *sz == 'M')
		dwValue *= 1024 * 1024;
	return dwValue;
}

//! Asked for? Fills in the threshold from VP_ALLOCTRACE_THRESHOLD or the marker file's content.
bool Requested()
{
	char szValue[64] = { 0 };
	bool bOn = false;
	if (GetEnvironmentVariableA("VP_ALLOCTRACE", szValue, sizeof(szValue)) != 0)
	{
		if (szValue[0] == '0')
			return false;
		bOn = true;
	}

	char szContent[64] = { 0 };
	HANDLE hMarker = CreateFileA("crashlogs\\allocTrace.enabled", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hMarker != INVALID_HANDLE_VALUE)
	{
		bOn = true;
		DWORD dwRead = 0;
		ReadFile(hMarker, szContent, sizeof(szContent) - 1, &dwRead, NULL);
		szContent[dwRead < sizeof(szContent) ? dwRead : sizeof(szContent) - 1] = '\0';
		CloseHandle(hMarker);
	}
	if (!bOn)
		return false;

	DWORD dwThreshold = ParseSize(szContent);
	char szEnvThreshold[32] = { 0 };
	if (GetEnvironmentVariableA("VP_ALLOCTRACE_THRESHOLD", szEnvThreshold, sizeof(szEnvThreshold)) != 0)
		dwThreshold = ParseSize(szEnvThreshold);
	if (dwThreshold != 0)
		g_dwThreshold = dwThreshold < MIN_THRESHOLD ? MIN_THRESHOLD : dwThreshold;
	return true;
}

bool PatchSlot(int iSlot, DWORD dwThunk)
{
	DWORD dwOldProtect = 0;
	if (!VirtualProtect(g_apSlot[iSlot], sizeof(DWORD), PAGE_READWRITE, &dwOldProtect))
		return false;
	g_adwThunk[iSlot] = dwThunk;
	InterlockedExchange(reinterpret_cast<LONG volatile*>(g_apSlot[iSlot]), static_cast<LONG>(dwThunk));
	DWORD dwIgnored = 0;
	VirtualProtect(g_apSlot[iSlot], sizeof(DWORD), dwOldProtect, &dwIgnored);
	return true;
}

void WriteHeader(const char* szStatus)
{
	Line kLine;
	kLine.Str("# ");
	FormatNow(kLine);
	kLine.Str(" EngineAllocTrace ");
	kLine.Str(szStatus);
	if (g_pBuild != NULL)
	{
		kLine.Str(" exe=");
		kLine.Str(g_pBuild->szName);
		kLine.Key("timestamp");
		kLine.Hex(g_pBuild->dwTimestamp, 8);
	}
	kLine.Key("base");
	kLine.Hex(g_dwBase, 8);
	kLine.Key("imagesize");
	kLine.Hex(g_dwImageSize);
	kLine.Key("threshold");
	kLine.Dec(g_dwThreshold);
	kLine.Key("pid");
	kLine.Dec(GetCurrentProcessId());
	kLine.Key("tid");
	kLine.Dec(GetCurrentThreadId());
	kLine.Key("tick");
	kLine.Dec(GetTickCount());
	kLine.End();
	EnterCriticalSection(&g_kLock);
	WriteLocked(kLine, false);

	if (g_bInstalled)
	{
		Line kSlots;
		kSlots.Str("# slots");
		for (int i = 0; i < NUM_SLOTS; i++)
		{
			kSlots.Char(' ');
			kSlots.Str(SLOT_IMPORT_NAMES[i]);
			kSlots.Char('@');
			kSlots.Code(reinterpret_cast<DWORD>(g_apSlot[i]));
			kSlots.Str(":was=");
			kSlots.Hex(g_adwPrevious[i], 8);
		}
		kSlots.End();
		WriteLocked(kSlots, false);

		Line kLegend;
		kLegend.Str("# A=allocation R=realloc F=free of a logged block T=turn; caller/stack are EXE RVAs (add 0x400000 for the disassembly); api new/anew = engine operator new / aligned new (file, line, tag read from its frame)");
		kLegend.End();
		WriteLocked(kLegend, false);
	}
	FlushLocked();
	LeaveCriticalSection(&g_kLock);
}

}	// namespace

namespace EngineAllocTrace
{

#ifdef ENGINE_ALLOC_TRACE_SELFTEST
void SetExeModuleForSelfTest(void* hExe)
{
	g_hSelfTestExe = static_cast<HMODULE>(hExe);
}
#endif

void Install()
{
	static bool s_bTried = false;
	if (s_bTried)
		return;
	s_bTried = true;

	if (!Requested())
		return;

	InitializeCriticalSectionAndSpinCount(&g_kLock, 2000);
	CreateDirectoryA("crashlogs", NULL);
	g_hLog = CreateFileA("crashlogs\\allocTrace.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	// High addresses: the low half of the address space is the one that runs out.
	g_pOut = static_cast<char*>(VirtualAlloc(NULL, OUT_BYTES, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
	g_dwLastFlush = GetTickCount();

	g_dwBase = reinterpret_cast<DWORD>(GetExeModule());
	const char* szProblem = VerifyExe();
	if (szProblem == NULL)
	{
		g_alTrackPtr = static_cast<volatile LONG*>(VirtualAlloc(NULL, TRACK_ENTRIES * sizeof(LONG), MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
		g_aTrackInfo = static_cast<TrackInfo*>(VirtualAlloc(NULL, TRACK_ENTRIES * sizeof(TrackInfo), MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
		if (g_alTrackPtr == NULL || g_aTrackInfo == NULL || g_pOut == NULL || g_hLog == INVALID_HANDLE_VALUE)
			szProblem = "off: could not set up the table or the log";
	}

	if (szProblem == NULL)
	{
		// Forward targets first, so a call that reaches a thunk the instant its slot changes is served.
		for (int i = 0; i < NUM_SLOTS; i++)
			g_adwPrevious[i] = *g_apSlot[i];
		g_pfnMalloc = reinterpret_cast<MallocFn>(g_adwPrevious[SLOT_MALLOC]);
		g_pfnAlignedMalloc = reinterpret_cast<AlignedMallocFn>(g_adwPrevious[SLOT_ALIGNED_MALLOC]);
		g_pfnRealloc = reinterpret_cast<ReallocFn>(g_adwPrevious[SLOT_REALLOC]);
		g_pfnFree = reinterpret_cast<FreeFn>(g_adwPrevious[SLOT_FREE]);
		g_pfnAlignedFree = reinterpret_cast<FreeFn>(g_adwPrevious[SLOT_ALIGNED_FREE]);
		MemoryBarrier();

		// Frees first: a block can only be tracked once its allocator is patched, and a free thunk
		// with nothing tracked is a plain forward.
		const DWORD adwThunks[NUM_SLOTS] =
		{
			reinterpret_cast<DWORD>(&TraceMalloc),
			reinterpret_cast<DWORD>(&TraceAlignedMalloc),
			reinterpret_cast<DWORD>(&TraceRealloc),
			reinterpret_cast<DWORD>(&TraceFree),
			reinterpret_cast<DWORD>(&TraceAlignedFree),
		};
		const int aiOrder[NUM_SLOTS] = { SLOT_FREE, SLOT_ALIGNED_FREE, SLOT_REALLOC, SLOT_MALLOC, SLOT_ALIGNED_MALLOC };
		int iPatched = 0;
		for (int i = 0; i < NUM_SLOTS; i++)
		{
			if (PatchSlot(aiOrder[i], adwThunks[aiOrder[i]]))
				iPatched++;
		}
		if (iPatched == NUM_SLOTS)
			g_bInstalled = true;
		else
			szProblem = "partly: some import slots could not be written";
	}

	WriteHeader(g_bInstalled ? "active" : szProblem);
}

void OnGameUpdate(int iGameTurn)
{
	if (!g_bInstalled)
		return;
	EnterCriticalSection(&g_kLock);
	if (iGameTurn != g_iLastTurn)
	{
		g_iLastTurn = iGameTurn;
		Line kLine;
		kLine.Char('T');
		kLine.Key("turn");
		kLine.Int(iGameTurn);
		kLine.Key("tick");
		kLine.Dec(GetTickCount());
		kLine.Key("tracked");
		kLine.Dec(static_cast<DWORD>(g_lTracked));
		kLine.Key("trackedbytes");
		kLine.Dec(g_dwTrackedBytes);
		kLine.Key("untracked");
		kLine.Dec(g_dwUntracked);
		kLine.End();
		WriteLocked(kLine, false);
	}
	if (g_dwOutUsed != 0)
		FlushLocked();
	LeaveCriticalSection(&g_kLock);
}

void Uninstall()
{
	if (!g_bInstalled)
		return;
	for (int i = 0; i < NUM_SLOTS; i++)
	{
		DWORD dwOldProtect = 0;
		if (!VirtualProtect(g_apSlot[i], sizeof(DWORD), PAGE_READWRITE, &dwOldProtect))
			continue;
		InterlockedCompareExchange(reinterpret_cast<LONG volatile*>(g_apSlot[i]), static_cast<LONG>(g_adwPrevious[i]), static_cast<LONG>(g_adwThunk[i]));
		DWORD dwIgnored = 0;
		VirtualProtect(g_apSlot[i], sizeof(DWORD), dwOldProtect, &dwIgnored);
	}
	g_bInstalled = false;

	// Try only: at process exit the other threads are already gone, and one may have died holding it.
	if (!TryEnterCriticalSection(&g_kLock))
		return;
	Line kLine;
	kLine.Str("# ");
	FormatNow(kLine);
	kLine.Str(" unloaded");
	kLine.Key("tracked");
	kLine.Dec(static_cast<DWORD>(g_lTracked));
	kLine.Key("trackedbytes");
	kLine.Dec(g_dwTrackedBytes);
	kLine.Key("events");
	kLine.Dec(static_cast<DWORD>(g_lSeq));
	kLine.End();
	WriteLocked(kLine, true);
	if (g_hLog != INVALID_HANDLE_VALUE)
	{
		CloseHandle(g_hLog);
		g_hLog = INVALID_HANDLE_VALUE;   // a thunk still running elsewhere now only fills the buffer
	}
	LeaveCriticalSection(&g_kLock);
}

}	// namespace EngineAllocTrace
