// Plugin.cpp - Complete implementation with Quotes and Historical data
#include "stdafx.h"
#include "resource.h"  // Include resource definitions
#include "OpenAlgoGlobals.h"
#include "Plugin.h"
#include "Plugin_Legacy.h"
#include "OpenAlgoPlugin.h"   // for theApp (COpenAlgoApp) and EnsureRegistryRoot()
#include "OpenAlgoConfigDlg.h"
#include <math.h>
#include <time.h>
#include <stdlib.h>  // For qsort

// Plugin identification
#define PLUGIN_NAME "OpenAlgo Data Plugin"
#define VENDOR_NAME "OpenAlgo Community"
#define PLUGIN_VERSION 10003
#define PLUGIN_ID PIDCODE('T', 'E', 'S', 'T')  // Unique 4-char code
#define THIS_PLUGIN_TYPE PLUGIN_TYPE_DATA
#define AGENT_NAME PLUGIN_NAME

// Timer IDs
#define TIMER_INIT 198
#define TIMER_REFRESH 199
#define TIMER_WEBSOCKET 200  // High-frequency timer for WebSocket data processing
#define RETRY_COUNT 8

////////////////////////////////////////
// Plugin Info Structure
////////////////////////////////////////
static struct PluginInfo oPluginInfo =
{
	sizeof(struct PluginInfo),
	THIS_PLUGIN_TYPE,
	PLUGIN_VERSION,
	PLUGIN_ID,
	PLUGIN_NAME,
	VENDOR_NAME,
	0,
	530000
};

///////////////////////////////
// Global Variables
///////////////////////////////
HWND g_hAmiBrokerWnd = NULL;
int g_nPortNumber = 5000;
int g_nRefreshInterval = 5;
int g_nTimeShift = 0;
CString g_oServer = _T("127.0.0.1");
CString g_oApiKey = _T("");  // API Key for authentication
CString g_oWebSocketUrl = _T("ws://127.0.0.1:8765");  // WebSocket URL
int g_nStatus = STATUS_WAIT;

// Backfill request tracking
int g_nBackfillDays = 0;        // Number of days to backfill (0 = use default logic)
int g_nBackfillPeriodicity = 0; // 60 for 1-minute, 86400 for daily
BOOL g_bBackfillRequested = FALSE;

// Local static variables
static int g_nRetryCount = RETRY_COUNT;
static struct RecentInfo* g_aInfos = NULL;
static int RecentInfoSize = 0;
static BOOL g_bPluginInitialized = FALSE;

// WebSocket connection management
static SOCKET g_websocket = INVALID_SOCKET;
static BOOL g_bWebSocketConnected = FALSE;
static BOOL g_bWebSocketAuthenticated = FALSE;
static BOOL g_bWebSocketConnecting = FALSE;
static DWORD g_dwLastConnectionAttempt = 0;
static CMap<CString, LPCTSTR, BOOL, BOOL> g_SubscribedSymbols;
static CRITICAL_SECTION g_WebSocketCriticalSection;
static BOOL g_bCriticalSectionInitialized = FALSE;

// Cache for recent quotes
struct QuoteCache {
	CString symbol;
	CString exchange;
	float ltp;
	float open;
	float high;
	float low;
	float close;
	float volume;
	float oi;
	DWORD lastUpdate;
	
	// Constructor to initialize all values
	QuoteCache() : ltp(0.0f), open(0.0f), high(0.0f), low(0.0f), 
	               close(0.0f), volume(0.0f), oi(0.0f), lastUpdate(0) {
	}
};

static CMap<CString, LPCTSTR, QuoteCache, QuoteCache&> g_QuoteCache;

typedef CArray< struct Quotation, struct Quotation > CQuoteArray;

//////////////////////////////////////////////////////////
// REAL-TIME CANDLE BUILDING STRUCTURES
//////////////////////////////////////////////////////////

// Real-time configuration (non-static so they can be accessed from OpenAlgoConfigDlg)
BOOL g_bRealTimeCandlesEnabled = TRUE;  // Default: enabled
int g_nBackfillIntervalMs = 5000;       // HTTP backfill every 5 seconds

// HTTP response caching (performance optimization)
// Cache HTTP responses to avoid calling HTTP API on every GetQuotesEx() call
CMapStringToPtr g_HttpResponseCache;  // Maps "SYMBOL-PERIODICITY" → last HTTP call time (DWORD*)
CRITICAL_SECTION g_HttpCacheCriticalSection;
const DWORD HTTP_CACHE_LIFETIME_MS = 60000;  // Cache HTTP responses for 60 seconds
static BOOL g_bHttpCacheCriticalSectionInitialized = FALSE;

// BarBuilder: Per-symbol tick-to-bar aggregation state
struct BarBuilder {
	CString symbol;
	CString exchange;
	int periodicity;  // 60 for 1-minute (only 1-minute supported initially)

	// Current bar being built from ticks
	struct Quotation currentBar;
	BOOL bBarStarted;
	time_t barStartTime;

	// Tick accumulation
	float volumeAccumulator;  // Sum of last_trade_quantity
	int tickCount;

	// Historical bars storage
	CArray<struct Quotation, struct Quotation> bars;  // Up to 10,000 bars
	int maxBars;

	// Timestamps for backfill management
	DWORD lastTickTime;      // Last tick received
	DWORD lastBackfillTime;  // Last HTTP backfill
	DWORD lastPostTick;      // Last WM_USER_STREAMING_UPDATE post tick (for throttling)

	// State flags
	BOOL bBackfillMerged;
	BOOL bFirstTickReceived;

	// Constructor
	BarBuilder() : periodicity(60), bBarStarted(FALSE), barStartTime(0),
	               volumeAccumulator(0.0f), tickCount(0), maxBars(500),
	               lastTickTime(0), lastBackfillTime(0), lastPostTick(0),
	               bBackfillMerged(FALSE), bFirstTickReceived(FALSE) {
		memset(&currentBar, 0, sizeof(struct Quotation));
	}
};

// Global cache of bar builders (one per symbol)
static CMap<CString, LPCTSTR, BarBuilder*, BarBuilder*> g_BarBuilders;
static CRITICAL_SECTION g_BarBuilderCriticalSection;
static BOOL g_bBarBuilderCriticalSectionInitialized = FALSE;

//////////////////////////////////////////////////////////
// FIX #3: HTTP WORKER THREAD INFRASTRUCTURE
// All HTTP backfill calls run on a background worker thread so the AmiBroker
// UI thread never blocks. GetQuotesEx serves O(1) from a per-symbol cache that
// the worker refreshes asynchronously and announces via WM_USER_STREAMING_UPDATE.
//////////////////////////////////////////////////////////

// Per-symbol HTTP snapshot: filled by worker, consumed by GetQuotesEx
struct SymbolBarCache
{
	CArray<struct Quotation, struct Quotation> oneMinBars;
	CArray<struct Quotation, struct Quotation> dailyBars;
	DWORD lastOneMinFetch;
	DWORD lastDailyFetch;
	BOOL  bOneMinFetchInProgress;
	BOOL  bDailyFetchInProgress;

	SymbolBarCache() : lastOneMinFetch(0), lastDailyFetch(0),
	                   bOneMinFetchInProgress(FALSE),
	                   bDailyFetchInProgress(FALSE) {}
};

static CMap<CString, LPCTSTR, SymbolBarCache*, SymbolBarCache*> g_SymbolBarCache;
static CRITICAL_SECTION g_SymbolBarCacheCS;
static BOOL g_bSymbolBarCacheCSInitialized = FALSE;

// One queued HTTP fetch request. nForceDays > 0 overrides the default range
// (used by the right-click "Backfill" menu so it works per-symbol asynchronously).
struct HttpWorkItem
{
	CString ticker;
	int     nPeriodicity;
	int     nForceDays;
};

static CList<HttpWorkItem, HttpWorkItem&> g_HttpWorkQueue;
static CRITICAL_SECTION g_HttpWorkQueueCS;
static BOOL g_bHttpWorkQueueCSInitialized = FALSE;
static HANDLE g_hHttpWorkEvent = NULL;          // auto-reset, signaled on enqueue
static CWinThread* g_pHttpWorkerThread = NULL;
static volatile LONG g_bHttpWorkerShouldStop = 0;

// Dedicated WS reader thread: blocks in select() so a tick is drained the
// instant it arrives, instead of relying on a UI WM_TIMER that gets coalesced
// for seconds when AmiBroker is busy painting.
static CWinThread* g_pWsReaderThread = NULL;
static volatile LONG g_bWsReaderShouldStop = 0;

// Cache freshness windows. Stale entries trigger a background refresh but the
// stale data is still served immediately so the chart never goes blank.
static const DWORD ONEMIN_CACHE_LIFETIME_MS = 60000;     // refresh 1m every 60s
static const DWORD DAILY_CACHE_LIFETIME_MS  = 3600000;   // refresh daily every 1h

// Forward declarations
VOID CALLBACK OnTimerProc(HWND, UINT, UINT_PTR, DWORD);
SymbolBarCache* GetOrCreateSymbolBarCache(const CString& ticker);
void QueueHttpFetch(const CString& ticker, int nPeriodicity, int nForceDays);
UINT __cdecl HttpWorkerThreadProc(LPVOID pArg);
void StartHttpWorker(void);
void StopHttpWorker(void);
void CleanupSymbolBarCache(void);
UINT __cdecl WsReaderThreadProc(LPVOID pArg);
void StartWsReader(void);
void StopWsReader(void);
void SetupRetry(void);
BOOL TestOpenAlgoConnection(void);
BOOL GetOpenAlgoQuote(LPCTSTR pszTicker, QuoteCache& quote);
int GetOpenAlgoHistory(LPCTSTR pszTicker, int nPeriodicity, int nLastValid, int nSize, struct Quotation* pQuotes);
CString GetExchangeFromTicker(LPCTSTR pszTicker);
CString GetIntervalString(int nPeriodicity);
void ConvertUnixToPackedDate(time_t unixTime, union AmiDate* pAmiDate);

// WebSocket functions
BOOL InitializeWebSocket(void);
void CleanupWebSocket(void);
BOOL ConnectWebSocket(void);
BOOL AuthenticateWebSocket(void);
BOOL SendWebSocketFrame(const CString& message);
CString DecodeWebSocketFrame(const char* buffer, int length);
BOOL SubscribeToSymbol(LPCTSTR pszTicker);
BOOL UnsubscribeFromSymbol(LPCTSTR pszTicker);
BOOL ProcessWebSocketData(void);
void GenerateWebSocketMaskKey(unsigned char* maskKey);
void SubscribePendingSymbols(void);

// Real-time candle building functions
BOOL ProcessTick(const CString& symbol, const CString& exchange, float ltp, float lastTradeQty, time_t timestamp);
time_t ParseISO8601Timestamp(const CString& isoTimestamp);
BarBuilder* GetOrCreateBarBuilder(const CString& ticker);
void CleanupBarBuilders(void);

// Helper function for mixed EOD/Intraday data
int FindLastBarOfMatchingType(int nPeriodicity, int nLastValid, struct Quotation* pQuotes);

// Helper function to compare two quotations for sorting by timestamp
int CompareQuotations(const void* a, const void* b);

///////////////////////////////
// Helper Functions
///////////////////////////////

// Compare two quotations for sorting by timestamp (oldest to newest)
// Used by qsort() to ensure quotes array is in chronological order
// This is CRITICAL for AmiBroker to display charts correctly
int CompareQuotations(const void* a, const void* b)
{
	const struct Quotation* qa = (const struct Quotation*)a;
	const struct Quotation* qb = (const struct Quotation*)b;

	// Compare 64-bit DateTime.Date field directly
	// This handles both EOD and Intraday data correctly
	if (qa->DateTime.Date < qb->DateTime.Date)
		return -1;
	else if (qa->DateTime.Date > qb->DateTime.Date)
		return 1;
	else
		return 0;
}

// Find last bar in array that matches the requested periodicity type
// This is CRITICAL for Mixed EOD/Intraday support (AllowMixedEODIntra = TRUE)
//
// When mixed data is enabled, pQuotes array contains BOTH:
// - Daily bars (Hour=31, Minute=63)
// - Intraday bars (Hour=0-23)
//
// We must find the last bar of the CORRECT type to calculate gaps properly
int FindLastBarOfMatchingType(int nPeriodicity, int nLastValid, struct Quotation* pQuotes)
{
	if (nLastValid < 0 || pQuotes == NULL)
		return -1;  // No data at all

	if (nPeriodicity == 86400)  // Looking for Daily data
	{
		// Scan backwards to find last Daily bar (Hour=31, Minute=63)
		for (int i = nLastValid; i >= 0; i--)
		{
			if (pQuotes[i].DateTime.PackDate.Hour == DATE_EOD_HOURS &&
				pQuotes[i].DateTime.PackDate.Minute == DATE_EOD_MINUTES)
			{
				return i;  // Found last Daily bar
			}
		}
		return -1;  // No Daily bars found in array
	}
	else if (nPeriodicity == 60)  // Looking for 1-minute (or other intraday) data
	{
		// Scan backwards to find last Intraday bar (Hour < 31)
		for (int i = nLastValid; i >= 0; i--)
		{
			if (pQuotes[i].DateTime.PackDate.Hour < DATE_EOD_HOURS)
			{
				return i;  // Found last Intraday bar
			}
		}
		return -1;  // No Intraday bars found in array
	}
	else
	{
		// Unknown periodicity - use last bar overall
		return nLastValid;
	}
}

