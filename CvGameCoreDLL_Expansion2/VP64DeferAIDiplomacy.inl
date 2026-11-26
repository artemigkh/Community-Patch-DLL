// VP64DeferAIDiplomacy.inl: the implementation of VP64DeferAIDiplomacy.h, included at the end of
// CvDiplomacyRequests.cpp (it needs the request queue's internals). Compiled only with VP64_DEFER_AI_DIPLOMACY.
//
// Everything here runs on the game thread, except the export, which a host calls between games and which only writes
// the switch and copies the counters.
#if defined(VP64_DEFER_AI_DIPLOMACY)

namespace VP64DeferAIDiplomacy
{
	static bool s_bInit = false;
	static bool s_bOn = false;
	static int s_iSource = 0;
	static VP64DeferAIDiplomacyStats s_tot;

	// Tests: VP64_DEFER_AI_DIPLOMACY_DIR = a folder; the DLL writes DeferAIDiplomacy.txt there when it unloads (the
	// switch, its source and the counters), so a host that calls no export still shows what the switch did
	static std::string s_dir;
	struct ReportAtUnload
	{
		~ReportAtUnload()
		{
			if (s_dir.empty())
				return;
			FILE* f = fopen((s_dir + "\\DeferAIDiplomacy.txt").c_str(), "w");
			if (!f)
				return;
			fprintf(f, "version=%d\nenabled=%d\nsource=%d\nqueued=%lld\nshown=%lld\nwithdrawn=%lld\nshown_as_stock=%lld\nblocks_skipped=%lld\nhandovers_skipped=%lld\n",
				VP64_DEFER_AI_DIPLOMACY_VERSION, s_bOn ? 1 : 0, s_iSource, s_tot.iQueued, s_tot.iShown, s_tot.iWithdrawn,
				s_tot.iShownAsStock, s_tot.iBlocksSkipped, s_tot.iHandoversSkipped);
			fclose(f);
		}
	};
	static ReportAtUnload s_reportAtUnload;

