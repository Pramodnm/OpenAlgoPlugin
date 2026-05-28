///////////////////////////////////////////////////////////
// Exported Functions
///////////////////////////////////////////////////////////
PLUGINAPI int GetPluginInfo(struct PluginInfo* pInfo)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	if (pInfo == NULL) return FALSE;

	*pInfo = oPluginInfo;
	return TRUE;
}

PLUGINAPI int Init(void)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	// ALWAYS log initialization
	OutputDebugString(_T("OpenAlgo: Init() called"));

	if (!g_bPluginInitialized)
	{
		// Defensive: Regular MFC DLLs do not reliably get InitInstance() called,
		// so the SetRegistryKey() call in COpenAlgoApp::InitInstance may never
		// run. Set it here too so the load path uses the same registry root
		// (HKCU\Software\OpenAlgo\OpenAlgo\OpenAlgo\...) as the save path.
		theApp.EnsureRegistryRoot();

		// Initialize on first call
		g_oServer = AfxGetApp()->GetProfileString(_T("OpenAlgo"), _T("Server"), _T("127.0.0.1"));

		// Read API key directly from the registry. MFC's GetProfileString was
		// returning empty here in production even when the value existed, so
		// we read it ourselves via RegQueryValueEx. We still attempt the MFC
		// read as a fallback in case anyone wrote with the old code path.
		if (!ReadApiKeyDirect(g_oApiKey) || g_oApiKey.IsEmpty())
		{
			g_oApiKey = AfxGetApp()->GetProfileString(_T("OpenAlgo"), _T("ApiKey"), _T(""));
		}

		g_oWebSocketUrl = AfxGetApp()->GetProfileString(_T("OpenAlgo"), _T("WebSocketUrl"), _T("ws://127.0.0.1:8765"));  // Load WebSocket URL
		g_nPortNumber = AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("Port"), 5000);
		g_nRefreshInterval = CONNECTION_HEARTBEAT_INTERVAL_SEC;
		g_nBackfillRefreshIntervalSec = ClampBackfillRefreshIntervalSec(
			AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("BackfillRefreshIntervalSec"), 30));
		g_nBackfillIntervalMs = g_nBackfillRefreshIntervalSec * 1000;
		g_nTimeShift = AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("TimeShift"), 0);

		// Mask key in log so DbgView traces stay safe
		{
			CString boot;
			int n = g_oApiKey.GetLength();
			if (n > 8)
				boot.Format(_T("OpenAlgo: Init loaded ApiKey=%s...%s (len=%d)"),
					(LPCTSTR)g_oApiKey.Left(4), (LPCTSTR)g_oApiKey.Right(4), n);
			else
				boot.Format(_T("OpenAlgo: Init loaded ApiKey (len=%d)"), n);
			OutputDebugString(boot);
		}

		// Real-time candle building settings
		g_bRealTimeCandlesEnabled = AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("EnableRealTimeCandles"), 1);  // Default: enabled
		// Backfill cadence is user-controlled via BackfillRefreshIntervalSec.
		// BackfillIntervalMs is retained as a legacy mirror for diagnostics.
		g_nBackfillIntervalMs = g_nBackfillRefreshIntervalSec * 1000;

		g_nStatus = STATUS_WAIT;
		g_bPluginInitialized = TRUE;

		// Initialize quote cache
		g_QuoteCache.InitHashTable(997); // Prime number for better hash distribution

		// Initialize critical section for WebSocket operations
		InitializeCriticalSection(&g_WebSocketCriticalSection);
		g_bCriticalSectionInitialized = TRUE;

		// Send-side serialization (see comment near declaration)
		InitializeCriticalSection(&g_WebSocketSendCS);
		g_bWebSocketSendCSInit = TRUE;

		// Initialize critical section for BarBuilder operations
		InitializeCriticalSection(&g_BarBuilderCriticalSection);
		g_bBarBuilderCriticalSectionInitialized = TRUE;

		// Initialize BarBuilders hash table
		g_BarBuilders.InitHashTable(503);  // Prime number for better distribution

		// Initialize critical section for HTTP cache operations
		InitializeCriticalSection(&g_HttpCacheCriticalSection);
		g_bHttpCacheCriticalSectionInitialized = TRUE;

		// Initialize HTTP response cache hash table
		g_HttpResponseCache.InitHashTable(127);  // Prime number for better distribution

		// Fix #3: Initialize per-symbol bar cache + work queue and start the worker
		InitializeCriticalSection(&g_SymbolBarCacheCS);
		g_bSymbolBarCacheCSInitialized = TRUE;
		g_SymbolBarCache.InitHashTable(503);

		InitializeCriticalSection(&g_HttpWorkQueueCS);
		g_bHttpWorkQueueCSInitialized = TRUE;

		// Per-symbol RecentInfo map for the Realtime Quote Window
		InitializeCriticalSection(&g_RecentInfoCS);
		g_bRecentInfoCSInit = TRUE;
		g_RecentInfoMap.InitHashTable(127);

		StartHttpWorker();

		// Launch the WS reader thread before (or alongside) the initial
		// InitializeWebSocket() so reconnect attempts also run off the UI
		// thread. The thread is the only caller of recv() on g_websocket.
		StartWsReader();

		// Log real-time settings
		CString rtMsg;
		rtMsg.Format(_T("OpenAlgo: Real-Time Candles Enabled = %d, Backfill Refresh = %d sec, Heartbeat = %d sec"),
			g_bRealTimeCandlesEnabled, g_nBackfillRefreshIntervalSec,
			CONNECTION_HEARTBEAT_INTERVAL_SEC);
		OutputDebugString(rtMsg);

		// Initialize WebSocket connection early (don't wait for GetRecentInfo)
		OutputDebugString(_T("OpenAlgo: Init() - Initializing WebSocket connection..."));
		if (InitializeWebSocket())
		{
			OutputDebugString(_T("OpenAlgo: Init() - WebSocket initialized successfully"));
		}
		else
		{
			OutputDebugString(_T("OpenAlgo: Init() - WebSocket initialization failed (will retry later)"));
		}
	}

	OutputDebugString(_T("OpenAlgo: Init() completed successfully"));
	return 1;
}

