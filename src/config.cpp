#include "config.h"
#include "sha1.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace ob {

void to_json(nlohmann::json& j, const PluginInfo& p) {
  j = {{"id", p.id}, {"format", p.format}, {"name", p.name}, {"vendor", p.vendor},
       {"version", p.version}, {"path", p.path}, {"instrument", p.instrument}};
}
void from_json(const nlohmann::json& j, PluginInfo& p) {
  p.id = j.value("id", "");
  p.format = j.value("format", "");
  p.name = j.value("name", "");
  p.vendor = j.value("vendor", "");
  p.version = j.value("version", "");
  p.path = j.value("path", "");
  p.instrument = j.value("instrument", false);
}

void randomBytes(uint8_t* out, size_t len) {
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, out, ULONG(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    throw std::runtime_error("no random");
#else
  FILE* f = std::fopen("/dev/urandom", "rb");
  if (!f) throw std::runtime_error("no random");
  size_t n = std::fread(out, 1, len, f);
  std::fclose(f);
  if (n != len) throw std::runtime_error("no random");
#endif
}

std::string randomHex(size_t bytes) {
  std::vector<uint8_t> b(bytes);
  randomBytes(b.data(), bytes);
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (auto c : b) { s += hex[c >> 4]; s += hex[c & 15]; }
  return s;
}

std::string hashToken(const std::string& token) {
  auto h = sha1("oros-bridge:" + token);
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (auto c : h) { s += hex[c >> 4]; s += hex[c & 15]; }
  return s;
}

Config::Config(std::string dir) : dir_(std::move(dir)) {}

std::string Config::defaultDir() {
#ifdef _WIN32
  PWSTR p = nullptr;
  std::string out = ".";
  if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p) == S_OK)
    out = (fs::path(p) / "oros-bridge").u8string();
  CoTaskMemFree(p);
  return out;
#else
  const char* x = std::getenv("XDG_CONFIG_HOME");
  const char* h = std::getenv("HOME");
  fs::path base = x && *x ? fs::path(x) : fs::path(h ? h : ".") / ".config";
  return (base / "oros-bridge").string();
#endif
}

std::vector<std::string> Config::defaultFolders() {
  std::vector<std::string> v;
#ifdef _WIN32
  auto known = [&](REFKNOWNFOLDERID id, const char* sub) {
    PWSTR p = nullptr;
    if (SHGetKnownFolderPath(id, 0, nullptr, &p) == S_OK) v.push_back((fs::path(p) / sub).u8string());
    CoTaskMemFree(p);
  };
  known(FOLDERID_ProgramFilesCommon, "VST3");
  known(FOLDERID_ProgramFilesCommon, "CLAP");
  known(FOLDERID_LocalAppData, "Programs\\Common\\VST3");
  known(FOLDERID_LocalAppData, "Programs\\Common\\CLAP");
#else
  const char* h = std::getenv("HOME");
  std::string home = h ? h : "";
  if (!home.empty()) { v.push_back(home + "/.vst3"); v.push_back(home + "/.clap"); }
  for (auto p : {"/usr/lib/vst3", "/usr/local/lib/vst3", "/usr/lib/clap", "/usr/local/lib/clap"}) v.push_back(p);
#endif
  return v;
}

void Config::load() {
  std::lock_guard<std::mutex> g(mu_);
  std::ifstream in(fs::u8path(dir_) / "config.json");
  if (!in) return;
  try {
    auto j = nlohmann::json::parse(in);
    if (j.is_object()) data_ = j;
  } catch (...) {
    // a damaged file: start over, keep the old one beside it
    std::error_code ec;
    fs::rename(fs::u8path(dir_) / "config.json", fs::u8path(dir_) / "config.json.bad", ec);
  }
}

void Config::save() {
  std::lock_guard<std::mutex> g(mu_);
  std::error_code ec;
  fs::create_directories(fs::u8path(dir_), ec);
  auto tmp = fs::u8path(dir_) / "config.json.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << data_.dump(1);
    if (!out) return;
  }
  fs::rename(tmp, fs::u8path(dir_) / "config.json", ec);
}

bool Config::hasToken(const std::string& token) {
  std::lock_guard<std::mutex> g(mu_);
  if (token.size() != 64) return false;
  auto h = hashToken(token);
  for (auto& t : data_.value("tokens", nlohmann::json::array()))
    if (t.is_string() && t.get<std::string>() == h) return true;
  return false;
}

void Config::addToken(const std::string& token) {
  {
    std::lock_guard<std::mutex> g(mu_);
    auto& t = data_["tokens"];
    if (!t.is_array()) t = nlohmann::json::array();
    t.push_back(hashToken(token));
    while (t.size() > 16) t.erase(t.begin());   // the oldest pairing goes first
  }
  save();
}

void Config::forgetTokens() {
  { std::lock_guard<std::mutex> g(mu_); data_["tokens"] = nlohmann::json::array(); }
  save();
}

std::vector<std::string> Config::extraFolders() {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<std::string> v;
  for (auto& f : data_.value("folders", nlohmann::json::array()))
    if (f.is_string()) v.push_back(f.get<std::string>());
  return v;
}

void Config::setExtraFolders(const std::vector<std::string>& f) {
  { std::lock_guard<std::mutex> g(mu_); data_["folders"] = f; }
  save();
}

std::vector<std::string> Config::folders() {
  auto v = defaultFolders();
  for (auto& f : extraFolders()) v.push_back(f);
  return v;
}

std::vector<PluginInfo> Config::plugins() {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<PluginInfo> v;
  for (auto& p : data_.value("plugins", nlohmann::json::array())) v.push_back(p.get<PluginInfo>());
  return v;
}

void Config::setPlugins(const std::vector<PluginInfo>& p, const nlohmann::json& stamps) {
  { std::lock_guard<std::mutex> g(mu_); data_["plugins"] = p; data_["stamps"] = stamps; }
  save();
}

nlohmann::json Config::stamps() {
  std::lock_guard<std::mutex> g(mu_);
  return data_.value("stamps", nlohmann::json::object());
}

const PluginInfo* Config::find(const std::string& id, PluginInfo& out) {
  for (auto& p : plugins())
    if (p.id == id) { out = p; return &out; }
  return nullptr;
}
}