	static bool EnvValue(const char* szName, std::string& out)
	{
		char buf[MAX_PATH];
		DWORD n = GetEnvironmentVariableA(szName, buf, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return false;
		out.assign(buf, n);
		return true;
	}

	static void Init()
	{
		if (s_bInit)
			return;
		s_bInit = true;
		EnvValue("VP64_DEFER_AI_DIPLOMACY_DIR", s_dir);
		if (s_iSource == 1)
			return;
		std::string v;
		if (EnvValue("VP64_DEFER_AI_DIPLOMACY", v))
		{
			s_bOn = (v == "1" || v == "on");
			s_iSource = 2;
		}
		else
		{
			s_bOn = (VP64_DEFER_AI_DIPLOMACY_DEFAULT != 0);
			s_iSource = 3;
		}
	}

	bool IsOn()
	{
		Init();
		return s_bOn;
	}

	bool IsDeferred()
	{
		return IsOn() && !CvPreGame::isReallyNetworkMultiPlayer() && !CvPreGame::isPlayByEmail();
	}

	bool QueueFor(PlayerTypes eFrom, PlayerTypes eTo)
	{
		if (eFrom == NO_PLAYER || eTo == NO_PLAYER || !IsDeferred())
			return false;
		if (CvPreGame::isHuman(eFrom) || !GET_PLAYER(eTo).isHuman(ISHUMAN_AI_DIPLOMACY))
			return false;
		// In the human's own turn nothing waits on the screen: show it at once, as the stock code does
		if (eTo == GC.getGame().getActivePlayer() && GET_PLAYER(eTo).isTurnActive())
			return false;
		return true;
	}

	void Queue(PlayerTypes eFrom, PlayerTypes eTo, CvDeal* pkDeal, DiploUIStateTypes eDiploType, const char* pszMessage, LeaderheadAnimationTypes eAnimationType, int iExtraGameData)
	{
		CvDiplomacyRequests* pkRequests = GET_PLAYER(eTo).GetDiplomacyRequests();
		if (!pkRequests)
			return;
		if (pkDeal)
		{
			// the deal as the queue's re-check expects it: from the AI to the human (as the multiplayer path stores it)
			if (pkDeal->GetFromPlayer() != eFrom)
			{
				PlayerTypes tmp = pkDeal->GetFromPlayer();
				pkDeal->SetFromPlayer(pkDeal->GetToPlayer());
				pkDeal->SetToPlayer(tmp);
				int tmpValue = pkDeal->GetFromPlayerValue();
				pkDeal->SetFromPlayerValue(pkDeal->GetToPlayerValue());
				pkDeal->SetToPlayerValue(tmpValue);
			}
			GC.getGame().GetGameDeals().AddProposedDeal(*pkDeal);
		}
		else
		{
			// an empty deal, so every queued contact has one stored deal in the same order as its request
			GC.getGame().GetGameDeals().AddProposedDeal(CvDeal(eFrom, eTo));
		}
		pkRequests->Add(eFrom, eDiploType, pszMessage, eAnimationType, iExtraGameData);
		s_tot.iQueued++;
	}

	void LeaderMessage(PlayerTypes eFrom, PlayerTypes eTo, DiploUIStateTypes eDiploType, const char* pszMessage, LeaderheadAnimationTypes eAnimationType, int iExtraGameData)
	{
		if (QueueFor(eFrom, eTo))
			Queue(eFrom, eTo, NULL, eDiploType, pszMessage, eAnimationType, iExtraGameData);
		else
			gDLL->GameplayDiplomacyAILeaderMessage(eFrom, eDiploType, pszMessage, eAnimationType, iExtraGameData);
	}

	void CancelProposedDealsWithAIs(PlayerTypes eCancelPlayer)
	{
		// CvGameDeals::DoCancelAllProposedDealsWithPlayer, without the human players
		CvGameDeals& kDeals = GC.getGame().GetGameDeals();
		for (int iPlayerLoop = 0; iPlayerLoop < MAX_MAJOR_CIVS; iPlayerLoop++)
		{
			PlayerTypes eLoopPlayer = (PlayerTypes)iPlayerLoop;
			if (GET_PLAYER(eLoopPlayer).isHuman(ISHUMAN_AI_DIPLOMACY))
				continue;

			if (kDeals.GetProposedDeal(eCancelPlayer, eLoopPlayer))
				kDeals.FinalizeDeal(eCancelPlayer, eLoopPlayer, false);
			if (kDeals.GetProposedDeal(eLoopPlayer, eCancelPlayer))
				kDeals.FinalizeDeal(eLoopPlayer, eCancelPlayer, false);
		}
	}

	bool SkipBlock()
	{
		if (!IsDeferred())
			return false;
		s_tot.iBlocksSkipped++;
		return true;
	}

	bool SkipHandover()
	{
		if (!IsDeferred())
			return false;
		s_tot.iHandoversSkipped++;
		return true;
	}

	void CountDrain(int iShown, int iWithdrawn, int iShownAsStock)
	{
		s_tot.iShown += iShown;
		s_tot.iWithdrawn += iWithdrawn;
		s_tot.iShownAsStock += iShownAsStock;
	}

	static bool IsTradeState(DiploUIStateTypes eState)
	{
		return eState == DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER || eState == DIPLO_UI_STATE_TRADE_AI_MAKES_DEMAND ||
			eState == DIPLO_UI_STATE_TRADE_AI_MAKES_REQUEST || eState == DIPLO_UI_STATE_TRADE_AI_MAKES_GENEROUS_OFFER;
	}
}

//	----------------------------------------------------------------------------
//	The queue outside network multiplayer (CvDiplomacyRequests::Update), whatever the switch says, so that a save made
//	with contacts waiting opens them at the human's turn start in any build.
//	A request from an AI with a stored deal (the deferred queue) opens through ActivateNext, the multiplayer queue's
//	re-check: one at a time, when no modal window, popup, leader screen or city screen is up. Any other request (from
//	a human, or one the stock code queued without a deal) is shown as the stock code shows it.
void CvDiplomacyRequests::VP64Drain()
{
	PlayerTypes eActivePlayer = GC.getGame().getActivePlayer();
	if (m_aRequests.empty() || m_ePlayer != eActivePlayer || !GET_PLAYER(eActivePlayer).isTurnActive())
		return;

	const bool bDeferred = VP64DeferAIDiplomacy::IsDeferred();
	if (HasActiveRequest() && bDeferred)
		return;

	ICvUserInterface2* pkIFace = GC.GetEngineUserInterface();
	const bool bScreenFree = !HasActiveRequest() && pkIFace->IsModalStackEmpty() && !pkIFace->isDiploOrPopupWaiting() && !pkIFace->isCityScreenUp();

	CvDiplomacyRequests::Request& kRequest = m_aRequests.front();
	PlayerTypes eFrom = kRequest.m_eFromPlayer;
	CvGameDeals& kDeals = GC.getGame().GetGameDeals();
	const bool bStoredDeal = eFrom != NO_PLAYER && !CvPreGame::isHuman(eFrom) && kDeals.GetProposedMPDeal(eFrom, m_ePlayer, false) != NULL;

	if (!bStoredDeal)
	{
		if (bDeferred && !bScreenFree)
			return;
		if (bDeferred && eFrom != NO_PLAYER && !CvPreGame::isHuman(eFrom) && VP64DeferAIDiplomacy::IsTradeState(kRequest.m_eDiploType))
		{
			// a trade screen needs its deal: an identical deal of the same pair was taken by an earlier request
			m_aRequests.pop_front();
			VP64DeferAIDiplomacy::CountDrain(0, 1, 0);
			return;
		}
		// the stock code of Update()
		if (kRequest.m_eFromPlayer != NO_PLAYER && GET_PLAYER(kRequest.m_eFromPlayer).isAlive())
		{
			Send(kRequest.m_eFromPlayer, kRequest.m_eDiploType, kRequest.m_strMessage, kRequest.m_eAnimationType, kRequest.m_iExtraGameData);
			VP64DeferAIDiplomacy::CountDrain(0, 0, 1);
		}
		m_aRequests.pop_front();
		return;
	}

	if (!bScreenFree)
		return;

	if (!GET_PLAYER(eFrom).isAlive())
	{
		kDeals.RemoveProposedDeal(eFrom, m_ePlayer, NULL, false);
		m_aRequests.pop_front();
		VP64DeferAIDiplomacy::CountDrain(0, 1, 0);
		return;
	}

	// Requests and their deals were stored in the same order; ActivateNext takes the newest deal of the pair, so this
	// request's deal (the oldest) is made the newest first
	CvDeal kDeal;
	kDeals.RemoveProposedDeal(eFrom, m_ePlayer, &kDeal, false);

	// ActivateNext re-checks an offer; a demand, a request for help or a gift whose items are no longer possible is
	// withdrawn the same way here
	if (VP64DeferAIDiplomacy::IsTradeState(kRequest.m_eDiploType) && kRequest.m_eDiploType != DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER &&
		kDeal.GetNumItems() > 0 && !kDeal.AreAllTradeItemsValid())
	{
		Localization::String strSummary = Localization::Lookup("TXT_KEY_DEAL_WITHDRAWN");
		Localization::String strMessage = Localization::Lookup("TXT_KEY_DEAL_WITHDRAWN_BY_THEM");
		strMessage << GET_PLAYER(eFrom).getName();
		GET_PLAYER(m_ePlayer).GetNotifications()->Add(NOTIFICATION_PLAYER_DEAL_RESOLVED, strMessage.toUTF8(), strSummary.toUTF8(), eFrom, -1, -1);
		m_aRequests.pop_front();
		VP64DeferAIDiplomacy::CountDrain(0, 1, 0);
		return;
	}

	kDeals.AddProposedDeal(kDeal);
	m_eRequestActiveFromPlayer = eFrom;
	ActivateNext();
	if (m_bRequestActive)
		VP64DeferAIDiplomacy::CountDrain(1, 0, 0);
	else
	{
		// withdrawn by the re-check (it sent the deal-withdrawn notice), or no deal found
		m_eRequestActiveFromPlayer = NO_PLAYER;
		VP64DeferAIDiplomacy::CountDrain(0, 1, 0);
	}
}

extern "C" __declspec(dllexport) int VP64DeferAIDiplomacyControl(int iSet, VP64DeferAIDiplomacyStats* pOut)
{
	using namespace VP64DeferAIDiplomacy;
	Init(); // the environment and the default first, so a read-only call reports what is in effect
	if (iSet == 0 || iSet == 1)
	{
		s_bOn = (iSet == 1);
		s_iSource = 1;
	}
	if (pOut != NULL && pOut->iSize > 0)
	{
		VP64DeferAIDiplomacyStats st = s_tot;
		st.iSize = (int)sizeof(VP64DeferAIDiplomacyStats);
		st.iVersion = VP64_DEFER_AI_DIPLOMACY_VERSION;
		st.iEnabled = s_bOn ? 1 : 0;
		st.iSource = s_iSource;
		st.iDeferredNow = (s_iSource != 0 && s_bOn && !CvPreGame::isReallyNetworkMultiPlayer() && !CvPreGame::isPlayByEmail()) ? 1 : 0;
		st.iReserved = 0;
		const int iCopy = pOut->iSize < (int)sizeof(st) ? pOut->iSize : (int)sizeof(st);
		memcpy(pOut, &st, iCopy);
		pOut->iSize = iCopy;
	}
	return VP64_DEFER_AI_DIPLOMACY_VERSION;
}
#endif
