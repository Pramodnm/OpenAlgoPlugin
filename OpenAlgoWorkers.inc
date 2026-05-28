//////////////////////////////////////////////////////////
// FIX #3 IMPLEMENTATIONS: per-symbol cache, work queue, worker thread
//////////////////////////////////////////////////////////

// Returns the SymbolBarCache for a ticker, creating it on first access.
SymbolBarCache* GetOrCreateSymbolBarCache(const CString& ticker)
{
	SymbolBarCache* pCache = NULL;
	EnterCriticalSection(&g_SymbolBarCacheCS);
	if (!g_SymbolBarCache.Lookup(ticker, pCache) || pCache == NULL)
	{
		pCache = new SymbolBarCache();
		g_SymbolBarCache.SetAt(ticker, pCache);
	}
	LeaveCriticalSection(&g_SymbolBarCacheCS);
	return pCache;
}

// Enqueue an HTTP fetch job; deduplicates against already-queued jobs for the
// same ticker+periodicity to keep the queue compact.
// nForceDays > 0 → use that backfill range; 0 → worker uses default range.
void QueueHttpFetch(const CString& ticker, int nPeriodicity, int nForceDays)
{
	if (!g_bHttpWorkQueueCSInitialized)
		return;

	HttpWorkItem item;
	item.ticker = ticker;
	item.nPeriodicity = nPeriodicity;
	item.nForceDays = nForceDays;

	EnterCriticalSection(&g_HttpWorkQueueCS);
	BOOL bAlreadyQueued = FALSE;
	POSITION pos = g_HttpWorkQueue.GetHeadPosition();
	while (pos != NULL)
	{
		HttpWorkItem& existing = g_HttpWorkQueue.GetNext(pos);
		if (existing.ticker == ticker && existing.nPeriodicity == nPeriodicity)
		{
			// If the new request specifies a force range, upgrade the queued
			// job so a later/larger manual backfill is honored.
			if (nForceDays > existing.nForceDays)
				existing.nForceDays = nForceDays;
			bAlreadyQueued = TRUE;
			break;
		}
	}
	if (!bAlreadyQueued)
		g_HttpWorkQueue.AddTail(item);
	LeaveCriticalSection(&g_HttpWorkQueueCS);

	CString qlog;
	qlog.Format(_T("OpenAlgo: queued history fetch ticker=%s periodicity=%d forceDays=%d alreadyQueued=%d"),
		(LPCTSTR)ticker, nPeriodicity, nForceDays, bAlreadyQueued);
	OutputDebugString(qlog);

	if (g_hHttpWorkEvent)
		SetEvent(g_hHttpWorkEvent);
}

