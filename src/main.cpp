// orOS Bridge: lets orOS Studio (in the browser) use the VST3 and CLAP
// plugins installed on this computer. See DESIGN.md.
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include "audio.h"
#include "midi.h"
#include "config.h"
#include "gui.h"
#include "mainloop.h"
#include "scan.h"
#include "server.h"
#ifdef _WIN32
#include <windows.h>
#endif

using namespace ob;

static const char* kUsage =
    "orOS Bridge " OROS_BRIDGE_VERSION "\n"
    "Lets orOS Studio use the VST3 and CLAP plugins on this computer.\n\n"
    "  oros-bridge                 run; on Windows in its own window and the tray\n"
    "  --tray                      start hidden in the tray (Windows)\n"
    "  --console                   run in the console instead of a window\n"
    "  --port N                    listen on 127.0.0.1:N (default 47621)\n"
    "  --allow-origin URL          also accept pages from URL (repeatable)\n"
    "  --config-dir DIR            keep settings in DIR\n"
    "  --scan                      look for plugins, print them, exit\n"
    "  --forget-pairings           forget every paired browser, exit\n"
    "  --test-audio                also offer a loopback sound card (tests)\n"
    "  --audio-module PATH         also offer the sound system in this library\n";

#ifdef _WIN32
// The program is built as a window program, so it has no console of its own.
// Command-line use writes to the console it was started from; output that
// is already redirected (a pipe, a file) is left as it is.
static void useConsole() {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h && h != INVALID_HANDLE_VALUE) return;
  if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
  }
}
#endif

int main(int argc, char** argv) {
#ifdef SIGPIPE
  std::signal(SIGPIPE, SIG_IGN);   // a page that went away is a send error, not the end (macOS has no MSG_NOSIGNAL)
#endif
  ServerOptions opt;
  opt.origins = {"https://useoros.online", "https://www.useoros.online"};
  std::string dir = Config::defaultDir();
  bool scanOnly = false, forget = false, console = false, tray = false, help = false;
  std::string unknown;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { std::cerr << a << " needs a value\n"; std::exit(2); }
      return argv[++i];
    };
    if (a == "--describe") {
      auto format = next();
      auto path = next();
      return describeMain(format, path);
    } else if (a == "--port") opt.port = uint16_t(std::stoi(next()));
    else if (a == "--allow-origin") opt.origins.push_back(next());
    else if (a == "--config-dir") dir = next();
    else if (a == "--scan") scanOnly = true;
    else if (a == "--forget-pairings") forget = true;
    else if (a == "--console") console = true;
    else if (a == "--tray") tray = true;
    else if (a == "--test-audio") { enableTestAudio(); enableTestMidi(); }
    else if (a == "--audio-module") addAudioModule(next());
    else if (a == "--help" || a == "-h") { console = true; help = true; }
    else { unknown = a; break; }
  }
#ifdef _WIN32
  bool gui = !console && !scanOnly && !forget && unknown.empty();
  if (!gui) useConsole();
#else
  const bool gui = false;
  (void)tray;
#endif
  if (help) { std::cout << kUsage; return 0; }
  if (!unknown.empty()) { std::cerr << "unknown option " << unknown << "\n" << kUsage; return 2; }
  if (gui && guiAlreadyRunning()) return 0;

  Config cfg(dir);
  cfg.load();
  if (forget) {
    cfg.forgetTokens();
    std::cout << "Every paired browser was forgotten.\n";
    return 0;
  }
  if (scanOnly) {
    auto list = scanPlugins(cfg, true, [](const std::string& p) { std::cerr << "scanning " << p << "\n"; });
    for (auto& p : list) std::cout << p.format << "\t" << p.name << "\t" << p.vendor << "\t" << (p.instrument ? "instrument" : "effect") << "\t" << p.path << "\n";
    return 0;
  }

  Pairing pairing(cfg, [gui](const std::string& code) {
    if (gui) guiPairingCode(code);
    std::cout << "\nPairing code: " << code << "\nType it in orOS Studio to let it use your plugins.\n" << std::flush;
  });
  Server server(opt, cfg, pairing);
  std::string err;
  if (!server.listen(err)) {
    if (gui) guiError(err);
    std::cerr << "orOS Bridge could not start: " << err << "\n";
    return 1;
  }
  std::cout << "orOS Bridge " OROS_BRIDGE_VERSION " is ready on 127.0.0.1:" << server.port() << "\n"
            << "Settings: " << cfg.dir() << "\n"
            << "Keep this window open while you use orOS Studio.\n" << std::flush;
  std::thread net([&] { server.run(); });
  if (gui) return guiRun(cfg, server, pairing, tray);
  // Ctrl+C or closing the window ends the bridge; the system frees the plugins
  std::signal(SIGINT, [](int) { std::_Exit(0); });
  std::signal(SIGTERM, [](int) { std::_Exit(0); });
  MainLoop::get().run();
  server.stop();
  net.join();
  return 0;
}
