///////////////////////////////
// Real-Time Candle Building Functions
///////////////////////////////

// Parse websocket timestamps used by OpenAlgo:
// - epoch milliseconds / seconds
// - ISO-8601 UTC:        2025-05-28T10:30:45.123Z
// - ISO-8601 with offset 2025-05-28T16:00:45+05:30
// - local exchange time: 2025-05-28 16:00:45
BOOL TryParseWebSocketTimestamp(const CString& rawTimestamp, time_t& outTimestamp)
{
	outTimestamp = 0;

	CString trimmed = rawTimestamp;
	trimmed.Trim();
	if (trimmed.IsEmpty())
		return FALSE;

	BOOL isNumeric = TRUE;
	for (int i = 0; i < trimmed.GetLength(); i++)
	{
		if (!_istdigit(trimmed[i]))
		{
			isNumeric = FALSE;
			break;
		}
	}

	if (isNumeric)
	{
		__int64 value = _tstoi64(trimmed);
		if (value > 1000000000000LL)       // epoch milliseconds
			outTimestamp = (time_t)(value / 1000);
		else if (value > 1000000000LL)     // epoch seconds
			outTimestamp = (time_t)value;
		else
			return FALSE;

		return TRUE;
	}

	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	int count = _stscanf_s(trimmed, _T("%d-%d-%dT%d:%d:%d"),
		&year, &month, &day, &hour, &minute, &second);
	if (count != 6)
	{
		count = _stscanf_s(trimmed, _T("%d-%d-%d %d:%d:%d"),
			&year, &month, &day, &hour, &minute, &second);
	}
	if (count != 6)
		return FALSE;

	struct tm timeinfo = {0};
	timeinfo.tm_year = year - 1900;
	timeinfo.tm_mon = month - 1;
	timeinfo.tm_mday = day;
	timeinfo.tm_hour = hour;
	timeinfo.tm_min = minute;
	timeinfo.tm_sec = second;
	timeinfo.tm_isdst = -1;

	int tzPos = -1;
	for (int i = 10; i < trimmed.GetLength(); i++)
	{
		if (trimmed[i] == _T('Z') || trimmed[i] == _T('z') ||
		    trimmed[i] == _T('+') || trimmed[i] == _T('-'))
		{
			tzPos = i;
			break;
		}
	}

	if (tzPos >= 0)
	{
		if (trimmed[tzPos] == _T('Z') || trimmed[tzPos] == _T('z'))
		{
			outTimestamp = _mkgmtime(&timeinfo);
			return outTimestamp != (time_t)-1;
		}

		int offHour = 0;
		int offMinute = 0;
		CString offsetPart = trimmed.Mid(tzPos + 1);
		if (_stscanf_s(offsetPart, _T("%d:%d"), &offHour, &offMinute) < 1)
			return FALSE;

		int offsetSeconds = (offHour * 3600) + (offMinute * 60);
		if (trimmed[tzPos] == _T('-'))
			offsetSeconds = -offsetSeconds;

		time_t utcAssumed = _mkgmtime(&timeinfo);
		if (utcAssumed == (time_t)-1)
			return FALSE;

		outTimestamp = utcAssumed - offsetSeconds;
		return TRUE;
	}

	outTimestamp = mktime(&timeinfo);
	return outTimestamp != (time_t)-1;
}

// Get or create BarBuilder for a ticker
BarBuilder* GetOrCreateBarBuilder(const CString& ticker)
{
	BarBuilder* pBuilder = NULL;

	EnterCriticalSection(&g_BarBuilderCriticalSection);

	if (!g_BarBuilders.Lookup(ticker, pBuilder))
	{
		// Create new BarBuilder
		pBuilder = new BarBuilder();

		// Extract symbol and exchange from ticker (e.g., "RELIANCE-NSE")
		int dashPos = ticker.ReverseFind('-');
		if (dashPos > 0)
		{
			pBuilder->symbol = ticker.Left(dashPos);
			pBuilder->exchange = ticker.Mid(dashPos + 1);
		}
		else
		{
			pBuilder->symbol = ticker;
			pBuilder->exchange = _T("NSE");  // Default exchange
		}

		g_BarBuilders.SetAt(ticker, pBuilder);
	}

	LeaveCriticalSection(&g_BarBuilderCriticalSection);

	return pBuilder;
}

