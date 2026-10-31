#pragma once
#include <string>
#include "config.h"
#include "server.h"

namespace ob {
// The bridge's own window and tray icon: Win32 (gui_win32.cpp), GTK 3 on
// Linux (gui_gtk.cpp, 0.7.0); elsewhere, or on Linux without a desktop, the
// bridge stays a command-line program. Everything runs on the main loop's thread.

// A window can be shown here (Linux: a display and GTK 3 are there).
bool guiAvailable();

// Another bridge already runs for this user: its window is brought up and
// true is returned, so this one can end.
bool guiAlreadyRunning();
// Studio asked to pair: show the code (callable from any thread).
void guiPairingCode(const std::string& code);
// Creates the window and the tray icon, then runs the main loop. Quitting
// from the tray ends the process.
int guiRun(Config& cfg, Server& server, Pairing& pairing, bool hidden);
// A message box for errors before the window exists.
void guiError(const std::string& msg);
}
