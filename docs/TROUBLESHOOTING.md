# Troubleshooting

This document covers current known runtime issues and diagnostic steps.

## Time & Sales Window Is Empty

Status: known issue.

The current version does not reliably populate AmiBroker's Time & Sales window.
The plugin subscribes to WebSocket LTP/quote/depth streams and sets AmiBroker
`RecentInfo` event flags, but the Time & Sales window remains empty in current
testing.

Do not add REST quote API polling as a workaround. Time & Sales must remain
WebSocket-only and should be fixed in the next implementation pass.

## Historical Backfill Loads But Chart Does Not Update

Expected current behavior:

- After a successful fetch, the plugin sends `WM_USER_STREAMING_UPDATE`.
- It marks the symbol as `RI_STATUS_BARSREADY`.
- If the completed symbol is the active chart symbol, it calls
  `Broker.Application.RefreshAll()`.

If the active chart still does not refresh:

1. Confirm you are running the latest `OpenAlgo.dll`.
2. Close AmiBroker before replacing the DLL.
3. Use DebugView and look for:
   - `OpenAlgo: Worker fetched ... bars`
   - `OpenAlgo: active chart symbol matched ... for refresh`
   - `OpenAlgo: AmiBroker RefreshAll result hr=...`
4. Switch symbols once to confirm the cached data is present.
5. If switching symbols reveals the data, the issue is chart invalidation, not
   history download.

## Manual Backfill Does Not Start

Use the plugin status menu in AmiBroker status bar.

Available actions:

- Backfill 1-Minute Data
  - 3 Months Current/All
  - 6 Months Current/All
  - 1 Year Current/All
- Backfill Daily Data
  - 5 Years Current/All
  - 10 Years Current/All
  - 25 Years Current/All

Diagnostics:

1. Open DebugView.
2. Select a menu item.
3. Confirm logs show:
   - `manual backfill queued`
   - `queued history fetch`
   - `history request`
   - `Worker fetched`

If "Current Symbol" does nothing, confirm the active chart symbol is in
`SYMBOL-EXCHANGE` format.

## Realtime Candles Are Misaligned

Realtime candles are built only from trade/LTP frames. Quote and depth frames
are ignored for candle construction because they may repeat stale LTP with a
newer book timestamp.

If alignment is still wrong:

1. Confirm the OpenAlgo WebSocket sends valid timestamps.
2. Look for `rejected WS timestamp` in DebugView.
3. Confirm system clock and exchange timestamp are not far apart.
4. Confirm the symbol is subscribed to WebSocket mode 1.
5. Wait for the next history refresh; historical backfill should overwrite bad
   partial data.

## Realtime Quote Window Is Blank

The Realtime Quote Window uses WebSocket only.

Check:

1. Test WebSocket succeeds in the configuration dialog.
2. The symbol exists in the Realtime Quote Window.
3. `GetRecentInfo(symbol)` appears in DebugView.
4. `SubscribeToSymbol` logs mode 1, 2, and 3 send results.
5. The OpenAlgo server is sending quote or depth frames.

The plugin intentionally does not call `/api/v1/quotes` as a fallback.

## WebSocket Reconnects Frequently

Check:

- WebSocket URL is correct, for example `ws://127.0.0.1:8765`
- OpenAlgo server is running
- API key is valid
- Firewall allows the WebSocket port
- Only one reader thread owns `recv`

The plugin sends ping frames every 30 seconds and retries reconnect every 5
seconds after disconnect.

## Historical Data Is Missing

Check the history request in DebugView:

```text
OpenAlgo: history request symbol=... exchange=... interval=... start=... end=...
```

Confirm:

- Symbol and exchange are correct.
- Interval is `1m` or `D`.
- OpenAlgo `/api/v1/history` supports the requested symbol.
- The broker has data for the date range.

## Backfill Refresh Setting Does Not Affect WebSocket Speed

This is expected.

`Backfill Refresh (sec)` controls automatic 1-minute historical backfill
cadence. WebSocket tick speed is controlled by the broker/OpenAlgo stream and
is not throttled by this setting.

The connection/status heartbeat is fixed internally at 30 seconds.

## Useful Debug Messages

Search DebugView for:

```text
OpenAlgo: Init
OpenAlgo: history request
OpenAlgo: queued history fetch
OpenAlgo: Worker fetched
OpenAlgo: WS update
OpenAlgo: SubscribeToSymbol
OpenAlgo: AuthenticateWebSocket
OpenAlgo: rejected WS timestamp
OpenAlgo: AmiBroker RefreshAll result
```
