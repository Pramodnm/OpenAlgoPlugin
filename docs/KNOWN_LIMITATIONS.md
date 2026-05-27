# Known Limitations

This document lists current limitations and intentional non-goals.

## Time & Sales

Time & Sales is not working reliably in this version.

The plugin attempts to publish events using AmiBroker `RecentInfo` status flags,
but the Time & Sales window remains empty in current testing. This is the main
known issue for the next version.

Constraint for the future fix:

- Use WebSocket streaming only.
- Do not add REST `/api/v1/quotes` polling as a workaround.

## Supported Intervals

The AmiBroker integration currently maps:

- 1-minute to OpenAlgo `1m`
- Daily to OpenAlgo `D`

Other OpenAlgo intervals may exist, but they are not exposed through the current
AmiBroker plugin path.

## Backfill Is Sequential

The HTTP worker processes one queued symbol at a time.

This is intentional to avoid hitting OpenAlgo/broker APIs with bursts. If "All
Symbols" queues many symbols, they will finish sequentially.

## Active Chart Refresh Uses RefreshAll

AmiBroker's plugin API provides `WM_USER_STREAMING_UPDATE`, but in practice a
large historical range expansion may not repaint the active chart immediately.

To solve this, the plugin calls `Broker.Application.RefreshAll()` only when the
completed history fetch belongs to the active chart symbol.

This may still refresh more than one visible chart inside AmiBroker, but it is
not called for every background symbol.

## No REST Quote Fallback For Streaming

The Realtime Quote Window and realtime chart streaming are WebSocket-only.

The plugin intentionally avoids `/api/v1/quotes` for streaming windows because
REST polling adds latency, rate-limit pressure, and inconsistent update timing.

## History API Data Quality

The plugin trusts OpenAlgo and the connected broker for historical data. It
normalizes timestamps and duplicate bars, but it cannot guarantee broker data
accuracy.

## Local Time Assumptions

The timestamp parser supports numeric epoch values, UTC timestamps, offset
timestamps such as `+05:30`, and local exchange datetime strings. If a broker
sends malformed timestamps, the plugin may reject them and fall back to local
time for realtime candles.

## Symbol Format

The recommended format is `SYMBOL-EXCHANGE`.

If no exchange suffix is present, the plugin defaults to `NSE`, which may be
wrong for futures, options, MCX, or BSE symbols.