// Process a tick and update bars
BOOL ProcessTick(const CString& symbol, const CString& exchange, float ltp, float lastTradeQty, time_t timestamp)
{
	if (!g_bRealTimeCandlesEnabled)
		return FALSE;

	// Create ticker key
	CString ticker = symbol + _T("-") + exchange;

	// Get or create BarBuilder
	BarBuilder* pBuilder = GetOrCreateBarBuilder(ticker);
	if (!pBuilder)
		return FALSE;

	EnterCriticalSection(&g_BarBuilderCriticalSection);

	// Determine bar boundary (1-minute intervals)
	time_t barPeriodStart = (timestamp / 60) * 60;  // Floor to minute boundary

	// Check if we need a new bar
	BOOL bNewBar = FALSE;
	if (pBuilder->barStartTime != barPeriodStart)
	{
		bNewBar = TRUE;

		// Finalize current bar if it exists
		if (pBuilder->bBarStarted && pBuilder->barStartTime > 0)
		{
			// Add current bar to history
			pBuilder->bars.Add(pBuilder->currentBar);

			// Check if we need to remove old bars (rolling window)
			if (pBuilder->bars.GetCount() >= pBuilder->maxBars)
			{
				int removeCount = pBuilder->maxBars / 10;
				pBuilder->bars.RemoveAt(0, removeCount);
			}
		}

		// Start new bar
		memset(&pBuilder->currentBar, 0, sizeof(struct Quotation));
		pBuilder->currentBar.Open = ltp;
		pBuilder->currentBar.High = ltp;
		pBuilder->currentBar.Low = ltp;
		pBuilder->currentBar.Price = ltp;
		pBuilder->currentBar.Volume = 0;
		pBuilder->currentBar.OpenInterest = 0;

		// Set normalized timestamp (critical for AmiBroker)
		ConvertUnixToPackedDate(barPeriodStart, &pBuilder->currentBar.DateTime);
		pBuilder->currentBar.DateTime.PackDate.Second = 0;
		pBuilder->currentBar.DateTime.PackDate.MilliSec = 0;
		pBuilder->currentBar.DateTime.PackDate.MicroSec = 0;

		pBuilder->barStartTime = barPeriodStart;
		pBuilder->bBarStarted = TRUE;
		pBuilder->volumeAccumulator = 0.0f;
		pBuilder->bFirstTickReceived = TRUE;
		pBuilder->tickCount = 0;
	}

	// Update current bar OHLC
	if (ltp > pBuilder->currentBar.High)
		pBuilder->currentBar.High = ltp;
	if (ltp < pBuilder->currentBar.Low || pBuilder->currentBar.Low == 0.0f)
		pBuilder->currentBar.Low = ltp;
	pBuilder->currentBar.Price = ltp;

	// Accumulate volume
	pBuilder->volumeAccumulator += lastTradeQty;
	pBuilder->currentBar.Volume = pBuilder->volumeAccumulator;
	pBuilder->tickCount++;

	// Update last tick time
	pBuilder->lastTickTime = (DWORD)GetTickCount64();

	// FIX #5: Throttle PostMessage to <=10 Hz per symbol so a tick burst does not
	// re-enter GetQuotesEx hundreds of times per second. Always post on a fresh
	// bar boundary so the new minute is rendered immediately.
	DWORD nowTick = (DWORD)GetTickCount64();
	BOOL bShouldPost = bNewBar || ((nowTick - pBuilder->lastPostTick) >= 100);
	if (bShouldPost)
		pBuilder->lastPostTick = nowTick;

	LeaveCriticalSection(&g_BarBuilderCriticalSection);

	// Notify AmiBroker of update (non-blocking) only when the throttle permits
	if (bShouldPost && g_hAmiBrokerWnd != NULL)
	{
		PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
	}

	return TRUE;
}

// Cleanup all BarBuilders
void CleanupBarBuilders(void)
{
	if (g_bBarBuilderCriticalSectionInitialized)
	{
		EnterCriticalSection(&g_BarBuilderCriticalSection);

		// Delete all BarBuilder objects
		POSITION pos = g_BarBuilders.GetStartPosition();
		while (pos != NULL)
		{
			CString ticker;
			BarBuilder* pBuilder;
			g_BarBuilders.GetNextAssoc(pos, ticker, pBuilder);

			if (pBuilder)
			{
				delete pBuilder;
			}
		}

		g_BarBuilders.RemoveAll();

		LeaveCriticalSection(&g_BarBuilderCriticalSection);
	}
}

