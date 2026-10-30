#pragma once
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace ob {

// One plugin found on this computer.
struct PluginInfo {
  std::string id;       // "vst3:<class uid>" or "clap:<plugin id>"
  std::string format;   // "vst3" or "clap"
  std::string name, vendor, version;
  std::string path;     // the .vst3 bundle or .clap file
  bool instrument = false;
};
void to_json(nlohmann::json& j, const PluginInfo& p);
void from_json(const nlohmann::json& j, PluginInfo& p);

// Random bytes from the system (BCryptGenRandom, /dev/urandom); throws when it cannot.
void randomBytes(uint8_t* out, size_t len);
std::string randomHex(size_t bytes);

// Settings kept in <config dir>/config.json: paired browsers (hashes of their
// tokens), extra plugin folders, and what the last scan found.
class Config {
public:
  explicit Config(std::string dir);
  const std::string& dir() const { return dir_; }
  void load();
  void save();

  bool hasToken(const std::string& token);
  void addToken(const std::string& token);
  void forgetTokens();

  std::vector<std::string> folders();       // default folders plus the person's own
  void setExtraFolders(const std::vector<std::string>& f);
  std::vector<std::string> extraFolders();

  std::vector<PluginInfo> plugins();
  void setPlugins(const std::vector<PluginInfo>& p, const nlohmann::json& stamps);
  nlohmann::json stamps();                  // path -> modification time when scanned
  const PluginInfo* find(const std::string& id, PluginInfo& out);

  static std::string defaultDir();
  static std::vector<std::string> defaultFolders();

private:
  std::mutex mu_;
  std::string dir_;
  nlohmann::json data_ = nlohmann::json::object();
};

std::string hashToken(const std::string& token);
}
