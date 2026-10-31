// The system's sound cards through miniaudio (public domain / MIT-0):
// WASAPI on Windows; JACK, PulseAudio (which PipeWire also answers) and ALSA
// on Linux; Core Audio on macOS. miniaudio loads each system library at run
// time, so a missing JACK or PulseAudio only hides that backend.
#include <cstring>
#include <map>
#include <mutex>
#include "audio.h"
#include "miniaudio.h"

namespace ob {
namespace {

// names as the person sees them; a second card with the same name gets " (2)"
std::vector<std::string> uniqueNames(const ma_device_info* d, ma_uint32 n) {
  std::vector<std::string> out;
  std::map<std::string, int> seen;
  for (ma_uint32 i = 0; i < n; i++) {
    std::string s = d[i].name;
    int k = ++seen[s];
    out.push_back(k == 1 ? s : s + " (" + std::to_string(k) + ")");
  }
  return out;
}

class MaStream : public AudioStream {
public:
  ~MaStream() override { ma_device_uninit(&dev); }
  static void onData(ma_device* d, void* out, const void* in, ma_uint32 frames) {
    auto* s = static_cast<MaStream*>(d->pUserData);
    s->cb(static_cast<const float*>(in), d->capture.channels, static_cast<float*>(out), d->playback.channels, frames);
  }
  ma_device dev{};
  AudioCallback cb;
};

class MaBackend : public AudioBackend {
public:
  MaBackend(ma_backend be, std::string id, std::string label, bool excl)
      : be_(be), id_(std::move(id)), label_(std::move(label)), excl_(excl) {}
  ~MaBackend() override {
    if (ok_) ma_context_uninit(&ctx_);
  }
  std::string id() const override { return id_; }
  std::string label() const override { return label_; }
  bool canExclusive() const override { return excl_; }

  bool devices(nlohmann::json& out, std::string& err) override {
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure(err)) return false;
    ma_device_info *pb = nullptr, *cap = nullptr;
    ma_uint32 npb = 0, ncap = 0;
    if (ma_context_get_devices(&ctx_, &pb, &npb, &cap, &ncap) != MA_SUCCESS) { err = "cannot list devices"; return false; }
    // copies: a second call to the context reuses its lists
    std::vector<ma_device_info> vpb(pb, pb + npb), vcap(cap, cap + ncap);
    auto list = [&](std::vector<ma_device_info>& v, ma_device_type type) {
      auto names = uniqueNames(v.data(), ma_uint32(v.size()));
      nlohmann::json a = nlohmann::json::array();
      for (size_t i = 0; i < v.size(); i++) {
        uint32_t ch = 0;
        ma_device_info full;
        if (ma_context_get_device_info(&ctx_, type, &v[i].id, &full) == MA_SUCCESS)
          for (ma_uint32 k = 0; k < full.nativeDataFormatCount; k++) ch = std::max<uint32_t>(ch, full.nativeDataFormats[k].channels);
        a.push_back({{"name", names[i]}, {"channels", ch ? ch : 2}, {"default", v[i].isDefault != 0}});
      }
      return a;
    };
    out = {{"inputs", list(vcap, ma_device_type_capture)}, {"outputs", list(vpb, ma_device_type_playback)}};
    return true;
  }

