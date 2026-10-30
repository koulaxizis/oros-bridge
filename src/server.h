#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "config.h"

namespace ob {

// Pairing: a browser proves it was let in by the person at this computer.
// The bridge shows a 6-digit code; the page sends it back once and gets a
// token it keeps. Three wrong codes end the code.
class Pairing {
public:
  explicit Pairing(Config& cfg, std::function<void(const std::string&)> show) : cfg_(cfg), show_(std::move(show)) {}
  void start();                                        // new code, shown on this computer
  std::string tryCode(const std::string& code);        // token, or "" when wrong
  bool active();
  std::string currentCodeForTests();

private:
  std::mutex mu_;
  Config& cfg_;
  std::function<void(const std::string&)> show_;
  std::string code_;
  int wrong_ = 0;
};

struct ServerOptions {
  uint16_t port = 47621;
  std::vector<std::string> origins;   // pages that may connect
};

// A WebSocket connection, as the session sees it.
class Connection {
public:
  virtual ~Connection() = default;
  virtual void sendText(const std::string& s) = 0;
  virtual void sendBinary(const std::vector<uint8_t>& b) = 0;
};

// HTTP + WebSocket on 127.0.0.1 only. A request must name 127.0.0.1 or
// localhost as its host (no DNS rebinding) and come from a listed origin.
class Server {
public:
  Server(ServerOptions opt, Config& cfg, Pairing& pairing);
  bool listen(std::string& err);
  void run();          // accepts until stop()
  void stop();
  uint16_t port() const { return opt_.port; }

private:
  void handle(intptr_t sock);
  ServerOptions opt_;
  Config& cfg_;
  Pairing& pairing_;
  intptr_t listener_ = -1;
  std::atomic<bool> stopping_{false};
  std::atomic<int> live_{0};
};

bool originAllowed(const std::vector<std::string>& allowed, const std::string& origin);
bool hostAllowed(const std::string& host, uint16_t port);
std::string wsAccept(const std::string& key);
}
