# User Guide

This guide explains how to use the OpenAlgo AmiBroker plugin.

## What The Plugin Does

The plugin makes OpenAlgo market data available inside AmiBroker.

It supports:

- Historical chart data
- Manual backfill from the plugin status menu
- Automatic intraday history refresh
- Realtime chart candles from WebSocket ticks
- Realtime Quote Window updates from WebSocket

Known issue:

- Time & Sales is not working in this version.

## Install

1. Build or download `OpenAlgo.dll`.
2. Close AmiBroker.
3. Copy `OpenAlgo.dll` to AmiBroker's `Plugins` folder.
4. Start AmiBroker.
5. Create or open an AmiBroker database using the OpenAlgo data plugin.

## Configure

Open:

```text
File -> Database Settings -> Configure
```

Set these fields:

| Field | Description |
| --- | --- |
| Server | OpenAlgo HTTP server host, for example `127.0.0.1` |
| Port | OpenAlgo HTTP port, usually `5000` |
| API Key | OpenAlgo app API key |
| Backfill Refresh (sec) | Automatic 1-minute history refresh interval |
| Time Shift (hours) | Time adjustment for AmiBroker |
| WebSocket URL | OpenAlgo WebSocket URL, for example `ws://127.0.0.1:8765` |

Click:

- Test Connection
- Test WebSocket

Then click OK.

## Symbol Format

Use:

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

If the exchange suffix is missing, the plugin defaults to `NSE`.

## Historical Data

Historical chart data is loaded from:

```text
POST /api/v1/history
```

Supported AmiBroker intervals in this plugin:

- 1-minute
- Daily

The plugin requests:

- `interval=1m` for 1-minute charts
- `interval=D` for daily charts

## Automatic Backfill

The `Backfill Refresh (sec)` field controls how often the plugin checks for
fresh 1-minute history.

Default:

```text
30 seconds
```

This is separate from WebSocket streaming. WebSocket ticks arrive independently
from the broker/OpenAlgo stream.

## Manual Backfill

Right-click the OpenAlgo plugin status area in AmiBroker.

Available 1-minute backfill options:

- 3 Months Current Symbol
- 3 Months All Symbols
- 6 Months Current Symbol
- 6 Months All Symbols
- 1 Year Current Symbol
- 1 Year All Symbols

Available daily backfill options:

- 5 Years Current Symbol
- 5 Years All Symbols
- 10 Years Current Symbol
- 10 Years All Symbols
- 25 Years Current Symbol
- 25 Years All Symbols

Current symbol means the active chart symbol. All symbols means symbols already
loaded in chart cache or subscribed through WebSocket.

## Realtime Charts

Realtime chart updates come from WebSocket mode 1 LTP/trade frames.

The plugin builds the in-progress 1-minute candle locally and overlays it on
top of the historical cache returned by `/api/v1/history`.

Historical backfill later overwrites/normalizes the completed candles.

## Realtime Quote Window

The Realtime Quote Window is updated from WebSocket quote/depth frames.

The plugin does not use `/api/v1/quotes` as a fallback because that endpoint is
not suitable for streaming updates.

## Time & Sales

Time & Sales is a known issue in this version.

The plugin currently subscribes to the required WebSocket streams and attempts
to publish trade/bid/ask events to AmiBroker, but the Time & Sales window is not
populating reliably. This is planned for a later version.

## Status Area

The AmiBroker plugin status area shows the connection state:

- WAIT: waiting for connection
- OK: connected
- ERR: disconnected
- DOWN: shutdown/disconnected by user

The internal connection heartbeat runs every 30 seconds.
