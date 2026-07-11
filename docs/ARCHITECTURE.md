# Architecture

This document describes how the OpenAlgo AmiBroker plugin is structured and how
data moves from OpenAlgo into AmiBroker.

## High-Level Design

```text
AmiBroker
  |
  | Plugin API
  |   GetQuotesEx      -> chart/history arrays
  |   GetRecentInfo    -> realtime quote window
  |   Notify           -> database/menu/status notifications
  |
OpenAlgo.dll
  |
  |-- HTTP worker thread
  |     POST /api/v1/history
  |     fills per-symbol bar cache
  |
  |-- WebSocket reader thread
  |     authenticate
  |     subscribe mode 1, 2, 3
  |     updates RecentInfo and live candle builders
  |
  |-- Main/UI thread entry points
        serve cached bars to AmiBroker
        queue history fetches
        open configuration dialog
```

## Core Components

### AmiBroker Plugin API

The primary exported functions are implemented in `Plugin.cpp`.

| Function | Responsibility |
| --- | --- |
| `GetPluginInfo` | Supplies plugin metadata to AmiBroker |
| `Init` | Loads settings, starts worker threads, initializes WebSocket |
| `Release` | Stops threads and cleans up plugin state |
| `GetStatus` | Reports plugin state to AmiBroker status area |
| `Configure` | Opens the OpenAlgo configuration dialog |
| `Notify` | Handles database load/unload and status-menu backfill actions |
| `GetQuotesEx` | Supplies chart bars from the in-memory cache |
| `GetRecentInfo` | Supplies realtime quote window data |

### HTTP Worker

The HTTP worker is the only code path that calls the history REST API.

Responsibilities:

- Drain `g_HttpWorkQueue`
- Call `GetOpenAlgoHistory`
- Store bars in `g_SymbolBarCache`
- Clear in-progress flags
- Notify AmiBroker that bars are ready
- Refresh the active chart if the completed symbol is active

The worker is intentionally single-threaded. If five symbols are queued for
backfill, they are processed sequentially. This avoids burst load on the
OpenAlgo server and keeps cache mutation simple.

### WebSocket Reader

The WebSocket reader runs independently of AmiBroker chart refreshes.

Responsibilities:

- Connect to the configured WebSocket URL
- Authenticate with the OpenAlgo API key
- Send WebSocket ping frames every 30 seconds
- Reconnect every 5 seconds after a disconnect
- Resubscribe previously active symbols after reconnect
- Parse incoming market data frames
- Update quote window state
- Build realtime 1-minute candles from trade/LTP frames

### Caches

`SymbolBarCache` stores historical bars by ticker:

- `oneMinBars`: 1-minute bars
- `dailyBars`: daily bars
- `lastOneMinFetch`: last successful intraday fetch timestamp
- `lastDailyFetch`: last successful daily fetch timestamp
- in-progress flags to prevent duplicate worker jobs

`RecentInfo` objects are stored per symbol and kept alive for the life of the
plugin. AmiBroker may keep pointers returned by `GetRecentInfo`, so these
objects must not be stack allocated.

`BarBuilder` stores the in-progress realtime candle for each active symbol.

## Historical Data Flow

```text
Chart requests bars
  |
GetQuotesEx(symbol, periodicity)
  |
Serve cached bars immediately if present
  |
If cache is stale, queue worker job
  |
HTTP worker calls /api/v1/history
  |
Worker writes cache
  |
Plugin sends WM_USER_STREAMING_UPDATE
  |
AmiBroker requests bars again when it needs them
```

The plugin does not block AmiBroker while downloading history. If a chart asks
for data and the cache is empty, the plugin returns the existing AmiBroker array
and waits for the worker to finish.

## Realtime Chart Flow

```text
WebSocket mode 1 LTP/trade tick
  |
ProcessWebSocketData
  |
Validate timestamp and trade frame
  |
ProcessTick
  |
Update current BarBuilder candle
  |
WM_USER_STREAMING_UPDATE
  |
GetQuotesEx overlays current realtime candle on cached history
```

Only trade/LTP frames are used for realtime candle construction. Quote and depth
frames can carry a stale last-traded price with a newer book timestamp, so they
are not used to build candles. This avoids false future candles and candle
alignment problems.

## Realtime Quote Window Flow

```text
GetRecentInfo(symbol)
  |
Create or return persistent RecentInfo
  |
Ensure WebSocket subscription
  |
WebSocket mode 2/3 frames update fields in-place
  |
AmiBroker refreshes quote window from RecentInfo
```

The Realtime Quote Window is WebSocket-only. The plugin does not call
`/api/v1/quotes` as a fallback.

## Time & Sales Flow

The intended flow is:

```text
WebSocket trade/bid/ask frame
  |
Update RecentInfo
  |
Set RI_STATUS_TRADE / RI_STATUS_NEW_BID / RI_STATUS_NEW_ASK
  |
Send WM_USER_STREAMING_UPDATE
  |
AmiBroker Time & Sales consumes event
```

This path is not working reliably in current testing. The Time & Sales window is
a known issue for the next version.

## Backfill Modes

Manual menu backfill:

- `3 Months`, `6 Months`, `1 Year` for 1-minute bars
- `5 Years`, `10 Years`, `25 Years` for daily bars
- Current symbol or all loaded/subscribed symbols

Automatic intraday refresh:

- Controlled by `Backfill Refresh (sec)`
- Default is 30 seconds
- Uses the existing cache as seed, so routine refreshes request from the latest
  cached date to today

Daily automatic refresh:

- Uses a one-hour stale window
- Daily menu backfill can still force larger ranges

## Chart Refresh Strategy

The plugin uses AmiBroker's data-plugin notification:

- `WM_USER_STREAMING_UPDATE`
- `RI_STATUS_BARSREADY`

For active-chart historical refresh, the plugin also calls AmiBroker automation
`Broker.Application.RefreshAll()` when the completed backfill symbol matches the
active chart symbol. This is intentionally gated so background symbol refreshes
do not repaint all AmiBroker charts every few seconds.
