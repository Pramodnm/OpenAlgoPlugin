# Build Guide

This guide explains how to build `OpenAlgo.dll` from source.

## Requirements

- Windows 10 or Windows 11
- Visual Studio 2022 Community or newer
- Workload: Desktop development with C++
- MFC component for the installed MSVC toolset
- Windows 10/11 SDK
- AmiBroker 64-bit for runtime testing
- A running OpenAlgo server for live testing

The AmiBroker ADK headers are included in this repository under `ADK`.

## Repository Layout

```text
OpenAlgoPlugin/
  OpenAlgoPlugin.sln
  OpenAlgoPlugin.vcxproj
  Plugin.cpp
  OpenAlgoConfigDlg.cpp
  OpenAlgo.rc
  ADK/
  docs/
```

## Build From Command Line

From the project directory:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' OpenAlgoPlugin.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Expected output:

```text
Release\OpenAlgo.dll
Release\OpenAlgo.pdb
Release\OpenAlgo.lib
```

Use `Release|x64` for normal AmiBroker testing.

## Build From Visual Studio

1. Open `OpenAlgoPlugin.sln`.
2. Select `Release`.
3. Select `x64`.
4. Build the solution.
5. Confirm `Release\OpenAlgo.dll` was produced.

## Install Into AmiBroker

1. Close AmiBroker.
2. Copy `Release\OpenAlgo.dll` into the AmiBroker `Plugins` directory.
3. Start AmiBroker.
4. Create or open a database using the OpenAlgo data plugin.
5. Configure server, port, API key, WebSocket URL, and backfill refresh.

Typical AmiBroker plugin directory:

```text
C:\Program Files\AmiBroker\Plugins
```

Use the path matching your local AmiBroker installation.

## Configuration After Install

Open:

```text
File -> Database Settings -> Configure
```

Set:

- Server: OpenAlgo HTTP host, usually `127.0.0.1`
- Port: usually `5000`
- API Key: OpenAlgo app API key
- Backfill Refresh (sec): default `30`
- Time Shift: normally `0`
- WebSocket URL: usually `ws://127.0.0.1:8765`

Run both tests:

- Test Connection
- Test WebSocket

## Debug Build

Debug builds are useful with Visual Studio or DebugView.

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' OpenAlgoPlugin.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

Install the generated debug DLL into AmiBroker's plugin directory, then watch
`OutputDebugString` messages using DebugView or the Visual Studio debugger.

## Runtime Test Checklist

After installing a new build:

1. AmiBroker status area shows the plugin loaded.
2. Configure dialog can read the saved API key.
3. Test Connection succeeds.
4. Test WebSocket succeeds.
5. A symbol such as `RELIANCE-NSE` loads historical data.
6. Realtime candles update during market hours.
7. Realtime Quote Window updates from WebSocket.
8. Manual 3-month backfill updates the active chart after completion.
9. Time & Sales is expected to remain non-working in this version.

## Common Build Issues

### MFC headers not found

Install the MFC component for your Visual Studio C++ toolset.

### Wrong output location

For `Release|x64`, the expected output is:

```text
Release\OpenAlgo.dll
```

not `x64\Release`.

### AmiBroker does not load the DLL

Check:

- DLL architecture matches AmiBroker architecture, normally x64.
- DLL is copied to the correct `Plugins` directory.
- Required Visual C++ runtime is installed.
- AmiBroker was restarted after copying the DLL.

### Old behavior still appears

Close AmiBroker before copying the DLL. Windows can keep the old plugin loaded
while AmiBroker is running.
