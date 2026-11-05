# orOS Bridge

orOS Studio runs in the browser, and a browser cannot load the VST3 or CLAP
plugins installed on your computer. orOS Bridge is a small program you run on
the computer: Studio talks to it over `127.0.0.1` (never the network), and it
runs your plugins for Studio.

Free, MIT, no account. It also records through your sound card, and scans
into orOS Scan PDF from your scanner (below).
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
   If you installed orOS (or use orOS Portable), the bridge is already inside
   it and you need nothing else: starting the downloaded bridge then only says
   that one is already running (0.9.1).
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

### Scan into orOS Scan PDF (0.9.0)

With the bridge running, orOS Scan PDF shows `Add from scanner`: choose the
scanner, the glass or the feeder (both sides when it can), the resolution and
colour, and the pages land in Scan PDF to crop, read (text recognition) and
save. Studio and Scan PDF share the pairing, so a browser paired once is not
asked again. On **Windows** every scanner with a Windows driver works (WIA).
On **Linux** the bridge uses SANE: install `libsane1` (Debian, Ubuntu, Mint)
or `sane-backends-libs` (Fedora), and `sane-airscan` and `ipp-usb` for
network and USB multifunction printers that need no driver; the .deb and
.rpm recommend them and orOS USB has them. `oros-bridge --scanners` lists the
scanners it finds, and the window shows how many the last look found.

### Play on a TV or speaker on the network (0.10.0)

orOS Radio and Television can send a station or channel to a smart TV, Kodi,
a network speaker or a receiver on your home network (UPnP/DLNA): the bridge
looks for them when you open `Play on…` and sends the stream's address, play,
pause, stop and the volume; the TV fetches the stream itself. Windows may ask
once whether the bridge may use the private network. Chromecast is not
searched by the bridge (orOS uses the browser's own Cast menu for it). The
bridge can also open the system's Bluetooth settings, where a Bluetooth
speaker is paired.

### Folders orOS watches (0.11.0)

The bridge can tell orOS about new files in folders you choose, for orOS
Automations: a new PDF in your scanner's or downloads folder can go to Scan
PDF or Invoices, a new photo to Files, and a file can be printed. Add a folder
with `Add folder…` in the bridge's window, or from orOS (the system's folder
chooser opens on this computer; orOS cannot name a folder itself);
`Stop watching` takes it off, and `oros-bridge --watch DIR` adds one from a
terminal. The bridge looks every 2 seconds, only in the folder itself (not
the folders inside it), and reports a file once its size stops changing;
files that were there when you added the folder, hidden files and downloads in
progress are left out. Files added while the bridge was not running are
reported at its next start. Printing uses the file type's own print command on
Windows and CUPS (`lp`) on Linux. On Linux without the bridge's window (orOS
Portable, orOS USB) the chooser is zenity, kdialog or yad, if one is
installed.

## Safety

- It listens on `127.0.0.1` only, so other computers cannot reach it.
- Only pages from `https://useoros.online` may connect, and only after the
  code shown on this computer was typed in (three wrong codes end a code).
- Each plugin file is first opened in a separate process, so a plugin that
  crashes while being read is left out instead of stopping the bridge.
- Plugins are programs: the bridge runs the ones you installed, as you.
- A scanner scans only when a paired page asks; the pages go to that page
  only and the bridge keeps nothing.
- TVs on the network: the bridge looks for them only when a paired page
  asks, talks only to devices that answered at a home network address, sends
  only http(s) stream addresses, and serves nothing to the network.
- Watched folders: only you add a folder, here on this computer. A paired
  page hears about new files in those folders and can read or print only a
  file the bridge told it about, while it is still the same file directly in
  a watched folder; links and the folders below are not followed, and
  nothing else on the disk is reachable.

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
