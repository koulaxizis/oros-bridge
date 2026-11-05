# orOS Bridge: design

Wave 4 of "free sounds in Studio" (orOS Bible Part IX, 2026-10-30): let
Studio use the person's own VST3 and CLAP plugins. A browser cannot load
native code, so a separate program hosts them and Studio talks to it.

## Decisions

1. **Render first ("freeze").** Studio sends a track's audio (and notes, for
   an instrument) block by block and gets the processed audio back, as fast
   as the plugin goes. The study measured live round trips through a
   worklet, a Worker and a WebSocket: fine at rest, rare dropouts when the
   page is busy. A live mode (latency reported as plugin delay) can come
   later on the same protocol.
2. **127.0.0.1 only**, port 47621. A request must name `127.0.0.1:port` or
   `localhost:port` as its Host (no DNS rebinding) and come from a listed
   Origin (`https://useoros.online`, `https://www.useoros.online`, plus
   `--allow-origin` for development). `/status` answers allowed pages with
   CORS and `Access-Control-Allow-Private-Network: true`.
3. **Pairing.** A page asks (`pair`), the bridge shows a 6-digit code in its
   own window, the page sends it back and receives a 256-bit token; only a
   SHA-1 of it is stored (`config.json`, at most 16). Three wrong codes end
   the code; each wrong try waits 500 ms. After that the page sends `auth`.
4. **Every plugin call on one main thread** (`MainLoop`), which also runs the
   plugins' windows (Win32 message loop). Network threads hand work over and
   wait. Plugins are processed offline (`kOffline` for VST 3), so a plugin may
   use its best quality.
5. **Scanning in a child process** (`oros-bridge --describe <format> <path>`,
   30 s limit): a crash or hang leaves that file out. Results are kept with
   each file's modification time and only changed files are read again.
6. **No third-party code beyond MIT**: VST 3 SDK 3.8.1 (MIT since 3.8.0),
   CLAP 1.2.9, nlohmann/json 3.11.3. No JUCE, no VSTGUI.
7. **Windows first.** The code builds and its tests run on Linux too (CI
   runs both); Linux has plugin windows and its own window since 0.7.0
   (decision 13); macOS later.
8. **A window and a tray icon on Windows (0.2.0).** The exe is a window
   program (`WIN32_EXECUTABLE`, entry `mainCRTStartup`) instead of a
   console. The window (`src/gui_win32.cpp`) shows whether Studio is
   connected, the pairing code in large type, the plugins found and their
   folders, and buttons to look for plugins and to forget paired browsers.
   Closing or minimising hides it to the tray (a one-time hint the first
   time); the tray menu has Open, Start with Windows (HKCU `Run`, value
   `orOS Bridge` = `"<exe>" --tray`, off until the person turns it on) and
   Quit. When a page asks to pair, the window comes up (plus a tray
   notification if it was hidden). One bridge per user (mutex
   `Local\orOS-Bridge`): a second launch shows the first one's window and
   ends. Greek when the Windows display language is Greek, English
   otherwise. The process stays DPI-unaware so plugin editors do not change;
   only the bridge's own window is created system-DPI-aware. `--console`
   keeps the command-line program (the tests use it); `--scan`,
   `--forget-pairings` and `--help` write to the console they were started
   from. Linux has the same window since 0.7.0 (decision 13); other systems
   have none (`src/gui_none.cpp`).

