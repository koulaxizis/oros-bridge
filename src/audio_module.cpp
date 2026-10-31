// A sound system from a separate library (audio_module.h): the ASIO module
// (oros-bridge-asio.dll beside the exe, GPLv3, asio/) or one named with
// --audio-module. A missing library hides nothing else; a library that does
// not answer the ABI is ignored.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include "audio.h"
#include "audio_module.h"
#include "dynlib.h"
#include "scan.h"

namespace ob {
namespace {

class ModuleStream : public AudioStream {
public:
  ~ModuleStream() override {
    if (h) mod->close(h);
  }
  static void onData(void* u, const float* in, uint32_t ic, float* out, uint32_t oc, uint32_t frames) {
    static_cast<ModuleStream*>(u)->cb(in, ic, out, oc, frames);
  }
  const oros_audio_module* mod = nullptr;
  void* h = nullptr;
  AudioCallback cb;
};

class ModuleBackend : public AudioBackend {
public:
  ModuleBackend(std::unique_ptr<DynLib> lib, const oros_audio_module* m) : lib_(std::move(lib)), m_(m) {}
  std::string id() const override { return m_->id; }
  std::string label() const override { return m_->label; }

  bool devices(nlohmann::json& out, std::string& err) override {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<char> buf(16384);
    char e[512] = {0};
    for (int tries = 0; tries < 3; tries++) {
      uint32_t need = m_->devices(buf.data(), uint32_t(buf.size()), e, sizeof e);
      if (!need) { err = e[0] ? e : "the sound system did not answer"; return false; }
      if (need <= buf.size()) {
        buf.back() = 0;
        out = nlohmann::json::parse(buf.data(), nullptr, false);
        if (out.is_discarded() || !out.contains("inputs") || !out.contains("outputs")) { err = "bad device list"; return false; }
        return true;
      }
      if (need > (1u << 22)) break;
      buf.assign(need, 0);
    }
    err = "bad device list";
    return false;
  }

  std::unique_ptr<AudioStream> open(const StreamSpec& spec, AudioCallback cb, std::string& err) override {
    std::lock_guard<std::mutex> g(mu_);
    auto s = std::make_unique<ModuleStream>();
    s->mod = m_;
    s->cb = std::move(cb);
    oros_audio_stream_info info{};
    char e[512] = {0};
    s->h = m_->open(spec.input.c_str(), spec.output.c_str(), spec.sampleRate, spec.period, &ModuleStream::onData,
                    s.get(), &info, e, sizeof e);
    if (!s->h) {
      err = e[0] ? e : "the sound card could not be opened";
      return nullptr;
    }
    s->sampleRate = info.sampleRate;
    s->inChannels = info.inChannels;
    s->outChannels = info.outChannels;
    s->period = info.period;
    s->latency = info.latency;
    return s;
  }

private:
  std::unique_ptr<DynLib> lib_;   // outlives every stream: backends live until the process ends
  const oros_audio_module* m_;
  std::mutex mu_;
};

std::vector<std::string>& extra() {
  static std::vector<std::string> v;
  return v;
}
}

void addAudioModule(const std::string& path) { extra().push_back(path); }

std::unique_ptr<AudioBackend> loadAudioModule(const std::string& path, std::string& err) {
  std::error_code ec;
  if (!std::filesystem::exists(std::filesystem::u8path(path), ec)) { err = "not found"; return nullptr; }
  auto lib = std::make_unique<DynLib>(path);
  if (!lib->ok()) { err = "cannot load: " + DynLib::lastError(); return nullptr; }
  auto entry = reinterpret_cast<oros_audio_module_entry_fn>(lib->symbol(OROS_AUDIO_MODULE_ENTRY));
  if (!entry) { err = "not an orOS Bridge audio module"; return nullptr; }
  const oros_audio_module* m = entry(OROS_AUDIO_MODULE_ABI);
  if (!m || m->abi != OROS_AUDIO_MODULE_ABI || !m->id || !m->label || !m->devices || !m->open || !m->close) {
    err = "another module version";
    return nullptr;
  }
  return std::make_unique<ModuleBackend>(std::move(lib), m);
}

// the modules to offer: oros-bridge-asio.dll beside the exe on Windows, then --audio-module
std::vector<std::unique_ptr<AudioBackend>> makeModuleBackends() {
  std::vector<std::pair<std::string, bool>> paths;   // path, may be missing
#ifdef _WIN32
  auto self = std::filesystem::u8path(selfPath());
  paths.push_back({(self.parent_path() / "oros-bridge-asio.dll").u8string(), true});
#endif
  for (auto& p : extra()) paths.push_back({p, false});
  std::vector<std::unique_ptr<AudioBackend>> v;
  for (auto& [p, optional] : paths) {
    std::string err;
    auto b = loadAudioModule(p, err);
    bool taken = false;
    for (auto& o : v) taken = taken || (b && o->id() == b->id());
    if (b && !taken) v.push_back(std::move(b));
    else if (b) std::fprintf(stderr, "audio module %s: %s is there already\n", p.c_str(), b->id().c_str());
    else if (!(optional && err == "not found")) std::fprintf(stderr, "audio module %s: %s\n", p.c_str(), err.c_str());
  }
  return v;
}
}
