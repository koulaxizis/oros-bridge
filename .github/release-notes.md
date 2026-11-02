**Windows x64:** download `oros-bridge-windows-x64.exe`, run it and keep its window open while you use orOS Studio (Studio → a track's settings → Plugin on this computer). The exe is not signed yet: Windows SmartScreen may ask once (More info → Run anyway).

**ASIO®:** `oros-bridge-asio.dll` goes next to the exe (the zip has both); it is GPLv3, source in `asio/`.

<img src="https://raw.githubusercontent.com/koulaxizis/oros-bridge/main/asio/logo/asio-compatible-bw.png" alt="ASIO Compatible" height="72">

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

**Linux x64:** install `oros-bridge-linux-x64.deb` (Debian, Ubuntu, Mint) or `.rpm` (Fedora, openSUSE) and open orOS Bridge from the applications menu, or make `oros-bridge-linux-x64.AppImage` executable and run it; the tar.gz has the bare program. Plugin windows need X11 or XWayland; the tray icon needs AppIndicator support (on GNOME the AppIndicator extension).

Record through your sound card: Studio → Settings → Sound card → orOS Bridge. Hear yourself through a plugin while you record (Sound card → Listen through a plugin, 0.5.0); play an instrument plugin from a MIDI keyboard (0.6.0). New versions show in the window and the tray menu and install with one click, checked against SHA256SUMS.txt (0.8.0). SHA-256 sums in SHA256SUMS.txt. Free, MIT.
