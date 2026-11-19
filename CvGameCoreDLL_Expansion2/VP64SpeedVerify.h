// VP64SpeedVerify.h: the verify-define pattern of the speedup epic (c5-64bit-patch, ticket 4803, epic 0504).
// x64 fork only. Include it ONLY inside `#if defined(VP64_SPEED_<P>)`: with every speed define off it must not be seen.
//
// A speed package P changes DLL code only inside `#if defined(VP64_SPEED_P)` blocks, each followed by a `#line N`
// that gives the next line its number in the pin's file, so that every later __LINE__ (PRECONDITION, the logging
// macros) keeps its value and the build with the define off is the pin's DLL (perf/speedkit/README.md, item 1).
//
// A kept value (a cache, a memo, an index built once, a value carried between passes) also gets a verify mode,
// VP64_SPEED_P_VERIFY: the original code runs next to the fast path at every use, VP64_SPEED_CHECK compares the two,
// counts every check and logs every difference. A verify build RETURNS THE ORIGINAL VALUE, so its game is the pin's
// game and one wrong kept value cannot hide later ones; its stats.db must therefore be IDENTICAL to the pin's too.
//
// The negative control, VP64_SPEED_P_SPOIL=<n> (n >= 1): the package spoils the n-th kept value it stores
// (`if (VP64_SPEED_SPOIL_NOW("P", VP64_SPEED_P_SPOIL)) value += 1;` or another wrong value). With VERIFY on, the run
// must report differences. One wrong value may never reach a decision, so the identity job's negative test (a DLL that
// changes the game must be caught) uses VP64_SPEED_P_SPOIL_EVERY=<n>: every n-th stored kept value wrong, VERIFY off
// (`if (VP64_SPEED_SPOIL_EVERY("P", VP64_SPEED_P_SPOIL_EVERY)) value += 1;`).
//
//   #if defined(VP64_SPEED_P)
//   #include "VP64SpeedVerify.h"
//   #endif
//   ...
//   #if defined(VP64_SPEED_P)
//       int iFast = <kept-value path>;
//   #  if defined(VP64_SPEED_P_VERIFY)
//       int iOrig = <the original code>;
//       VP64_SPEED_CHECK_K("P", "CvFoo::Bar", iFast, iOrig, iKey1, iKey2, 0, 0);
//       return iOrig;
//   #  else
//       return iFast;
//   #  endif
//   #endif
//   #line 1234
//       <the original code, unchanged>
//
// Output, in %VP64_SPEED_DIR% (else the process's working folder; the headless server's is its run folder):
//   VP64SpeedDiffs.csv   package,site,turn,check,fast,orig,k1,k2,k3,k4: the first 10,000 differences per package
//   VP64SpeedTotals.csv  package,checks,differences,spoils,stores,first_diff_turn,last_turn: one row per package,
//                        rewritten at each new game turn and when the DLL unloads (perf/speedkit/verifyrun.py reads it)
//   VP64SpeedInfo.txt    the speed, verify and spoil defines this DLL was built with (written at the first check)
//
// Two threads (the Architect's review of 2026-11-17, section 2: in the client a ui-thread DLL call can run in the
// middle of an AI computation at any hook raise). The logger is shared by every thread and takes its own SRW lock. The
// kept values are the package's own to make safe: owned by the working object, per thread, or shown never live across
// a raise. The logger itself adds only this file's statics: the lock, the per-package table and two FILE handles.
#pragma once
#ifndef VP64_SPEED_VERIFY_H
#define VP64_SPEED_VERIFY_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vp64speed
{
	static const long long kMaxLoggedPerPackage = 10000;

	struct PackageStats
	{
		const char* szName;
		long long iChecks;
		long long iDiffs;
		long long iSpoils;
		long long iStores;
		long long iLogged;
		int iFirstDiffTurn;
	};

	class Log
	{
	public:
		static Log& Get()
		{
			static Log s_log;
			return s_log;
		}

		void Lock() { AcquireSRWLockExclusive(&m_lock); }
		void Unlock() { ReleaseSRWLockExclusive(&m_lock); }

		// Every call below is made with the lock held.
		PackageStats& Package(const char* szName)
		{
			for (size_t i = 0; i < m_packages.size(); ++i)
				if (strcmp(m_packages[i].szName, szName) == 0)
					return m_packages[i];
			PackageStats s = { szName, 0, 0, 0, 0, 0, -1 };
			m_packages.push_back(s);
			return m_packages.back();
		}

		void Turn(int iTurn)
		{
			if (!m_bInfo)
				WriteInfo();
			if (iTurn != m_iTurn)
			{
				if (m_iTurn != -1)
					WriteTotals();
				m_iTurn = iTurn;
			}
		}

		void Diff(const char* szPackage, const char* szSite, int iTurn, long long iCheck, const char* szFast,
		          const char* szOrig, long long k1, long long k2, long long k3, long long k4)
		{
			if (m_pDiffs == NULL)
			{
				m_pDiffs = Open("VP64SpeedDiffs.csv");
				if (m_pDiffs)
					fprintf(m_pDiffs, "package,site,turn,check,fast,orig,k1,k2,k3,k4\n");
			}
			if (m_pDiffs)
			{
				fprintf(m_pDiffs, "%s,%s,%d,%lld,%s,%s,%lld,%lld,%lld,%lld\n", szPackage, szSite, iTurn, iCheck, szFast,
				        szOrig, k1, k2, k3, k4);
				fflush(m_pDiffs);
			}
		}

		void WriteTotals()
		{
			FILE* f = Open("VP64SpeedTotals.csv");
			if (f == NULL)
				return;
			fprintf(f, "package,checks,differences,spoils,stores,first_diff_turn,last_turn\n");
			for (size_t i = 0; i < m_packages.size(); ++i)
			{
				const PackageStats& p = m_packages[i];
				fprintf(f, "%s,%lld,%lld,%lld,%lld,%d,%d\n", p.szName, p.iChecks, p.iDiffs, p.iSpoils, p.iStores,
				        p.iFirstDiffTurn, m_iTurn);
			}
			fclose(f);
		}

		~Log()
		{
			Lock();
			WriteTotals();
			if (m_pDiffs)
				fclose(m_pDiffs);
			m_pDiffs = NULL;
			Unlock();
		}

	private:
		Log() : m_pDiffs(NULL), m_iTurn(-1), m_bInfo(false) { InitializeSRWLock(&m_lock); }
		Log(const Log&);
		Log& operator=(const Log&);

		FILE* Open(const char* szFile)
		{
			char szPath[1024];
			const char* szDir = getenv("VP64_SPEED_DIR");
			if (szDir && *szDir)
				_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "%s\\%s", szDir, szFile);
			else
				_snprintf_s(szPath, sizeof(szPath), _TRUNCATE, "%s", szFile);
			FILE* f = NULL;
			if (fopen_s(&f, szPath, "w") != 0)
				return NULL;
			return f;
		}

		void WriteInfo()
		{
			m_bInfo = true;
			FILE* f = Open("VP64SpeedInfo.txt");
			if (f == NULL)
				return;
			fprintf(f, "VP64SpeedVerify.h (c5-64bit-patch ticket 4803)\n");
#if defined(VP64_SPEED_KITDEMO)
			fprintf(f, "define VP64_SPEED_KITDEMO\n");
#endif
#if defined(VP64_SPEED_KITDEMO_VERIFY)
			fprintf(f, "define VP64_SPEED_KITDEMO_VERIFY\n");
#endif
#if defined(VP64_SPEED_KITDEMO_SPOIL)
			fprintf(f, "define VP64_SPEED_KITDEMO_SPOIL=%d\n", (int)(VP64_SPEED_KITDEMO_SPOIL));
#endif
#if defined(VP64_SPEED_KITDEMO_SPOIL_EVERY)
			fprintf(f, "define VP64_SPEED_KITDEMO_SPOIL_EVERY=%d\n", (int)(VP64_SPEED_KITDEMO_SPOIL_EVERY));
#endif
#if defined(VP64_SPEED_DANGER)
			fprintf(f, "define VP64_SPEED_DANGER\n");
#endif
#if defined(VP64_SPEED_DANGER_VERIFY)
			fprintf(f, "define VP64_SPEED_DANGER_VERIFY\n");
#endif
#if defined(VP64_SPEED_DANGER_STATS)
			fprintf(f, "define VP64_SPEED_DANGER_STATS\n");
#endif
#if defined(VP64_SPEED_DANGER_SPOIL)
			fprintf(f, "define VP64_SPEED_DANGER_SPOIL=%d\n", (int)(VP64_SPEED_DANGER_SPOIL));
#endif
#if defined(VP64_SPEED_DANGER_SPOIL_EVERY)
			fprintf(f, "define VP64_SPEED_DANGER_SPOIL_EVERY=%d\n", (int)(VP64_SPEED_DANGER_SPOIL_EVERY));
#endif
#if defined(VP64_SPEED_DANGER_DG7)
			fprintf(f, "define VP64_SPEED_DANGER_DG7\n");
#endif
#if defined(VP64_SPEED_DANGER_DG7_VERIFY)
			fprintf(f, "define VP64_SPEED_DANGER_DG7_VERIFY\n");
#endif
#if defined(VP64_SPEED_DANGER_DG7_SPOIL)
			fprintf(f, "define VP64_SPEED_DANGER_DG7_SPOIL=%d\n", (int)(VP64_SPEED_DANGER_DG7_SPOIL));
#endif
#if defined(VP64_SPEED_DANGER_DG7_SPOIL_EVERY)
			fprintf(f, "define VP64_SPEED_DANGER_DG7_SPOIL_EVERY=%d\n", (int)(VP64_SPEED_DANGER_DG7_SPOIL_EVERY));
#endif
#if defined(VP64_SPEED_P3)
			fprintf(f, "define VP64_SPEED_P3\n");
#endif
#if defined(VP64_SPEED_P3_MB2_CAP_MB)
			fprintf(f, "define VP64_SPEED_P3_MB2_CAP_MB=%d\n", (int)(VP64_SPEED_P3_MB2_CAP_MB));
#endif
#if defined(VP64_SPEED_P3_VERIFY)
			fprintf(f, "define VP64_SPEED_P3_VERIFY\n");
#endif
			// A package adds its own three lines here, in its own #if blocks.
			fclose(f);
		}

		SRWLOCK m_lock;
		std::vector<PackageStats> m_packages;
		FILE* m_pDiffs;
		int m_iTurn;
		bool m_bInfo;
	};

	class Guard
	{
	public:
		Guard() { Log::Get().Lock(); }
		~Guard() { Log::Get().Unlock(); }
	};

	inline void CheckText(const char* szPackage, const char* szSite, int iTurn, bool bEqual, const char* szFast,
	                      const char* szOrig, long long k1, long long k2, long long k3, long long k4)
	{
		Guard g;
		Log& kLog = Log::Get();
		kLog.Turn(iTurn);
		PackageStats& p = kLog.Package(szPackage);
		++p.iChecks;
		if (bEqual)
			return;
		++p.iDiffs;
		if (p.iFirstDiffTurn < 0)
			p.iFirstDiffTurn = iTurn;
		if (p.iLogged < kMaxLoggedPerPackage)
		{
			++p.iLogged;
			kLog.Diff(szPackage, szSite, iTurn, p.iChecks, szFast, szOrig, k1, k2, k3, k4);
		}
	}

	// Integers, enums and bools: exact compare.
	inline void Check(const char* szPackage, const char* szSite, int iTurn, long long iFast, long long iOrig,
	                  long long k1, long long k2, long long k3, long long k4)
	{
		char szFast[32], szOrig[32];
		const bool bEqual = (iFast == iOrig);
		if (!bEqual)
		{
			_snprintf_s(szFast, sizeof(szFast), _TRUNCATE, "%lld", iFast);
			_snprintf_s(szOrig, sizeof(szOrig), _TRUNCATE, "%lld", iOrig);
		}
		CheckText(szPackage, szSite, iTurn, bEqual, bEqual ? "" : szFast, bEqual ? "" : szOrig, k1, k2, k3, k4);
	}

	// Floating point: bit-exact compare (an exact speedup keeps every bit; -0 against +0 and NaN payloads differ).
	inline void CheckD(const char* szPackage, const char* szSite, int iTurn, double dFast, double dOrig,
	                   long long k1, long long k2, long long k3, long long k4)
	{
		char szFast[48], szOrig[48];
		const bool bEqual = (memcmp(&dFast, &dOrig, sizeof(double)) == 0);
		if (!bEqual)
		{
			_snprintf_s(szFast, sizeof(szFast), _TRUNCATE, "%.17g", dFast);
			_snprintf_s(szOrig, sizeof(szOrig), _TRUNCATE, "%.17g", dOrig);
		}
		CheckText(szPackage, szSite, iTurn, bEqual, bEqual ? "" : szFast, bEqual ? "" : szOrig, k1, k2, k3, k4);
	}

	// Counts one stored kept value of the package; true exactly at its n-th store (the verify mode's negative control:
	// one wrong kept value, which a verify run must catch). With bEvery, true at every n-th store (the identity job's
	// negative control: enough wrong values that the game must change).
	inline bool Spoil(const char* szPackage, int iTurn, long long n, bool bEvery)
	{
		Guard g;
		Log& kLog = Log::Get();
		kLog.Turn(iTurn);
		PackageStats& p = kLog.Package(szPackage);
		++p.iStores;
		if (n <= 0 || (bEvery ? (p.iStores % n) != 0 : p.iStores != n))
			return false;
		++p.iSpoils;
		return true;
	}

	inline int Turn()
	{
		return GC.getGamePointer() ? GC.getGame().getGameTurn() : -1;
	}
}

#define VP64_SPEED_CHECK(pkg, site, fast, orig) \
	::vp64speed::Check(pkg, site, ::vp64speed::Turn(), (long long)(fast), (long long)(orig), 0, 0, 0, 0)
#define VP64_SPEED_CHECK_K(pkg, site, fast, orig, k1, k2, k3, k4) \
	::vp64speed::Check(pkg, site, ::vp64speed::Turn(), (long long)(fast), (long long)(orig), (long long)(k1), \
	                   (long long)(k2), (long long)(k3), (long long)(k4))
#define VP64_SPEED_CHECK_D(pkg, site, fast, orig, k1, k2, k3, k4) \
	::vp64speed::CheckD(pkg, site, ::vp64speed::Turn(), (double)(fast), (double)(orig), (long long)(k1), \
	                    (long long)(k2), (long long)(k3), (long long)(k4))
#define VP64_SPEED_SPOIL_NOW(pkg, n) ::vp64speed::Spoil(pkg, ::vp64speed::Turn(), (long long)(n), false)
#define VP64_SPEED_SPOIL_EVERY(pkg, n) ::vp64speed::Spoil(pkg, ::vp64speed::Turn(), (long long)(n), true)

#endif // VP64_SPEED_VERIFY_H
