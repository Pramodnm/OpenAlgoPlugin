// OpenAlgoGlobals.h - Global variables and functions shared across the plugin
#ifndef OPENALGO_GLOBALS_H
#define OPENALGO_GLOBALS_H

#include "stdafx.h"
#include "resource.h"  // Include resource definitions

// Status enum - MUST be defined before any usage
enum OpenAlgoStatus
{
	STATUS_WAIT = 0,
	STATUS_CONNECTED = 1,
	STATUS_DISCONNECTED = 2,
	STATUS_SHUTDOWN = 3
};

// Global variables - declare as extern here
extern HWND g_hAmiBrokerWnd;
extern int g_nPortNumber;
extern int g_nRefreshInterval;
extern int g_nTimeShift;
extern CString g_oServer;
extern CString g_oApiKey;  // API Key for authentication
extern CString g_oWebSocketUrl;  // WebSocket URL for real-time data
extern int g_nStatus;

// Backfill request tracking
extern int g_nBackfillDays;
extern int g_nBackfillPeriodicity;
extern BOOL g_bBackfillRequested;

// Real-time candle building settings
extern BOOL g_bRealTimeCandlesEnabled;
extern int g_nBackfillIntervalMs;

// HTTP response caching (performance optimization)
// Cache HTTP responses to avoid calling HTTP API on every GetQuotesEx() call
extern CMapStringToPtr g_HttpResponseCache;  // Maps "SYMBOL-PERIODICITY" → last HTTP response time (DWORD*)
extern CRITICAL_SECTION g_HttpCacheCriticalSection;
extern const DWORD HTTP_CACHE_LIFETIME_MS;  // How long to cache HTTP responses (default: 60000ms = 60 seconds)

// Global function declarations
CString GetAvailableSymbols(void);
CString BuildOpenAlgoURL(const CString& server, int port, const CString& endpoint);
// AddToOpenAlgoPortfolio is internal to Plugin.cpp, not needed here

// Direct-registry API-key persistence (bypasses MFC's WriteProfileString,
// which had been failing silently for this specific value).
// Both functions target HKCU\Software\OpenAlgo\OpenAlgo\OpenAlgo\ApiKey,
// the same path MFC would use, so anything written here is also visible to
// any existing MFC read path.
BOOL WriteApiKeyDirect(const CString& key);
BOOL ReadApiKeyDirect(CString& outKey);

#endif // OPENALGO_GLOBALS_H