// Worker thread: drains the queue, fetches HTTP for each item, and updates
// the per-symbol cache. Marks the symbol's RecentInfo as BARSREADY and sends
// WM_USER_STREAMING_UPDATE on success so AmiBroker re-reads GetQuotesEx for
// the active chart without needing a symbol switch.
UINT __cdecl HttpWorkerThreadProc(LPVOID /*pArg*/)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	OutputDebugString(_T("OpenAlgo: HttpWorkerThread started"));

	// Cap a single fetch at 200k bars. 1 year of 1-minute is ~98k; the cap
	// gives plenty of headroom while keeping the per-thread buffer to ~8MB.
	const int TMP_SIZE = 200000;

	while (InterlockedCompareExchange(&g_bHttpWorkerShouldStop, 0, 0) == 0)
	{
		// Wait for work, with a periodic poll for the stop flag
		WaitForSingleObject(g_hHttpWorkEvent, 1000);

		while (InterlockedCompareExchange(&g_bHttpWorkerShouldStop, 0, 0) == 0)
		{
			HttpWorkItem item;
			BOOL bHaveWork = FALSE;
			EnterCriticalSection(&g_HttpWorkQueueCS);
			if (!g_HttpWorkQueue.IsEmpty())
			{
				item = g_HttpWorkQueue.RemoveHead();
				bHaveWork = TRUE;
			}
			LeaveCriticalSection(&g_HttpWorkQueueCS);

			if (!bHaveWork)
				break;

			struct Quotation* tmpBars =
				(struct Quotation*)malloc(TMP_SIZE * sizeof(struct Quotation));
			if (tmpBars == NULL)
			{
				// OOM - clear in-progress flag so we can retry
				SymbolBarCache* pCacheOOM = GetOrCreateSymbolBarCache(item.ticker);
				EnterCriticalSection(&g_SymbolBarCacheCS);
				if (item.nPeriodicity == 60) pCacheOOM->bOneMinFetchInProgress = FALSE;
				else                         pCacheOOM->bDailyFetchInProgress  = FALSE;
				LeaveCriticalSection(&g_SymbolBarCacheCS);
				continue;
			}
			memset(tmpBars, 0, TMP_SIZE * sizeof(struct Quotation));

			SymbolBarCache* pCache = GetOrCreateSymbolBarCache(item.ticker);
			int nSeedLastValid = -1;
			if (item.nForceDays <= 0)
			{
				EnterCriticalSection(&g_SymbolBarCacheCS);
				CArray<struct Quotation, struct Quotation>& cachedBars =
					(item.nPeriodicity == 60) ? pCache->oneMinBars : pCache->dailyBars;
				int seedCount = min((int)cachedBars.GetCount(), TMP_SIZE);
				if (seedCount > 0)
				{
					memcpy(tmpBars, cachedBars.GetData(),
					       seedCount * sizeof(struct Quotation));
					nSeedLastValid = seedCount - 1;
				}
				LeaveCriticalSection(&g_SymbolBarCacheCS);
			}

			int nResult = GetOpenAlgoHistory(item.ticker, item.nPeriodicity,
			                                 nSeedLastValid, TMP_SIZE, tmpBars,
			                                 item.nForceDays);

			DWORD now = (DWORD)GetTickCount64();

			EnterCriticalSection(&g_SymbolBarCacheCS);
			if (nResult > 0)
			{
				if (item.nPeriodicity == 60)
				{
					pCache->oneMinBars.SetSize(nResult);
					memcpy(pCache->oneMinBars.GetData(), tmpBars,
					       nResult * sizeof(struct Quotation));
					pCache->lastOneMinFetch = now;
				}
				else
				{
					pCache->dailyBars.SetSize(nResult);
					memcpy(pCache->dailyBars.GetData(), tmpBars,
					       nResult * sizeof(struct Quotation));
					pCache->lastDailyFetch = now;
				}
			}
			// Always clear the in-progress flag so the next stale-check can
			// reschedule a fresh fetch even if this one failed.
			if (item.nPeriodicity == 60) pCache->bOneMinFetchInProgress = FALSE;
			else                         pCache->bDailyFetchInProgress  = FALSE;
			LeaveCriticalSection(&g_SymbolBarCacheCS);

			free(tmpBars);

			CString log;
			log.Format(_T("OpenAlgo: Worker fetched %d %s bars for %s"),
				nResult, (item.nPeriodicity == 60) ? _T("1m") : _T("daily"),
				(LPCTSTR)item.ticker);
			OutputDebugString(log);

			if (nResult > 0)
			{
				NotifyBarsReadyForSymbol(item.ticker);
				if (IsActiveChartSymbol(item.ticker))
					ForceAmiBrokerRefreshAll();
			}
		}
	}

	OutputDebugString(_T("OpenAlgo: HttpWorkerThread exiting"));
	return 0;
}

void StartHttpWorker(void)
{
	if (g_pHttpWorkerThread != NULL)
		return;

	g_hHttpWorkEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (g_hHttpWorkEvent == NULL)
	{
		OutputDebugString(_T("OpenAlgo: StartHttpWorker - CreateEvent failed"));
		return;
	}

	InterlockedExchange(&g_bHttpWorkerShouldStop, 0);

	g_pHttpWorkerThread = AfxBeginThread(HttpWorkerThreadProc, NULL,
	                                     THREAD_PRIORITY_NORMAL, 0,
	                                     CREATE_SUSPENDED);
	if (g_pHttpWorkerThread)
	{
		g_pHttpWorkerThread->m_bAutoDelete = FALSE;
		g_pHttpWorkerThread->ResumeThread();
		OutputDebugString(_T("OpenAlgo: HttpWorker thread launched"));
	}
	else
	{
		OutputDebugString(_T("OpenAlgo: StartHttpWorker - AfxBeginThread failed"));
		CloseHandle(g_hHttpWorkEvent);
		g_hHttpWorkEvent = NULL;
	}
}

