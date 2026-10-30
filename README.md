# orOS Bridge

orOS Studio runs in the browser, and a browser cannot load the VST3 or CLAP
plugins installed on your computer. orOS Bridge is a small program you run on
the computer: Studio talks to it over `127.0.0.1` (never the network), and it
runs your plugins for Studio.

Free, MIT, no account. First version: **Windows**, plugins render ahead of
time ("freeze"): Studio sends a track through the plugin and gets the sound
back, as fast as the plugin goes, so nothing crackles on a busy computer.

## Use it

1. Download [`oros-bridge-windows-x64.exe`](https://github.com/koulaxizis/oros-bridge/releases/latest/download/oros-bridge-windows-x64.exe)
   from the [latest release](https://github.com/koulaxizis/oros-bridge/releases/latest)
   and run it; it needs no setup. Keep its window open while you use Studio.
   It is not signed yet, so Windows SmartScreen may ask once
   (More info → Run anyway).
2. In Studio, open a track's settings and press `Plugin on this computer`. The bridge window shows a 6-digit code;
   type it in Studio once. Studio keeps a key, so next time it just connects.
3. Studio lists your plugins (from `C:\Program Files\Common Files\VST3`,
   `...\CLAP` and the same folders under your user). Pick one for a track,
   open its window to set it up, then freeze the track.

`oros-bridge --forget-pairings` forgets every browser you paired.
`oros-bridge --scan` lists the plugins it finds.

## Safety

- It listens on `127.0.0.1` only, so other computers cannot reach it.
- Only pages from `https://useoros.online` may connect, and only after the
  code shown on this computer was typed in (three wrong codes end a code).
- Each plugin file is first opened in a separate process, so a plugin that
  crashes while being read is left out instead of stopping the bridge.
- Plugins are programs: the bridge runs the ones you installed, as you.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

CMake 3.28+, a C++17 compiler, Node 22 for the tests. The VST 3 SDK, CLAP and
nlohmann/json are fetched at configure time (see THIRD_PARTY.md). How it works:
[DESIGN.md](DESIGN.md).