PLUGINAPI int Release(void)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	// Stop the WS reader thread FIRST so it isn't sitting in select() on a
	// socket we're about to close from CleanupWebSocket().
	StopWsReader();

	// Fix #3: stop worker before touching shared state it might still be reading
	StopHttpWorker();

	// Clean up WebSocket connections
	CleanupWebSocket();

	// Clear cache
	g_QuoteCache.RemoveAll();

	// Clean up BarBuilders
	CleanupBarBuilders();

	// Fix #3: free per-symbol bar cache
	CleanupSymbolBarCache();

	// Clean up critical sections
	if (g_bCriticalSectionInitialized)
	{
		DeleteCriticalSection(&g_WebSocketCriticalSection);
		g_bCriticalSectionInitialized = FALSE;
	}

	if (g_bWebSocketSendCSInit)
	{
		DeleteCriticalSection(&g_WebSocketSendCS);
		g_bWebSocketSendCSInit = FALSE;
	}

	if (g_bBarBuilderCriticalSectionInitialized)
	{
		DeleteCriticalSection(&g_BarBuilderCriticalSection);
		g_bBarBuilderCriticalSectionInitialized = FALSE;
	}

	if (g_bSymbolBarCacheCSInitialized)
	{
		DeleteCriticalSection(&g_SymbolBarCacheCS);
		g_bSymbolBarCacheCSInitialized = FALSE;
	}

	if (g_bHttpWorkQueueCSInitialized)
	{
		// Drain any items still in the queue
		EnterCriticalSection(&g_HttpWorkQueueCS);
		g_HttpWorkQueue.RemoveAll();
		LeaveCriticalSection(&g_HttpWorkQueueCS);

		DeleteCriticalSection(&g_HttpWorkQueueCS);
		g_bHttpWorkQueueCSInitialized = FALSE;
	}

	// Free per-symbol RecentInfo entries
	if (g_bRecentInfoCSInit)
	{
		EnterCriticalSection(&g_RecentInfoCS);
		POSITION ripos = g_RecentInfoMap.GetStartPosition();
		while (ripos != NULL)
		{
			CString tk;
			struct RecentInfo* pInfo = NULL;
			g_RecentInfoMap.GetNextAssoc(ripos, tk, pInfo);
			if (pInfo) delete pInfo;
		}
		g_RecentInfoMap.RemoveAll();
		LeaveCriticalSection(&g_RecentInfoCS);

		DeleteCriticalSection(&g_RecentInfoCS);
		g_bRecentInfoCSInit = FALSE;
	}

	if (g_bHttpCacheCriticalSectionInitialized)
	{
		// Clean up HTTP cache - free allocated memory
		POSITION pos = g_HttpResponseCache.GetStartPosition();
		while (pos != NULL)
		{
			CString key;
			void* pValue;
			g_HttpResponseCache.GetNextAssoc(pos, key, pValue);
			if (pValue != NULL)
			{
				delete (DWORD*)pValue;
			}
		}
		g_HttpResponseCache.RemoveAll();

		DeleteCriticalSection(&g_HttpCacheCriticalSection);
		g_bHttpCacheCriticalSectionInitialized = FALSE;
	}

	return 1;
}

PLUGINAPI int Configure(LPCTSTR pszPath, struct InfoSite* pSite)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	COpenAlgoConfigDlg oDlg;
	oDlg.m_pSite = pSite;

	if (oDlg.DoModal() == IDOK)
	{
		// Force status update after config change
		if (g_hAmiBrokerWnd != NULL)
		{
			::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
		}
	}

	return 1;
}

PLUGINAPI AmiVar GetExtraData(LPCTSTR pszTicker, LPCTSTR pszName, int nArraySize, int nPeriodicity, void* (*pfAlloc)(unsigned int nSize))
{
	AmiVar var;
	var.type = VAR_NONE;
	var.val = 0;
	return var;
}

PLUGINAPI int SetTimeBase(int nTimeBase)
{
	return 1;
}

PLUGINAPI int GetSymbolLimit(void)
{
	return 1000; // Default symbol limit since we removed the configurable option
}

// CRITICAL FUNCTION - This makes the status LED work!
PLUGINAPI int GetStatus(struct PluginStatus* status)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	if (status == NULL) return 0;

	// MUST set structure size
	status->nStructSize = sizeof(struct PluginStatus);

	// Ensure we have valid status
	if (g_nStatus < STATUS_WAIT || g_nStatus > STATUS_SHUTDOWN)
	{
		g_nStatus = STATUS_WAIT;
	}

	switch (g_nStatus)
	{
	case STATUS_WAIT:
		status->nStatusCode = 0x10000000; // WARNING
		strcpy_s(status->szShortMessage, 32, "WAIT");
		strcpy_s(status->szLongMessage, 256, "OpenAlgo: Waiting to connect");
		status->clrStatusColor = RGB(255, 255, 0); // Yellow
		break;

	case STATUS_CONNECTED:
		status->nStatusCode = 0x00000000; // OK
		strcpy_s(status->szShortMessage, 32, "OK");
		strcpy_s(status->szLongMessage, 256, "OpenAlgo: Connected");
		status->clrStatusColor = RGB(0, 255, 0); // Green
		break;

	case STATUS_DISCONNECTED:
		status->nStatusCode = 0x20000000; // MINOR ERROR
		strcpy_s(status->szShortMessage, 32, "ERR");
		strcpy_s(status->szLongMessage, 256, "OpenAlgo: Connection failed. Will retry in 15 seconds.");
		status->clrStatusColor = RGB(255, 0, 0); // Red
		break;

	case STATUS_SHUTDOWN:
		status->nStatusCode = 0x30000000; // SEVERE ERROR
		strcpy_s(status->szShortMessage, 32, "OFF");
		strcpy_s(status->szLongMessage, 256, "OpenAlgo: Offline. Right-click to reconnect.");
		status->clrStatusColor = RGB(192, 0, 192); // Purple
		break;

	default:
		status->nStatusCode = 0x30000000; // SEVERE ERROR
		strcpy_s(status->szShortMessage, 32, "???");
		strcpy_s(status->szLongMessage, 256, "OpenAlgo: Unknown status");
		status->clrStatusColor = RGB(128, 128, 128); // Gray
		break;
	}

	return 1; // MUST return 1!
}

CString GetAvailableSymbols(void)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	CString oResult;
	try
	{
		CString oURL = BuildOpenAlgoURL(g_oServer, g_nPortNumber, _T("/api/v1/symbols"));

		CInternetSession oSession(AGENT_NAME, 1, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, INTERNET_FLAG_DONT_CACHE);
		oSession.SetOption(INTERNET_OPTION_CONNECT_TIMEOUT, 5000);
		oSession.SetOption(INTERNET_OPTION_RECEIVE_TIMEOUT, 5000);

		CStdioFile* poFile = oSession.OpenURL(oURL, 1,
			INTERNET_FLAG_TRANSFER_ASCII | INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE);

		if (poFile)
		{
			CString oLine;
			if (poFile->ReadString(oLine))
			{
				if (oLine.Left(2) == _T("OK"))
				{
					poFile->ReadString(oResult);
				}
				else
				{
					oResult = oLine;
				}
			}
			poFile->Close();
			delete poFile;
		}
		oSession.Close();
	}
	catch (CInternetException* e)
	{
		e->Delete();
		g_nStatus = STATUS_DISCONNECTED;
	}
	return oResult;
}

