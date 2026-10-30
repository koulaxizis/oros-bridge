# Third-party code in orOS Bridge

Fetched at build time by CMake, pinned to a release, all MIT:

| Library | Version | Licence | Use |
|---|---|---|---|
| [VST 3 SDK](https://github.com/steinbergmedia/vst3sdk) (Steinberg Media Technologies GmbH) | 3.8.1 build 84 | MIT | hosting VST 3 plugins (`base`, `pluginterfaces`, `public.sdk` hosting) |
| [CLAP](https://github.com/free-audio/clap) (Alexandre Bique and contributors) | 1.2.9 | MIT | hosting CLAP plugins (headers only) |
| [JSON for Modern C++](https://github.com/nlohmann/json) (Niels Lohmann) | 3.11.3 | MIT | the protocol's JSON |

VST is a registered trademark of Steinberg Media Technologies GmbH. orOS Bridge
is not made or endorsed by Steinberg. The plugins people load are their own and
keep their own licences; the bridge ships none.
