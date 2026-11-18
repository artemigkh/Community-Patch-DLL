// VP64Trace.h: a diagnostic trace of the core unit path finder (c5-64bit-patch ticket 4807, slice "trace"; scratch
// branch, never a product build). With VP64_TRACE_FINDER every search of GC.GetPathFinder() on the game-core thread
// writes one line (inputs hash, result hash, reads of node data the search did not refresh, per site) to
// %VP64_TRACE_DIR%/VP64Trace.csv; VP64_TRACE_DETAIL="turn:seq[,seq...]" adds every such read and every closed node of
// those searches to VP64TraceDetail.csv. Logging only: the trace makes no search and changes no node.
#pragma once

#if defined(VP64_TRACE_FINDER)
class CvAStar;
class CvAStarNode;

namespace VP64Trace
{
	enum Site { S_PV_NODE, S_PV_PARENT, S_PC_NODE, S_PC_PARENT, S_CETAN, S_CNIP, S_DR_NATIVE, S_EXTRA, S_STOP_NEXT, S_COUNT };

	extern int g_iSkip;      // > 0: the verify build's repeat of a cached reach (not logged, not numbered)
	extern int g_iInReach;   // > 0: inside VP64Danger::Reach (kind R)

	void Begin(const CvAStar* finder);                          // a search starts (before its Reset)
	void End(const CvAStar* finder, bool bSuccess, int iWhere, const CvAStarNode* pBest); // a search ends
	void Read(int eSite, const CvAStarNode* node, const CvAStar* finder);
	void Stamp(const CvAStarNode* node, const CvAStar* finder); // the node's cache data written by the current search
	void Cleared(const CvAStarNode* node, const CvAStar* finder);

	struct ReachScope
	{
		ReachScope() { g_iInReach++; }
		~ReachScope() { g_iInReach--; }
	};
	struct SkipScope
	{
		SkipScope() { g_iSkip++; }
		~SkipScope() { g_iSkip--; }
	};
}
#define VP64_TRACE_READ(site, node, finder) VP64Trace::Read(VP64Trace::site, (node), (finder))
#else
#define VP64_TRACE_READ(site, node, finder) ((void)0)
#endif
