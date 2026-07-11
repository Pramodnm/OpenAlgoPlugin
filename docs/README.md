# OpenAlgo AmiBroker Plugin Docs

This folder documents the current OpenAlgo AmiBroker data plugin implementation.

The plugin connects AmiBroker to a running OpenAlgo server. Historical bars are
loaded through the OpenAlgo REST history API. Realtime quote updates and realtime
candle construction use OpenAlgo WebSocket streams.

## Current Status

Working in this version:

- Historical 1-minute and daily data through `/api/v1/history`
- Manual backfill from the AmiBroker plugin status menu
- Automatic intraday refresh for the active chart
- Realtime chart updates from WebSocket ticks
- Realtime candle building from WebSocket LTP/trade frames
- Realtime Quote Window updates from WebSocket quote/depth frames
- WebSocket reconnect, authentication, ping/pong, and resubscription
- Active-chart refresh after manual and periodic backfill

Known issue:

- Time & Sales is not working reliably in this version. The code still attempts
  to publish trade/bid/ask events through AmiBroker `RecentInfo`, but the
  AmiBroker Time & Sales window remains empty in current testing. This is a
  known limitation and will be fixed in a later version. No REST quote API
  fallback should be added for Time & Sales; streaming must remain WebSocket
  only.

## Documentation Map

Start here:

- [User Guide](USER_GUIDE.md): installation, configuration, symbols, and daily use
- [Architecture](ARCHITECTURE.md): system design and data flows
- [Technical Documentation](TECHNICAL_DOCUMENTATION.md): implementation details
- [Build Guide](BUILD_GUIDE.md): build, package, and deploy from source
- [Troubleshooting](TROUBLESHOOTING.md): diagnostics for common issues
- [Known Limitations](KNOWN_LIMITATIONS.md): current gaps and non-goals
- [Release Notes](RELEASE_NOTES.md): current behavior and recent changes

Reference material kept as-is:

- `docs/api`: OpenAlgo REST API reference
- `docs/prompt`: OpenAlgo symbol, WebSocket, and indicator reference notes

## Supported AmiBroker Data Paths

| AmiBroker feature | Current plugin source | Status |
| --- | --- | --- |
| Chart historical bars | REST `/api/v1/history` | Working |
| Manual backfill | REST `/api/v1/history` | Working |
| Realtime chart current candle | WebSocket mode 1 LTP/trade ticks | Working |
| Realtime Quote Window | WebSocket mode 2 quote and mode 3 depth | Working |
| Time & Sales | WebSocket mode 1/mode 3 mapped to `RecentInfo` events | Known issue |

## Important Runtime Rules

- Streaming windows must use WebSocket data only. Do not use `/api/v1/quotes` as
  a fallback for realtime quote or Time & Sales updates.
- Historical backfill must use `/api/v1/history`.
- The plugin supports `1m` and `D` history intervals in the current AmiBroker
  integration.
- Symbol names should normally be in `SYMBOL-EXCHANGE` format, for example
  `RELIANCE-NSE`, `CRUDEOIL18JUN26FUT-MCX`, or `NIFTY28MAY26FUT-NFO`.
- Data quality depends on the connected OpenAlgo broker feed. Verify data before
  using it for decisions.

## Configuration Summary

| Setting | Purpose | Default |
| --- | --- | --- |
| Server | OpenAlgo HTTP server host | `127.0.0.1` |
| Port | OpenAlgo HTTP API port | `5000` |
| API Key | OpenAlgo app API key | Required |
| Backfill Refresh (sec) | Intraday history refresh cadence | `30` |
| Time Shift (hours) | AmiBroker time adjustment | `0` |
| WebSocket URL | OpenAlgo WebSocket endpoint | `ws://127.0.0.1:8765` |

The connection/status heartbeat is internal and fixed at 30 seconds. The UI
interval controls intraday backfill refresh, not WebSocket tick speed.
