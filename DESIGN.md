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
   runs both), but plugin windows are Win32 only for now; macOS later.
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
   from. Other systems have no window (`src/gui_none.cpp`).

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
   Studio adds the person's recording offset. ASIO comes as a separate
   GPLv3 module (decision 2026-10-31: the ASIO SDK is GPLv3 or proprietary),
   which adds one more backend and leaves this program MIT.

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
| `audioClose` | | |

Events: `hello` {app, version, editors} on connect, `scanning`,
`editorClosed`, `renderError` {h, seq, error}, `levels` {peaks (one per
input channel), running, pos}, `audioError` {offset, error} for a bad
`ORP1` block.

Sound to play and takes are binary too:

```
"ORP1" | u32 offset | u32 frames | u16 channels | u16 0 | channels × frames × float32
"ORT1" | u32 offset | u32 frames | u16 channels | u16 0 | channels × frames × float32
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
origin and host rules, pairing, scanning, rendering (exact samples, notes at
their frame), state in both formats and bad requests. With `--test-audio`
the bridge also offers a loopback card (4 inputs, 2 outputs, every output
back on an input 333 frames later, in real time), and the test checks that a
take lines up with what was played sample for sample, the take blocks, the
meters, a second take and bad requests. Real cards are tested by hand.
