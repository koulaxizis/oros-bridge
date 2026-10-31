#pragma once
#include <functional>
#include <string>
#include "config.h"

namespace ob {
// New versions of the bridge (0.8.0, DESIGN.md decision 15). The window asks
// GitHub a minute after start and then once a day for the latest release;
// when it is newer, the window and the tray menu offer it (never a popup).
// One click downloads it, checks every file against the release's
// SHA256SUMS.txt and puts it in place for this copy: the exe (and the ASIO
// module beside it) on Windows, the AppImage, the .deb or .rpm (through
// pkexec), or the program from the tar.gz. Then the bridge starts again.
// A copy that orOS brought (Portable, the installers, orOS USB: --bundled or
// oros-bridge.bundled beside the program) never updates itself; orOS
// updates it. OROS_BRIDGE_UPDATE_URL points the checks elsewhere (tests).
namespace update {

// Keeps the program's path and arguments for kind() and relaunch(); call first in main().
void init(int argc, char** argv);
// orOS updates this copy: --bundled was given, or oros-bridge.bundled lies beside the program.
void setBundled();
bool bundled();

// How this copy updates: "exe", "appimage", "deb", "rpm", "tarball", or ""
// (only the download page: a folder this user cannot write, or a bundled copy).
std::string kind();

// "v0.9.0" is newer than this build ("0.8.0"); false for anything that is not x.y.z.
bool newer(const std::string& current, const std::string& latestTag);
std::string releasePage(const std::string& tag);

// The tag of the latest published release ("v0.9.0"), "" on failure (err says why).
std::string latestTag(std::string& err);

// Downloads, checks and installs `tag` for this copy, without starting it.
// On failure err is one of "download", "checksum", "install" (pkexec
// refused or failed), "files" (the program could not be replaced) or
// "manual" (kind() is "") and detail says more, in English.
bool install(const std::string& tag, std::string& err, std::string& detail);

// Starts the program again with the same options (without --tray) and
// --after-update <this process>; the caller then ends.
bool relaunch();
// --after-update PID: wait (up to 15 s) until the bridge that started us has ended.
void waitForOld(long pid);
// Removes what an update on Windows left behind (oros-bridge.exe.old).
void cleanOld();
// Opens a page in the default browser.
void openPage(const std::string& url);

// ---- for the window ----
struct State {
  std::string tag;       // a newer release, "" when none is known
  bool busy = false;     // downloading or installing
  std::string error;     // the last install's error code, "" when none
  bool canInstall = false;   // kind() != "": the button installs, else it opens the page
};
// Checks a minute after start and then every 24 hours while cfg.updateCheck() is on.
void startChecks(Config& cfg);
// Checks again soon (the person turned the checks back on).
void checkNow();
State state();
// Installs state().tag on another thread; on success `done` runs on the main
// loop after the new bridge was started (the window then ends this one).
void installAsync(std::function<void()> done);
}  // namespace update
}  // namespace ob
