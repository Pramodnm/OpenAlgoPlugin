// Plugin.cpp - AmiBroker exports, shared plugin state, and modular includes
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
#define CONNECTION_HEARTBEAT_INTERVAL_SEC 30

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
int g_nRefreshInterval = CONNECTION_HEARTBEAT_INTERVAL_SEC;
int g_nBackfillRefreshIntervalSec = 30;
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

// Send-side mutex. WS send() must be serialized across the UI thread
// (SubscribeToSymbol / UnsubscribeFromSymbol via SendWebSocketFrame) and the
// WS reader thread (PING / PONG). Without this, two simultaneous send()s
// interleave bytes on the wire, the server gets a malformed frame, and
// closes the connection with code 1006 (abnormal closure, no close frame).
// That was the cause of the ~20s reconnect cycle visible in the OpenAlgo
// server log.
static CRITICAL_SECTION g_WebSocketSendCS;
static BOOL g_bWebSocketSendCSInit = FALSE;

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

// Per-symbol persistent RecentInfo for the Realtime Quote Window + Time & Sales.
// AmiBroker may hold the pointer returned by GetRecentInfo across calls, and
// reads it on every quote-window refresh. Backing it with a per-symbol
// heap-allocated struct (rather than a single static) keeps every pointer
// stable for the lifetime of the plugin, and lets the WS reader thread
// update fields in-place while the UI thread is reading.
static CMap<CString, LPCTSTR, struct RecentInfo*, struct RecentInfo*> g_RecentInfoMap;
static CRITICAL_SECTION g_RecentInfoCS;
static BOOL g_bRecentInfoCSInit = FALSE;

typedef CArray< struct Quotation, struct Quotation > CQuoteArray;

//////////////////////////////////////////////////////////
// REAL-TIME CANDLE BUILDING STRUCTURES
//////////////////////////////////////////////////////////

// Real-time configuration (non-static so they can be accessed from OpenAlgoConfigDlg)
BOOL g_bRealTimeCandlesEnabled = TRUE;  // Default: enabled
int g_nBackfillIntervalMs = 30000;      // Legacy mirror of g_nBackfillRefreshIntervalSec

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
static CString g_LastRequestedTicker;

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
int GetOpenAlgoHistory(LPCTSTR pszTicker, int nPeriodicity, int nLastValid, int nSize, struct Quotation* pQuotes, int nForceDays);
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
BOOL TryParseWebSocketTimestamp(const CString& rawTimestamp, time_t& outTimestamp);

// Real-time candle building functions
BOOL ProcessTick(const CString& symbol, const CString& exchange, float ltp, float lastTradeQty, time_t timestamp);
BarBuilder* GetOrCreateBarBuilder(const CString& ticker);
void CleanupBarBuilders(void);

// Helper function for mixed EOD/Intraday data
int FindLastBarOfMatchingType(int nPeriodicity, int nLastValid, struct Quotation* pQuotes);

// Helper function to compare two quotations for sorting by timestamp
int CompareQuotations(const void* a, const void* b);

//////////////////////////////////////////////////////////
// Modular implementation sections
//
// These files are intentionally included into this translation unit.
// The refactor keeps existing static state, export names, and initialization
// order intact while splitting the implementation by responsibility.
//////////////////////////////////////////////////////////

#include "OpenAlgoUtilities.inc"
#include "OpenAlgoHistory.inc"
#include "OpenAlgoAmiBroker.inc"
#include "OpenAlgoWebSocket.inc"
#include "OpenAlgoRealtimeBars.inc"
#include "OpenAlgoWorkers.inc"