CString BuildOpenAlgoURL(const CString& server, int port, const CString& endpoint)
{
	CString result;
	result.Format(_T("http://%s:%d%s"), (LPCTSTR)server, port, (LPCTSTR)endpoint);
	return result;
}

// ---------------------------------------------------------------------------
// API-key persistence
//
// History on this machine:
//   1. CWinApp::WriteProfileString silently dropped just the ApiKey value
//      across sessions, even though Server/Port/etc persisted fine.
//   2. Switching to direct RegSetValueEx (still in the same MFC-style
//      HKCU\Software\OpenAlgo\... subtree) still ended up with the whole
//      subkey wiped after each AmiBroker restart. Something on this machine
//      is cleaning that registry tree (AV, group policy, optimization
//      utility, etc.). PowerShell round-trips through the same Win32 API
//      against the same path do work, so the Win32 call is correct; the
//      registry location itself is the problem.
//
// Solution: store the API key in a plain text file under
//   %LOCALAPPDATA%\OpenAlgoPlugin\settings.dat
// Files don't get scrubbed by registry cleaners. We still write the
// registry value too as a secondary store. On read we try the file first
// and fall back to the registry; the writeback re-syncs whichever was
// missing so the two locations stay consistent over time.
// ---------------------------------------------------------------------------

#include <shlobj.h>   // SHGetFolderPath
#pragma comment(lib, "Shell32.lib")

static const TCHAR* kOpenAlgoRegPath = _T("Software\\OpenAlgo\\OpenAlgo\\OpenAlgo");

// Returns "<LOCALAPPDATA>\OpenAlgoPlugin\settings.dat", creating the folder
// if needed. Returns empty string on failure (very rare).
static CString GetApiKeyFilePath()
{
	CString path;
	TCHAR buf[MAX_PATH] = {0};
	HRESULT hr = SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
	                             NULL, SHGFP_TYPE_CURRENT, buf);
	if (FAILED(hr) || buf[0] == 0)
		return path;

	path.Format(_T("%s\\OpenAlgoPlugin"), buf);
	::CreateDirectory(path, NULL);   // OK if it already exists
	path += _T("\\settings.dat");
	return path;
}

static BOOL WriteApiKeyToFile(const CString& key)
{
	CString path = GetApiKeyFilePath();
	if (path.IsEmpty()) return FALSE;

	// Write atomically: settings.dat.tmp first, then MoveFileEx replace.
	// Avoids leaving an empty file on crash mid-write.
	CString tmp = path + _T(".tmp");

	HANDLE h = ::CreateFile(tmp, GENERIC_WRITE, 0, NULL,
	                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		CString log;
		log.Format(_T("OpenAlgo: WriteApiKeyToFile CreateFile failed err=%lu path=%s"),
			GetLastError(), (LPCTSTR)tmp);
		OutputDebugString(log);
		return FALSE;
	}

	// Single line: key=<value>\n  (so we can grow this later if we want to
	// add more settings without breaking older parsers).
	CStringA line;
	line.Format("key=%s\n", (LPCSTR)CStringA(key));
	DWORD written = 0;
	BOOL wrote = ::WriteFile(h, (LPCSTR)line, (DWORD)line.GetLength(), &written, NULL);
	::FlushFileBuffers(h);
	::CloseHandle(h);

	if (!wrote || written != (DWORD)line.GetLength())
	{
		CString log;
		log.Format(_T("OpenAlgo: WriteApiKeyToFile WriteFile failed err=%lu wrote=%lu"),
			GetLastError(), written);
		OutputDebugString(log);
		::DeleteFile(tmp);
		return FALSE;
	}

	if (!::MoveFileEx(tmp, path, MOVEFILE_REPLACE_EXISTING))
	{
		CString log;
		log.Format(_T("OpenAlgo: WriteApiKeyToFile MoveFileEx failed err=%lu"),
			GetLastError());
		OutputDebugString(log);
		::DeleteFile(tmp);
		return FALSE;
	}

	return TRUE;
}

static BOOL ReadApiKeyFromFile(CString& outKey)
{
	outKey.Empty();
	CString path = GetApiKeyFilePath();
	if (path.IsEmpty()) return FALSE;

	HANDLE h = ::CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return FALSE;

	char raw[1024] = {0};
	DWORD read = 0;
	::ReadFile(h, raw, sizeof(raw) - 1, &read, NULL);
	::CloseHandle(h);
	if (read == 0) return FALSE;

	CStringA content(raw, (int)read);
	int eqPos = content.Find("key=");
	if (eqPos < 0) return FALSE;
	int start = eqPos + 4;
	int end = start;
	while (end < content.GetLength() && content[end] != '\r' && content[end] != '\n')
		end++;
	if (end <= start) return FALSE;

	outKey = CString(CStringA(content.Mid(start, end - start)));
	return !outKey.IsEmpty();
}

// Write to both the file (canonical) and the registry (legacy/secondary).
// Either success counts as a successful write -- we only fail if both fail.
BOOL WriteApiKeyDirect(const CString& key)
{
	BOOL fileOk = WriteApiKeyToFile(key);

	// Best-effort registry write -- not fatal if it fails.
	BOOL regOk = FALSE;
	HKEY hKey = NULL;
	DWORD disposition = 0;
	if (RegCreateKeyEx(HKEY_CURRENT_USER, kOpenAlgoRegPath, 0, NULL,
	                   REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
	                   &hKey, &disposition) == ERROR_SUCCESS && hKey)
	{
		DWORD dataBytes = (DWORD)((key.GetLength() + 1) * sizeof(TCHAR));
		regOk = (RegSetValueEx(hKey, _T("ApiKey"), 0, REG_SZ,
		                       (const BYTE*)(LPCTSTR)key, dataBytes) == ERROR_SUCCESS);
		RegCloseKey(hKey);
	}

	CString log;
	int n = key.GetLength();
	CString mask;
	if (n > 8) mask.Format(_T("%s...%s"), (LPCTSTR)key.Left(4), (LPCTSTR)key.Right(4));
	else       mask = _T("(short)");
	log.Format(_T("OpenAlgo: WriteApiKeyDirect ApiKey=%s len=%d file=%d reg=%d"),
		(LPCTSTR)mask, n, fileOk, regOk);
	OutputDebugString(log);

	return (fileOk || regOk);
}

// Try file first; fall back to registry. If only one source has the value,
// write the other so they re-sync.
BOOL ReadApiKeyDirect(CString& outKey)
{
	outKey.Empty();

	if (ReadApiKeyFromFile(outKey) && !outKey.IsEmpty())
		return TRUE;

	HKEY hKey = NULL;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, kOpenAlgoRegPath, 0,
	                 KEY_READ, &hKey) != ERROR_SUCCESS || hKey == NULL)
		return FALSE;

	DWORD dataType = 0;
	DWORD dataBytes = 0;
	LONG sizeResult = RegQueryValueEx(hKey, _T("ApiKey"), NULL, &dataType, NULL, &dataBytes);
	if (sizeResult != ERROR_SUCCESS || dataType != REG_SZ || dataBytes == 0)
	{
		RegCloseKey(hKey);
		return FALSE;
	}

	int chars = (int)(dataBytes / sizeof(TCHAR));
	if (chars < 1) chars = 1;
	TCHAR* buf = outKey.GetBuffer(chars);
	DWORD copyBytes = dataBytes;
	LONG readResult = RegQueryValueEx(hKey, _T("ApiKey"), NULL, &dataType,
		(LPBYTE)buf, &copyBytes);
	outKey.ReleaseBuffer();
	RegCloseKey(hKey);

	if (readResult != ERROR_SUCCESS)
	{
		outKey.Empty();
		return FALSE;
	}

	// Recovered from registry -- write back to the file so next time the
	// file path serves it (in case the registry tree gets wiped again).
	if (!outKey.IsEmpty())
		WriteApiKeyToFile(outKey);

	return TRUE;
}

// Extract exchange from ticker format (e.g., "RELIANCE-NSE" -> "NSE")
CString GetExchangeFromTicker(LPCTSTR pszTicker)
{
	CString ticker(pszTicker);
	int dashPos = ticker.ReverseFind(_T('-'));
	if (dashPos != -1)
	{
		return ticker.Mid(dashPos + 1);
	}
	
	// Default to NSE if no exchange suffix found
	return _T("NSE");
}

// Get clean symbol without exchange suffix
CString GetCleanSymbol(LPCTSTR pszTicker)
{
	CString ticker(pszTicker);
	int dashPos = ticker.ReverseFind(_T('-'));
	if (dashPos != -1)
	{
		return ticker.Left(dashPos);
	}
	return ticker;
}

// Convert periodicity to OpenAlgo interval string
// Currently supporting only 1m and D (daily) intervals
CString GetIntervalString(int nPeriodicity)
{
	// Only support 1-minute and Daily for now
	if (nPeriodicity == 60)  // 1 minute in seconds
		return _T("1m");
	else if (nPeriodicity == 86400) // Daily in seconds (24*60*60)
		return _T("D");  // Daily
	else
		return _T("D");  // Default to daily for all other timeframes
}

// Convert Unix timestamp to AmiBroker date format
// Works for all market types including 24x7 markets
void ConvertUnixToPackedDate(time_t unixTime, union AmiDate* pAmiDate)
{
	struct tm* timeinfo = localtime(&unixTime);

	pAmiDate->PackDate.Year = timeinfo->tm_year + 1900;
	pAmiDate->PackDate.Month = timeinfo->tm_mon + 1;
	pAmiDate->PackDate.Day = timeinfo->tm_mday;
	pAmiDate->PackDate.Hour = timeinfo->tm_hour;
	pAmiDate->PackDate.Minute = timeinfo->tm_min;
	pAmiDate->PackDate.Second = timeinfo->tm_sec;
	pAmiDate->PackDate.MilliSec = 0;
	pAmiDate->PackDate.MicroSec = 0;
	pAmiDate->PackDate.Reserved = 0;
	pAmiDate->PackDate.IsFuturePad = 0;
}