BOOL TestOpenAlgoConnection(void)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	// Check if API key is configured
	if (g_oApiKey.IsEmpty())
	{
		return FALSE;
	}

	BOOL bConnected = FALSE;

	try
	{
		CString oURL = BuildOpenAlgoURL(g_oServer, g_nPortNumber, _T("/api/v1/ping"));

		CInternetSession oSession(AGENT_NAME, 1, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL,
			INTERNET_FLAG_DONT_CACHE);
		oSession.SetOption(INTERNET_OPTION_CONNECT_TIMEOUT, 2000);
		oSession.SetOption(INTERNET_OPTION_RECEIVE_TIMEOUT, 2000);

		// Prepare POST data with API key
		CString oPostData;
		oPostData.Format(_T("{\"apikey\":\"%s\"}"), (LPCTSTR)g_oApiKey);

		CHttpConnection* pConnection = NULL;
		CHttpFile* pFile = NULL;

		// Parse server and port
		INTERNET_PORT nPort = (INTERNET_PORT)g_nPortNumber;
		CString oServer = g_oServer;

		// Remove http:// or https:// if present
		oServer.Replace(_T("http://"), _T(""));
		oServer.Replace(_T("https://"), _T(""));

		pConnection = oSession.GetHttpConnection(oServer, nPort);

		if (pConnection)
		{
			// Create POST request
			pFile = pConnection->OpenRequest(
				CHttpConnection::HTTP_VERB_POST,
				_T("/api/v1/ping"),
				NULL,
				1,
				NULL,
				NULL,
				INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE);

			if (pFile)
			{
				// Set headers
				CString oHeaders = _T("Content-Type: application/json\r\n");

				// Convert string to UTF-8 for sending
				CStringA oPostDataA(oPostData);

				// Send the request
				BOOL bResult = pFile->SendRequest(oHeaders, (LPVOID)(LPCSTR)oPostDataA, oPostDataA.GetLength());

				if (bResult)
				{
					DWORD dwStatusCode = 0;
					pFile->QueryInfoStatusCode(dwStatusCode);

					if (dwStatusCode == 200)
					{
						// Read response to verify it's valid
						// Fix #6: ping body is small; preallocate to skip reallocations
						CString oResponse;
						oResponse.Preallocate(512);
						CString oLine;
						while (pFile->ReadString(oLine))
						{
							oResponse += oLine;
							if (oResponse.GetLength() > 500) break; // Limit response size
						}

						// Check if response contains "success" and "pong"
						if ((oResponse.Find(_T("\"status\":\"success\"")) >= 0 ||
							 oResponse.Find(_T("\"status\": \"success\"")) >= 0) &&
							(oResponse.Find(_T("\"message\":\"pong\"")) >= 0 ||
							 oResponse.Find(_T("\"message\": \"pong\"")) >= 0))
						{
							bConnected = TRUE;
						}
					}
				}

				pFile->Close();
				delete pFile;
			}

			pConnection->Close();
			delete pConnection;
		}

		oSession.Close();
	}
	catch (CInternetException* e)
	{
		e->Delete();
		bConnected = FALSE;
	}

	return bConnected;
}

void SetupRetry(void)
{
	if (--g_nRetryCount > 0)
	{
		if (g_hAmiBrokerWnd != NULL)
		{
			SetTimer(g_hAmiBrokerWnd, TIMER_INIT, 15000, (TIMERPROC)OnTimerProc);
		}
		g_nStatus = STATUS_DISCONNECTED;
	}
	else
	{
		g_nStatus = STATUS_SHUTDOWN;
	}

	if (g_hAmiBrokerWnd != NULL)
	{
		::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
	}
}

VOID CALLBACK OnTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	// TIMER_WEBSOCKET was previously fired from the AmiBroker UI thread every
	// 100 ms to drain the WS socket. WM_TIMER messages are coalesced when the
	// UI is busy (chart paint, indicator recompute), so ticks were arriving
	// 2-7 s late. The WS read now runs on a dedicated WsReaderThread that
	// blocks in select() and never depends on the UI message pump.

	if (idEvent == TIMER_INIT || idEvent == TIMER_REFRESH)
	{
		if (!TestOpenAlgoConnection())
		{
			if (g_hAmiBrokerWnd != NULL)
			{
				KillTimer(g_hAmiBrokerWnd, idEvent);
			}
			SetupRetry();
			return;
		}

		g_nStatus = STATUS_CONNECTED;
		g_nRetryCount = RETRY_COUNT;

		if (g_hAmiBrokerWnd != NULL)
		{
			::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);

			if (idEvent == TIMER_INIT)
			{
				KillTimer(g_hAmiBrokerWnd, TIMER_INIT);
				SetTimer(g_hAmiBrokerWnd, TIMER_REFRESH,
					CONNECTION_HEARTBEAT_INTERVAL_SEC * 1000,
					(TIMERPROC)OnTimerProc);
			}
		}
	}
}

