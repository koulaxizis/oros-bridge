# Third-party code in orOS Bridge

Fetched at build time by CMake, pinned to a release, all MIT or more open:

| Library | Version | Licence | Use |
|---|---|---|---|
| [VST 3 SDK](https://github.com/steinbergmedia/vst3sdk) (Steinberg Media Technologies GmbH) | 3.8.1 build 84 | MIT | hosting VST 3 plugins (`base`, `pluginterfaces`, `public.sdk` hosting) |
| [CLAP](https://github.com/free-audio/clap) (Alexandre Bique and contributors) | 1.2.9 | MIT | hosting CLAP plugins (headers only) |
| [JSON for Modern C++](https://github.com/nlohmann/json) (Niels Lohmann) | 3.11.3 | MIT | the protocol's JSON |
| [miniaudio](https://github.com/mackron/miniaudio) (David Reid) | 0.11.25 | public domain (Unlicense) or MIT No Attribution | the sound card: WASAPI, JACK, PulseAudio/PipeWire, ALSA |

## The ASIO module (`oros-bridge-asio.dll`, Windows, GPLv3)

| Library | Version | Licence | Use |
|---|---|---|---|
| [ASIO SDK](https://www.steinberg.net/developers/) (Steinberg Media Technologies GmbH) | 2.3.4 (2025-10-15) | GPLv3 (dual licence; used under its GPLv3 option) | ASIO drivers (`common/asio.cpp`, `host/asiodrivers.cpp`, `host/pc/asiolist.cpp`) |

It is a separate library under the GPLv3 ([asio/LICENSE](asio/LICENSE)),
loaded by the bridge only when it lies beside the exe, and also uses JSON for
Modern C++ (MIT). The bridge itself stays MIT and contains no ASIO SDK code.
ASIO is a registered trademark of Steinberg Media Technologies GmbH.

VST is a registered trademark of Steinberg Media Technologies GmbH. orOS Bridge
is not made or endorsed by Steinberg. The plugins people load are their own and
keep their own licences; the bridge ships none.
