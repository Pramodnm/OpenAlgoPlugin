# Technical Documentation

This document describes the current implementation details of the OpenAlgo
AmiBroker plugin.

## Main Files

| File | Purpose |
| --- | --- |
| `Plugin.cpp` | AmiBroker exports, HTTP history, WebSocket streaming, caches |
| `OpenAlgoConfigDlg.cpp` | Configuration dialog and connection tests |
| `OpenAlgoGlobals.h` | Shared global settings |
| `OpenAlgo.rc` | Dialog resources |
| `OpenAlgoPlugin.vcxproj` | Visual Studio project |
| `ADK/Include/Plugin.h` | AmiBroker plugin API reference |

## Supported Data Interfaces

### Historical Bars

Endpoint:

```http
POST /api/v1/history
```

Request fields:

```json
{
  "apikey": "...",
  "symbol": "RELIANCE",
  "exchange": "NSE",
  "interval": "1m",
  "start_date": "2026-02-26",
  "end_date": "2026-05-27"
}
```

Supported intervals in this plugin:

- `1m` for 1-minute AmiBroker periodicity
- `D` for daily AmiBroker periodicity

The parser accepts both numeric epoch timestamps and documented timestamp
strings such as `2025-04-01 09:15:00+05:30`.

### WebSocket Streaming

The plugin subscribes to three OpenAlgo WebSocket modes per active symbol:

| Mode | Use |
| --- | --- |
| `1` | LTP/trade stream for realtime chart candles |
| `2` | Quote fields for Realtime Quote Window |
| `3` | Depth/top-of-book and quote fields |

The subscribe message shape is:

```json
{"action":"subscribe","symbol":"RELIANCE","exchange":"NSE","mode":1}
```

The plugin sends mode 1, 2, and 3 subscriptions for each symbol.

## Symbol Parsing

AmiBroker symbols should use:

```text
SYMBOL-EXCHANGE
```

Examples:

```text
RELIANCE-NSE
INFY-NSE
CRUDEOIL18JUN26FUT-MCX
NIFTY28MAY26FUT-NFO
```

`GetCleanSymbol` removes the exchange suffix. `GetExchangeFromTicker` reads the
suffix and defaults to `NSE` if no suffix is present.

## Configuration Storage

The plugin reads and writes settings under the OpenAlgo profile section.

Important settings:

| Setting | Meaning |
| --- | --- |
| `Server` | OpenAlgo HTTP host |
| `Port` | OpenAlgo HTTP port |
| `WebSocketUrl` | WebSocket endpoint |
| `BackfillRefreshIntervalSec` | 1-minute history refresh cadence |
| `BackfillIntervalMs` | Legacy mirror of the same interval |
| `RefreshInterval` | Legacy heartbeat setting, now internally fixed to 30 |
| `TimeShift` | AmiBroker time shift |
| `EnableRealTimeCandles` | Enables realtime candle overlay |

The API key is persisted through direct helper functions because MFC profile
storage was unreliable on the target machine. The helper writes a local settings
file and registry value.

## Threading Model

### AmiBroker/UI Thread

Runs:

- `GetQuotesEx`
- `GetRecentInfo`
- `Notify`
- `Configure`
- `GetStatus`

This thread must not perform slow HTTP history fetches.

### HTTP Worker Thread

Runs:

- `HttpWorkerThreadProc`
- `GetOpenAlgoHistory`

The worker is sequential. It removes one item from the queue, fetches history,
updates the relevant cache, notifies AmiBroker, then moves to the next item.

### WebSocket Reader Thread

Runs:

- `WsReaderThreadProc`
- `ProcessWebSocketData`

It blocks in `select`/`recv` and is independent of AmiBroker chart refresh
timing.

## Cache and Refresh Logic

### Intraday Cache

The 1-minute cache is considered stale when:

```text
now - lastOneMinFetch > BackfillRefreshIntervalSec
```

Default: 30 seconds.

When stale, `GetQuotesEx` queues a 1-minute worker fetch and immediately returns
the current cache to AmiBroker.

### Daily Cache

Daily cache stale window:

```text
1 hour
```

### Routine Backfill Range

For automatic refreshes, the worker seeds the temporary array with existing
cached bars before calling `GetOpenAlgoHistory`. This lets the gap-detection
logic request from the latest cached date to today.

For forced menu backfills, the worker does not seed from cache. It directly
fetches the requested range:

| Menu item | Force days |
| --- | --- |
| 3 Months | 90 |
| 6 Months | 180 |
| 1 Year | 365 |
| Daily 5 Years | 1825 |
| Daily 10 Years | 3650 |
| Daily 25 Years | 9125 |

## Data Merge Rules

Historical rows are parsed into AmiBroker `Quotation` structures.

Daily bars:

- Marked with `DAILY_MASK`
- Use AmiBroker EOD markers `Hour=31`, `Minute=63`

1-minute bars:

- Seconds, milliseconds, and microseconds are normalized to zero
- Duplicates are detected by year, month, day, hour, and minute
- Duplicate rows update the existing bar

Mixed EOD/intraday:

- For 1-minute charts, daily bars are copied only for dates before the first
  available 1-minute date
- AmiBroker can then compress intraday data into daily bars for the overlap
  window

Realtime candle overlay:

- Completed BarBuilder bars are appended only if newer than the last cached bar
- Current in-progress bar replaces the last bar if it has the same minute

## Active Chart Refresh

After a successful history fetch, the plugin sends:

- `WM_USER_STREAMING_UPDATE`
- `RI_STATUS_BARSREADY`

If the fetched symbol matches the active chart symbol, it also calls:

```text
Broker.Application.RefreshAll()
```

The active symbol is read from `Broker.Application.ActiveDocument.Name`. If that
is unavailable, the plugin falls back to the last symbol requested by
`GetQuotesEx`.

## Realtime Quote Window

`GetRecentInfo` creates a persistent `RecentInfo` for the requested symbol and
ensures a WebSocket subscription exists. WebSocket quote/depth frames update:

- last price
- open/high/low/close fields where available
- volume
- open interest
- bid/ask and bid/ask sizes
- update/change timestamps

This path does not use `/api/v1/quotes`.

## Time & Sales Known Issue

The implementation sets AmiBroker event flags such as:

- `RI_STATUS_TRADE`
- `RI_STATUS_BIDASK`
- `RI_STATUS_NEW_BID`
- `RI_STATUS_NEW_ASK`

However, the AmiBroker Time & Sales window remains empty in current testing.
This is documented as a known issue. Future work should focus on AmiBroker's
expected `RecentInfo` event semantics and message timing. Do not solve this by
polling REST quote APIs.

## Logging

The plugin writes diagnostics with `OutputDebugString`. Use Sysinternals
DebugView or Visual Studio debugger output to inspect:

- initialization
- API key load/save state
- history request ranges
- queued worker jobs
- WebSocket connection/authentication
- subscription attempts
- rejected timestamps
- active chart refresh results