void StopHttpWorker(void)
{
	if (g_pHttpWorkerThread == NULL)
		return;

	InterlockedExchange(&g_bHttpWorkerShouldStop, 1);
	if (g_hHttpWorkEvent)
		SetEvent(g_hHttpWorkEvent);

	WaitForSingleObject(g_pHttpWorkerThread->m_hThread, 15000);
	delete g_pHttpWorkerThread;
	g_pHttpWorkerThread = NULL;

	if (g_hHttpWorkEvent)
	{
		CloseHandle(g_hHttpWorkEvent);
		g_hHttpWorkEvent = NULL;
	}
}

void CleanupSymbolBarCache(void)
{
	if (!g_bSymbolBarCacheCSInitialized)
		return;
	EnterCriticalSection(&g_SymbolBarCacheCS);
	POSITION pos = g_SymbolBarCache.GetStartPosition();
	while (pos != NULL)
	{
		CString key;
		SymbolBarCache* pCache;
		g_SymbolBarCache.GetNextAssoc(pos, key, pCache);
		if (pCache)
			delete pCache;
	}
	g_SymbolBarCache.RemoveAll();
	LeaveCriticalSection(&g_SymbolBarCacheCS);
}

//////////////////////////////////////////////////////////
// Dedicated WS reader thread
//
// Replaces the old TIMER_WEBSOCKET (100 ms UI-thread polling) which suffered
// from WM_TIMER coalescing whenever AmiBroker's UI was busy. The thread
// blocks in select() with a 1 s timeout, so a tick is drained the instant
// it arrives and the chart sees it within one PostMessage hop.
//
// Threading contract:
//   - This thread is the SOLE caller of recv() on g_websocket.
//   - The UI thread may call send() (SubscribeToSymbol/UnsubscribeFromSymbol);
//     Winsock allows concurrent send+recv on the same socket from different
//     threads. Socket lifecycle (close/recreate) is owned by this thread.
//   - g_bWebSocketConnected is set FALSE before closesocket() so a stale UI
//     send() at most gets an error return, never a use-after-free.
//////////////////////////////////////////////////////////

UINT __cdecl WsReaderThreadProc(LPVOID /*pArg*/)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	OutputDebugString(_T("OpenAlgo: WsReaderThread started"));

	while (InterlockedCompareExchange(&g_bWsReaderShouldStop, 0, 0) == 0)
	{
		// Real-time disabled in settings: idle until re-enabled
		if (!g_bRealTimeCandlesEnabled)
		{
			Sleep(500);
			continue;
		}

		if (g_bWebSocketConnected && g_websocket != INVALID_SOCKET)
		{
			// Block up to 1 s waiting for inbound data. Worst-case latency
			// from tick on the wire to ProcessTick is one select() wake-up
			// plus the drain loop inside ProcessWebSocketData -- a few ms.
			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(g_websocket, &readfds);
			struct timeval tv;
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			select(0, &readfds, NULL, NULL, &tv);

			// Whether select returned >0 (data), 0 (timeout) or <0 (error),
			// let ProcessWebSocketData figure it out. It already handles
			// "no data", "graceful close", and "ping due" internally.
			ProcessWebSocketData();
		}
		else
		{
			// Not connected: ProcessWebSocketData has a rate-limited
			// (5 s) reconnect attempt. Call it then back off so we don't
			// CPU-spin while the server is unreachable.
			ProcessWebSocketData();
			Sleep(500);
		}
	}

	OutputDebugString(_T("OpenAlgo: WsReaderThread exiting"));
	return 0;
}

void StartWsReader(void)
{
	if (g_pWsReaderThread != NULL)
		return;

	InterlockedExchange(&g_bWsReaderShouldStop, 0);

	g_pWsReaderThread = AfxBeginThread(WsReaderThreadProc, NULL,
	                                   THREAD_PRIORITY_NORMAL, 0,
	                                   CREATE_SUSPENDED);
	if (g_pWsReaderThread)
	{
		g_pWsReaderThread->m_bAutoDelete = FALSE;
		g_pWsReaderThread->ResumeThread();
		OutputDebugString(_T("OpenAlgo: WsReader thread launched"));
	}
	else
	{
		OutputDebugString(_T("OpenAlgo: StartWsReader - AfxBeginThread failed"));
	}
}

void StopWsReader(void)
{
	if (g_pWsReaderThread == NULL)
		return;

	InterlockedExchange(&g_bWsReaderShouldStop, 1);

	// Wait up to 2 s for the loop to exit on its next select() timeout.
	WaitForSingleObject(g_pWsReaderThread->m_hThread, 2000);
	delete g_pWsReaderThread;
	g_pWsReaderThread = NULL;
}
