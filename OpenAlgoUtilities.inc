///////////////////////////////
// Helper Functions
///////////////////////////////

static BOOL IsJsonWhitespace(TCHAR ch)
{
	return ch == _T(' ') || ch == _T('\t') || ch == _T('\r') || ch == _T('\n');
}

static int ClampBackfillRefreshIntervalSec(int seconds)
{
	if (seconds < 5) return 5;
	if (seconds > 3600) return 3600;
	return seconds;
}

static DWORD GetOneMinCacheLifetimeMs(void)
{
	int seconds = ClampBackfillRefreshIntervalSec(g_nBackfillRefreshIntervalSec);
	return (DWORD)seconds * 1000;
}

// Finds the first character of a JSON value after "key":, allowing the
// default spaces emitted by Python/json.dumps.
static int FindJsonValueStart(const CString& json, LPCTSTR key, int startPos = 0)
{
	CString needle;
	needle.Format(_T("\"%s\""), key);

	int keyPos = json.Find(needle, startPos);
	while (keyPos >= 0)
	{
		int pos = keyPos + needle.GetLength();
		while (pos < json.GetLength() && IsJsonWhitespace(json[pos]))
			pos++;

		if (pos < json.GetLength() && json[pos] == _T(':'))
		{
			pos++;
			while (pos < json.GetLength() && IsJsonWhitespace(json[pos]))
				pos++;
			return pos;
		}

		keyPos = json.Find(needle, keyPos + needle.GetLength());
	}

	return -1;
}

static CString ExtractJsonStringValue(const CString& json, LPCTSTR key)
{
	CString value;
	int pos = FindJsonValueStart(json, key);
	if (pos < 0 || pos >= json.GetLength() || json[pos] != _T('"'))
		return value;

	pos++;
	int endPos = json.Find(_T("\""), pos);
	if (endPos > pos)
		value = json.Mid(pos, endPos - pos);

	return value;
}

static float ExtractJsonFloatValue(const CString& json, LPCTSTR key)
{
	int pos = FindJsonValueStart(json, key);
	if (pos < 0)
		return 0.0f;

	int endPos = pos;
	while (endPos < json.GetLength() &&
	       json[endPos] != _T(',') &&
	       json[endPos] != _T('}') &&
	       json[endPos] != _T(']'))
	{
		endPos++;
	}

	CString val = json.Mid(pos, endPos - pos);
	val.Trim();
	return (float)_tstof(val);
}

static int ExtractJsonIntValue(const CString& json, LPCTSTR key)
{
	int pos = FindJsonValueStart(json, key);
	if (pos < 0)
		return 0;

	int endPos = pos;
	while (endPos < json.GetLength() &&
	       json[endPos] != _T(',') &&
	       json[endPos] != _T('}') &&
	       json[endPos] != _T(']'))
	{
		endPos++;
	}

	CString val = json.Mid(pos, endPos - pos);
	val.Trim();
	return _ttoi(val);
}

// Extract top-of-book from a Mode 3 (Depth) WS frame. sideKey is "buy"
// or "sell" for protocol.md frames, and "bids" or "asks" for the REST-style
// websocket docs. We want the first {price, quantity} inside that array.
static BOOL ExtractFirstDepthLevel(const CString& json, LPCTSTR sideKey,
                                   float& outPrice, float& outQty)
{
	outPrice = 0.0f;
	outQty   = 0.0f;

	int pos = FindJsonValueStart(json, sideKey);
	if (pos < 0) return FALSE;

	while (pos < json.GetLength() && IsJsonWhitespace(json[pos]))
		pos++;
	if (pos >= json.GetLength() || json[pos] != _T('['))
		return FALSE;
	pos++;

	// Advance to the first '{' inside the array (skip whitespace)
	while (pos < json.GetLength() &&
	       json[pos] != _T('{') && json[pos] != _T(']'))
		pos++;
	if (pos >= json.GetLength() || json[pos] != _T('{')) return FALSE;

	int objEnd = json.Find(_T('}'), pos);
	if (objEnd <= pos) return FALSE;
	CString obj = json.Mid(pos, objEnd - pos + 1);

	int pp = obj.Find(_T("\"price\":"));
	if (pp >= 0)
	{
		pp += 8;
		int e = obj.Find(_T(','), pp);
		if (e < 0) e = obj.Find(_T('}'), pp);
		if (e < 0) e = obj.GetLength();
		outPrice = (float)_tstof(obj.Mid(pp, e - pp));
	}

	int qp = obj.Find(_T("\"quantity\":"));
	if (qp >= 0)
	{
		qp += 11;
		int e = obj.Find(_T(','), qp);
		if (e < 0) e = obj.Find(_T('}'), qp);
		if (e < 0) e = obj.GetLength();
		outQty = (float)_tstof(obj.Mid(qp, e - qp));
	}

	return (outPrice > 0.0f);
}

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