// Fetch real-time quote from OpenAlgo
// WARNING: This is ONLY for Level 1 quotes in Real-time Quote Window
// NEVER use this data for creating OHLC bars or historical charts
BOOL GetOpenAlgoQuote(LPCTSTR pszTicker, QuoteCache& quote)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	if (g_oApiKey.IsEmpty())
		return FALSE;

	BOOL bSuccess = FALSE;

	try
	{
		CString oURL = BuildOpenAlgoURL(g_oServer, g_nPortNumber, _T("/api/v1/quotes"));

		// Prepare POST data
		CString symbol = GetCleanSymbol(pszTicker);
		CString exchange = GetExchangeFromTicker(pszTicker);

		CString oPostData;
		oPostData.Format(_T("{\"apikey\":\"%s\",\"symbol\":\"%s\",\"exchange\":\"%s\"}"),
			(LPCTSTR)g_oApiKey, (LPCTSTR)symbol, (LPCTSTR)exchange);

		CInternetSession oSession(AGENT_NAME, 1, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL,
			INTERNET_FLAG_DONT_CACHE);
		oSession.SetOption(INTERNET_OPTION_CONNECT_TIMEOUT, 3000);
		oSession.SetOption(INTERNET_OPTION_RECEIVE_TIMEOUT, 3000);

		CHttpConnection* pConnection = NULL;
		CHttpFile* pFile = NULL;

		// Parse server
		INTERNET_PORT nPort = (INTERNET_PORT)g_nPortNumber;
		CString oServer = g_oServer;
		oServer.Replace(_T("http://"), _T(""));
		oServer.Replace(_T("https://"), _T(""));

		pConnection = oSession.GetHttpConnection(oServer, nPort);

		if (pConnection)
		{
			pFile = pConnection->OpenRequest(
				CHttpConnection::HTTP_VERB_POST,
				_T("/api/v1/quotes"),
				NULL, 1, NULL, NULL,
				INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE);

			if (pFile)
			{
				CString oHeaders = _T("Content-Type: application/json\r\n");
				CStringA oPostDataA(oPostData);

				if (pFile->SendRequest(oHeaders, (LPVOID)(LPCSTR)oPostDataA, oPostDataA.GetLength()))
				{
					DWORD dwStatusCode = 0;
					pFile->QueryInfoStatusCode(dwStatusCode);

					if (dwStatusCode == 200)
					{
						// Fix #6: Preallocate to keep CString growth from re-allocating
						// many times when a long JSON body is streamed line-by-line.
						CString oResponse;
						oResponse.Preallocate(4096);
						CString oLine;
						while (pFile->ReadString(oLine))
						{
							oResponse += oLine;
						}

						// Parse JSON response (simple parsing)
						if (oResponse.Find(_T("\"status\":\"success\"")) >= 0)
						{
							// Extract values using simple string parsing
							int pos;

							// Parse LTP
							pos = oResponse.Find(_T("\"ltp\":"));
							if (pos >= 0)
							{
								pos += 6;
								int endPos = oResponse.Find(_T(","), pos);
								if (endPos < 0) endPos = oResponse.Find(_T("}"), pos);
								CString val = oResponse.Mid(pos, endPos - pos);
								quote.ltp = (float)_tstof(val);
							}

							// Parse Open
							pos = oResponse.Find(_T("\"open\":"));
							if (pos >= 0)
							{
								pos += 7;
								int endPos = oResponse.Find(_T(","), pos);
								if (endPos < 0) endPos = oResponse.Find(_T("}"), pos);
								CString val = oResponse.Mid(pos, endPos - pos);
								quote.open = (float)_tstof(val);
							}

							// Parse High
							pos = oResponse.Find(_T("\"high\":"));
							if (pos >= 0)
							{
								pos += 7;
								int endPos = oResponse.Find(_T(","), pos);
								if (endPos < 0) endPos = oResponse.Find(_T("}"), pos);
								CString val = oResponse.Mid(pos, endPos - pos);
								quote.high = (float)_tstof(val);
							}

							// Parse Low
							pos = oResponse.Find(_T("\"low\":"));
							if (pos >= 0)
							{
								pos += 6;
								int endPos = oResponse.Find(_T(","), pos);
								if (endPos < 0) endPos = oResponse.Find(_T("}"), pos);
								CString val = oResponse.Mid(pos, endPos - pos);
								quote.low = (float)_tstof(val);
							}

							// Parse Volume
							pos = oResponse.Find(_T("\"volume\":"));
							if (pos >= 0)
							{
								pos += 9;
								int endPos = oResponse.Find(_T(","), pos);
								if (endPos < 0) endPos = oResponse.Find(_T("}"), pos);
								CString val = oResponse.Mid(pos, endPos - pos);
								quote.volume = (float)_tstof(val);
							}

							// Parse OI
							pos = oResponse.Find(_T("\"oi\":"));
							if (pos >= 0)
							{
								pos += 5;
								int endPos = oResponse.Find(_T(","), pos);
								if (endPos < 0) endPos = oResponse.Find(_T("}"), pos);
								CString val = oResponse.Mid(pos, endPos - pos);
								quote.oi = (float)_tstof(val);
							}

							// Parse Previous Close
							pos = oResponse.Find(_T("\"prev_close\":"));
							if (pos >= 0)
							{
								pos += 13;
								int endPos = oResponse.Find(_T(","), pos);
								if (endPos < 0) endPos = oResponse.Find(_T("}"), pos);
								CString val = oResponse.Mid(pos, endPos - pos);
								quote.close = (float)_tstof(val);
							}

							quote.symbol = symbol;
							quote.exchange = exchange;
							quote.lastUpdate = (DWORD)GetTickCount64();

							bSuccess = TRUE;
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
	}

	return bSuccess;
}

// Fetch historical data from OpenAlgo with intelligent backfill strategy
// COMPLETELY EXCHANGE-AGNOSTIC - Works with ANY exchange and ANY trading hours
//
// OPTIMAL GAP-FREE BACKFILL STRATEGY (DATE-BASED):
// - First load: 30 days (1m) or 10 years (daily) of historical data
// - Subsequent refreshes: Smart gap detection from last bar's date
// - Automatically fills data gaps (user absences, network outages, etc.)
// - Safety limits: Max 30 days for 1m, max 730 days (2 years) for daily
// - Zero data holes within safety limits
//
// HOW IT WORKS:
// 1. Extract last bar's DATE (not time) from existing data
// 2. Calculate gap in days between last bar and today
// 3. Apply safety limits to prevent server overload
// 4. Request date range from OpenAlgo API (DATE-only, no time)
// 5. API returns ALL bars for each date in range
// 6. Duplicate detection handles overlapping bars
//
// EXCHANGE SUPPORT:
// - NSE: Regular + evening sessions + special Sunday sessions
// - MCX: Overnight sessions + extended hours
// - Crypto: 24x7 including weekends
// - Any future sessions that exchanges may introduce
//
// Currently supports 1m and D (daily) intervals
int GetOpenAlgoHistory(LPCTSTR pszTicker, int nPeriodicity, int nLastValid, int nSize, struct Quotation* pQuotes)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	if (g_oApiKey.IsEmpty())
		return nLastValid + 1;

	try
	{
		CString oURL = BuildOpenAlgoURL(g_oServer, g_nPortNumber, _T("/api/v1/history"));

		// Prepare POST data
		CString symbol = GetCleanSymbol(pszTicker);
		CString exchange = GetExchangeFromTicker(pszTicker);
		CString interval = GetIntervalString(nPeriodicity);

		// Get current time and today's date (at midnight)
		CTime currentTime = CTime::GetCurrentTime();
		CTime todayDate = CTime(currentTime.GetYear(), currentTime.GetMonth(), currentTime.GetDay(), 0, 0, 0);

		CTime startTime;
		CTime endTime = todayDate;  // Request up to today for both Daily and Intraday

		// Check if manual backfill was requested - override normal logic
		if (g_bBackfillRequested && g_nBackfillPeriodicity == nPeriodicity && g_nBackfillDays > 0)
		{
			// Manual backfill - fetch full requested period regardless of existing data
			startTime = todayDate - CTimeSpan((LONG)g_nBackfillDays, 0, 0, 0);
			g_bBackfillRequested = FALSE;  // Clear the request
			goto skip_gap_detection;
		}

		// SMART GAP-FREE BACKFILL LOGIC WITH MIXED EOD/INTRADAY SUPPORT
		if (nLastValid >= 0 && pQuotes != NULL)
		{
			// ============================================================
			// EXISTING DATA FOUND - Smart Gap Detection
			// ============================================================

			// CRITICAL: Find last bar of MATCHING TYPE for mixed data support
			// When AllowMixedEODIntra = TRUE, array contains both Daily and Intraday bars
			// We must find the last bar that matches our requested periodicity!
			int lastMatchingBarIndex = FindLastBarOfMatchingType(nPeriodicity, nLastValid, pQuotes);

			if (lastMatchingBarIndex < 0)
			{
				// No bars of matching type found → Treat as initial load
				// Example: Requesting Daily data but only 1-minute bars exist
				if (nPeriodicity == 60)
					startTime = todayDate - CTimeSpan(30, 0, 0, 0);
				else
					startTime = todayDate - CTimeSpan(3650, 0, 0, 0);

				goto skip_gap_detection;
			}

			// Extract last matching bar's DATE (ignore time component)
			int lastBarYear = pQuotes[lastMatchingBarIndex].DateTime.PackDate.Year;
			int lastBarMonth = pQuotes[lastMatchingBarIndex].DateTime.PackDate.Month;
			int lastBarDay = pQuotes[lastMatchingBarIndex].DateTime.PackDate.Day;

			// Create CTime for last bar's date (at midnight)
			CTime lastBarDate;
			try
			{
				lastBarDate = CTime(lastBarYear, lastBarMonth, lastBarDay, 0, 0, 0);
			}
			catch (...)
			{
				// Invalid date in last bar (corrupted data)
				// Fall back to initial load
				if (nPeriodicity == 60)
					startTime = todayDate - CTimeSpan(30, 0, 0, 0);
				else
					startTime = todayDate - CTimeSpan(3650, 0, 0, 0);

				goto skip_gap_detection;
			}

			// VALIDATE: Check if last bar is in the future (corrupted data)
			if (lastBarDate > todayDate)
			{
				// Last bar is in future - corrupted data detected
				// Fall back to initial load
				if (nPeriodicity == 60)
					startTime = todayDate - CTimeSpan(30, 0, 0, 0);
				else
					startTime = todayDate - CTimeSpan(3650, 0, 0, 0);

				goto skip_gap_detection;
			}

			// Calculate gap in days
			CTimeSpan gap = todayDate - lastBarDate;
			int gapDays = (int)gap.GetDays();

			// DEBUG: Log backfill info for diagnosis
			//CString debugMsg;
			//debugMsg.Format(_T("BACKFILL - Symbol: %s, GapDays: %d, LastBar: %s, Today: %s, Periodicity: %d"),
			//	pszTicker, gapDays,
			//	lastBarDate.Format(_T("%Y-%m-%d")).GetString(),
			//	todayDate.Format(_T("%Y-%m-%d")).GetString(),
			//	nPeriodicity);
			//OutputDebugString(debugMsg);

			// Apply different logic for Daily vs Intraday data
			if (nPeriodicity == 60)  // 1-minute data
			{
				const int MAX_BACKFILL_DAYS_1M = 30;  // Safety limit for 1-minute data

				// Check if data is too old → Force initial load
				if (gapDays > MAX_BACKFILL_DAYS_1M)
				{
					// Data too stale - do fresh 30-day load
					startTime = todayDate - CTimeSpan(MAX_BACKFILL_DAYS_1M, 0, 0, 0);
				}
				else
				{
					// Recent data - backfill from last bar's date
					startTime = lastBarDate;
				}
			}
			else  // Daily data (nPeriodicity == 86400)
			{
				const int MAX_BACKFILL_DAYS_DAILY = 730;     // Safety limit: 2 years
				const int MIN_DAILY_BARS = 250;               // ~1 year of trading days
				const int STALENESS_THRESHOLD_DAYS = 365;    // 1 year staleness check

				// CHECK 1: Do we have enough bars for proper analysis?
				if (lastMatchingBarIndex < MIN_DAILY_BARS)
				{
					// Too few Daily bars - do initial 10-year load
					startTime = todayDate - CTimeSpan(3650, 0, 0, 0);
				}
				// CHECK 2: Is data too stale?
				else if (gapDays > STALENESS_THRESHOLD_DAYS)
				{
					// Gap > 1 year - do initial 10-year load
					startTime = todayDate - CTimeSpan(3650, 0, 0, 0);
				}
				// CHECK 3: Gap within safety limit?
				else if (gapDays > MAX_BACKFILL_DAYS_DAILY)
				{
					// Gap exceeds 2-year safety limit - cap at maximum
					startTime = todayDate - CTimeSpan(MAX_BACKFILL_DAYS_DAILY, 0, 0, 0);
				}
				else
				{
					// Good recent data - backfill from last bar's date
					startTime = lastBarDate;
				}
			}
		}
		else
		{
			// ============================================================
			// NO EXISTING DATA - Initial Backfill
			// ============================================================

			// Check if manual backfill was requested
			if (g_bBackfillRequested && g_nBackfillPeriodicity == nPeriodicity && g_nBackfillDays > 0)
			{
				// Use requested backfill period
				startTime = todayDate - CTimeSpan((LONG)g_nBackfillDays, 0, 0, 0);
				g_bBackfillRequested = FALSE;  // Clear the request
			}
			else if (nPeriodicity == 60)  // 1-minute data
			{
				// Initial load: 30 days of 1-minute data
				startTime = todayDate - CTimeSpan(30, 0, 0, 0);
			}
			else  // Daily data
			{
				// Initial load: 10 years of daily data (increased from 1 year)
				// This provides sufficient historical context for technical analysis
				startTime = todayDate - CTimeSpan(3650, 0, 0, 0);
			}
		}

skip_gap_detection:

		CString startDate = startTime.Format(_T("%Y-%m-%d"));
		CString endDate = endTime.Format(_T("%Y-%m-%d"));

		// DEBUG: Log API request details
		//CString apiDebugMsg;
		//apiDebugMsg.Format(_T("API REQUEST - Symbol: %s, Exchange: %s, Interval: %s, Start: %s, End: %s"),
		//	(LPCTSTR)symbol, (LPCTSTR)exchange, (LPCTSTR)interval, (LPCTSTR)startDate, (LPCTSTR)endDate);
		//OutputDebugString(apiDebugMsg);

		CString oPostData;
		oPostData.Format(_T("{\"apikey\":\"%s\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"interval\":\"%s\",\"start_date\":\"%s\",\"end_date\":\"%s\"}"),
			(LPCTSTR)g_oApiKey, (LPCTSTR)symbol, (LPCTSTR)exchange, (LPCTSTR)interval, (LPCTSTR)startDate, (LPCTSTR)endDate);

		CInternetSession oSession(AGENT_NAME, 1, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL,
			INTERNET_FLAG_DONT_CACHE);
		oSession.SetOption(INTERNET_OPTION_CONNECT_TIMEOUT, 10000);
		oSession.SetOption(INTERNET_OPTION_RECEIVE_TIMEOUT, 10000);

		CHttpConnection* pConnection = NULL;
		CHttpFile* pFile = NULL;

		// Parse server
		INTERNET_PORT nPort = (INTERNET_PORT)g_nPortNumber;
		CString oServer = g_oServer;
		oServer.Replace(_T("http://"), _T(""));
		oServer.Replace(_T("https://"), _T(""));

		pConnection = oSession.GetHttpConnection(oServer, nPort);

		if (pConnection)
		{
			pFile = pConnection->OpenRequest(
				CHttpConnection::HTTP_VERB_POST,
				_T("/api/v1/history"),
				NULL, 1, NULL, NULL,
				INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE);

			if (pFile)
			{
				CString oHeaders = _T("Content-Type: application/json\r\n");
				CStringA oPostDataA(oPostData);

				if (pFile->SendRequest(oHeaders, (LPVOID)(LPCSTR)oPostDataA, oPostDataA.GetLength()))
				{
					DWORD dwStatusCode = 0;
					pFile->QueryInfoStatusCode(dwStatusCode);

					if (dwStatusCode == 200)
					{
						// Fix #6: Preallocate 1 MB - 30 days of 1m JSON is roughly
						// 9700 bars * ~100 bytes each. Avoids O(n^2) reallocations.
						CString oResponse;
						oResponse.Preallocate(1024 * 1024);
						CString oLine;
						while (pFile->ReadString(oLine))
						{
							oResponse += oLine;
						}

						// Parse JSON response
						if (oResponse.Find(_T("\"status\":\"success\"")) >= 0)
						{
							// Find data array
							int dataStart = oResponse.Find(_T("\"data\":["));
							if (dataStart >= 0)
							{
								dataStart += 8;
								int dataEnd = oResponse.Find(_T("]"), dataStart);
								if (dataEnd < 0) dataEnd = oResponse.GetLength();
								CString dataArray = oResponse.Mid(dataStart, dataEnd - dataStart);

								// DEBUG: Log data received
								//CString dataDebugMsg;
								//dataDebugMsg.Format(_T("API RESPONSE - DataLength: %d bytes"), dataArray.GetLength());
								//OutputDebugString(dataDebugMsg);

								// Debug: Check if we have meaningful data
								if (dataArray.GetLength() < 10)
								{
									// Very little data, might be an issue
									//OutputDebugString(_T("WARNING: Very little data received from API"));
									return nLastValid + 1;
								}

								// Parse each candle and merge with existing data
								int quoteIndex = 0;
								int pos = 0;
								int originalLastValid = nLastValid;  // Save for accurate counting

								// If we have existing data, we'll need to merge properly
								BOOL bHasExistingData = (nLastValid >= 0);
								if (bHasExistingData)
								{
									// CRITICAL: Check if array is near full before adding new data
									// If array is 95% full, remove oldest 10% to make room for new bars
									const int ARRAY_THRESHOLD = (int)(nSize * 0.95);  // 95% full
									const int BARS_TO_REMOVE = (int)(nSize * 0.10);   // Remove 10%

									if (nLastValid >= ARRAY_THRESHOLD)
									{
										// Array is nearly full - shift data to remove oldest bars
										memmove(pQuotes, pQuotes + BARS_TO_REMOVE, (nLastValid - BARS_TO_REMOVE + 1) * sizeof(struct Quotation));
										nLastValid -= BARS_TO_REMOVE;

										// DEBUG: Log array cleanup
										//CString cleanupMsg;
										//cleanupMsg.Format(_T("ARRAY CLEANUP - Removed %d oldest bars, New nLastValid: %d"), BARS_TO_REMOVE, nLastValid);
										//OutputDebugString(cleanupMsg);
									}

									// Start appending after existing data
									quoteIndex = nLastValid + 1;
								}

								// Count duplicates for debugging
								int duplicateCount = 0;
								int uniqueCount = 0;

								while (pos < dataArray.GetLength() && quoteIndex < nSize)
								{
									int candleStart = dataArray.Find(_T("{"), pos);
									if (candleStart < 0) break;

									int candleEnd = dataArray.Find(_T("}"), candleStart);
									if (candleEnd < 0) break;

									CString candle = dataArray.Mid(candleStart, candleEnd - candleStart + 1);

									// Parse timestamp
									int tsPos = candle.Find(_T("\"timestamp\":"));
									if (tsPos >= 0)
									{
										tsPos += 12;
										int tsEnd = candle.Find(_T(","), tsPos);
										if (tsEnd < 0) tsEnd = candle.Find(_T("}"), tsPos);
										CString tsStr = candle.Mid(tsPos, tsEnd - tsPos);
										time_t timestamp = (time_t)_tstoi64(tsStr);

										// Convert to AmiBroker date
									if (nPeriodicity == 86400) // Daily data
									{
										// For daily data, set the DAILY_MASK and EOD markers
										ConvertUnixToPackedDate(timestamp, &pQuotes[quoteIndex].DateTime);
										pQuotes[quoteIndex].DateTime.Date |= DAILY_MASK;

										// Set EOD markers and normalize ALL time fields
										// CRITICAL: All Daily bars must have identical time fields
										// to avoid display issues with the last candle
										pQuotes[quoteIndex].DateTime.PackDate.Hour = 31;      // EOD marker
										pQuotes[quoteIndex].DateTime.PackDate.Minute = 63;    // EOD marker
										pQuotes[quoteIndex].DateTime.PackDate.Second = 0;     // Normalize
										pQuotes[quoteIndex].DateTime.PackDate.MilliSec = 0;   // Normalize
										pQuotes[quoteIndex].DateTime.PackDate.MicroSec = 0;   // Normalize
									}
									else
									{
										// For intraday data
										ConvertUnixToPackedDate(timestamp, &pQuotes[quoteIndex].DateTime);

										// CRITICAL FIX: Normalize sub-minute time fields for 1-minute bars
										// This prevents freak candles during live updates when seconds change
										// Same principle as Daily bars - all bars for the same minute must have
										// identical time fields to avoid duplicate bar creation
										if (nPeriodicity == 60) // 1-minute data
										{
											pQuotes[quoteIndex].DateTime.PackDate.Second = 0;     // Normalize
											pQuotes[quoteIndex].DateTime.PackDate.MilliSec = 0;   // Normalize
											pQuotes[quoteIndex].DateTime.PackDate.MicroSec = 0;   // Normalize
										}
									}

										// Parse OHLCV
										int oPos = candle.Find(_T("\"open\":"));
										if (oPos >= 0)
										{
											oPos += 7;
											int oEnd = candle.Find(_T(","), oPos);
											CString val = candle.Mid(oPos, oEnd - oPos);
											pQuotes[quoteIndex].Open = (float)_tstof(val);
										}

										int hPos = candle.Find(_T("\"high\":"));
										if (hPos >= 0)
										{
											hPos += 7;
											int hEnd = candle.Find(_T(","), hPos);
											CString val = candle.Mid(hPos, hEnd - hPos);
											pQuotes[quoteIndex].High = (float)_tstof(val);
										}

										int lPos = candle.Find(_T("\"low\":"));
										if (lPos >= 0)
										{
											lPos += 6;
											int lEnd = candle.Find(_T(","), lPos);
											CString val = candle.Mid(lPos, lEnd - lPos);
											pQuotes[quoteIndex].Low = (float)_tstof(val);
										}

										int cPos = candle.Find(_T("\"close\":"));
										if (cPos >= 0)
										{
											cPos += 8;
											int cEnd = candle.Find(_T(","), cPos);
											if (cEnd < 0) cEnd = candle.Find(_T("}"), cPos);
											CString val = candle.Mid(cPos, cEnd - cPos);
											pQuotes[quoteIndex].Price = (float)_tstof(val);
										}

										int vPos = candle.Find(_T("\"volume\":"));
										if (vPos >= 0)
										{
											vPos += 9;
											int vEnd = candle.Find(_T(","), vPos);
											if (vEnd < 0) vEnd = candle.Find(_T("}"), vPos);
											CString val = candle.Mid(vPos, vEnd - vPos);
											pQuotes[quoteIndex].Volume = (float)_tstof(val);
										}

										int oiPos = candle.Find(_T("\"oi\":"));
										if (oiPos >= 0)
										{
											oiPos += 5;
											int oiEnd = candle.Find(_T(","), oiPos);
											if (oiEnd < 0) oiEnd = candle.Find(_T("}"), oiPos);
											CString val = candle.Mid(oiPos, oiEnd - oiPos);
											pQuotes[quoteIndex].OpenInterest = (float)_tstof(val);
										}

										// Set auxiliary data
										pQuotes[quoteIndex].AuxData1 = 0;
										pQuotes[quoteIndex].AuxData2 = 0;

										// Check for duplicate timestamps against existing data
										// FIXED: Properly handle mixed EOD/Intraday data without mktime() corruption
										BOOL bIsDuplicate = FALSE;
										if (bHasExistingData)
										{
											// Get new bar's properties
											BOOL bNewBarIsEOD = (pQuotes[quoteIndex].DateTime.PackDate.Hour == DATE_EOD_HOURS &&
											                     pQuotes[quoteIndex].DateTime.PackDate.Minute == DATE_EOD_MINUTES);

											// Check against ALL existing bars for same-day duplicates
											// CRITICAL: After sorting, today's bars are scattered throughout array
											// Must check entire array, not just "last N bars by index"
											// Optimization: array is sorted, so stop when date changes
											unsigned int newBarYear = pQuotes[quoteIndex].DateTime.PackDate.Year;
											unsigned int newBarMonth = pQuotes[quoteIndex].DateTime.PackDate.Month;
											unsigned int newBarDay = pQuotes[quoteIndex].DateTime.PackDate.Day;

											for (int i = nLastValid; i >= 0; i--)
											{
												// SOLUTION 2: Filter by periodicity - Never compare across interval types
												BOOL bExistingBarIsEOD = (pQuotes[i].DateTime.PackDate.Hour == DATE_EOD_HOURS &&
												                          pQuotes[i].DateTime.PackDate.Minute == DATE_EOD_MINUTES);

												// Skip if different interval types (EOD vs Intraday)
												if (bNewBarIsEOD != bExistingBarIsEOD)
													continue;

												// Optimization: Since sorted by time, if existing bar is older than new bar's date, stop searching
												if (pQuotes[i].DateTime.PackDate.Year < newBarYear ||
												    (pQuotes[i].DateTime.PackDate.Year == newBarYear && pQuotes[i].DateTime.PackDate.Month < newBarMonth) ||
												    (pQuotes[i].DateTime.PackDate.Year == newBarYear && pQuotes[i].DateTime.PackDate.Month == newBarMonth && pQuotes[i].DateTime.PackDate.Day < newBarDay))
												{
													break;  // No more bars from same day
												}

												// SOLUTION 3: Direct PackDate comparison instead of mktime()
												BOOL bSameBar = FALSE;

												if (bNewBarIsEOD)
												{
													// For EOD bars: Compare DATE ONLY (Year, Month, Day)
													// Ignore time components since Hour=31, Minute=63 are markers, not actual time
													bSameBar = (pQuotes[quoteIndex].DateTime.PackDate.Year == pQuotes[i].DateTime.PackDate.Year &&
													           pQuotes[quoteIndex].DateTime.PackDate.Month == pQuotes[i].DateTime.PackDate.Month &&
													           pQuotes[quoteIndex].DateTime.PackDate.Day == pQuotes[i].DateTime.PackDate.Day);
												}
												else
												{
													// For Intraday bars: Compare full timestamp (Year, Month, Day, Hour, Minute)
													// Allow same minute to be considered duplicate
													bSameBar = (pQuotes[quoteIndex].DateTime.PackDate.Year == pQuotes[i].DateTime.PackDate.Year &&
													           pQuotes[quoteIndex].DateTime.PackDate.Month == pQuotes[i].DateTime.PackDate.Month &&
													           pQuotes[quoteIndex].DateTime.PackDate.Day == pQuotes[i].DateTime.PackDate.Day &&
													           pQuotes[quoteIndex].DateTime.PackDate.Hour == pQuotes[i].DateTime.PackDate.Hour &&
													           pQuotes[quoteIndex].DateTime.PackDate.Minute == pQuotes[i].DateTime.PackDate.Minute);
												}

												if (bSameBar)
												{
													bIsDuplicate = TRUE;
													// Update existing bar with latest data instead of adding new
													pQuotes[i].Price = pQuotes[quoteIndex].Price; // Close
													pQuotes[i].High = max(pQuotes[i].High, pQuotes[quoteIndex].High);
													pQuotes[i].Low = (pQuotes[i].Low == 0) ? pQuotes[quoteIndex].Low : min(pQuotes[i].Low, pQuotes[quoteIndex].Low);
													pQuotes[i].Volume = pQuotes[quoteIndex].Volume;
													pQuotes[i].OpenInterest = pQuotes[quoteIndex].OpenInterest;
													break;
												}
											}
										}

										// Only add new bar if it's not a duplicate
										if (!bIsDuplicate)
										{
											quoteIndex++;
											uniqueCount++;
										}
										else
										{
											duplicateCount++;
										}
									}

									pos = candleEnd + 1;
								}

								// DO NOT mix quote data with historical interval data
								// Quote data is for real-time window only, not for OHLC bars
								// Historical data from OpenAlgo is already complete and accurate

								pFile->Close();
								delete pFile;
								pConnection->Close();
								delete pConnection;
								oSession.Close();

								// CRITICAL: Sort quotes by timestamp (oldest to newest)
								// This ensures proper chronological order after merging new data with existing data
								// Without sorting, timestamps can be mixed up when filling gaps or adding historical data
								if (quoteIndex > 0)
								{
									qsort(pQuotes, quoteIndex, sizeof(struct Quotation), CompareQuotations);
								}

								// If we have more data than the array can hold, keep the most recent data
								if (quoteIndex > nSize)
								{
									int excessBars = quoteIndex - nSize;
									memmove(pQuotes, pQuotes + excessBars, nSize * sizeof(struct Quotation));
									quoteIndex = nSize;
								}

								// DEBUG: Log final result
								//CString resultDebugMsg;
								//resultDebugMsg.Format(_T("BACKFILL COMPLETE - Total: %d, Original: %d, Unique: %d, Duplicates: %d"),
								//	quoteIndex, originalLastValid + 1, uniqueCount, duplicateCount);
								//OutputDebugString(resultDebugMsg);

								return quoteIndex;
							}
						}
					}
				}

				if (pFile)
				{
					pFile->Close();
					delete pFile;
				}
			}

			if (pConnection)
			{
				pConnection->Close();
				delete pConnection;
			}
		}

		oSession.Close();
	}
	catch (CInternetException* e)
	{
		e->Delete();
	}

	return nLastValid + 1;
}

BOOL AddToOpenAlgoPortfolio(LPCTSTR pszTicker)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	BOOL bOK = FALSE;
	try
	{
		CString endpoint;
		endpoint.Format(_T("/api/v1/watchlist/add?symbol=%s"), pszTicker);
		CString oURL = BuildOpenAlgoURL(g_oServer, g_nPortNumber, endpoint);

		CInternetSession oSession(AGENT_NAME, 1, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, INTERNET_FLAG_DONT_CACHE);

		CStdioFile* poFile = oSession.OpenURL(oURL, 1, INTERNET_FLAG_TRANSFER_ASCII | INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE);

		CString oLine;
		if (poFile && poFile->ReadString(oLine))
		{
			if (oLine.Find(_T("OK")) >= 0 || oLine.Find(_T("success")) >= 0)
			{
				bOK = TRUE;
			}
		}

		if (poFile)
		{
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
	return bOK;
}

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
		g_nRefreshInterval = AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("RefreshInterval"), 5);
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
		g_nBackfillIntervalMs = AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("BackfillIntervalMs"), 5000);  // Default: 5 seconds

		g_nStatus = STATUS_WAIT;
		g_bPluginInitialized = TRUE;

		// Initialize quote cache
		g_QuoteCache.InitHashTable(997); // Prime number for better hash distribution

		// Initialize critical section for WebSocket operations
		InitializeCriticalSection(&g_WebSocketCriticalSection);
		g_bCriticalSectionInitialized = TRUE;

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

		StartHttpWorker();

		// Launch the WS reader thread before (or alongside) the initial
		// InitializeWebSocket() so reconnect attempts also run off the UI
		// thread. The thread is the only caller of recv() on g_websocket.
		StartWsReader();

		// Log real-time settings
		CString rtMsg;
		rtMsg.Format(_T("OpenAlgo: Real-Time Candles Enabled = %d, Backfill Interval = %d ms"),
			g_bRealTimeCandlesEnabled, g_nBackfillIntervalMs);
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
				SetTimer(g_hAmiBrokerWnd, TIMER_REFRESH, g_nRefreshInterval * 1000, (TIMERPROC)OnTimerProc);
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
		g_nRefreshInterval = AfxGetApp()->GetProfileInt(_T("OpenAlgo"), _T("RefreshInterval"), 5);

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
			// "Current Symbol" relies on the next GetQuotesEx call from AmiBroker
			// (which has the actual ticker) to enqueue the per-symbol fetch via
			// the cache-invalidate path. "All Symbols" enumerates the subscribed
			// symbols and queues a fetch for each with the requested range.
			case 105: // 3 Months - Current Symbol
				g_nBackfillDays = 90;
				g_nBackfillPeriodicity = 60;
				g_bBackfillRequested = TRUE;
				::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
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

				// Walk every currently-subscribed symbol and queue a fetch.
				EnterCriticalSection(&g_WebSocketCriticalSection);
				POSITION wpos = g_SubscribedSymbols.GetStartPosition();
				while (wpos != NULL)
				{
					CString sym;
					BOOL bSub;
					g_SubscribedSymbols.GetNextAssoc(wpos, sym, bSub);
					QueueHttpFetch(sym, per, days);
				}
				LeaveCriticalSection(&g_WebSocketCriticalSection);

				::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
				break;
			}
			case 107: // 6 Months - Current Symbol
				g_nBackfillDays = 180;
				g_nBackfillPeriodicity = 60;
				g_bBackfillRequested = TRUE;
				::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
				break;
			case 109: // 1 Year - Current Symbol
				g_nBackfillDays = 365;
				g_nBackfillPeriodicity = 60;
				g_bBackfillRequested = TRUE;
				::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
				break;

			// Daily backfill (Current Symbol variants)
			case 201: // 5 Years - Current Symbol
				g_nBackfillDays = 1825;
				g_nBackfillPeriodicity = 86400;
				g_bBackfillRequested = TRUE;
				::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
				break;
			case 203: // 10 Years - Current Symbol
				g_nBackfillDays = 3650;
				g_nBackfillPeriodicity = 86400;
				g_bBackfillRequested = TRUE;
				::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
				break;
			case 205: // 25 Years - Current Symbol
				g_nBackfillDays = 9125;
				g_nBackfillPeriodicity = 86400;
				g_bBackfillRequested = TRUE;
				::PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
				break;
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
	                    ((now - pCache->lastOneMinFetch) > ONEMIN_CACHE_LIFETIME_MS);

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
		// 1-minute periodicity: daily first (chronologically older), then 1m
		if (dailyCount > 0 && nSize > 0)
		{
			int copyCount = min(dailyCount, nSize);
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


// GetRecentInfo is ONLY for Real-time Quote Window display
// This function provides Level 1 quotes for the quote window
// It should NEVER be used for chart data or OHLC bars
PLUGINAPI struct RecentInfo* GetRecentInfo(LPCTSTR pszTicker)
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	// Check if we're connected and have an API key
	if (g_nStatus != STATUS_CONNECTED || g_oApiKey.IsEmpty())
		return NULL;

	static struct RecentInfo ri;
	memset(&ri, 0, sizeof(ri));
	ri.nStructSize = sizeof(struct RecentInfo);

	CString ticker(pszTicker);
	
	// Initialize WebSocket connection if needed (but don't block if connection is in progress)
	// Also add a delay between connection attempts to avoid hammering the server
	DWORD dwNow = (DWORD)GetTickCount64();
	if (!g_bWebSocketConnected && !g_bWebSocketConnecting && 
		(dwNow - g_dwLastConnectionAttempt) > 10000) // Wait 10 seconds between attempts
	{
		g_dwLastConnectionAttempt = dwNow;
		InitializeWebSocket();
	}

	// Critical section should already be initialized in Init()

	// Check if this symbol is already subscribed via WebSocket
	BOOL bSubscribed = FALSE;
	EnterCriticalSection(&g_WebSocketCriticalSection);
	
	if (!g_SubscribedSymbols.Lookup(ticker, bSubscribed))
	{
		// Symbol not subscribed yet, subscribe to it
		if (g_bWebSocketConnected)
		{
			// Give authentication a moment to complete if it's still processing
			if (!g_bWebSocketAuthenticated)
			{
				Sleep(100);
			}
			
			// Try to subscribe - authentication will be handled automatically
			if (SubscribeToSymbol(pszTicker))
			{
				g_SubscribedSymbols.SetAt(ticker, TRUE);
				
				// Mark as authenticated since we successfully sent a subscribe request
				// (this handles cases where auth response parsing failed but server accepted subscription)
				if (!g_bWebSocketAuthenticated)
				{
					g_bWebSocketAuthenticated = TRUE;
				}
			}
		}
	}
	
	LeaveCriticalSection(&g_WebSocketCriticalSection);

	// Do NOT call ProcessWebSocketData() here. The dedicated WsReaderThread is
	// the only socket reader; if GetRecentInfo also called recv() concurrently
	// from the UI thread, two readers could split a frame between them and
	// corrupt the parser. The thread's drain populates g_QuoteCache for us.

	// Check cache for WebSocket data first
	QuoteCache cachedQuote;
	BOOL bCached = FALSE;

	if (g_QuoteCache.Lookup(ticker, cachedQuote))
	{
		// Use cached data if it's less than 5 seconds old
		DWORD dwNow = (DWORD)GetTickCount64();
		if ((dwNow - cachedQuote.lastUpdate) < 5000)
		{
			bCached = TRUE;
		}
	}

	// Fallback to HTTP API if WebSocket data not available
	if (!bCached)
	{
		if (!GetOpenAlgoQuote(pszTicker, cachedQuote))
			return NULL;

		// Store in cache
		g_QuoteCache.SetAt(ticker, cachedQuote);
	}

	// Fill RecentInfo structure
	_tcsncpy_s(ri.Name, sizeof(ri.Name) / sizeof(TCHAR), pszTicker, _TRUNCATE);
	_tcsncpy_s(ri.Exchange, sizeof(ri.Exchange) / sizeof(TCHAR), cachedQuote.exchange, _TRUNCATE);

	ri.nStatus = RI_STATUS_UPDATE | RI_STATUS_TRADE | RI_STATUS_BARSREADY;
	ri.nBitmap = RI_LAST | RI_OPEN | RI_HIGHLOW | RI_TRADEVOL | RI_OPENINT;

	ri.fLast = cachedQuote.ltp;
	ri.fOpen = cachedQuote.open;
	ri.fHigh = cachedQuote.high;
	ri.fLow = cachedQuote.low;
	ri.fPrev = cachedQuote.close;
	ri.fChange = cachedQuote.ltp - cachedQuote.close;
	ri.fTradeVol = cachedQuote.volume;
	ri.fTotalVol = cachedQuote.volume;
	ri.fOpenInt = cachedQuote.oi;

	// Set update times
	CTime now = CTime::GetCurrentTime();
	ri.nDateUpdate = now.GetYear() * 10000 + now.GetMonth() * 100 + now.GetDay();
	ri.nTimeUpdate = now.GetHour() * 10000 + now.GetMinute() * 100 + now.GetSecond();
	ri.nDateChange = ri.nDateUpdate;
	ri.nTimeChange = ri.nTimeUpdate;

	return &ri;
}

///////////////////////////////
// WebSocket Functions
///////////////////////////////

void GenerateWebSocketMaskKey(unsigned char* maskKey)
{
	// Generate a simple random mask key
	srand((unsigned int)GetTickCount64());
	maskKey[0] = (unsigned char)(rand() & 0xFF);
	maskKey[1] = (unsigned char)(rand() & 0xFF);
	maskKey[2] = (unsigned char)(rand() & 0xFF);
	maskKey[3] = (unsigned char)(rand() & 0xFF);
}

BOOL SendWebSocketFrame(const CString& message)
{
	if (g_websocket == INVALID_SOCKET)
		return FALSE;

	// Convert message to UTF-8
	CStringA messageA(message);
	int messageLen = messageA.GetLength();
	
	// Create WebSocket frame
	unsigned char frame[1024];
	int frameLen = 0;
	
	// First byte: FIN=1, OpCode=1 (text frame)
	frame[frameLen++] = 0x81;
	
	// Second byte: MASK=1 + Payload length
	if (messageLen < 126)
	{
		frame[frameLen++] = 0x80 | messageLen;
	}
	else if (messageLen < 65536)
	{
		frame[frameLen++] = 0x80 | 126;
		frame[frameLen++] = (messageLen >> 8) & 0xFF;
		frame[frameLen++] = messageLen & 0xFF;
	}
	else
	{
		return FALSE; // Message too long
	}
	
	// Generate masking key
	unsigned char maskKey[4];
	GenerateWebSocketMaskKey(maskKey);
	memcpy(&frame[frameLen], maskKey, 4);
	frameLen += 4;
	
	// Masked payload
	for (int i = 0; i < messageLen; i++)
	{
		frame[frameLen++] = messageA[i] ^ maskKey[i % 4];
	}
	
	// Send the frame
	int sent = send(g_websocket, (char*)frame, frameLen, 0);
	return (sent == frameLen);
}

CString DecodeWebSocketFrame(const char* buffer, int length)
{
	CString result;
	
	if (length < 2) return result;
	
	int pos = 0;
	unsigned char firstByte = (unsigned char)buffer[pos++];
	unsigned char secondByte = (unsigned char)buffer[pos++];
	
	// Check frame type
	unsigned char opcode = firstByte & 0x0F;
	
	// Extract payload info (needed for all frame types)
	BOOL masked = (secondByte & 0x80) != 0;
	int payloadLen = secondByte & 0x7F;

	// Handle different frame types
	if (opcode == 0x08) // Close frame
	{
		// Extract close status code and reason from payload
		CString closeInfo = _T("CLOSE_FRAME");

		// Handle extended payload length for close frames
		int closePos = pos;
		if (payloadLen == 126)
		{
			if (closePos + 2 > length) return closeInfo;
			payloadLen = ((unsigned char)buffer[closePos] << 8) | (unsigned char)buffer[closePos + 1];
			closePos += 2;
		}

		// Extract masking key if present
		unsigned char closeMaskKey[4] = {0};
		if (masked)
		{
			if (closePos + 4 > length) return closeInfo;
			memcpy(closeMaskKey, &buffer[closePos], 4);
			closePos += 4;
		}

		// Extract status code (first 2 bytes of payload)
		if (payloadLen >= 2 && closePos + 2 <= length)
		{
			unsigned char byte1 = masked ? (buffer[closePos] ^ closeMaskKey[0]) : buffer[closePos];
			unsigned char byte2 = masked ? (buffer[closePos + 1] ^ closeMaskKey[1]) : buffer[closePos + 1];
			int statusCode = (byte1 << 8) | byte2;

			// Extract reason text if present
			CString reason;
			if (payloadLen > 2 && closePos + payloadLen <= length)
			{
				CStringA reasonA;
				char* reasonBuf = reasonA.GetBuffer(payloadLen - 2 + 1);
				for (int i = 2; i < payloadLen && (closePos + i) < length; i++)
				{
					reasonBuf[i - 2] = masked ? (buffer[closePos + i] ^ closeMaskKey[i % 4]) : buffer[closePos + i];
				}
				reasonBuf[payloadLen - 2] = '\0';
				reasonA.ReleaseBuffer();
				reason = CString(reasonA);
			}

			if (reason.IsEmpty())
			{
				closeInfo.Format(_T("CLOSE_FRAME (Status Code: %d)"), statusCode);
			}
			else
			{
				closeInfo.Format(_T("CLOSE_FRAME (Status Code: %d, Reason: %s)"), statusCode, reason);
			}
		}

		return closeInfo;
	}
	else if (opcode == 0x09) // Ping frame
	{
		// Extract PING payload to echo back in PONG (RFC 6455 Section 5.5.3 requirement)
		// The Python websockets library sends PING with a 4-byte payload and expects
		// the PONG to echo it back exactly. If we send empty PONG, server closes with code 1011.
		CString pingResult = _T("PING_FRAME");

		// Handle extended payload length
		int pingPos = pos;
		if (payloadLen == 126)
		{
			if (pingPos + 2 > length) return pingResult;
			payloadLen = ((unsigned char)buffer[pingPos] << 8) | (unsigned char)buffer[pingPos + 1];
			pingPos += 2;
		}

		// Extract masking key if present
		unsigned char pingMaskKey[4] = {0};
		if (masked)
		{
			if (pingPos + 4 > length) return pingResult;
			memcpy(pingMaskKey, &buffer[pingPos], 4);
			pingPos += 4;
		}

		// Extract payload (usually 4 bytes from Python websockets library)
		if (payloadLen > 0 && payloadLen <= 125 && pingPos + payloadLen <= length)
		{
			CStringA hexPayload;
			for (int i = 0; i < payloadLen; i++)
			{
				unsigned char byte = masked ? (buffer[pingPos + i] ^ pingMaskKey[i % 4]) : buffer[pingPos + i];
				CStringA hexByte;
				hexByte.Format("%02X", byte);
				hexPayload += hexByte;
			}

			// Return "PING_FRAME:XXXXXXXX" where X is hex-encoded payload
			pingResult = _T("PING_FRAME:") + CString(hexPayload);
		}

		return pingResult;
	}
	else if (opcode == 0x0A) // Pong frame
	{
		return _T("PONG_FRAME");
	}
	else if (opcode != 0x01) // Not a text frame
	{
		return result;
	}
	
	// Handle extended payload length
	if (payloadLen == 126)
	{
		if (pos + 2 > length) return result;
		payloadLen = ((unsigned char)buffer[pos] << 8) | (unsigned char)buffer[pos + 1];
		pos += 2;
	}
	else if (payloadLen == 127)
	{
		return result; // 64-bit length not supported
	}
	
	// Validate payload length doesn't exceed buffer (increased to support larger frames)
	if (payloadLen <= 0 || payloadLen > 16000) return result;
	
	// Handle masking key
	unsigned char maskKey[4] = {0};
	if (masked)
	{
		if (pos + 4 > length) return result;
		memcpy(maskKey, &buffer[pos], 4);
		pos += 4;
	}
	
	// Validate we have enough data for the payload
	if (pos + payloadLen > length) return result;
	
	// Extract and unmask payload
	CStringA payloadA;
	char* payloadBuffer = payloadA.GetBuffer(payloadLen + 1);
	
	for (int i = 0; i < payloadLen; i++)
	{
		if (masked)
		{
			payloadBuffer[i] = buffer[pos + i] ^ maskKey[i % 4];
		}
		else
		{
			payloadBuffer[i] = buffer[pos + i];
		}
	}
	payloadBuffer[payloadLen] = '\0';
	payloadA.ReleaseBuffer(payloadLen);
	
	result = CString(payloadA);
	
	return result;
}

BOOL InitializeWebSocket(void)
{
	OutputDebugString(_T("OpenAlgo: InitializeWebSocket() called"));

	if (g_bWebSocketConnected)
	{
		OutputDebugString(_T("OpenAlgo: InitializeWebSocket - Already connected, returning TRUE"));
		return TRUE;
	}

	if (g_bWebSocketConnecting)
	{
		OutputDebugString(_T("OpenAlgo: InitializeWebSocket - Connection in progress, returning FALSE"));
		return FALSE; // Connection in progress, don't start another
	}

	if (g_oWebSocketUrl.IsEmpty() || g_oApiKey.IsEmpty())
	{
		CString errMsg;
		errMsg.Format(_T("OpenAlgo: InitializeWebSocket - FAILED: URL='%s' APIKey='%s'"),
			g_oWebSocketUrl.IsEmpty() ? _T("EMPTY") : g_oWebSocketUrl,
			g_oApiKey.IsEmpty() ? _T("EMPTY") : _T("SET"));
		OutputDebugString(errMsg);
		return FALSE;
	}

	CString startMsg;
	startMsg.Format(_T("OpenAlgo: InitializeWebSocket - Starting connection to %s"), g_oWebSocketUrl);
	OutputDebugString(startMsg);

	// Initialize Winsock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		OutputDebugString(_T("OpenAlgo: InitializeWebSocket - WSAStartup FAILED"));
		return FALSE;
	}

	OutputDebugString(_T("OpenAlgo: InitializeWebSocket - WSAStartup succeeded, calling ConnectWebSocket()"));

	g_bWebSocketConnecting = TRUE;
	BOOL result = ConnectWebSocket();
	g_bWebSocketConnecting = FALSE;

	CString resultMsg;
	resultMsg.Format(_T("OpenAlgo: InitializeWebSocket - ConnectWebSocket returned %d"), result);
	OutputDebugString(resultMsg);

	return result;
}