9. **The sound card (0.3.0).** Studio in a browser hears and records
   through the system's shared mixer (WASAPI shared mode, PulseAudio): no
   exclusive mode, no JACK, one stereo input. The bridge can open the card
   itself (`src/audio.h`): WASAPI (shared or exclusive) on Windows; JACK,
   PipeWire/PulseAudio and ALSA on Linux, through miniaudio 0.11.25 (public
   domain or MIT-0), which loads each system library at run time, so a
   missing JACK only hides that backend. A take never streams in real time
   from the page: Studio renders the song from the playhead first and sends
   it ahead (`audioPlay` + `ORP1` blocks), then `audioStart` plays it from
   frame 0 and records the chosen inputs **in the same callback**, so a
   recorded frame and the frame played with it share the card's clock (no
   drift, no gaps from a busy page). The callback only copies into a ring;
   a writer thread empties it into a temporary file, read back with
   `takeRead` after `audioStop` and deleted on `audioClose` or when the page
   goes. `latency` is the bridge's estimate of the round trip (output buffer
   plus one input period); the card's own converters are not known, so
   Studio adds the person's recording offset.

10. **ASIO® as a separate GPLv3 module (0.4.0).** The ASIO SDK is GPLv3 or
   proprietary (since 2025-10), so it lives in its own library,
   `oros-bridge-asio.dll` (`asio/`, GPLv3, ASIO SDK 2.3.4 fetched at build
   time), and the bridge stays MIT. The whole boundary is the plain C header
   `src/audio_module.h` (MIT): one exported function returns a table with
   `devices`, `open` and `close`; the stream calls the bridge back with
   interleaved floats, exactly like the other backends. The bridge loads
   `oros-bridge-asio.dll` from beside the exe if it is there
   (`src/audio_module.cpp`); `--audio-module PATH` loads any other module
   (the tests). In the module, one worker thread (COM single-threaded
   apartment, a hidden window as the driver's `sysRef`) makes every driver
   call, because ASIO allows one driver per process and many drivers want
   their calls on the thread that loaded them; `bufferSwitch` converts the
   card's samples (16/24/32-bit integer, 32/64-bit float) to and from float
   and calls the bridge. ASIO has one driver for input and output, so the
   same card name is required for both, and the buffer size is the one in the
   driver's control panel. `latency` is the input plus output latency the
   driver reports. While a card is open, listing returns the last list
   instead of loading other drivers. Wherever ASIO is named (this repo's
   docs, the release notes, Studio's sound card settings, the download
   page), the ASIO Compatible Logo (`asio/logo/`) is shown, small (72 px high) and on its own line, with "ASIO is a
   registered trademark of Steinberg Media Technologies GmbH.", as the SDK's
   usage guidelines ask (decision 2026-10-31).

   <img src="asio/logo/asio-compatible-bw.svg" alt="ASIO Compatible" height="72">


11. **A plugin live on an input (0.5.0).** To hear yourself through an amp
   or a reverb while you record, the plugin has to run in the card's own
   callback: a round trip through the page would add its buffers and its
   dropouts (decision 1). `liveOpen` opens a **second instance** of the
   plugin, activated for real time (VST 3 `kRealtime`; CLAP starts and stops
   processing on the card's thread) at the card's rate, with the state the
   page gives (the track's plugin, so the live sound is the sound a freeze
   gives later). Its `process()` runs only on the card's thread; opening,
   state, its window and closing stay on the main thread (decision 4). The
   chosen inputs (1 or 2; one input feeds every plugin input) go through it
   and its sound is added to two outputs from `out` at `gain`, also while no
   take runs. Nothing on that path allocates or waits: the port layout and
   the CLAP event storage are made at activation, the VST 3 edit queue is only
   taken with `try_lock`. Safety: monitoring starts off; the output is clamped
   to full scale and non-finite samples become silence; a plugin later than
   the period 8 periods in a row turns itself off (`slow`), a failed
   `process()` too (`failed`), both shown in `levels`. A take records the
   inputs **dry** by default (the plugin stays on the track); `record: "wet"`
   puts the plugin's sound in the take's channels that are its inputs, not
   moved by its latency (the page gets `latency` and moves them). One live
   plugin per page; changing it or the card only while stopped. Taking it off
   waits until the card's thread lets go of it. Instruments come with MIDI
   read by the bridge (next wave).

12. **An instrument live from a MIDI keyboard (0.6.0).** A note that went
   through the page would arrive with the page's jitter, so the bridge reads
   the keyboard itself (`src/midi.cpp`): WinMM on Windows, the raw MIDI
   devices of ALSA (`/dev/snd/midiC*D*`, names from `/proc/asound/cards`) on
   Linux, no extra library. The port's thread parses bytes into channel
   messages (running status kept; system exclusive and real-time bytes
   skipped) and stamps each with the time it came; a lock-free queue hands
   them to the card's thread, which places each message in the period by
   when it came during the period before (one period later, without
   jitter) and gives it to the plugin with the block it falls in. During a
   take the messages are kept with their frame from the take's start
   (`midiTake`), so Studio can put the notes on a track. `liveOpen` with
   `midi` takes an instrument (no inputs) or gives an effect the keyboard
   too. `--test-audio` adds the keyboard "Test MIDI", played with
   `midiTest`.

13. **Linux like Windows (0.7.0).** The bridge on Linux gets the same window
   and tray icon (`src/gui_gtk.cpp`: status, pairing code, plugins and
   folders, look for plugins, forget browsers, `Start when I log in` as an
   XDG autostart file `~/.config/autostart/oros-bridge.desktop` with
   `--tray`, the AppImage's own path when run from one) and plugin windows
   on X11 (`src/editor.cpp`; XWayland under Wayland). GTK 3, the
   AppIndicator (Ayatana, or the older libappindicator) and libX11 are
   loaded at run time, so one binary runs everywhere: without a display or
   GTK it is the terminal program it was (as with `--console`), without an
   AppIndicator it has a window and no tray icon (closing minimises it,
   Quit is a button, `--tray` starts it minimised), without X11 it offers
   no plugin windows. Linux has no shared event loop, so `MainLoop` is a
   `poll()` loop with a wake pipe, file descriptors and timers that plugins
   borrow (VST 3 `IRunLoop` through the editor frame; CLAP
   `timer-support` and `posix-fd-support`), and GLib's loop runs inside it
   (`MainLoop::Guest`: prepare, query, poll together, check, dispatch), so
   plugin calls, plugin windows and the bridge window share one thread. No
   GTK dialog runs a nested loop (it would stop every plugin call). One
   bridge per user: an abstract Unix socket `oros-bridge-<uid>`; a second
   launch tells the first to show its window and ends. Greek when the
   language (`LC_ALL`, `LC_MESSAGES`, `LANG`, `LANGUAGE`) is Greek. The
   release adds `.deb` and `.rpm` (nfpm, GTK and the AppIndicator only
   recommended) and an AppImage (appimagetool, GTK from the system) beside
   the tar.gz (`packaging/linux/package-all.sh`); build.yml makes and
   installs them on every PR.

14. **The pairing code in a file (0.7.1).** orOS USB runs the bridge with
   no window of its own (inside its kiosk session, `--console`), so
   `--code-file PATH` keeps the open code in PATH ("123456\n", written to a
   temporary file and renamed) and removes it when the code is used or
   ended; the file is removed at start too. orOS USB's System button reads
   it and shows the code over orOS.

15. **New versions, and copies orOS brings (0.8.0).** The window and tray
   ask GitHub's `releases/latest` a minute after start and then every 24
   hours (`src/update.cpp`; WinHTTP on Windows, libcurl loaded at run time
   on Linux with the `curl` or `wget` command as the fallback, so there is
   no new build dependency). A newer `vX.Y.Z` that is not a draft or a
   prerelease shows a line and a button in the window and an item in the tray
   menu, and never pops up (as orOS Drive's notice: free forever, no nagging);
   `Tell me about new versions` (tray menu, `updateCheck` in config.json)
   turns the checks off. `Update` downloads the release's `SHA256SUMS.txt` and
   the files this copy needs and refuses any file whose SHA-256 differs or
   that the list does not name (our own SHA-256, `src/sha256.cpp`). How the
   new version takes this copy's place depends on how it was installed
   (`update::kind()`): Windows renames the running exe (and the ASIO module
   beside it, so both stay of one version) to `.old` and moves the new ones
   in, putting the old pair back if a step fails; Linux replaces the
   AppImage (`$APPIMAGE`) or the program from the tar.gz with a rename in the
   same folder, or installs the `.deb` (`pkexec apt-get install`) or `.rpm`
   (`pkexec rpm -U`) when the program is `/usr/bin/oros-bridge` from a
   package. A folder this user cannot write gets the release page. Then the
   bridge starts itself again with its options (without `--tray`) and
   `--after-update <pid>`, which waits for the old one to end before it takes
   the port. `--bundled` (orOS Portable and the installers start the bridge
   with it, together with `--code-file`) means no window, no console of its
   own on Windows and no updates: the product that carries the bridge
   updates it, as does an `oros-bridge.bundled` file beside the program.
   `--check-update` and `--update` do the same from a terminal.
   `OROS_BRIDGE_UPDATE_URL` points every request at a stand-in (tests).

16. **Scanners for orOS Scan PDF (0.9.0).** A page cannot reach a scanner, so
   the bridge does (improvement item 20, approved 2026-10-31). Windows: WIA
   2.0 (`IWiaDevMgr2`, `src/scanner_wia.cpp`), part of Windows, which every
   scanner driver speaks; each WIA call runs on a short-lived thread of its
   own in the multithreaded apartment, so the WIA service's callbacks never
   need a message loop; the glass and the feeder are the device's child items
   (`WIA_CATEGORY_FLATBED` / `FEEDER`, duplex from
   `WIA_IPS_DOCUMENT_HANDLING_CAPABILITIES`), resolutions and colour modes
   from the item's valid values, and each page comes as a BMP in memory
   (`IWiaTransfer::Download`), read into rows. Linux: SANE
   (`src/scanner_sane.cpp`). `libsane.so.1` is loaded at run time like GTK, so
   no SANE code or header is in the bridge (the types are written from the
   SANE standard) and the bridge stays MIT; without SANE the list says
   `nosane`. Network and USB multifunction printers need no vendor driver
   through `sane-airscan` (eSCL, WSD) and `ipp-usb`, which the .deb/.rpm
   recommend and orOS USB ships. Every SANE call runs under one lock (SANE is
   not thread safe); each backend's own words for sources and modes ("Flatbed",
   "ADF Duplex", "Color", "Gray", "Lineart" and the variants backends use) are
   mapped to `flatbed` / `adf` / `adf-duplex` and `color` / `gray` /
   `lineart`; 1- and 16-bit samples become 8-bit; a feeder scans until the
   backend says it is empty. One scan at a time on this computer (a second
   gets `busy`) on a thread of the page's session, never the plugin main
   thread, so Studio renders on meanwhile. The list is kept: a page asks
   `scanners` with `full` to look again (network scanners can take seconds),
   and during a scan the kept list comes back. The window shows how many
   scanners the last look found; it never looks by itself, so starting the
   bridge never wakes a scanner. A page is sent in strips of about 1 MB
   (`ORS1`, 8-bit rows, 1 or 3 channels) and closed with `scanPage`; a page
   cut short by Cancel is not given. `--scanners` lists them in a terminal,
   `--test-scanner` adds a test scanner (below). The window now says "orOS is
   connected", since Studio and Scan PDF share the pairing.

17. **Already running inside orOS (0.9.1).** orOS Portable and the orOS
   installers run their own copy of the bridge (decision 15), which holds
   port 47621. A person who also starts the downloaded bridge used to get
   "could not start: port 47621 is in use". Now, when the port is taken, the
   bridge asks `/status` on it (Origin `https://useoros.online`, 2 s limit);
   if an orOS Bridge answers, it says in a note (not an error) that the
   bridge already runs on this computer, for example inside orOS, and ends
   with code 0. Anything else holding the port is still an error. A copy
   started `--bundled` never shows the note (orOS uses the one that answers).

18. **TVs and speakers on the network (0.10.0).** orOS "Play on" (orOS Bible
   CAST-1, approved 2026-11-02): a page cannot search the network, so the
   bridge does (`src/cast.cpp`). SSDP `M-SEARCH` for
   `urn:schemas-upnp-org:device:MediaRenderer:1` to 239.255.255.250:1900
   (twice, 2.5 s, TTL 2), only when a paired page asks `renderers` with
   `full` (or the first time); the list is kept. Each answer's `LOCATION`
   must be `http://` to an IPv4 address in 10/8, 172.16/12, 192.168/16,
   169.254/16 or 127/8; the description (at most 512 KB, 3 s) gives the name,
   maker, model, UDN (the id, `upnp:<uuid>`) and the AVTransport and
   RenderingControl control URLs, which must be on the same host and port
   (relative ones are resolved against `URLBase` or the description). A
   device without AVTransport is left out. Control is SOAP over plain HTTP
   (`SetAVTransportURI` with DIDL-Lite metadata, `Play`, `Pause`, `Stop`,
   `GetTransportInfo`, `SetVolume`/`GetVolume`), only to a device in the kept
   list; a refused `SetAVTransportURI` gets `Stop` and one more try (some
   TVs take a new address only when stopped). Only `http(s)` stream addresses
   go out, every value is XML-escaped (the title twice, inside the metadata),
   device text is cleaned (no control characters, at most 120 bytes). The
   bridge listens on nothing new: the TV fetches the stream itself; files
   stored in orOS would need the bridge to serve them on the network, which
   waits for its own yes. No Google Cast (orOS rule 29, option A): Chromecast
   goes through the browser's Remote Playback. `bluetoothSettings` opens the
   system's Bluetooth page (`ms-settings:bluetooth`; on Linux the first of
   GNOME Settings, KDE System Settings, Blueman, Blueberry). The calls run on
   the page's session thread (a few seconds at most), never the plugin
   thread. `--test-renderer HOST:PORT` sends the search to one more address,
   for the fake TV in the tests.

19. **Folders orOS watches (0.11.0).** orOS "Everything talks to everything",
   item 25 (approved 2026-11-04): the bridge as the computer's sensor for orOS
   Automations (`src/watch.cpp`). **Only the person at this computer adds a
   folder**: `Add folder…` in the bridge's window, `--watch DIR`, or the
   system's folder chooser that a paired page can ask to open (`watchPick`;
   the answer comes when the person closes it). A page can never name a
   folder to add, so a page cannot reach a file the person did not offer. A
   page may take a folder off (`watchRemove`), which only removes access. At
   most 16 folders, kept in config.json (`watch`: [{path, upto}]). One thread
   looks at every folder every 2 s (`OROS_BRIDGE_WATCH_MS` in the tests):
   plain files directly in the folder (no folders below, no links, at most
   20000), without hidden files, `~$` lock files, `Thumbs.db`/`desktop.ini`
   and names ending `.tmp`, `.part`, `.crdownload`, `.download` and the like.
   Polling and not ReadDirectoryChangesW/inotify: one code path, network and
   USB folders work, and a file must keep its size and time over two looks
   before it counts, which is how "finished writing" is known anyway. What is
   there when a folder is added is the baseline and is never reported; a new
   name is reported once finished, and a reported name written again (a
   scanner's `scan.pdf`) is reported again; a file of the baseline edited in
   place is not. `upto` (the last look) is saved at most once a minute and on
   every report, so a file written while the bridge was off is reported at its
   next start; a copy that keeps an older time is not (honest limit). Each
   report is a `fileNew` event to every paired page, and the last 100 stay in
   `watched` for a page that was closed. A file's `key` is the first 16 hex of
   the SHA-256 of folder, name, size and time, so the same file gives the same
   key on every look (orOS runs a rule once per key). `fileRead` and `print`
   take only a key from that list, and only while the file still has that size
   and time, is a plain file and sits in a folder still watched (`changed`,
   `notfound` otherwise); reads are at most 4 MB a block and 512 MB a file.
   Printing: Windows `ShellExecuteEx` with the `print` verb (the file type's
   own print command; `noapp` without one), Linux `lp -- <file>` without a
   shell (`noapp` without CUPS). The chooser: Windows `IFileOpenDialog`
   (`FOS_PICKFOLDERS`) on a thread of its own, owned by a hidden topmost window
   so it comes up above the browser, also in `--bundled` copies; Linux the
   window's GTK chooser through its `response` signal (never a nested loop,
   decision 13), else zenity, kdialog or yad (`nodialog` without one). One
   chooser at a time (`busy`). `--test-pick FILE` and `--test-print DIR` stand
   in for the chooser and the printer in the tests. A USB device being plugged
   in (also in item 25) waits for its own wave.

## Protocol (WebSocket, `ws://127.0.0.1:47621/`)

Text messages are JSON. A command carries `op` and an optional `id`; the
answer repeats the `id` and has `ok` (and `error` when false). Events have
`event` instead.

| op | fields | answer |
|---|---|---|
| `pair` | (none) | `shown`: a code is on the bridge's screen |
| `pair` | `code` | `token` |
| `auth` | `token` | `ok` |
| `list` | | `plugins`: [{id, format, name, vendor, version, path, instrument}] |
| `scan` | `full`? | `plugins`; events `scanning` {path} meanwhile |
| `folders` / `setFolders` | `extra`: [paths] | the default and extra plugin folders |
| `open` | `plugin`, `sr` (8000..384000), `block` (16..16384) | `h`, `inputs`, `outputs`, `latency`, `tail`, `editor`, `instrument` |
| `info` | `h` | `latency`, `tail`, `dirty` |
| `getState` / `setState` | `h`, `data` (base64) | the plugin's state (VST 3: `OBV3` + component + controller, each with its length) |
| `reset` | `h` | the plugin forgets its sound (a new render) |
| `editor` / `closeEditor` | `h` | the plugin's window; event `editorClosed` {h} when the person closes it |
| `close` | `h` | |
| `audioDevices` | | `backends`: [{id, label, exclusive, ok, inputs, outputs \| error}], each device {name, channels, default} |
| `audioOpen` | `backend`, `input`?, `output`? (names, "" = default), `sr`? (0 = the card's), `period`? (16..8192), `exclusive`?, `record`: [input channel, from 0] | `sr`, `period`, `inputs`, `outputs`, `latency`, `record`; events `levels` every 100 ms |
| `audioPlay` | `frames`, `channels` (0..2), `out` (first output channel) | then `ORP1` blocks fill it |
| `audioStart` / `audioStop` | | stop: `frames` recorded, `lost` (frames the disk could not keep up with) |
| `takeRead` | `offset`, `frames` (at most 262144) | an `ORT1` block, then `frames` |
| `audioClose` | | (also closes the live plugin) |
| `midiDevices` | | `inputs`: [names] |
| `liveOpen` | `plugin`, `inputs` (1 or 2 card inputs; none for an instrument), `midi`? (a keyboard's name; needed by an instrument), `out`? (first output), `gain`? (0..4), `on`?, `record`? (`dry` \| `wet`), `state`? (base64) | `h` (for `editor`, `getState`, `setState`, `info`, `close`), `latency` (the plugin's), `monitor` (card + plugin, frames), `inputs`, `outputs`, `editor`, `on`, `record` |
| `liveSet` | `gain`?, `on`? | `on`, `gain`, `slow`, `failed` |
| `liveClose` | | |
| `midiTake` | | `events`: [[frame, status, data1, data2]] the live plugin got during the last take, `lost` |
| `midiTest` | `bytes` | the test keyboard plays them (`--test-audio`) |
| `scanners` | `full`? | `scanners`: [{id, name, vendor, model, sources (`flatbed` \| `adf` \| `adf-duplex`), modes (`color` \| `gray` \| `lineart`), dpi}], `missing`: [{code, error}] for a backend that is not there (`nosane`) |
| `scanStart` | `scanner`, `source`?, `mode`?, `dpi`? (50..1200; default flatbed, color, 300) | then `ORS1` strips, `scanProgress` {page, pct}, `scanPage` {page, width, height, channels} per page, and `scanDone` {pages, cancelled} or `scanError` {pages, code, error}; `code`: busy, nopaper, jammed, cover, denied, gone, unsupported, nosane, other |
| `scanCancel` | | `scanning` |
| `renderers` | `full`? | `renderers`: [{id, name, vendor, model, volume (it has a volume control)}] (0.10.0) |
| `castStart` | `renderer`, `url` (http or https), `title`?, `kind`? (`audio` \| `video`), `mime`? | the device plays it; on failure `code` (notfound, unreachable, refused, badurl, other) and `error` |
| `castControl` | `renderer`, `action` (`play` \| `pause` \| `stop` \| `volume`), `volume`? (0..100) | |
| `castStatus` | `renderer` | `state` (playing, paused, stopped, loading, idle, other), `volume`? |
| `bluetoothSettings` | | `opened`: whether a settings app was started |
| `watched` | | `folders`: [{path, ok (it is there now)}], `files`: the last 100 reported [{key, folder, name, kind (`pdf` \| `image` \| `other`), size, mtime, seen}] (0.11.0) |
| `watchPick` | `lang`? (`en` \| `el`) | the system's folder chooser on this computer: `folder` and `folders`, or `cancelled`; `code` busy, nodialog |
| `watchRemove` | `folder` | `removed`, `folders` |
| `fileRead` | `key`, `offset`?, `length`? (1..4194304) | an `ORF1` block, then `size`, `offset`, `length`; `code` notfound, changed, toolarge, unreadable |
| `print` | `key` | `printed`; `code` notfound, changed, noapp, failed |

Events: `hello` {app, version, editors, scanner (0.9.0), cast (0.10.0), watch (0.11.0)} on connect, `scanning`,
`fileNew` {file} (a new file in a watched folder, as in `watched`),
`editorClosed`, `renderError` {h, seq, error}, `levels` {peaks (one per
input channel), running, pos, live? {h, on, peak, slow, failed}}, `audioError` {offset, error} for a bad
`ORP1` block.

Sound to play and takes are binary too:

```
"ORP1" | u32 offset | u32 frames | u16 channels | u16 0 | channels × frames × float32
"ORT1" | u32 offset | u32 frames | u16 channels | u16 0 | channels × frames × float32
```

A watched file is read in blocks:

```
"ORF1" | u32 offset | u32 length | length bytes
```

A scanned page comes in strips, top to bottom:

```
"ORS1" | u32 page | u32 width | u32 y | u16 rows | u8 channels | u8 0 | rows × width × channels bytes
```

A block to render is a binary message, little endian:

```
"ORB1" | u32 h | u32 seq | u32 frames | u16 channels | u16 events
events × (u32 frame | u8 status | u8 data1 | u8 data2 | u8 0)
channels × frames × float32 (planar)
```

The answer has the same header with the plugin's output channel count and no
events. A mono input feeds every input channel. Blocks of one plugin are
processed in order; the first `latency` frames of a render are the plugin's
delay and Studio drops them, then sends `tail` frames of silence so reverbs
finish.

## Tests

`tests/protocol.test.js` runs a real bridge with our own test plugins
(`tests/test_plugins.c`: a CLAP gain with 32 frames of latency and a CLAP
sine instrument; `tests/vst3_test_gain.cpp`: a VST 3 gain) and checks the
origin and host rules, pairing (and the code file), scanning, rendering (exact samples, notes at
their frame), state in both formats and bad requests. With `--test-audio`
the bridge also offers a loopback card (4 inputs, 2 outputs, every output
back on an input 333 frames later, in real time), and the test checks that a
take lines up with what was played sample for sample, the take blocks, the
meters, a second take and bad requests. `tests/test_audio_module.cpp` is an
audio module with a loopback card (100 frames), loaded with `--audio-module`
on every platform: the same take check through the module boundary. On
Windows, `asio/test_driver.cpp` is a loopback ASIO driver (32-bit integer
samples, 480 frames of reported and real latency): the test registers it as
installers do (`HKLM\SOFTWARE\ASIO` and its COM class), checks the
list, a take within one 32-bit step, the same-card rule and the one-driver
rule, then removes it. The live test puts the CLAP test gain (latency 32)
on a loopback input that carries an output back, so the plugin's sound
returns through the card; a dry and a wet take must match that loop sample
for sample, and switching it off must give the played sound back; a VST 3 goes
live too. The live instrument test plays the CLAP test sine from the test
keyboard during a take: silence before the note, the sine while it is held
(starting within a frame of where `midiTake` puts the note), silence after.
On Linux CI runs the test a second time under Xvfb: the CLAP test gain then
has a window (Linux only) that asks the host for a 20 ms timer and a pipe to
watch, and the test checks that the timer ticked, the pipe was read, the
plugin got an X11 window id, and the ticks stop once the window is closed.
With `--test-scanner` the bridge offers a test scanner (a glass, a feeder of
3 sheets, both sides; every sample follows a formula), and the test checks
the list, a page from the glass sample by sample with its progress, six
duplex pages in order, line art, Cancel (the page cut short is not given),
one scan at a time and bad requests. On Linux CI installs `libsane1` and the
test also scans through the real libsane with SANE's own `test` backend only
(`SANE_CONFIG_DIR` with `dll.conf` = `test`): the list, a grey and a colour
page from the glass, and the feeder until it is empty. WIA has no stand-in:
it is built on Windows CI and needs a real scanner to test.
The test also runs a fake TV (Node: a UDP port answering `M-SEARCH`, given
with `--test-renderer`, and an HTTP server with a UPnP description and SOAP
control): it is found with its name unescaped; a device whose control URL is
on another host and one at a public address are left out; a stream goes in
with its title escaped in the metadata, then plays, pauses, changes volume
and stops; a TV that refuses a new address gets Stop and the address again;
bad addresses (javascript:, file:, data:), unknown devices, bad actions and
volumes never reach the TV. Real TVs, speakers and Windows' firewall need
real hardware.
The watch tests start the bridge with `--watch` on a folder that already
holds a file (never reported), then check: a file written in pieces slower
than a look is reported once, whole; temporary, hidden and lock files, a file
in a folder below and a link are not; a photo's kind; reading back in 5-byte
blocks; printing to the test printer; the same name written again gives a new
key and the old key answers `changed`; a removed file `notfound`; the chooser
cancelled and chosen; a folder taken off is quiet and its files unreadable;
bad keys and ranges, and that a page cannot add a folder itself; and a second
bridge on a saved config reports a file newer than the last look and not one
older.
`tests/update.test.js` runs a copy of the bridge against a local stand-in
for GitHub: older, equal, newer and prerelease tags; an update that replaces
the copy (Windows: the exe and the ASIO module, the old ones kept as `.old`;
Linux: the program from the tar.gz, mode 755) with no temporary files left;
a wrong or missing checksum that leaves the program untouched; and
`--bundled` and `oros-bridge.bundled`, which refuse to update and run with no
window. build.yml also makes the .deb, .rpm and AppImage, installs the .deb and runs
both. Real cards and keyboards, the bridge's own window and real plugin
windows are tested by hand.
