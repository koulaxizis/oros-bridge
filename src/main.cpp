// orOS Bridge: lets orOS Studio (in the browser) use the VST3 and CLAP
// plugins installed on this computer. See DESIGN.md.
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include "config.h"
#include "mainloop.h"
#include "scan.h"
#include "server.h"

using namespace ob;

static const char* kUsage =
    "orOS Bridge " OROS_BRIDGE_VERSION "\n"
    "Lets orOS Studio use the VST3 and CLAP plugins on this computer.\n\n"
    "  oros-bridge                 run (keep this window open while you use Studio)\n"
    "  --port N                    listen on 127.0.0.1:N (default 47621)\n"
    "  --allow-origin URL          also accept pages from URL (repeatable)\n"
    "  --config-dir DIR            keep settings in DIR\n"
    "  --scan                      look for plugins, print them, exit\n"
    "  --forget-pairings           forget every paired browser, exit\n";


int main(int argc, char** argv) {
  ServerOptions opt;
  opt.origins = {"https://useoros.online", "https://www.useoros.online"};
  std::string dir = Config::defaultDir();
  bool scanOnly = false, forget = false;
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
    else if (a == "--help" || a == "-h") { std::cout << kUsage; return 0; }
    else { std::cerr << "unknown option " << a << "\n" << kUsage; return 2; }
  }

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

  Pairing pairing(cfg, [](const std::string& code) {
    std::cout << "\nPairing code: " << code << "\nType it in orOS Studio to let it use your plugins.\n" << std::flush;
  });
  Server server(opt, cfg, pairing);
  std::string err;
  if (!server.listen(err)) {
    std::cerr << "orOS Bridge could not start: " << err << "\n";
    return 1;
  }
  std::cout << "orOS Bridge " OROS_BRIDGE_VERSION " is ready on 127.0.0.1:" << server.port() << "\n"
            << "Settings: " << cfg.dir() << "\n"
            << "Keep this window open while you use orOS Studio.\n" << std::flush;
  // Ctrl+C or closing the window ends the bridge; the system frees the plugins
  std::signal(SIGINT, [](int) { std::_Exit(0); });
  std::signal(SIGTERM, [](int) { std::_Exit(0); });
  std::thread net([&] { server.run(); });
  MainLoop::get().run();
  server.stop();
  net.join();
  return 0;
}