BOOL ConnectWebSocket(void)
{
	// Parse WebSocket URL
	CString host, path;
	int port = 80;
	
	CString url = g_oWebSocketUrl;
	if (url.Left(5) == _T("wss://"))
	{
		port = 443;
		url = url.Mid(6);
	}
	else if (url.Left(5) == _T("ws://"))
	{
		url = url.Mid(5);
	}
	
	// Extract host and port
	int slashPos = url.Find(_T('/'));
	if (slashPos > 0)
	{
		host = url.Left(slashPos);
		path = url.Mid(slashPos);
	}
	else
	{
		host = url;
		path = _T("/");
	}
	
	int colonPos = host.Find(_T(':'));
	if (colonPos > 0)
	{
		CString portStr = host.Mid(colonPos + 1);
		port = _ttoi(portStr);
		host = host.Left(colonPos);
	}
	
	// Create socket
	g_websocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_websocket == INVALID_SOCKET)
		return FALSE;
	
	// Set socket timeouts (blocking mode initially)
	int timeout = 5000; // 5 seconds
	setsockopt(g_websocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
	setsockopt(g_websocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
	
	// Resolve hostname
	struct addrinfo hints, *result;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	
	CStringA hostA(host);
	CStringA portStrA;
	portStrA.Format("%d", port);
	
	if (getaddrinfo(hostA, portStrA, &hints, &result) != 0)
	{
		closesocket(g_websocket);
		g_websocket = INVALID_SOCKET;
		return FALSE;
	}
	
	// Connect to server (blocking with timeout)
	if (connect(g_websocket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
	{
		freeaddrinfo(result);
		closesocket(g_websocket);
		g_websocket = INVALID_SOCKET;
		return FALSE;
	}
	
	freeaddrinfo(result);
	
	// Send WebSocket upgrade request
	CString upgradeRequest;
	upgradeRequest.Format(
		_T("GET %s HTTP/1.1\r\n")
		_T("Host: %s:%d\r\n")
		_T("Upgrade: websocket\r\n")
		_T("Connection: Upgrade\r\n")
		_T("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n")
		_T("Sec-WebSocket-Version: 13\r\n")
		_T("\r\n"),
		(LPCTSTR)path, (LPCTSTR)host, port);
	
	CStringA requestA(upgradeRequest);
	if (send(g_websocket, requestA, requestA.GetLength(), 0) == SOCKET_ERROR)
	{
		closesocket(g_websocket);
		g_websocket = INVALID_SOCKET;
		return FALSE;
	}
	
	// Wait for upgrade response with proper timeout
	char buffer[1024];
	int received = recv(g_websocket, buffer, sizeof(buffer) - 1, 0);
	if (received > 0)
	{
		buffer[received] = '\0';
		CString response(buffer);
		
		if (response.Find(_T("101")) > 0 && response.Find(_T("Switching Protocols")) > 0)
		{
			g_bWebSocketConnected = TRUE;
			
			// Small delay to allow WebSocket connection to stabilize
			Sleep(200);
			
			// Now set to non-blocking mode for ongoing operations
			u_long mode = 1;
			ioctlsocket(g_websocket, FIONBIO, &mode);
			
			// Authenticate after switching to non-blocking mode
			return AuthenticateWebSocket();
		}
	}
	
	closesocket(g_websocket);
	g_websocket = INVALID_SOCKET;
	return FALSE;
}

BOOL AuthenticateWebSocket(void)
{
	OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket() called"));

	if (!g_bWebSocketConnected)
	{
		OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - WebSocket NOT CONNECTED, cannot authenticate"));
		return FALSE;
	}

	// Send authentication message
	CString authMsg = _T("{\"action\":\"authenticate\",\"api_key\":\"") + g_oApiKey + _T("\"}");

	// Log authentication message (mask API key for security)
	CString logMsg;
	if (g_oApiKey.GetLength() > 4)
	{
		logMsg.Format(_T("OpenAlgo: AuthenticateWebSocket - Sending auth with API key: %s...%s"),
			g_oApiKey.Left(2), g_oApiKey.Right(2));
	}
	else
	{
		logMsg = _T("OpenAlgo: AuthenticateWebSocket - Sending auth (API key too short or empty!)");
	}
	OutputDebugString(logMsg);

	if (SendWebSocketFrame(authMsg))
	{
		OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Sent auth message, waiting for response..."));
		// Wait for authentication response with select (for non-blocking socket)
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(g_websocket, &readfds);
		
		struct timeval timeout;
		timeout.tv_sec = 5;  // Increased timeout to 5 seconds
		timeout.tv_usec = 0;
		
		if (select(0, &readfds, NULL, NULL, &timeout) > 0)
		{
			OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Response received from server"));
			char authBuffer[1024];
			int received = recv(g_websocket, authBuffer, sizeof(authBuffer) - 1, 0);

			CString recvLog;
			recvLog.Format(_T("OpenAlgo: AuthenticateWebSocket - recv() returned %d bytes"), received);
			OutputDebugString(recvLog);

			if (received > 0)
			{
				authBuffer[received] = '\0';
				CString authResponse = DecodeWebSocketFrame(authBuffer, received);

				CString respLog;
				respLog.Format(_T("OpenAlgo: AuthenticateWebSocket - Decoded response: %s"), authResponse);
				OutputDebugString(respLog);

				// Check for success status in authentication response
				// Look for various success indicators that OpenAlgo might send
				if (authResponse.Find(_T("success")) >= 0 ||
					authResponse.Find(_T("authenticated")) >= 0 ||
					authResponse.Find(_T("\"status\":\"ok\"")) >= 0 ||
					authResponse.Find(_T("\"status\":\"success\"")) >= 0)
				{
					OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Authentication SUCCESSFUL!"));
					g_bWebSocketAuthenticated = TRUE;
					// Small delay to ensure server has processed authentication
					Sleep(200);

					// Trigger any pending subscriptions now that we're authenticated
					SubscribePendingSymbols();
					return TRUE;
				}
				else if (authResponse.Find(_T("error")) >= 0 || authResponse.Find(_T("failed")) >= 0)
				{
					// Explicit authentication failure
					OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Authentication FAILED (error in response)"));
					OutputDebugString(authResponse);
					return FALSE;
				}
				else
				{
					OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Received response but no clear success/failure indicator"));
				}
			}
			else if (received == 0)
			{
				OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - SERVER CLOSED CONNECTION during auth!"));
				g_bWebSocketConnected = FALSE;
				return FALSE;
			}
		}
		else
		{
			OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - No response within 5 seconds (timeout)"));
		}

		// If we reach here, either timeout or no clear response
		// Since authentication was sent successfully, assume success
		// This is a fallback since the test button works with the same flow
		OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Assuming authentication succeeded (fallback)"));
		g_bWebSocketAuthenticated = TRUE;
		// Longer delay to ensure server has processed authentication
		Sleep(1000);  // Increased delay to ensure server processes auth

		// Trigger any pending subscriptions now that we're authenticated
		SubscribePendingSymbols();
		return TRUE;
	}
	
	return FALSE;
}

BOOL SubscribeToSymbol(LPCTSTR pszTicker)
{
	CString logStart;
	logStart.Format(_T("OpenAlgo: SubscribeToSymbol called for: %s"), pszTicker);
	OutputDebugString(logStart);

	if (!g_bWebSocketConnected)
	{
		OutputDebugString(_T("OpenAlgo: SubscribeToSymbol - WebSocket NOT CONNECTED, cannot subscribe"));
		return FALSE;
	}

	// Extract symbol and exchange
	CString symbol = GetCleanSymbol(pszTicker);
	CString exchange = GetExchangeFromTicker(pszTicker);

	CString extractLog;
	extractLog.Format(_T("OpenAlgo: SubscribeToSymbol - Extracted: Symbol='%s' Exchange='%s'"),
		symbol, exchange);
	OutputDebugString(extractLog);

	// Send TWO subscriptions per symbol in the flat/legacy server format:
	//   Mode 1 (LTP)   = every-tick price - feeds the chart's live current bar
	//   Mode 2 (Quote) = OHLC + volume on significant changes - feeds the
	//                    Realtime Quote Window (Last/Open/High/Low/Volume)
	// Without both, either the chart lags (no LTP ticks) or the quote window
	// shows zeros for everything except Last.
	CString subLtp, subQuote;
	subLtp.Format(_T("{\"action\":\"subscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":1}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);
	subQuote.Format(_T("{\"action\":\"subscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":2}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);

	BOOL r1 = SendWebSocketFrame(subLtp);
	BOOL r2 = SendWebSocketFrame(subQuote);

	CString resultLog;
	resultLog.Format(_T("OpenAlgo: SubscribeToSymbol %s -- ltp_send=%d quote_send=%d"),
		(LPCTSTR)pszTicker, r1, r2);
	OutputDebugString(resultLog);

	BOOL result = (r1 || r2);

	return result;
}

BOOL UnsubscribeFromSymbol(LPCTSTR pszTicker)
{
	if (!g_bWebSocketConnected)
		return FALSE;
	
	// Extract symbol and exchange
	CString symbol = GetCleanSymbol(pszTicker);
	CString exchange = GetExchangeFromTicker(pszTicker);
	
	// Mirror SubscribeToSymbol's dual subscribe: unsubscribe both modes
	CString unsubLtp, unsubQuote;
	unsubLtp.Format(_T("{\"action\":\"unsubscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":1}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);
	unsubQuote.Format(_T("{\"action\":\"unsubscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":2}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);
	BOOL r1 = SendWebSocketFrame(unsubLtp);
	BOOL r2 = SendWebSocketFrame(unsubQuote);
	return (r1 || r2);
}

void SubscribePendingSymbols(void)
{
	// This function subscribes to symbols that are in the real-time quote window
	// but haven't been subscribed to WebSocket yet
	
	if (!g_bWebSocketConnected || !g_bWebSocketAuthenticated)
		return;
	
	EnterCriticalSection(&g_WebSocketCriticalSection);
	
	// Since we don't have a list of symbols from the real-time quote window directly,
	// we'll rely on GetRecentInfo being called for active symbols to trigger subscriptions
	// For now, just ensure we're ready to handle subscriptions
	
	LeaveCriticalSection(&g_WebSocketCriticalSection);
}

BOOL ProcessWebSocketData(void)
{
	// ALWAYS log that this function is called (every time)
	static int s_callCount = 0;
	s_callCount++;
	if (s_callCount <= 5 || s_callCount % 100 == 0)  // Log first 5 calls and every 100th
	{
		CString callMsg;
		callMsg.Format(_T("OpenAlgo: ProcessWebSocketData() call #%d - Connected=%d Socket=%d"),
			s_callCount, g_bWebSocketConnected, (g_websocket != INVALID_SOCKET));
		OutputDebugString(callMsg);
	}

	if (!g_bWebSocketConnected || g_websocket == INVALID_SOCKET)
	{
		// Auto-reconnect if disconnected
		static DWORD lastReconnectAttempt = 0;
		DWORD now = (DWORD)GetTickCount64();

		// Try reconnecting every 5 seconds
		if ((now - lastReconnectAttempt) > 5000)
		{
			lastReconnectAttempt = now;
			OutputDebugString(_T("OpenAlgo: ProcessWebSocketData() - NOT CONNECTED, attempting reconnect..."));

			if (InitializeWebSocket())
			{
				OutputDebugString(_T("OpenAlgo: *** AUTO-RECONNECT SUCCESSFUL! ***"));

				// Re-issue every previously-active subscription. We deliberately
				// keep g_SubscribedSymbols intact across disconnects so that the
				// list of symbols the user is viewing survives a brief WS drop.
				EnterCriticalSection(&g_WebSocketCriticalSection);
				int reSubbed = 0;
				POSITION rpos = g_SubscribedSymbols.GetStartPosition();
				while (rpos != NULL)
				{
					CString sym;
					BOOL b;
					g_SubscribedSymbols.GetNextAssoc(rpos, sym, b);
					SubscribeToSymbol(sym);
					reSubbed++;
				}
				LeaveCriticalSection(&g_WebSocketCriticalSection);

				CString resubLog;
				resubLog.Format(_T("OpenAlgo: Re-subscribed %d symbol(s) after reconnect"), reSubbed);
				OutputDebugString(resubLog);
				return TRUE;
			}
			else
			{
				OutputDebugString(_T("OpenAlgo: Auto-reconnect failed, will retry in 5 seconds"));
			}
		}
		return FALSE;
	}

	// Send periodic ping to keep connection alive
	static DWORD lastPingTime = 0;
	static BOOL pingTimerInitialized = FALSE;
	DWORD currentTime = (DWORD)GetTickCount64();

	// Initialize timer on first call (don't send PING immediately)
	if (!pingTimerInitialized)
	{
		lastPingTime = currentTime;
		pingTimerInitialized = TRUE;
	}

	if ((currentTime - lastPingTime) > 30000) // Ping every 30 seconds
	{
		// Send WebSocket ping frame (opcode 0x09)
		unsigned char pingFrame[6] = {0x89, 0x84, 0x00, 0x00, 0x00, 0x00}; // Ping with 4-byte mask
		GenerateWebSocketMaskKey(&pingFrame[2]);
		send(g_websocket, (char*)pingFrame, 6, 0);
		lastPingTime = currentTime;
		OutputDebugString(_T("OpenAlgo: Sent WebSocket ping"));
	}
	
	// CRITICAL: Read ALL pending data in a loop
	// The server sends subscription ACKs and market data continuously
	// We must drain the receive buffer or the server will close the connection!
	int messagesProcessed = 0;
	const int MAX_MESSAGES_PER_CALL = 100; // Prevent infinite loop

	while (messagesProcessed < MAX_MESSAGES_PER_CALL)
	{
		// Check for incoming data (non-blocking)
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(g_websocket, &readfds);

		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 0; // Non-blocking

		int selectResult = select(0, &readfds, NULL, NULL, &timeout);

		if (selectResult <= 0)
		{
			// No more data available
			if (messagesProcessed > 0)
			{
				CString doneMsg;
				doneMsg.Format(_T("OpenAlgo: Processed %d messages this call"), messagesProcessed);
				OutputDebugString(doneMsg);
			}
			break;
		}

		messagesProcessed++;

		if (selectResult > 0)
	{
		// Increased buffer size to 16KB to handle large WebSocket frames without fragmentation
		// This prevents misinterpreting partial frames as CLOSE frames
		char buffer[16384];
		int received = recv(g_websocket, buffer, sizeof(buffer) - 1, 0);

		// Avoid per-recv logging for performance
		
		if (received > 0)
		{
			CString data = DecodeWebSocketFrame(buffer, received);

			// Only log control frames and errors (not every data message)
			if (data.Find(_T("CLOSE_FRAME")) == 0)
			{
				CString closeLog;
				closeLog.Format(_T("OpenAlgo: Received %s"), data);
				OutputDebugString(closeLog);
			}

			// Handle WebSocket control frames
			if (data.Find(_T("PING_FRAME")) == 0)  // Starts with "PING_FRAME"
			{
				// RFC 6455 Section 5.5.3: PONG must echo the PING's payload exactly
				// Extract payload from "PING_FRAME:XXXXXXXX" format (hex-encoded)
				CString payload;
				int colonPos = data.Find(':');
				if (colonPos > 0)
				{
					payload = data.Mid(colonPos + 1);
				}

				// Convert hex payload back to bytes
				int payloadLen = payload.GetLength() / 2;
				unsigned char payloadBytes[125] = {0};  // Max control frame payload size
				for (int i = 0; i < payloadLen && i < 125; i++)
				{
					CString hexByte = payload.Mid(i * 2, 2);
					payloadBytes[i] = (unsigned char)_tcstoul(hexByte, NULL, 16);
				}

				// Build PONG frame with echoed payload
				unsigned char pongFrame[256];
				int frameLen = 0;

				pongFrame[frameLen++] = 0x8A;  // FIN + opcode 0x0A (PONG)
				pongFrame[frameLen++] = 0x80 | payloadLen;  // MASK bit + payload length

				// Generate and add masking key
				unsigned char maskKey[4];
				GenerateWebSocketMaskKey(maskKey);
				memcpy(&pongFrame[frameLen], maskKey, 4);
				frameLen += 4;

				// Add masked payload (echo back the PING payload)
				for (int i = 0; i < payloadLen; i++)
				{
					pongFrame[frameLen++] = payloadBytes[i] ^ maskKey[i % 4];
				}

				// Send PONG with echoed payload
				send(g_websocket, (char*)pongFrame, frameLen, 0);

				CString pongLog;
				pongLog.Format(_T("OpenAlgo: Received PING with %d-byte payload, sent PONG with echoed payload"), payloadLen);
				OutputDebugString(pongLog);

				continue; // Continue processing more messages
			}
			else if (data.Find(_T("CLOSE_FRAME")) == 0)  // Starts with "CLOSE_FRAME"
			{
				// Connection closed by server - log the reason
				CString closeLog;
				closeLog.Format(_T("OpenAlgo: Received %s from server - closing connection"), data);
				OutputDebugString(closeLog);
				g_bWebSocketConnected = FALSE;
				g_bWebSocketAuthenticated = FALSE;
				closesocket(g_websocket);
				g_websocket = INVALID_SOCKET;
				break; // Exit loop
			}
			else if (data == _T("PONG_FRAME"))
			{
				// Pong received, connection is alive
				continue; // Continue processing more messages
			}

			// Handle subscription acknowledgment.
			// The server formats responses with whitespace ("type": "subscribe"),
			// so check for the substring rather than a strict no-space pattern.
			if (!data.IsEmpty() &&
			    data.Find(_T("\"subscribe\"")) >= 0 &&
			    data.Find(_T("\"status\"")) >= 0 &&
			    data.Find(_T("\"ltp\"")) < 0)   // exclude tick frames that mention "subscribe" by accident
			{
				CString ackLog;
				ackLog.Format(_T("OpenAlgo: Subscription ACK: %s"),
					data.GetLength() > 240 ? CString(data).Left(240) : data);
				OutputDebugString(ackLog);
				continue;
			}

			// Detect market-data frames by structural presence of the core tick
			// fields (symbol + exchange + ltp). This is whitespace-tolerant and
			// works whether the server emits {"type":"ltp", ...}, {"type":"quote", ...},
			// {"type":"market_data", ...}, or {"type":"depth", ...}.
			BOOL bIsMarketData = !data.IsEmpty() &&
				data.Find(_T("\"symbol\""))   >= 0 &&
				data.Find(_T("\"exchange\"")) >= 0 &&
				data.Find(_T("\"ltp\""))      >= 0;

			if (bIsMarketData)
			{
				// Simple JSON parsing to extract quote data
				CString symbol, exchange, timestamp;
				float ltp = 0, open = 0, high = 0, low = 0, close = 0, volume = 0, oi = 0;
				float lastTradeQty = 0;  // NEW: For real-time candle building

				// Extract symbol (handle JSON with or without spaces after colon)
				int symbolPos = data.Find(_T("\"symbol\":"));
				if (symbolPos >= 0)
				{
					symbolPos += 9;  // Skip "symbol":
					// Skip optional whitespace
					while (symbolPos < data.GetLength() && (data[symbolPos] == ' ' || data[symbolPos] == '\t'))
						symbolPos++;
					// Skip opening quote
					if (symbolPos < data.GetLength() && data[symbolPos] == '\"')
						symbolPos++;
					int endPos = data.Find(_T("\""), symbolPos);
					if (endPos > symbolPos)
						symbol = data.Mid(symbolPos, endPos - symbolPos);
				}

				// Extract exchange (handle JSON with or without spaces after colon)
				int exchangePos = data.Find(_T("\"exchange\":"));
				if (exchangePos >= 0)
				{
					exchangePos += 11;  // Skip "exchange":
					// Skip optional whitespace
					while (exchangePos < data.GetLength() && (data[exchangePos] == ' ' || data[exchangePos] == '\t'))
						exchangePos++;
					// Skip opening quote
					if (exchangePos < data.GetLength() && data[exchangePos] == '\"')
						exchangePos++;
					int endPos = data.Find(_T("\""), exchangePos);
					if (endPos > exchangePos)
						exchange = data.Mid(exchangePos, endPos - exchangePos);
				}

				// Extract LTP
				int ltpPos = data.Find(_T("\"ltp\":"));
				if (ltpPos >= 0)
				{
					ltpPos += 6;
					int endPos = data.Find(_T(","), ltpPos);
					if (endPos < 0) endPos = data.Find(_T("}"), ltpPos);
					CString val = data.Mid(ltpPos, endPos - ltpPos);
					ltp = (float)_tstof(val);
				}

				// Extract Mode 2 (Quote) fields: open / high / low / close /
				// volume. Mode 1 (LTP) frames don't have these so the locals
				// stay 0, and we'll preserve any earlier values when we copy
				// into the cache (see below).
				{
					int p = data.Find(_T("\"open\":"));
					if (p >= 0) {
						p += 7;
						int e = data.Find(_T(","), p);
						if (e < 0) e = data.Find(_T("}"), p);
						open = (float)_tstof(data.Mid(p, e - p));
					}
					p = data.Find(_T("\"high\":"));
					if (p >= 0) {
						p += 7;
						int e = data.Find(_T(","), p);
						if (e < 0) e = data.Find(_T("}"), p);
						high = (float)_tstof(data.Mid(p, e - p));
					}
					p = data.Find(_T("\"low\":"));
					if (p >= 0) {
						p += 6;
						int e = data.Find(_T(","), p);
						if (e < 0) e = data.Find(_T("}"), p);
						low = (float)_tstof(data.Mid(p, e - p));
					}
					p = data.Find(_T("\"close\":"));
					if (p >= 0) {
						p += 8;
						int e = data.Find(_T(","), p);
						if (e < 0) e = data.Find(_T("}"), p);
						close = (float)_tstof(data.Mid(p, e - p));
					}
					p = data.Find(_T("\"volume\":"));
					if (p >= 0) {
						p += 9;
						int e = data.Find(_T(","), p);
						if (e < 0) e = data.Find(_T("}"), p);
						volume = (float)_tstof(data.Mid(p, e - p));
					}
					p = data.Find(_T("\"oi\":"));
					if (p >= 0) {
						p += 5;
						int e = data.Find(_T(","), p);
						if (e < 0) e = data.Find(_T("}"), p);
						oi = (float)_tstof(data.Mid(p, e - p));
					}
				}

				// Extract last trade quantity - try both field names
				// Legacy format: "last_trade_quantity":100
				// Docs format: "ltq":100
				int lastTradeQtyPos = data.Find(_T("\"ltq\":"));
				if (lastTradeQtyPos >= 0)
				{
					lastTradeQtyPos += 6;
					int endPos = data.Find(_T(","), lastTradeQtyPos);
					if (endPos < 0) endPos = data.Find(_T("}"), lastTradeQtyPos);
					CString val = data.Mid(lastTradeQtyPos, endPos - lastTradeQtyPos);
					lastTradeQty = (float)_tstof(val);
				}
				else
				{
					lastTradeQtyPos = data.Find(_T("\"last_trade_quantity\":"));
					if (lastTradeQtyPos >= 0)
					{
						lastTradeQtyPos += 22;
						int endPos = data.Find(_T(","), lastTradeQtyPos);
						if (endPos < 0) endPos = data.Find(_T("}"), lastTradeQtyPos);
						CString val = data.Mid(lastTradeQtyPos, endPos - lastTradeQtyPos);
						lastTradeQty = (float)_tstof(val);
					}
				}

				// NEW: Extract timestamp (supports both Unix milliseconds and ISO 8601 string)
				// Server sends: "timestamp":1761157800000 (Unix milliseconds, no quotes)
				// OR: "timestamp":"2025-05-28T10:30:45.123Z" (ISO 8601 string)
				int timestampPos = data.Find(_T("\"timestamp\":"));
				if (timestampPos >= 0)
				{
					timestampPos += 12;  // Skip "timestamp":

					// Check if it's a string (starts with quote) or number
					CString nextChar = data.Mid(timestampPos, 1);
					if (nextChar == _T("\""))
					{
						// ISO 8601 string format: "timestamp":"2025-05-28..."
						timestampPos += 1;  // Skip opening quote
						int endPos = data.Find(_T("\""), timestampPos);
						timestamp = data.Mid(timestampPos, endPos - timestampPos);
					}
					else
					{
						// Unix milliseconds format: "timestamp":1761157800000
						int endPos = data.Find(_T(","), timestampPos);
						if (endPos < 0) endPos = data.Find(_T("}"), timestampPos);
						timestamp = data.Mid(timestampPos, endPos - timestampPos);
						timestamp.Trim();  // Remove whitespace
					}
				}

				// Throttled logging - only log every 100th tick to avoid performance impact
				static int s_wsCounter = 0;
				s_wsCounter++;
				if (s_wsCounter <= 3 || s_wsCounter % 100 == 0)
				{
					CString debugMsg;
					debugMsg.Format(_T("OpenAlgo: WS Tick #%d: %s-%s LTP=%.2f Qty=%.0f"),
						s_wsCounter, symbol, exchange, ltp, lastTradeQty);
					OutputDebugString(debugMsg);
				}

				// Extract other fields similarly...
				// (Simplified implementation - you could add more fields)

				// Update cache (for GetRecentInfo() compatibility).
				// Mode 1 (LTP) frames carry only ltp+timestamp; Mode 2 (Quote)
				// frames carry full OHLC+volume. Merge into the existing cache
				// entry so a Mode 1 tick doesn't blow away OHLC populated by
				// the last Mode 2 quote.
				if (!symbol.IsEmpty() && !exchange.IsEmpty())
				{
					CString ticker = symbol + _T("-") + exchange;
					QuoteCache quote;
					g_QuoteCache.Lookup(ticker, quote);   // start from cached, if any

					quote.symbol = symbol;
					quote.exchange = exchange;
					if (ltp    > 0.0f) quote.ltp    = ltp;
					if (open   > 0.0f) quote.open   = open;
					if (high   > 0.0f) quote.high   = high;
					if (low    > 0.0f) quote.low    = low;
					if (close  > 0.0f) quote.close  = close;
					if (volume > 0.0f) quote.volume = volume;
					if (oi     > 0.0f) quote.oi     = oi;
					quote.lastUpdate = (DWORD)GetTickCount64();

					g_QuoteCache.SetAt(ticker, quote);

					// Process tick for real-time candle building
					if (g_bRealTimeCandlesEnabled && ltp > 0)
					{
						// Default quantity to 1 if missing (quote mode doesn't include ltq)
						if (lastTradeQty <= 0)
							lastTradeQty = 1.0f;

						// Use server timestamp if available (epoch ms), fall back to system time
						time_t tickTimestamp = time(NULL);
						if (!timestamp.IsEmpty())
						{
							// Try parsing as epoch milliseconds first
							__int64 tsValue = _ttoi64(timestamp);
							if (tsValue > 1000000000000LL)  // Epoch milliseconds
							{
								time_t serverTime = (time_t)(tsValue / 1000);
								// Validate server time is within reasonable range (not stale)
								time_t diff = tickTimestamp - serverTime;
								if (diff >= 0 && diff < 300)  // Within 5 minutes
									tickTimestamp = serverTime;
							}
							else if (tsValue > 1000000000LL)  // Epoch seconds
							{
								time_t serverTime = (time_t)tsValue;
								time_t diff = tickTimestamp - serverTime;
								if (diff >= 0 && diff < 300)
									tickTimestamp = serverTime;
							}
						}

						ProcessTick(symbol, exchange, ltp, lastTradeQty, tickTimestamp);
					}
				}

				continue; // Continue processing more messages
			}
			else
			{
				// Unknown message type - just continue
				OutputDebugString(_T("OpenAlgo: Received unknown/unhandled message type"));
				continue;
			}
		}
		else if (received == 0)
		{
			// Connection closed by server (graceful close)
			OutputDebugString(_T("OpenAlgo: ========== CRITICAL: SERVER CLOSED CONNECTION =========="));
			OutputDebugString(_T("OpenAlgo: recv() returned 0 - server sent FIN packet (connection closed gracefully)"));
			OutputDebugString(_T("OpenAlgo: Server closed after ~1 minute - will attempt auto-reconnect"));
			OutputDebugString(_T("OpenAlgo: ========================================================"));

			// Mark as disconnected. IMPORTANT: do NOT clear g_SubscribedSymbols
			// here -- we need that list to re-issue subscribes after reconnect.
			g_bWebSocketConnected = FALSE;
			g_bWebSocketAuthenticated = FALSE;
			closesocket(g_websocket);
			g_websocket = INVALID_SOCKET;

			// Attempt immediate reconnection
			OutputDebugString(_T("OpenAlgo: Attempting WebSocket reconnection..."));
			if (InitializeWebSocket())
			{
				OutputDebugString(_T("OpenAlgo: *** RECONNECTED SUCCESSFULLY! ***"));

				// Re-send subscribe for every symbol we were tracking.
				EnterCriticalSection(&g_WebSocketCriticalSection);
				int reSubbed = 0;
				POSITION rpos = g_SubscribedSymbols.GetStartPosition();
				while (rpos != NULL)
				{
					CString sym;
					BOOL b;
					g_SubscribedSymbols.GetNextAssoc(rpos, sym, b);
					SubscribeToSymbol(sym);
					reSubbed++;
				}
				LeaveCriticalSection(&g_WebSocketCriticalSection);

				CString resubLog;
				resubLog.Format(_T("OpenAlgo: Re-subscribed %d symbol(s) after immediate reconnect"), reSubbed);
				OutputDebugString(resubLog);
			}
			else
			{
				OutputDebugString(_T("OpenAlgo: *** RECONNECTION FAILED - will retry on next call ***"));
			}

			break; // Exit loop after reconnect attempt
		}
	}
	} // End of while loop for processing messages

	return (messagesProcessed > 0); // Return TRUE if we processed any messages
}

void CleanupWebSocket(void)
{
	if (g_bCriticalSectionInitialized)
	{
		EnterCriticalSection(&g_WebSocketCriticalSection);
		
		// Unsubscribe from all symbols
		POSITION pos = g_SubscribedSymbols.GetStartPosition();
		while (pos != NULL)
		{
			CString symbol;
			BOOL subscribed;
			g_SubscribedSymbols.GetNextAssoc(pos, symbol, subscribed);
			
			if (subscribed)
			{
				UnsubscribeFromSymbol(symbol);
			}
		}
		
		g_SubscribedSymbols.RemoveAll();
		
		LeaveCriticalSection(&g_WebSocketCriticalSection);
		DeleteCriticalSection(&g_WebSocketCriticalSection);
		g_bCriticalSectionInitialized = FALSE;
	}
	
	// Close WebSocket connection
	if (g_websocket != INVALID_SOCKET)
	{
		closesocket(g_websocket);
		g_websocket = INVALID_SOCKET;
	}
	
	g_bWebSocketConnected = FALSE;
	g_bWebSocketAuthenticated = FALSE;
	g_bWebSocketConnecting = FALSE;
	
	WSACleanup();
}

///////////////////////////////
// Real-Time Candle Building Functions
///////////////////////////////

// Parse ISO 8601 timestamp (e.g., "2025-05-28T10:30:45.123Z")
time_t ParseISO8601Timestamp(const CString& isoTimestamp)
{
	if (isoTimestamp.IsEmpty())
		return time(NULL);

	// Check if it's a Unix timestamp (all digits, possibly with whitespace)
	// Example: "1761157800000" (milliseconds since epoch)
	CString trimmed = isoTimestamp;
	trimmed.Trim();

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
		// Parse as Unix timestamp in MILLISECONDS
		__int64 milliseconds = _tstoi64(trimmed);

		// Convert milliseconds to seconds
		time_t timestamp = (time_t)(milliseconds / 1000);

		return timestamp;
	}

	// Otherwise, try to parse as ISO 8601: YYYY-MM-DDTHH:MM:SS.sssZ
	struct tm timeinfo = { 0 };
	int year, month, day, hour, minute, second;

	int count = _stscanf_s(isoTimestamp, _T("%d-%d-%dT%d:%d:%d"),
		&year, &month, &day, &hour, &minute, &second);

	if (count == 6)
	{
		timeinfo.tm_year = year - 1900;
		timeinfo.tm_mon = month - 1;
		timeinfo.tm_mday = day;
		timeinfo.tm_hour = hour;
		timeinfo.tm_min = minute;
		timeinfo.tm_sec = second;
		timeinfo.tm_isdst = -1;

		// Convert to Unix timestamp
		time_t timestamp = mktime(&timeinfo);

		// Note: This assumes local time. For UTC, we'd need _mkgmtime or adjust for timezone
		// For simplicity, using local time which matches server time in most cases
		return timestamp;
	}

	// If all parsing fails, return current time as fallback
	return time(NULL);
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
			// If the new request specifies a force range and the queued one
			// doesn't, upgrade the queued one in place so we still honor it.
			if (nForceDays > 0 && existing.nForceDays <= 0)
				existing.nForceDays = nForceDays;
			bAlreadyQueued = TRUE;
			break;
		}
	}
	if (!bAlreadyQueued)
		g_HttpWorkQueue.AddTail(item);
	LeaveCriticalSection(&g_HttpWorkQueueCS);

	if (g_hHttpWorkEvent)
		SetEvent(g_hHttpWorkEvent);
}

// Worker thread: drains the queue, fetches HTTP for each item, and updates
// the per-symbol cache. Posts WM_USER_STREAMING_UPDATE on success so the
// AmiBroker UI thread will re-read the cache through GetQuotesEx.
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

			// If the work item carries a forced range, set the globals that
			// GetOpenAlgoHistory consumes. Single-threaded worker, so no race.
			if (item.nForceDays > 0)
			{
				g_nBackfillDays = item.nForceDays;
				g_nBackfillPeriodicity = item.nPeriodicity;
				g_bBackfillRequested = TRUE;
			}

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

			int nResult = GetOpenAlgoHistory(item.ticker, item.nPeriodicity,
			                                 -1, TMP_SIZE, tmpBars);

			SymbolBarCache* pCache = GetOrCreateSymbolBarCache(item.ticker);
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

			if (nResult > 0 && g_hAmiBrokerWnd != NULL)
				PostMessage(g_hAmiBrokerWnd, WM_USER_STREAMING_UPDATE, 0, 0);
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
