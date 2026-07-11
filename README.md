# OpenAlgo AmiBroker Data Plugin

OpenAlgo AmiBroker Data Plugin connects AmiBroker to a running OpenAlgo server.
It provides historical market data through the OpenAlgo REST history API and
realtime chart/quote updates through OpenAlgo WebSocket streams.

## Current Status

Working:

- Historical 1-minute and daily charts through `/api/v1/history`
- Manual historical backfill from AmiBroker plugin status menu
- Automatic intraday history refresh using configurable backfill cadence
- Realtime chart candles from WebSocket LTP/trade ticks
- Realtime Quote Window from WebSocket quote/depth frames
- WebSocket reconnect, ping/pong, and resubscription
- Active chart refresh after matching backfill completes

Known issue:

- Time & Sales is not working reliably in this version. It is documented as a
  known limitation and will be fixed in a later version.

## Documentation

See the rewritten documentation in [docs/README.md](docs/README.md).

Main docs:

- [User Guide](docs/USER_GUIDE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Technical Documentation](docs/TECHNICAL_DOCUMENTATION.md)
- [Build Guide](docs/BUILD_GUIDE.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Known Limitations](docs/KNOWN_LIMITATIONS.md)
- [Release Notes](docs/RELEASE_NOTES.md)

OpenAlgo API reference material remains under [docs/api](docs/api/README.md).

## Configuration

Configure the plugin from AmiBroker:

```text
File -> Database Settings -> Configure
```

Fields:

| Field | Purpose | Default |
| --- | --- | --- |
| Server | OpenAlgo HTTP host | `127.0.0.1` |
| Port | OpenAlgo HTTP port | `5000` |
| API Key | OpenAlgo app API key | Required |
| Backfill Refresh (sec) | Automatic 1-minute history refresh cadence | `30` |
| Time Shift (hours) | AmiBroker time adjustment | `0` |
| WebSocket URL | OpenAlgo WebSocket endpoint | `ws://127.0.0.1:8765` |

The old user-facing Refresh Interval field has been repurposed. It now controls
intraday backfill refresh. Connection/status heartbeat is fixed internally at
30 seconds.

## Data Sources

Historical data:

```text
POST /api/v1/history
```

Streaming data:

```text
WebSocket mode 1: LTP/trade ticks
WebSocket mode 2: Quote fields
WebSocket mode 3: Depth/top-of-book
```

Streaming windows intentionally do not use `/api/v1/quotes` as a fallback.

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

## Build

From the project directory:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' OpenAlgoPlugin.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Output:

```text
Release\OpenAlgo.dll
```

See [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md) for detailed build and install
steps.

## Disclaimer

This plugin is for education, research, and analysis. Market data comes from the
connected OpenAlgo broker feed. The maintainers do not guarantee accuracy,
completeness, timeliness, or suitability for trading decisions. Verify data from
official broker/exchange sources before using it.
