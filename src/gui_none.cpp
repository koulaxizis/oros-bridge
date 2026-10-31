// Without Windows the bridge has no window of its own: it stays a
// command-line program (see gui.h).
#include "gui.h"
#include "mainloop.h"

namespace ob {
bool guiAvailable() { return false; }
bool guiAlreadyRunning() { return false; }
void guiPairingCode(const std::string&) {}
int guiRun(Config&, Server&, Pairing&, bool) {
  MainLoop::get().run();
  return 0;
}
void guiError(const std::string&) {}
}