PLUGINAPI int Notify(struct PluginNotification* pn)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	if (pn == NULL) return 0;

	// Database loaded - start connection
	if ((pn->nReason & REASON_DATABASE_LOADED))
	{
		g_hAmiBrokerWnd = pn->hMainWnd;

		// Same defensive SetRegistryKey as Init() — ensures the reload below
		// reads from the same registry root that Configure/OnOK writes to.
		theApp.EnsureRegistryRoot();

		// Reload settings (direct registry read for the API key — see Init)
		g_oServer = AfxGetApp()->GetProfileString(_T("OpenAlgo"), _T("Server"), _T("127.0.0.1"));
		if (!ReadApiKeyDirect(g_oApiKey) || g_oApiKey.IsEmpty())
		{
			g_oApiKey = AfxGetApp()->GetProfileString(_T("OpenAlgo"), _T("ApiKey"), _T(""));
		}
		g_oWebSocketUrl = AfxGetApp()->GetProfileString(_T("OpenAlgo"), _T("WebSocketUrl"), _T("ws://127.0.0.1:8765"));  // Load WebSocket URL
		g_nPortNumber = AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("Port"), 5000);
		g_nRefreshInterval = CONNECTION_HEARTBEAT_INTERVAL_SEC;
		g_nBackfillRefreshIntervalSec = ClampBackfillRefreshIntervalSec(
			AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("BackfillRefreshIntervalSec"), 30));
		g_nBackfillIntervalMs = g_nBackfillRefreshIntervalSec * 1000;

		g_nStatus = STATUS_WAIT;
		g_nRetryCount = RETRY_COUNT;

		// Start connection timer
		if (g_hAmiBrokerWnd != NULL)
		{
			SetTimer(g_hAmiBrokerWnd, TIMER_INIT, 1000, (TIMERPROC)OnTimerProc);

			// WS draining now runs on the dedicated WsReaderThread (started in
			// Init via StartWsReader), so we no longer schedule TIMER_WEBSOCKET.

			// Force immediate status update
			::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
		}
	}

	// Database unloaded - cleanup
	if (pn->nReason & REASON_DATABASE_UNLOADED)
	{
		if (g_hAmiBrokerWnd != NULL)
		{
			KillTimer(g_hAmiBrokerWnd, TIMER_INIT);
			KillTimer(g_hAmiBrokerWnd, TIMER_REFRESH);
			// (TIMER_WEBSOCKET no longer created; nothing to kill)
		}
		g_hAmiBrokerWnd = NULL;
		g_nStatus = STATUS_SHUTDOWN;

		free(g_aInfos);
		g_aInfos = NULL;
		RecentInfoSize = 0;

		// Clear cache
		g_QuoteCache.RemoveAll();
	}

	// Right-click on status area - show menu
	if (pn->nReason & REASON_STATUS_RMBCLICK)
	{
		HWND hWnd = pn->hMainWnd ? pn->hMainWnd : g_hAmiBrokerWnd;
		if (hWnd != NULL)
		{
			HMENU hMenu = CreatePopupMenu();

			// Connection control
			if (g_nStatus == STATUS_SHUTDOWN || g_nStatus == STATUS_DISCONNECTED)
			{
				AppendMenu(hMenu, MF_STRING | MF_ENABLED, 1, _T("Connect"));
			}
			else
			{
				AppendMenu(hMenu, MF_STRING | MF_ENABLED, 2, _T("Disconnect"));
			}

			AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

			// Backfill submenu for Intraday (1-minute)
			HMENU hBackfill1M = CreatePopupMenu();
			AppendMenu(hBackfill1M, MF_STRING | MF_ENABLED, 105, _T("3 Months (Current Symbol)"));
			AppendMenu(hBackfill1M, MF_STRING | MF_ENABLED, 106, _T("3 Months (All Symbols)"));
			AppendMenu(hBackfill1M, MF_STRING | MF_ENABLED, 107, _T("6 Months (Current Symbol)"));
			AppendMenu(hBackfill1M, MF_STRING | MF_ENABLED, 108, _T("6 Months (All Symbols)"));
			AppendMenu(hBackfill1M, MF_STRING | MF_ENABLED, 109, _T("1 Year (Current Symbol)"));
			AppendMenu(hBackfill1M, MF_STRING | MF_ENABLED, 110, _T("1 Year (All Symbols)"));

			// Backfill submenu for Daily (EOD)
			HMENU hBackfillDaily = CreatePopupMenu();
			AppendMenu(hBackfillDaily, MF_STRING | MF_ENABLED, 201, _T("5 Years (Current Symbol)"));
			AppendMenu(hBackfillDaily, MF_STRING | MF_ENABLED, 202, _T("5 Years (All Symbols)"));
			AppendMenu(hBackfillDaily, MF_STRING | MF_ENABLED, 203, _T("10 Years (Current Symbol)"));
			AppendMenu(hBackfillDaily, MF_STRING | MF_ENABLED, 204, _T("10 Years (All Symbols)"));
			AppendMenu(hBackfillDaily, MF_STRING | MF_ENABLED, 205, _T("25 Years (Current Symbol)"));
			AppendMenu(hBackfillDaily, MF_STRING | MF_ENABLED, 206, _T("25 Years (All Symbols)"));

			// Add submenus to main menu
			AppendMenu(hMenu, MF_POPUP | MF_ENABLED, (UINT_PTR)hBackfill1M, _T("Backfill 1-Minute Data"));
			AppendMenu(hMenu, MF_POPUP | MF_ENABLED, (UINT_PTR)hBackfillDaily, _T("Backfill Daily Data"));

			AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
			AppendMenu(hMenu, MF_STRING | MF_ENABLED, 3, _T("Configure..."));

			POINT pt;
			GetCursorPos(&pt);

			int nCmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_NONOTIFY,
				pt.x, pt.y, 0, hWnd, NULL);

			// Destroy menus after use (submenus are destroyed automatically with parent)
			DestroyMenu(hMenu);

			// Resolve the "current symbol" once for all the Current-Symbol menu
			// cases below. PluginNotification.pCurrentSINew->ShortName is the
			// ticker AmiBroker is currently displaying; this lets us enqueue a
			// per-symbol HTTP fetch directly instead of relying on the next
			// generic WSUM hop (which would race a routine fetch in progress
			// and could be dropped by the in-progress flag).
			CString currentTicker;
			if (pn->pCurrentSINew != NULL && pn->pCurrentSINew->ShortName[0] != 0)
				currentTicker = pn->pCurrentSINew->ShortName;
			else if (pn->pCurrentSI != NULL && pn->pCurrentSI->ShortName[0] != 0)
				currentTicker = pn->pCurrentSI->ShortName;
			else if (g_bSymbolBarCacheCSInitialized)
			{
				EnterCriticalSection(&g_SymbolBarCacheCS);
				currentTicker = g_LastRequestedTicker;
				LeaveCriticalSection(&g_SymbolBarCacheCS);
			}

			// Helper macro -- declared before the switch because C++ doesn't
			// allow initialized declarations between case labels.
			#define BACKFILL_CURRENT(daysVal, perVal)                              \
				do {                                                               \
					if (!currentTicker.IsEmpty()) {                                \
						QueueHttpFetch(currentTicker, (perVal), (daysVal));        \
						CString log;                                                \
						log.Format(_T("OpenAlgo: manual backfill queued current symbol %s, periodicity=%d, days=%d"), \
							(LPCTSTR)currentTicker, (perVal), (daysVal));          \
						OutputDebugString(log);                                     \
					} else {                                                       \
						g_nBackfillDays        = (daysVal);                        \
						g_nBackfillPeriodicity = (perVal);                         \
						g_bBackfillRequested   = TRUE;                             \
						OutputDebugString(_T("OpenAlgo: manual backfill queued as deferred current-symbol request")); \
					}                                                              \
					::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0); \
				} while (0)

			switch (nCmd)
			{
			case 1: // Connect
				g_nStatus = STATUS_WAIT;
				g_nRetryCount = RETRY_COUNT;
				SetTimer(g_hAmiBrokerWnd, TIMER_INIT, 1000, (TIMERPROC)OnTimerProc);
				break;

			case 2: // Disconnect
				KillTimer(g_hAmiBrokerWnd, TIMER_INIT);
				KillTimer(g_hAmiBrokerWnd, TIMER_REFRESH);
				g_nStatus = STATUS_SHUTDOWN;
				break;

			case 3: // Configure
				Configure(pn->pszDatabasePath, NULL);
				break;

			// 1-Minute backfill options.
			// "Current Symbol" enqueues a per-symbol fetch directly via
			// QueueHttpFetch with nForceDays, so the user-requested range is
			// honored even if a routine refresh is mid-flight. "All Symbols"
			// enumerates both chart-requested symbols and WS-subscribed symbols.
			case 105: // 3 Months - Current Symbol
				BACKFILL_CURRENT(90, 60);
				break;
			case 106: // 3 Months - All Symbols
			case 108: // 6 Months - All Symbols
			case 110: // 1 Year - All Symbols
			case 202: // Daily 5 Years - All Symbols
			case 204: // Daily 10 Years - All Symbols
			case 206: // Daily 25 Years - All Symbols
			{
				int days = 0; int per = 0;
				switch (nCmd)
				{
				case 106: days = 90;   per = 60;    break;
				case 108: days = 180;  per = 60;    break;
				case 110: days = 365;  per = 60;    break;
				case 202: days = 1825; per = 86400; break;
				case 204: days = 3650; per = 86400; break;
				case 206: days = 9125; per = 86400; break;
				}

				CArray<CString, CString&> symbols;

				// Chart-requested symbols live in the HTTP cache map, even if
				// they are not currently present in the Realtime Quote window.
				EnterCriticalSection(&g_SymbolBarCacheCS);
				POSITION cpos = g_SymbolBarCache.GetStartPosition();
				while (cpos != NULL)
				{
					CString sym;
					SymbolBarCache* pCacheUnused = NULL;
					g_SymbolBarCache.GetNextAssoc(cpos, sym, pCacheUnused);
					symbols.Add(sym);
				}
				LeaveCriticalSection(&g_SymbolBarCacheCS);

				// Add WS-subscribed symbols too; skip duplicates.
				EnterCriticalSection(&g_WebSocketCriticalSection);
				POSITION wpos = g_SubscribedSymbols.GetStartPosition();
				while (wpos != NULL)
				{
					CString sym;
					BOOL bSub;
					g_SubscribedSymbols.GetNextAssoc(wpos, sym, bSub);
					BOOL bExists = FALSE;
					for (int i = 0; i < symbols.GetCount(); i++)
					{
						if (symbols[i] == sym)
						{
							bExists = TRUE;
							break;
						}
					}
					if (!bExists)
						symbols.Add(sym);
				}
				LeaveCriticalSection(&g_WebSocketCriticalSection);

				for (int i = 0; i < symbols.GetCount(); i++)
					QueueHttpFetch(symbols[i], per, days);

				CString log;
				log.Format(_T("OpenAlgo: manual backfill queued %d symbol(s), periodicity=%d, days=%d"),
					(int)symbols.GetCount(), per, days);
				OutputDebugString(log);

				::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
				break;
			}
			case 107: BACKFILL_CURRENT(180,  60);    break;  // 6 Months
			case 109: BACKFILL_CURRENT(365,  60);    break;  // 1 Year
			case 201: BACKFILL_CURRENT(1825, 86400); break;  // Daily 5 Years
			case 203: BACKFILL_CURRENT(3650, 86400); break;  // Daily 10 Years
			case 205: BACKFILL_CURRENT(9125, 86400); break;  // Daily 25 Years
			#undef BACKFILL_CURRENT
			}

			// Update status display
			::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
		}
	}

	return 1;
}