  std::unique_ptr<AudioStream> open(const StreamSpec& spec, AudioCallback cb, std::string& err) override {
    std::lock_guard<std::mutex> g(mu_);
    if (!ensure(err)) return nullptr;
    ma_device_info *pb = nullptr, *cap = nullptr;
    ma_uint32 npb = 0, ncap = 0;
    if (ma_context_get_devices(&ctx_, &pb, &npb, &cap, &ncap) != MA_SUCCESS) { err = "cannot list devices"; return nullptr; }
    ma_device_id inId{}, outId{};
    auto pick = [&](const std::string& want, ma_device_info* d, ma_uint32 n, ma_device_id& id) -> int {
      if (want.empty()) return 0;   // the default
      auto names = uniqueNames(d, n);
      for (ma_uint32 i = 0; i < n; i++)
        if (names[i] == want) { id = d[i].id; return 1; }
      return -1;
    };
    int hasIn = pick(spec.input, cap, ncap, inId), hasOut = pick(spec.output, pb, npb, outId);
    if (hasIn < 0) { err = "no such input: " + spec.input; return nullptr; }
    if (hasOut < 0) { err = "no such output: " + spec.output; return nullptr; }

    auto s = std::make_unique<MaStream>();
    s->cb = std::move(cb);
    ma_device_config dc = ma_device_config_init(ma_device_type_duplex);
    dc.capture.pDeviceID = hasIn ? &inId : nullptr;
    dc.capture.format = ma_format_f32;
    dc.capture.channels = 0;   // every input the card has
    dc.playback.pDeviceID = hasOut ? &outId : nullptr;
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 0;
    dc.sampleRate = spec.sampleRate;   // 0: the card's own, nothing resampled
    dc.periodSizeInFrames = spec.period;
    dc.periods = 2;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.noClip = MA_TRUE;
    dc.dataCallback = &MaStream::onData;
    dc.pUserData = s.get();
    if (spec.exclusive && excl_) {
      dc.capture.shareMode = ma_share_mode_exclusive;
      dc.playback.shareMode = ma_share_mode_exclusive;
    }
    dc.wasapi.noAutoConvertSRC = MA_TRUE;   // the rate stays the card's; miniaudio converts if it must
    ma_result r = ma_device_init(&ctx_, &dc, &s->dev);
    if (r != MA_SUCCESS) {
      err = std::string("the sound card refused: ") + ma_result_description(r);
      return nullptr;
    }
    auto& d = s->dev;
    s->sampleRate = d.sampleRate;
    s->inChannels = d.capture.channels;
    s->outChannels = d.playback.channels;
    s->period = d.playback.internalPeriodSizeInFrames;
    // output buffer + one input period, at the stream's rate
    double outLat = double(d.playback.internalPeriodSizeInFrames) * d.playback.internalPeriods * d.sampleRate /
                    (d.playback.internalSampleRate ? d.playback.internalSampleRate : d.sampleRate);
    double inLat = double(d.capture.internalPeriodSizeInFrames) * d.sampleRate /
                   (d.capture.internalSampleRate ? d.capture.internalSampleRate : d.sampleRate);
    s->latency = uint32_t(outLat + inLat + 0.5);
    if (ma_device_start(&s->dev) != MA_SUCCESS) { err = "the sound card did not start"; return nullptr; }
    return s;
  }

private:
  bool ensure(std::string& err) {
    if (ok_) return true;
    ma_context_config cc = ma_context_config_init();
    cc.jack.pClientName = "orOS Bridge";
    cc.jack.tryStartServer = MA_FALSE;
    cc.pulse.pApplicationName = "orOS Bridge";
    ma_backend list[1] = {be_};
    if (ma_context_init(list, 1, &cc, &ctx_) != MA_SUCCESS) { err = label_ + " is not running"; return false; }
    ok_ = true;
    return true;
  }
  ma_backend be_;
  std::string id_, label_;
  bool excl_;
  bool ok_ = false;
  ma_context ctx_{};
  std::mutex mu_;
};
}

std::vector<std::unique_ptr<AudioBackend>> makeSystemBackends() {
  std::vector<std::unique_ptr<AudioBackend>> v;
#if defined(_WIN32)
  v.push_back(std::make_unique<MaBackend>(ma_backend_wasapi, "wasapi", "Windows Audio (WASAPI)", true));
#elif defined(__APPLE__)
  v.push_back(std::make_unique<MaBackend>(ma_backend_coreaudio, "coreaudio", "Core Audio", false));
#else
  v.push_back(std::make_unique<MaBackend>(ma_backend_jack, "jack", "JACK", false));
  v.push_back(std::make_unique<MaBackend>(ma_backend_pulseaudio, "pulse", "PipeWire / PulseAudio", false));
  v.push_back(std::make_unique<MaBackend>(ma_backend_alsa, "alsa", "ALSA", false));
#endif
  return v;
}
}
