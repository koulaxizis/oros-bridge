#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "audio.h"
#include "midi.h"
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
  nlohmann::json audioCommand(const std::string& op, const nlohmann::json& m);
  nlohmann::json liveCommand(const std::string& op, const nlohmann::json& m);
  void closeLive();   // with audioMu_ held
  void playBlock(const std::vector<uint8_t>& d);
  void meters();
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
    std::unique_ptr<LiveFx> fx;   // live on the card's inputs: processed there, never rendered here
    std::unique_ptr<MidiInput> midi;   // the keyboard it plays from (0.6.0)
  };
  std::map<int, std::shared_ptr<Open>> open_;
  // the sound card (one per page), and the thread that sends its meters
  std::mutex audioMu_;
  AudioEngine audio_;
  int liveH_ = 0;                 // the handle of the plugin live on the inputs, 0 = none
  std::thread meterThread_;
  std::atomic<bool> meterQuit_{false};
};

constexpr size_t kMaxOpen = 64;
}