PLUGINAPI int GetQuotes(LPCTSTR pszTicker, int nPeriodicity, int nLastValid, int nSize, struct QuotationFormat4* pQuotes)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	Quotation* pQuote5 = (struct Quotation*)malloc(nSize * sizeof(Quotation));
	QuotationFormat4* src = pQuotes;
	Quotation* dst = pQuote5;

	int i;
	for (i = 0; i <= nLastValid; i++, src++, dst++)
	{
		ConvertFormat4Quote(src, dst);
	}

	int nQty = GetQuotesEx(pszTicker, nPeriodicity, nLastValid, nSize, pQuote5, NULL);

	dst = pQuote5;
	src = pQuotes;

	for (i = 0; i < nQty; i++, dst++, src++)
	{
		ConvertFormat5Quote(dst, src);
	}

	free(pQuote5);
	return nQty;
}

// Main quote retrieval function - ZERO exchange restrictions
// Handles ALL trading scenarios dynamically through OpenAlgo server.
// Fix #3: This function now runs entirely on cached data. The HTTP backfill
// happens in HttpWorkerThreadProc on a background thread; when fresh data
// lands in the per-symbol SymbolBarCache the worker posts WM_USER_STREAMING_UPDATE
// so AmiBroker re-calls us and we serve the new snapshot synchronously.
PLUGINAPI int GetQuotesEx(LPCTSTR pszTicker, int nPeriodicity, int nLastValid, int nSize, struct Quotation* pQuotes, GQEContext* pContext)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	if (g_nStatus == STATUS_DISCONNECTED || g_nStatus == STATUS_SHUTDOWN)
		return nLastValid + 1;

	if (nPeriodicity != 60 && nPeriodicity != 86400)
		return nLastValid + 1;   // Only 1m and Daily supported

	CString ticker(pszTicker);
	SymbolBarCache* pCache = GetOrCreateSymbolBarCache(ticker);
	DWORD now = (DWORD)GetTickCount64();

	EnterCriticalSection(&g_SymbolBarCacheCS);
	g_LastRequestedTicker = ticker;
	LeaveCriticalSection(&g_SymbolBarCacheCS);

	// Honor manual backfill request from the right-click menu: invalidate the
	// matching cache so the worker can refill it with the user-requested range.
	// The per-symbol fetch with nForceDays is enqueued by Notify(); here we
	// only make sure we don't keep serving stale data on top of it.
	if (g_bBackfillRequested && g_nBackfillPeriodicity == nPeriodicity && g_nBackfillDays > 0)
	{
		EnterCriticalSection(&g_SymbolBarCacheCS);
		if (nPeriodicity == 60)
		{
			pCache->oneMinBars.RemoveAll();
			pCache->lastOneMinFetch = 0;
		}
		else
		{
			pCache->dailyBars.RemoveAll();
			pCache->lastDailyFetch = 0;
		}
		LeaveCriticalSection(&g_SymbolBarCacheCS);
	}

	int nQty = 0;
	BOOL bNeedDaily  = FALSE;
	BOOL bNeedOneMin = FALSE;

	EnterCriticalSection(&g_SymbolBarCacheCS);
	int dailyCount  = (int)pCache->dailyBars.GetCount();
	int oneMinCount = (int)pCache->oneMinBars.GetCount();
	BOOL bDailyStale  = (pCache->lastDailyFetch == 0) ||
	                    ((now - pCache->lastDailyFetch) > DAILY_CACHE_LIFETIME_MS);
	BOOL bOneMinStale = (pCache->lastOneMinFetch == 0) ||
	                    ((now - pCache->lastOneMinFetch) > GetOneMinCacheLifetimeMs());

	if (nPeriodicity == 86400)
	{
		if (dailyCount > 0 && nSize > 0)
		{
			int copyCount = min(dailyCount, nSize);
			memcpy(pQuotes, pCache->dailyBars.GetData(), copyCount * sizeof(struct Quotation));
			nQty = copyCount;
		}
		if (bDailyStale && !pCache->bDailyFetchInProgress)
		{
			pCache->bDailyFetchInProgress = TRUE;
			bNeedDaily = TRUE;
		}
	}
	else
	{
		// 1-minute periodicity (also serves Mixed EOD/Intraday daily compression).
		//
		// Bug we're fixing here:
		//   Previously we memcpy'd [all daily bars] then [all 1m bars]. Each
		//   slice was sorted internally, but the concatenation was NOT --
		//   today's daily EOD bar (Hour=31) sat *before* a 1-min bar from 30
		//   days ago in the array. With AmiBroker's Mixed EOD/Intraday mode,
		//   that out-of-order layout caused today's daily candle to absorb
		//   the H/L of every 1-min bar in the file, displaying the month's
		//   range on today's bar. Probe of the live history endpoint
		//   confirmed the server returns the right per-day values; the bug
		//   was purely the array layout.
		//
		// Fix: emit each daily EOD bar only for dates that have no 1-minute
		// coverage. AmiBroker compresses the 1-minute range to daily on its
		// own for the overlap window. The result is fully chronological with
		// no duplicates, so today's daily candle is composed from today's
		// (and only today's) 1-minute data + the live BarBuilder bar.
		int dailyToCopy = dailyCount;
		if (dailyCount > 0 && oneMinCount > 0)
		{
			AmiDate firstOneMin = pCache->oneMinBars[0].DateTime;
			unsigned int oY = firstOneMin.PackDate.Year;
			unsigned int oM = firstOneMin.PackDate.Month;
			unsigned int oD = firstOneMin.PackDate.Day;
			dailyToCopy = 0;
			for (int i = 0; i < dailyCount; i++)
			{
				AmiDate d = pCache->dailyBars[i].DateTime;
				BOOL strictlyBefore =
					(d.PackDate.Year <  oY) ||
					(d.PackDate.Year == oY && d.PackDate.Month <  oM) ||
					(d.PackDate.Year == oY && d.PackDate.Month == oM && d.PackDate.Day < oD);
				if (strictlyBefore)
					dailyToCopy = i + 1;
				else
					break;   // cache is sorted; everything after also overlaps
			}
		}

		if (dailyToCopy > 0 && nSize > 0)
		{
			int copyCount = min(dailyToCopy, nSize);
			memcpy(pQuotes, pCache->dailyBars.GetData(), copyCount * sizeof(struct Quotation));
			nQty = copyCount;
		}
		if (oneMinCount > 0 && nQty < nSize)
		{
			int copyCount = min(oneMinCount, nSize - nQty);
			memcpy(pQuotes + nQty, pCache->oneMinBars.GetData(),
			       copyCount * sizeof(struct Quotation));
			nQty += copyCount;
		}
		if (bDailyStale && !pCache->bDailyFetchInProgress)
		{
			pCache->bDailyFetchInProgress = TRUE;
			bNeedDaily = TRUE;
		}
		if (bOneMinStale && !pCache->bOneMinFetchInProgress)
		{
			pCache->bOneMinFetchInProgress = TRUE;
			bNeedOneMin = TRUE;
		}
	}
	LeaveCriticalSection(&g_SymbolBarCacheCS);

	// First call for a symbol: cache empty, fetch in flight. Keep whatever
	// AmiBroker already had so the chart does not blank out during the
	// async warmup. The worker's WM_USER_STREAMING_UPDATE will re-invoke us.
	if (nQty == 0)
		nQty = nLastValid + 1;

	// Queue background refreshes (1m first so live charts unblock fastest)
	if (bNeedOneMin) QueueHttpFetch(ticker, 60, 0);
	if (bNeedDaily)  QueueHttpFetch(ticker, 86400, 0);

	// Daily-only request stops here
	if (nPeriodicity == 86400)
		return nQty;

	// ============ 1-minute real-time overlay ============
	if (!g_bRealTimeCandlesEnabled)
		return nQty;

	// Auto-subscribe to WS feed for this symbol so ticks start flowing
	BOOL bSubscribed = FALSE;
	EnterCriticalSection(&g_WebSocketCriticalSection);
	if (!g_SubscribedSymbols.Lookup(ticker, bSubscribed))
	{
		if (g_bWebSocketConnected && SubscribeToSymbol(pszTicker))
			g_SubscribedSymbols.SetAt(ticker, TRUE);
	}
	LeaveCriticalSection(&g_WebSocketCriticalSection);

	// Merge BarBuilder's completed bars (Fix #2) + overlay in-progress bar
	EnterCriticalSection(&g_BarBuilderCriticalSection);
	BarBuilder* pBuilder = NULL;
	if (g_BarBuilders.Lookup(ticker, pBuilder) && pBuilder != NULL)
	{
		int barsCount = (int)pBuilder->bars.GetCount();
		if (barsCount > 0)
		{
			DATE_TIME_INT lastTs = (nQty > 0) ? pQuotes[nQty - 1].DateTime.Date : 0;
			for (int i = 0; i < barsCount && nQty < nSize; i++)
			{
				DATE_TIME_INT bts = pBuilder->bars[i].DateTime.Date;
				if (bts > lastTs)
				{
					pQuotes[nQty++] = pBuilder->bars[i];
					lastTs = bts;
				}
			}
		}

		if (pBuilder->bBarStarted)
		{
			int idx = nQty;
			BOOL bReplaced = FALSE;
			if (nQty > 0)
			{
				AmiDate lastDt = pQuotes[nQty - 1].DateTime;
				AmiDate tickDt = pBuilder->currentBar.DateTime;
				if (lastDt.PackDate.Year   == tickDt.PackDate.Year   &&
				    lastDt.PackDate.Month  == tickDt.PackDate.Month  &&
				    lastDt.PackDate.Day    == tickDt.PackDate.Day    &&
				    lastDt.PackDate.Hour   == tickDt.PackDate.Hour   &&
				    lastDt.PackDate.Minute == tickDt.PackDate.Minute)
				{
					idx = nQty - 1;
					bReplaced = TRUE;
				}
			}
			if (idx < nSize)
			{
				pQuotes[idx] = pBuilder->currentBar;
				if (!bReplaced) nQty++;
			}
		}
	}
	LeaveCriticalSection(&g_BarBuilderCriticalSection);

	return nQty;
}


