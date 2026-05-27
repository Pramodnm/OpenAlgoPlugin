# Release Notes

## Current Development Build

Current behavior documented in this folder reflects the latest source state on
the `master` branch.

### Working

- Historical data through `/api/v1/history`
- Manual current-symbol and all-symbol backfill
- Configurable automatic 1-minute backfill refresh
- Realtime chart candles from WebSocket LTP/trade ticks
- Realtime Quote Window from WebSocket quote/depth frames
- Active chart refresh after matching backfill completes
- WebSocket reconnect and resubscription
- Timestamp parsing for numeric and timezone string history values
- Candle alignment fix by ignoring quote/depth frames for candle construction

### Known Issue

- Time & Sales window is not working reliably. This will be addressed in a later
  version.

### Recent Implementation Notes

- `Backfill Refresh (sec)` now controls the intraday historical refresh cadence.
- Connection/status heartbeat is fixed internally at 30 seconds.
- Manual backfill requests force the requested date range.
- Periodic refreshes seed from the existing cache, so routine fetches request
  from the latest cached date to today.
- Active-chart fetch completion triggers `RefreshAll()` only when the fetched
  symbol matches the active chart symbol.
