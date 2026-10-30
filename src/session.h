#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "config.h"
#include "plugin.h"
#include "server.h"

namespace ob {

// One connected page: pairing or token first, then the plugin commands.
class Session {
public:
  Session(Connection& conn, Config& cfg, Pairing& pairing);
  ~Session();
  void onText(const std::string& text);
  void onBinary(const std::vector<uint8_t>& data);

private:
  nlohmann::json command(const nlohmann::json& m);
  void reply(const nlohmann::json& m, nlohmann::json out);
  Connection& conn_;
  Config& cfg_;
  Pairing& pairing_;
  bool authed_ = false;
  int nextHandle_ = 1;
  struct Open {
    std::unique_ptr<Plugin> plugin;
    PluginInfo info;
    uint32_t maxFrames = 0;
  };
  std::map<int, std::shared_ptr<Open>> open_;
};

constexpr size_t kMaxOpen = 64;
}