// Get-or-create a persistent RecentInfo struct for a ticker. The pointer
// stays valid for the lifetime of the plugin, so it's safe to (a) return to
// AmiBroker from GetRecentInfo and (b) pass as LPARAM in a posted
// WM_USER_STREAMING_UPDATE.
struct RecentInfo* GetOrCreateRecentInfoEntry(const CString& ticker)
{
	struct RecentInfo* pInfo = NULL;
	EnterCriticalSection(&g_RecentInfoCS);
	if (!g_RecentInfoMap.Lookup(ticker, pInfo) || pInfo == NULL)
	{
		pInfo = new struct RecentInfo();
		memset(pInfo, 0, sizeof(struct RecentInfo));
		pInfo->nStructSize = sizeof(struct RecentInfo);

		// Pre-populate name + exchange so the Quote Window column always shows
		// the right ticker even before the first frame arrives.
		CStringA tickerA(ticker);
		strncpy_s(pInfo->Name, sizeof(pInfo->Name), tickerA, _TRUNCATE);
		CStringA exchA(GetExchangeFromTicker(ticker));
		strncpy_s(pInfo->Exchange, sizeof(pInfo->Exchange), exchA, _TRUNCATE);

		g_RecentInfoMap.SetAt(ticker, pInfo);
	}
	LeaveCriticalSection(&g_RecentInfoCS);
	return pInfo;
}

