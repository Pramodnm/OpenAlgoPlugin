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
int GetOpenAlgoHistory(LPCTSTR pszTicker, int nPeriodicity, int nLastValid, int nSize, struct Quotation* pQuotes, int nForceDays)
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

		// Forced menu backfill: fetch the full requested period directly.
		if (nForceDays > 0)
		{
			startTime = todayDate - CTimeSpan((LONG)nForceDays, 0, 0, 0);
			goto skip_gap_detection;
		}

		// Legacy deferred current-symbol request fallback.
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

		CString apiDebugMsg;
		apiDebugMsg.Format(_T("OpenAlgo: history request symbol=%s exchange=%s interval=%s start=%s end=%s forceDays=%d"),
			(LPCTSTR)symbol, (LPCTSTR)exchange, (LPCTSTR)interval,
			(LPCTSTR)startDate, (LPCTSTR)endDate, nForceDays);
		OutputDebugString(apiDebugMsg);

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

									// Parse timestamp. OpenAlgo history may return either
									// numeric epoch values or documented strings such as
									// "2025-04-01 09:15:00+05:30".
									int tsPos = candle.Find(_T("\"timestamp\":"));
									if (tsPos >= 0)
									{
										tsPos += 12;
										while (tsPos < candle.GetLength() && IsJsonWhitespace(candle[tsPos]))
											tsPos++;

										CString tsStr;
										if (tsPos < candle.GetLength() && candle[tsPos] == _T('"'))
										{
											tsPos++;
											int tsEnd = candle.Find(_T("\""), tsPos);
											if (tsEnd < 0)
											{
												pos = candleEnd + 1;
												continue;
											}
											tsStr = candle.Mid(tsPos, tsEnd - tsPos);
										}
										else
										{
											int tsEnd = candle.Find(_T(","), tsPos);
											if (tsEnd < 0) tsEnd = candle.Find(_T("}"), tsPos);
											if (tsEnd < 0)
											{
												pos = candleEnd + 1;
												continue;
											}
											tsStr = candle.Mid(tsPos, tsEnd - tsPos);
										}
										tsStr.Trim();

										time_t timestamp = 0;
										if (!TryParseWebSocketTimestamp(tsStr, timestamp))
										{
											pos = candleEnd + 1;
											continue;
										}

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

