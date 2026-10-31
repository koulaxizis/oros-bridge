# orOS Bridge

orOS Studio runs in the browser, and a browser cannot load the VST3 or CLAP
plugins installed on your computer. orOS Bridge is a small program you run on
the computer: Studio talks to it over `127.0.0.1` (never the network), and it
runs your plugins for Studio.

Free, MIT, no account. It also records through your sound card (below).
For **Windows** and **Linux** (since 0.7.0 with its own window, plugin
windows, and .deb, .rpm and AppImage downloads). Plugins render ahead of
time ("freeze"): Studio sends a track through the plugin and gets the sound
back, as fast as the plugin goes, so nothing crackles on a busy computer.

## Use it

1. Download [`oros-bridge-windows-x64.exe`](https://github.com/koulaxizis/oros-bridge/releases/latest/download/oros-bridge-windows-x64.exe)
   from the [latest release](https://github.com/koulaxizis/oros-bridge/releases/latest)
   and run it; it needs no setup. Its window shows whether Studio is
   connected and which plugins it found. Closing or minimising the window
   keeps the bridge running in the tray (by the clock); right-click the
   icon for Open, Start with Windows and Quit.
   It is not signed yet, so Windows SmartScreen may ask once
   (More info → Run anyway).
   On **Linux**, install [`oros-bridge-linux-x64.deb`](https://github.com/koulaxizis/oros-bridge/releases/latest/download/oros-bridge-linux-x64.deb)
   (Debian, Ubuntu, Mint) or [`oros-bridge-linux-x64.rpm`](https://github.com/koulaxizis/oros-bridge/releases/latest/download/oros-bridge-linux-x64.rpm)
   (Fedora, openSUSE) and open orOS Bridge from the applications menu, or
   download [`oros-bridge-linux-x64.AppImage`](https://github.com/koulaxizis/oros-bridge/releases/latest/download/oros-bridge-linux-x64.AppImage),
   make it executable and run it. The window is the same as on Windows; the
   tray icon needs AppIndicator support (built into Ubuntu, KDE and most
   desktops; on plain GNOME the AppIndicator extension), and without it
   closing the window minimises it. Plugin windows open on X11 (or XWayland
   under Wayland). Without a desktop, or with `--console`, the bridge runs in
   the terminal.
2. In Studio, open a track's settings and press `Plugin on this computer`. The bridge window shows a 6-digit code;
   type it in Studio once. Studio keeps a key, so next time it just connects.
3. Studio lists your plugins (from `C:\Program Files\Common Files\VST3`,
   `...\CLAP` and the same folders under your user; on Linux `~/.vst3`,
   `~/.clap`, `/usr/lib/vst3`, `/usr/lib/clap` and the same under `/usr/local`). Pick one for a track,
   open its window to set it up, then freeze the track.

### Record through your sound card (0.3.0)

In Studio › Settings › Sound card, choose `orOS Bridge` and your card:
Windows Audio (WASAPI, with exclusive mode for the lowest delay), JACK,
PipeWire/PulseAudio or ALSA. Studio then records through the bridge: every
input you pick goes to its own track, in step with what you hear, even when
the computer is busy. Listen through your card's own monitoring (direct
monitor).

### ASIO® (0.4.0, Windows)

<img src="asio/logo/asio-compatible-bw.svg" alt="ASIO Compatible" height="72">

If your card has an ASIO driver, put `oros-bridge-asio.dll` next to
`oros-bridge.exe` (the Windows zip has both) and choose `ASIO` in Studio. One
card both records and plays, at the buffer size set in the driver's own
control panel, so pick the same card for input and output. Only one program
page at a time can use ASIO. `oros-bridge-asio.dll` is a separate program
under the GPLv3 (its source and licence are in [asio/](asio/)), built with
the Steinberg ASIO SDK under the SDK's GPLv3 licence; without it the bridge is
MIT and offers everything else.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

### Hear yourself through a plugin (0.5.0)

In Studio › Settings › Sound card › Listen through a plugin, choose an effect
(an amp for a guitar, a reverb for a voice) and the inputs it takes. The
bridge runs that plugin in the sound card's own callback, so you hear the
inputs through it with only the card's buffers and the plugin's own latency in
between. A take records the inputs dry (the plugin stays on the track, freeze
it when you like the sound), or with the plugin's sound if you choose so. Use
headphones: a microphone near speakers howls. A plugin that cannot keep up
with the card turns itself off and Studio says so; nothing past full scale
reaches the card.

### Play an instrument plugin from a MIDI keyboard (0.6.0)

Choose an instrument in `Listen through a plugin` and the keyboard it plays
from: the bridge reads the keyboard itself (Windows MIDI, or ALSA's MIDI
devices on Linux) and the notes reach the plugin in the sound card's
callback. A take keeps the notes you played, and Studio puts them on the
track.

The window's `Forget paired browsers` (or `oros-bridge --forget-pairings`)
forgets every browser you paired. `oros-bridge --scan` lists the plugins it
finds, `oros-bridge --console` runs it in the console without a window, and
`oros-bridge --code-file PATH` keeps the open pairing code in a file for a
system that shows it itself (orOS USB does), and
`oros-bridge --tray` starts it hidden in the tray (`Start with Windows`, or
`Start when I log in` on Linux, uses it).

### New versions (0.8.0)

A minute after it starts, and then once a day, the bridge asks GitHub for
its latest release. When a newer one is out, the window shows it with an
`Update` button and the tray menu gets `Update to …`; nothing pops up.
`Update` downloads the new version, checks every file against the release's
`SHA256SUMS.txt`, puts it in place of this copy and starts the bridge again:

- Windows: the exe, and `oros-bridge-asio.dll` when it is beside it (the
  old ones stay as `.old` until the next start);
- Linux: the AppImage, or the `.deb` / `.rpm` through your package tool
  (your password is asked by `pkexec`), or the program from the tar.gz.

A copy in a folder you cannot write gets the release page instead. The tray
menu's `Tell me about new versions` turns the checks off; only the request
for the latest release goes out, nothing about you or your plugins.
`oros-bridge --check-update` says whether a newer version is out and
`oros-bridge --update` installs it from a terminal.

orOS Portable and the orOS installers carry the bridge and start it with
`--bundled` (no window, no console, the pairing code shown over orOS through
`--code-file`); such a copy, and any copy with an `oros-bridge.bundled` file
beside it, never updates itself: a new orOS brings the new bridge.

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

CMake 3.28+, a C++17 compiler, Node 22 for the tests. The VST 3 SDK, CLAP,
nlohmann/json, miniaudio and, on Windows, the ASIO SDK are fetched at
configure time (see THIRD_PARTY.md). `-DOROS_BRIDGE_ASIO=OFF` leaves out the
GPLv3 ASIO module. How it works:
[DESIGN.md](DESIGN.md).