void NotifyBarsReadyForSymbol(const CString& ticker)
{
	if (g_hAmiBrokerWnd == NULL)
		return;

	if (!g_bRecentInfoCSInit)
	{
		PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
		return;
	}

	struct RecentInfo* pInfo = GetOrCreateRecentInfoEntry(ticker);
	if (pInfo == NULL)
	{
		PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
		return;
	}

	CTime nowT = CTime::GetCurrentTime();
	EnterCriticalSection(&g_RecentInfoCS);
	pInfo->nStatus = RI_STATUS_UPDATE | RI_STATUS_BARSREADY;
	pInfo->nDateUpdate = nowT.GetYear() * 10000 + nowT.GetMonth() * 100 + nowT.GetDay();
	pInfo->nTimeUpdate = nowT.GetHour() * 10000 + nowT.GetMinute() * 100 + nowT.GetSecond();
	pInfo->nBitmap |= RI_DATEUPDATE;
	LeaveCriticalSection(&g_RecentInfoCS);

	SendMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, (LPARAM)pInfo);
	SendMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
}

void NotifyRecentInfoUpdate(struct RecentInfo* pInfo, BOOL bRealtimeEvent)
{
	if (g_hAmiBrokerWnd == NULL)
		return;

	// AmiBroker updates the quote window by polling GetRecentInfo(), but
	// Bid/Ask Trend and Time & Sales require a per-symbol RecentInfo pointer
	// in LPARAM. A catch-all WM_USER_STREAMING_UPDATE only refreshes charts.
	if (bRealtimeEvent && pInfo != NULL)
	{
		SendMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, (LPARAM)pInfo);
		return;
	}

	PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
}

void ForceAmiBrokerRefreshAll(void)
{
#ifndef _AFX_NO_OLE_SUPPORT
	HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BOOL bCoUninit = SUCCEEDED(hrCo);
	if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
	{
		CString log;
		log.Format(_T("OpenAlgo: RefreshAll skipped, CoInitializeEx failed hr=0x%08lX"), (unsigned long)hrCo);
		OutputDebugString(log);
		return;
	}

	CLSID clsid;
	HRESULT hr = CLSIDFromProgID(L"Broker.Application", &clsid);
	if (SUCCEEDED(hr))
	{
		IUnknown* pUnknown = NULL;
		IDispatch* pDispatch = NULL;
		hr = GetActiveObject(clsid, NULL, &pUnknown);
		if (SUCCEEDED(hr) && pUnknown != NULL)
		{
			hr = pUnknown->QueryInterface(IID_IDispatch, (void**)&pDispatch);
			pUnknown->Release();
		}
		else
		{
			hr = CoCreateInstance(clsid, NULL, CLSCTX_LOCAL_SERVER,
			                      IID_IDispatch, (void**)&pDispatch);
		}

		if (SUCCEEDED(hr) && pDispatch != NULL)
		{
			OLECHAR methodName[] = L"RefreshAll";
			LPOLESTR pMethodName = methodName;
			DISPID dispid = DISPID_UNKNOWN;
			hr = pDispatch->GetIDsOfNames(IID_NULL, &pMethodName, 1,
			                              LOCALE_USER_DEFAULT, &dispid);
			if (SUCCEEDED(hr))
			{
				DISPPARAMS params = { NULL, NULL, 0, 0 };
				EXCEPINFO excep = {0};
				UINT argErr = 0;
				hr = pDispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
				                       DISPATCH_METHOD, &params, NULL,
				                       &excep, &argErr);
			}
			pDispatch->Release();
		}
	}

	CString log;
	log.Format(_T("OpenAlgo: AmiBroker RefreshAll result hr=0x%08lX"), (unsigned long)hr);
	OutputDebugString(log);

	if (bCoUninit)
		CoUninitialize();
#endif
}

