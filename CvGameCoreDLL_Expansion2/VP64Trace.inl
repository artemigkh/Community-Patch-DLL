// VP64Trace.inl: the implementation of VP64Trace.h (c5-64bit-patch ticket 4807, slice "trace"). Included once, at the
// end of CvAStar.cpp, only with VP64_TRACE_FINDER. Game-core thread only (Begin refuses any other thread or finder).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace VP64Trace
{
int g_iSkip = 0;
int g_iInReach = 0;

namespace
{
	typedef unsigned long long u64;
	typedef unsigned int u32;

	const u64 kSeed = 14695981039346656037ULL;
	const char* const kSiteNames[S_COUNT] = { "pv_node", "pv_parent", "pc_node", "pc_parent", "cetan", "cnip", "dr_native", "extra", "stop_next" };

	bool s_bInit = false;
	FILE* s_f = NULL;
	FILE* s_fd = NULL;
	int s_iFrom = 0;
	int s_iDetailTurn = -1;
	std::vector<int> s_aDetailSeq;

	const CvAStar* s_pActive = NULL;  // the core finder while it runs a traced search
	bool s_bSkipCur = false;
	bool s_bDetailCur = false;
	int s_iTurn = -1;
	int s_iSeq = 0;                   // logged searches of this turn (the first is 1)
	int s_iVer = 0;                   // skipped searches since the last logged one
	int s_iVerBefore = 0;
	u64 s_uCur = 0;                   // the current search: turn << 32 | seq << 8 | skipped count
	int s_aiCount[S_COUNT];
	u64 s_uStaleHash = kSeed;
	std::vector<u64> s_aStampBy;      // [plot * 2 + layer]: the search that last wrote the node's cache data
	std::vector<u64> s_aClearBy;      // [plot * 2 + layer]: the search whose Reset last cleared the node

	inline u64 Mix(u64 h, u64 v)
	{
		for (int i = 0; i < 8; i++)
		{
			h ^= (v >> (i * 8)) & 0xFF;
			h *= 1099511628211ULL;
		}
		return h;
	}

	void Init()
	{
		s_bInit = true;
		const char* dir = getenv("VP64_TRACE_DIR");
		if (!dir || !*dir)
			return;
		char path[1024];
		_snprintf(path, sizeof(path) - 1, "%s\\VP64Trace.csv", dir);
		path[sizeof(path) - 1] = 0;
		s_f = fopen(path, "w");
		if (!s_f)
			return;
		setvbuf(s_f, NULL, _IOFBF, 1 << 20);
		fprintf(s_f, "turn,seq,kind,ver,player,unit,path,flags,sx,sy,dx,dy,in,res,succ,where,closed,stale");
		for (int i = 0; i < S_COUNT; i++)
			fprintf(s_f, ",%s", kSiteNames[i]);
		fprintf(s_f, ",stalehash\n");
		const char* from = getenv("VP64_TRACE_FROM");
		if (from && *from)
			s_iFrom = atoi(from);
		const char* det = getenv("VP64_TRACE_DETAIL");
		if (det && *det)
		{
			s_iDetailTurn = atoi(det);
			const char* p = strchr(det, ':');
			while (p && *p)
			{
				p++;
				s_aDetailSeq.push_back(atoi(p));
				p = strchr(p, ',');
			}
			_snprintf(path, sizeof(path) - 1, "%s\\VP64TraceDetail.csv", dir);
			path[sizeof(path) - 1] = 0;
			s_fd = fopen(path, "w");
		}
	}

	inline int NodeIndex(const CvAStarNode* n, const CvAStar* f)
	{
		return GC.getMap().plotNum(n->m_iX, n->m_iY) * 2 + (f->GetNode(n->m_iX, n->m_iY) == n ? 0 : 1);
	}

	inline u32 Flags(const CvPathNodeCacheData& d)
	{
		u32 u = 0;
		u |= d.bIsRevealedToTeam ? 1u << 0 : 0;
		u |= d.bPlotVisibleToTeam ? 1u << 1 : 0;
		u |= d.bIsNonNativeDomain ? 1u << 2 : 0;
		u |= d.bCanEnterTerrainIntermediate ? 1u << 3 : 0;
		u |= d.bCanEnterTerrainPermanent ? 1u << 4 : 0;
		u |= d.bCanEnterTerritoryIntermediate ? 1u << 5 : 0;
		u |= d.bCanEnterTerritoryPermanent ? 1u << 6 : 0;
		u |= d.bIsNonEnemyCity ? 1u << 7 : 0;
		u |= d.bIsEnemyCity ? 1u << 8 : 0;
		u |= d.bCanAirliftFromPlotCity ? 1u << 9 : 0;
		u |= d.bCanAirliftFromPlotImprovement ? 1u << 10 : 0;
		u |= d.bCanSealiftFromPlotCity ? 1u << 11 : 0;
		u |= d.bCanChangePortFromPlot ? 1u << 12 : 0;
		u |= d.bIsVisibleEnemyUnit ? 1u << 13 : 0;
		u |= d.bIsVisibleEnemyCombatUnit ? 1u << 14 : 0;
		u |= d.bIsVisibleNeutralCombatUnit ? 1u << 15 : 0;
		u |= d.bUnitStackingLimitReached ? 1u << 16 : 0;
		u |= d.bIsValidRoute ? 1u << 17 : 0;
		return u;
	}

	void IdText(u64 id, char* buf, size_t n)
	{
		if (id == 0)
			_snprintf(buf, n - 1, "-");
		else if ((id & 0xFF) == 0)
			_snprintf(buf, n - 1, "%u:%u", (u32)(id >> 32), (u32)((id >> 8) & 0xFFFFFF));
		else
			_snprintf(buf, n - 1, "%u:%u+v%u", (u32)(id >> 32), (u32)((id >> 8) & 0xFFFFFF), (u32)(id & 0xFF));
		buf[n - 1] = 0;
	}

	void NodeLine(const char* tag, const CvAStarNode* n, const CvAStar* f)
	{
		const CvPathNodeCacheData& d = n->m_kCostCacheData;
		const int idx = NodeIndex(n, f);
		char sb[48], cb[48];
		IdText(s_aStampBy[idx], sb, sizeof(sb));
		IdText(s_aClearBy[idx], cb, sizeof(cb));
		const CvAStarNode* p = n->m_pParent;
		fprintf(s_fd, "%s,%d,%d,%d,flags=%05x,mf=%x,cost=%d/%d,etd=%d/%d,stamp=%s,fresh=%d,stampby=%s,clearby=%s,turns=%d,moves=%d,known=%ld,total=%ld,parent=%d/%d/%d\n",
			tag, n->m_iX, n->m_iY, idx & 1, Flags(d), (u32)d.iMoveFlags, (int)d.plotMovementCostMultiplier, (int)d.plotMovementCostAdder,
			d.bEndTurnDangerKnown ? 1 : 0, d.iEndTurnDanger, d.iGenerationID == 0 ? "0" : "set",
			d.iGenerationID == f->GetCurrentGenerationID() ? 1 : 0, sb, cb, (int)n->m_iTurns, (int)n->m_iMoves, n->m_iKnownCost,
			n->m_iTotalCost, p ? p->m_iX : -1, p ? p->m_iY : -1, p ? (NodeIndex(p, f) & 1) : -1);
	}
}

void Begin(const CvAStar* finder)
{
	if (!s_bInit)
		Init();
	s_pActive = NULL;
	if (!s_f)
		return;
	if (finder != static_cast<const CvAStar*>(&GC.GetPathFinder()) || !gDLL->IsGameCoreThread())
		return;
	const int iTurn = GC.getGame().getGameTurn();
	if (iTurn != s_iTurn)
	{
		s_iTurn = iTurn;
		s_iSeq = 0;
		s_iVer = 0;
		fflush(s_f);
		if (s_fd)
			fflush(s_fd);
	}
	const size_t nNodes = (size_t)GC.getMap().numPlots() * 2;
	if (s_aStampBy.size() != nNodes)
	{
		s_aStampBy.assign(nNodes, 0);
		s_aClearBy.assign(nNodes, 0);
	}
	s_bSkipCur = g_iSkip > 0;
	if (s_bSkipCur)
	{
		s_iVer++;
		s_uCur = ((u64)(u32)iTurn << 32) | ((u64)(u32)s_iSeq << 8) | (u64)(s_iVer > 255 ? 255 : s_iVer);
	}
	else
	{
		s_iSeq++;
		s_iVerBefore = s_iVer;
		s_iVer = 0;
		s_uCur = ((u64)(u32)iTurn << 32) | ((u64)(u32)s_iSeq << 8);
	}
	memset(s_aiCount, 0, sizeof(s_aiCount));
	s_uStaleHash = kSeed;
	s_bDetailCur = false;
	if (!s_bSkipCur && s_fd && iTurn == s_iDetailTurn)
		for (size_t i = 0; i < s_aDetailSeq.size(); i++)
			if (s_aDetailSeq[i] == s_iSeq)
				s_bDetailCur = true;
	if (s_bDetailCur)
	{
		const SPathFinderUserData& d = finder->GetData();
		fprintf(s_fd, "begin,%d,%d,player=%d,unit=%d,path=%d,flags=%x,start=%d/%d,dest=%d/%d,maxturns=%d,minmoves=%d,startmoves=%d,maxdist=%d,inreach=%d\n",
			iTurn, s_iSeq, (int)d.ePlayer, d.iUnitID, (int)d.ePath, d.iFlags, finder->GetStartX(), finder->GetStartY(),
			finder->GetDestX(), finder->GetDestY(), d.iMaxTurns, d.iMinMovesLeft, d.iStartMoves, d.iMaxNormalizedDistance, g_iInReach);
	}
	s_pActive = finder;
}

void Read(int eSite, const CvAStarNode* node, const CvAStar* finder)
{
	if (finder != s_pActive || !node)
		return;
	const CvPathNodeCacheData& d = node->m_kCostCacheData;
	if (d.iGenerationID == finder->GetCurrentGenerationID())
		return;
	s_aiCount[eSite]++;
	u64 h = s_uStaleHash;
	h = Mix(h, (u64)eSite);
	h = Mix(h, (u64)NodeIndex(node, finder));
	h = Mix(h, (u64)Flags(d));
	h = Mix(h, (u64)(u32)d.iMoveFlags);
	h = Mix(h, ((u64)(unsigned short)d.plotMovementCostMultiplier << 16) | (u64)(unsigned short)d.plotMovementCostAdder);
	h = Mix(h, d.bEndTurnDangerKnown ? (u64)(u32)d.iEndTurnDanger : 0xFFFFFFFFFFFFFFFFULL);
	h = Mix(h, d.iGenerationID == 0 ? 1 : 2);
	s_uStaleHash = h;
	if (s_bDetailCur)
	{
		char tag[32];
		_snprintf(tag, sizeof(tag) - 1, "read_%s", kSiteNames[eSite]);
		tag[sizeof(tag) - 1] = 0;
		NodeLine(tag, node, finder);
	}
}

void Stamp(const CvAStarNode* node, const CvAStar* finder)
{
	if (finder != s_pActive || !node)
		return;
	s_aStampBy[NodeIndex(node, finder)] = s_uCur;
}

void Cleared(const CvAStarNode* node, const CvAStar* finder)
{
	if (finder != s_pActive || !node)
		return;
	s_aClearBy[NodeIndex(node, finder)] = s_uCur;
}

void End(const CvAStar* finder, bool bSuccess, int iWhere, const CvAStarNode* pBest)
{
	if (finder != s_pActive)
		return;
	s_pActive = NULL;
	if (s_bSkipCur || s_iTurn < s_iFrom)
		return;
	const SPathFinderUserData& d = finder->GetData();
	u64 in = kSeed;
	in = Mix(in, (u64)(u32)d.ePath);
	in = Mix(in, (u64)(u32)d.iFlags);
	in = Mix(in, (u64)(u32)d.ePlayer);
	in = Mix(in, (u64)(u32)d.eEnemy);
	in = Mix(in, (u64)(u32)d.iUnitID);
	in = Mix(in, (u64)(u32)d.iMaxTurns);
	in = Mix(in, (u64)(u32)d.iMaxNormalizedDistance);
	in = Mix(in, (u64)(u32)d.iMinMovesLeft);
	in = Mix(in, (u64)(u32)d.iStartMoves);
	in = Mix(in, (u64)(u32)d.eRoute);
	in = Mix(in, (u64)(u32)d.eBuild);
	in = Mix(in, ((u64)(u32)finder->GetStartX() << 32) | (u32)finder->GetStartY());
	in = Mix(in, ((u64)(u32)finder->GetDestX() << 32) | (u32)finder->GetDestY());
	for (size_t i = 0; i < d.plotsToIgnoreForZOC.size(); i++)
		in = Mix(in, (u64)(u32)d.plotsToIgnoreForZOC[i]);

	u64 res = kSeed;
	res = Mix(res, bSuccess ? 1 : 0);
	res = Mix(res, (u64)(u32)iWhere);
	if (pBest)
		res = Mix(res, ((u64)(u32)NodeIndex(pBest, finder) << 32) | ((u64)pBest->m_iTurns << 16) | (u64)pBest->m_iMoves);
	const std::vector<CvAStarNode*>& closed = finder->VP64ClosedNodes();
	for (size_t i = 0; i < closed.size(); i++)
	{
		const CvAStarNode* n = closed[i];
		res = Mix(res, ((u64)(u32)NodeIndex(n, finder) << 32) | ((u64)n->m_iTurns << 16) | (u64)n->m_iMoves);
		res = Mix(res, (u64)(u32)n->m_iKnownCost);
		res = Mix(res, n->m_pParent ? (u64)(u32)NodeIndex(n->m_pParent, finder) : 0xFFFFFFFFULL);
	}
	int iStale = 0;
	for (int i = 0; i < S_COUNT; i++)
		iStale += s_aiCount[i];
	fprintf(s_f, "%d,%d,%c,%d,%d,%d,%d,%x,%d,%d,%d,%d,%016llx,%016llx,%d,%d,%d,%d", s_iTurn, s_iSeq, g_iInReach > 0 ? 'R' : 'N',
		s_iVerBefore, (int)d.ePlayer, d.iUnitID, (int)d.ePath, d.iFlags, finder->GetStartX(), finder->GetStartY(), finder->GetDestX(),
		finder->GetDestY(), in, res, bSuccess ? 1 : 0, iWhere, (int)closed.size(), iStale);
	for (int i = 0; i < S_COUNT; i++)
		fprintf(s_f, ",%d", s_aiCount[i]);
	fprintf(s_f, ",%016llx\n", iStale ? s_uStaleHash : 0ULL);
	if (s_bDetailCur)
	{
		fprintf(s_fd, "end,%d,%d,succ=%d,where=%d,closed=%d,in=%016llx,res=%016llx\n", s_iTurn, s_iSeq, bSuccess ? 1 : 0, iWhere,
			(int)closed.size(), in, res);
		if (pBest)
			NodeLine("best", pBest, finder);
		for (size_t i = 0; i < closed.size(); i++)
			NodeLine("closed", closed[i], finder);
		fflush(s_fd);
	}
}

} // namespace VP64Trace