BOOL GetActiveAmiBrokerSymbol(CString& activeTicker)
{
	activeTicker.Empty();

#ifndef _AFX_NO_OLE_SUPPORT
	HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BOOL bCoUninit = SUCCEEDED(hrCo);
	if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
		return FALSE;

	BOOL bOK = FALSE;
	CLSID clsid;
	HRESULT hr = CLSIDFromProgID(L"Broker.Application", &clsid);
	if (SUCCEEDED(hr))
	{
		IUnknown* pUnknown = NULL;
		IDispatch* pApp = NULL;
		hr = GetActiveObject(clsid, NULL, &pUnknown);
		if (SUCCEEDED(hr) && pUnknown != NULL)
		{
			hr = pUnknown->QueryInterface(IID_IDispatch, (void**)&pApp);
			pUnknown->Release();
		}

		if (SUCCEEDED(hr) && pApp != NULL)
		{
			OLECHAR activeDocName[] = L"ActiveDocument";
			LPOLESTR pActiveDocName = activeDocName;
			DISPID dispidActiveDoc = DISPID_UNKNOWN;
			hr = pApp->GetIDsOfNames(IID_NULL, &pActiveDocName, 1,
			                         LOCALE_USER_DEFAULT, &dispidActiveDoc);
			if (SUCCEEDED(hr))
			{
				DISPPARAMS noArgs = { NULL, NULL, 0, 0 };
				VARIANT docVar;
				VariantInit(&docVar);
				hr = pApp->Invoke(dispidActiveDoc, IID_NULL, LOCALE_USER_DEFAULT,
				                  DISPATCH_PROPERTYGET, &noArgs, &docVar, NULL, NULL);
				if (SUCCEEDED(hr) && docVar.vt == VT_DISPATCH && docVar.pdispVal != NULL)
				{
					OLECHAR nameProp[] = L"Name";
					LPOLESTR pNameProp = nameProp;
					DISPID dispidName = DISPID_UNKNOWN;
					hr = docVar.pdispVal->GetIDsOfNames(IID_NULL, &pNameProp, 1,
					                                    LOCALE_USER_DEFAULT, &dispidName);
					if (SUCCEEDED(hr))
					{
						VARIANT nameVar;
						VariantInit(&nameVar);
						hr = docVar.pdispVal->Invoke(dispidName, IID_NULL, LOCALE_USER_DEFAULT,
						                             DISPATCH_PROPERTYGET, &noArgs,
						                             &nameVar, NULL, NULL);
						if (SUCCEEDED(hr) && nameVar.vt == VT_BSTR && nameVar.bstrVal != NULL)
						{
							activeTicker = nameVar.bstrVal;
							activeTicker.Trim();
							bOK = !activeTicker.IsEmpty();
						}
						VariantClear(&nameVar);
					}
				}
				VariantClear(&docVar);
			}
			pApp->Release();
		}
	}

	if (bCoUninit)
		CoUninitialize();

	if (bOK)
		return TRUE;
#endif

	if (g_bSymbolBarCacheCSInitialized)
	{
		EnterCriticalSection(&g_SymbolBarCacheCS);
		activeTicker = g_LastRequestedTicker;
		LeaveCriticalSection(&g_SymbolBarCacheCS);
		activeTicker.Trim();
		return !activeTicker.IsEmpty();
	}

	return FALSE;
}

BOOL IsActiveChartSymbol(const CString& ticker)
{
	CString activeTicker;
	if (!GetActiveAmiBrokerSymbol(activeTicker))
		return FALSE;

	BOOL bMatch = (activeTicker.CompareNoCase(ticker) == 0);
	if (bMatch)
	{
		CString log;
		log.Format(_T("OpenAlgo: active chart symbol matched %s for refresh"), (LPCTSTR)ticker);
		OutputDebugString(log);
	}
	return bMatch;
}

// GetRecentInfo is ONLY for the Realtime Quote Window + Time & Sales.
// WS-only -- no HTTP fallback. The broker's /api/v1/quotes endpoint is
// severely rate-limited and was driving the empty rows for slower symbols.
// Mode 1/2/3 WS frames now populate a persistent per-symbol RecentInfo
// entry on the worker thread; this function just returns the pointer to it.
PLUGINAPI struct RecentInfo* GetRecentInfo(LPCTSTR pszTicker)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	// Permissive: only refuse if we literally have no API key configured or
	// the plugin is in shutdown state. Previous strict "== STATUS_CONNECTED"
	// check returned NULL during STATUS_WAIT (before the first /ping
	// succeeds), which meant the Quote Window stayed empty for the first
	// several seconds after every restart, masking the data we DO have.
	if (g_oApiKey.IsEmpty() || g_nStatus == STATUS_SHUTDOWN)
		return NULL;

	if (!g_bRecentInfoCSInit)   // Plugin not fully initialized yet
		return NULL;

	CString ticker(pszTicker);

	// Ensure this symbol is on the WS subscription list. We set it in the map
	// EVEN IF the send fails -- the WS reader thread's reconnect path walks
	// g_SubscribedSymbols and re-issues subscribe for every entry, so missed
	// initial sends are self-healing on the next reconnect.
	EnterCriticalSection(&g_WebSocketCriticalSection);
	BOOL bAlready = FALSE;
	BOOL bFreshSubscribe = !g_SubscribedSymbols.Lookup(ticker, bAlready);
	if (bFreshSubscribe)
	{
		g_SubscribedSymbols.SetAt(ticker, TRUE);
		if (g_bWebSocketConnected)
			SubscribeToSymbol(pszTicker);
	}
	LeaveCriticalSection(&g_WebSocketCriticalSection);

	struct RecentInfo* pInfo = GetOrCreateRecentInfoEntry(ticker);

	// Throttled diagnostic so we can confirm in DbgView whether AmiBroker
	// is actually polling us and what state the entry is in. First 5 calls
	// per ticker + every 200th after that.
	if (pInfo)
	{
		EnterCriticalSection(&g_RecentInfoCS);
		static CMap<CString, LPCTSTR, int, int> s_callCounts;
		int n = 0;
		s_callCounts.Lookup(ticker, n);
		n++;
		s_callCounts.SetAt(ticker, n);
		int bm = pInfo->nBitmap;
		float last = pInfo->fLast;
		LeaveCriticalSection(&g_RecentInfoCS);

		if (n <= 5 || (n % 200) == 0)
		{
			CString log;
			log.Format(_T("OpenAlgo: GetRecentInfo(%s) call=%d  bitmap=0x%X  last=%.2f  fresh_sub=%d"),
				(LPCTSTR)ticker, n, bm, last, bFreshSubscribe);
			OutputDebugString(log);
		}
	}

	// Return the persistent per-symbol RecentInfo. It starts zeroed (nBitmap=0
	// means "no field is valid yet"), so the Quote Window row shows empty
	// cells until the first WS Mode 2 frame lands and the worker thread fills
	// it in. The pointer itself is stable across all calls.
	return pInfo;
}

