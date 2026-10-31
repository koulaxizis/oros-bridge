#include "audio.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include "config.h"

namespace ob {

std::vector<std::unique_ptr<AudioBackend>> makeSystemBackends();   // audio_ma.cpp

// ---------- the loopback card (tests) ----------
// 4 inputs, 2 outputs: input 1/2 is output 1/2 again kLoopDelay frames
// later, input 3/4 the same at half the level. Runs in real time.
namespace {
constexpr uint32_t kLoopDelay = 333;

class LoopStream : public AudioStream {
public:
  LoopStream(uint32_t sr, uint32_t per, AudioCallback cb) : cb_(std::move(cb)) {
    sampleRate = sr;
    inChannels = 4;
    outChannels = 2;
    period = per;
    latency = kLoopDelay;
    hist_.assign(size_t(kLoopDelay + per) * 2, 0.f);
    in_.assign(size_t(per) * 4, 0.f);
    out_.assign(size_t(per) * 2, 0.f);
    th_ = std::thread([this] { run(); });
  }
  ~LoopStream() override {
    quit_ = true;
    th_.join();
  }

private:
  void run() {
    const size_t n = hist_.size() / 2;
    auto next = std::chrono::steady_clock::now();
    const auto step = std::chrono::nanoseconds(uint64_t(period) * 1000000000ull / sampleRate);
    while (!quit_) {
      for (uint32_t f = 0; f < period; f++) {
        uint64_t t = frame_ + f;
        for (uint32_t c = 0; c < 4; c++) {
          float v = t >= kLoopDelay ? hist_[((t - kLoopDelay) % n) * 2 + (c & 1)] : 0.f;
          in_[size_t(f) * 4 + c] = c < 2 ? v : v * 0.5f;
        }
      }
      cb_(in_.data(), 4, out_.data(), 2, period);
      for (uint32_t f = 0; f < period; f++)
        for (uint32_t c = 0; c < 2; c++) hist_[((frame_ + f) % n) * 2 + c] = out_[size_t(f) * 2 + c];
      frame_ += period;
      next += step;
      std::this_thread::sleep_until(next);
    }
  }
  AudioCallback cb_;
  std::vector<float> hist_, in_, out_;
  uint64_t frame_ = 0;
  std::atomic<bool> quit_{false};
  std::thread th_;
};

class LoopBackend : public AudioBackend {
public:
  std::string id() const override { return "test"; }
  std::string label() const override { return "Loopback (test)"; }
  bool devices(nlohmann::json& out, std::string&) override {
    out = {{"inputs", {{{"name", "Loopback"}, {"channels", 4}, {"default", true}}}},
           {"outputs", {{{"name", "Loopback"}, {"channels", 2}, {"default", true}}}}};
    return true;
  }
  std::unique_ptr<AudioStream> open(const StreamSpec& s, AudioCallback cb, std::string& err) override {
    if ((!s.input.empty() && s.input != "Loopback") || (!s.output.empty() && s.output != "Loopback")) {
      err = "no such device";
      return nullptr;
    }
    uint32_t per = std::clamp<uint32_t>(s.period, 16, kLoopDelay);
    return std::make_unique<LoopStream>(s.sampleRate ? s.sampleRate : 48000, per, std::move(cb));
  }
};

std::mutex gMu;
std::vector<std::unique_ptr<AudioBackend>>* gBackends = nullptr;
bool gTest = false;

std::vector<std::unique_ptr<AudioBackend>>& all() {
  std::lock_guard<std::mutex> g(gMu);
  if (!gBackends) {
    gBackends = new std::vector<std::unique_ptr<AudioBackend>>(makeSystemBackends());
    if (gTest) gBackends->insert(gBackends->begin(), std::make_unique<LoopBackend>());
  }
  return *gBackends;
}
}

void enableTestAudio() { gTest = true; }

std::vector<AudioBackend*> audioBackends() {
  std::vector<AudioBackend*> out;
  for (auto& b : all()) out.push_back(b.get());
  return out;
}

AudioBackend* findAudioBackend(const std::string& id) {
  for (auto* b : audioBackends())
    if (b->id() == id) return b;
  return nullptr;
}

// ---------- the engine ----------
static std::string takePath() {
  static std::atomic<int> n{0};
  std::error_code ec;
  auto dir = std::filesystem::temp_directory_path(ec);
  if (ec) dir = ".";
  return (dir / ("oros-bridge-take-" + randomHex(8) + "-" + std::to_string(++n) + ".f32")).string();
}

static int seek64(std::FILE* f, uint64_t at) {
#ifdef _WIN32
  return _fseeki64(f, int64_t(at), SEEK_SET);
#else
  return fseeko(f, off_t(at), SEEK_SET);
#endif
}

AudioEngine::~AudioEngine() { close(); }

static std::mutex& fileMu() {
  static std::mutex m;
  return m;
}

bool AudioEngine::open(AudioBackend& b, const StreamSpec& spec, const std::vector<uint32_t>& record, std::string& err) {
  close();
  if (record.size() > kMaxRecordChannels) { err = "too many inputs"; return false; }
  rec_ = record;
  path_ = takePath();
  file_ = std::fopen(path_.c_str(), "w+b");
  if (!file_) { err = "cannot write the take file"; return false; }
  peaks_.reset(new std::atomic<float>[kMaxRecordChannels]);
  for (uint32_t c = 0; c < kMaxRecordChannels; c++) peaks_[c] = 0.f;
  // inChannels is known only once the card is open: the ring is sized after,
  // and the callback records nothing until start()
  stream_ = b.open(spec, [this](const float* in, uint32_t ic, float* out, uint32_t oc, uint32_t frames) { callback(in, ic, out, oc, frames); }, err);
  if (!stream_) { close(); return false; }
  inCh_ = stream_->inChannels;
  outCh_ = stream_->outChannels;
  for (auto c : rec_)
    if (c >= inCh_) { err = "the card has no input " + std::to_string(c + 1); close(); return false; }
  if (!rec_.empty()) {
    ringFrames_ = std::min<size_t>(size_t(stream_->sampleRate) * 8, (size_t(1) << 24) / rec_.size());
    ring_.assign(ringFrames_ * rec_.size(), 0.f);
  }
  writerQuit_ = false;
  writerThread_ = std::thread([this] { writer(); });
  return true;
}

void AudioEngine::close() {
  stream_.reset();   // the callback has stopped after this
  running_ = false;
  startReq_ = stopReq_ = false;
  if (writerThread_.joinable()) {
    writerQuit_ = true;
    writerThread_.join();
  }
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  ring_.clear();
  ring_.shrink_to_fit();
  play_.clear();
  play_.shrink_to_fit();
  playFrames_ = 0;
  takeFrames_ = 0;
}

void AudioEngine::callback(const float* in, uint32_t inCh, float* out, uint32_t outCh, uint32_t frames) {
  if (stopReq_.load(std::memory_order_acquire) && running_.load()) running_ = false;
  if (startReq_.load(std::memory_order_acquire) && !running_.load()) {
    pos_ = 0;
    running_ = true;
    startReq_ = false;
  }
  for (uint32_t c = 0; c < inCh && c < kMaxRecordChannels; c++) {
    float pk = 0.f;
    for (uint32_t f = 0; f < frames; f++) pk = std::max(pk, std::fabs(in[size_t(f) * inCh + c]));
    if (pk > peaks_[c].load(std::memory_order_relaxed)) peaks_[c].store(pk, std::memory_order_relaxed);
  }
  std::memset(out, 0, sizeof(float) * frames * outCh);
  if (running_.load()) {
    uint64_t p = pos_.load();
    if (playCh_ && p < playFrames_) {
      uint32_t n = uint32_t(std::min<uint64_t>(frames, playFrames_ - p));
      for (uint32_t k = 0; k < playCh_; k++) {
        uint32_t oc = playOut_ + k;
        if (oc >= outCh) break;
        const float* src = play_.data() + size_t(k) * playFrames_ + p;
        for (uint32_t f = 0; f < n; f++) out[size_t(f) * outCh + oc] = src[f];
      }
      if (playCh_ == 1 && playOut_ + 1 < outCh)   // mono: both sides
        for (uint32_t f = 0; f < n; f++) out[size_t(f) * outCh + playOut_ + 1] = out[size_t(f) * outCh + playOut_];
    }
    if (!rec_.empty()) {
      uint64_t w = wrote_.load(std::memory_order_relaxed), r = read_.load(std::memory_order_acquire);
      uint64_t room = ringFrames_ - (w - r);
      uint32_t n = uint32_t(std::min<uint64_t>(frames, room));
      const size_t nr = rec_.size();
      for (uint32_t f = 0; f < n; f++) {
        float* dst = ring_.data() + size_t((w + f) % ringFrames_) * nr;
        for (size_t k = 0; k < nr; k++) dst[k] = in[size_t(f) * inCh + rec_[k]];
      }
      wrote_.store(w + n, std::memory_order_release);
      if (n < frames) lost_ += frames - n;
    }
    pos_ += frames;
  }
  calls_++;
}

void AudioEngine::writer() {
  const size_t nr = rec_.size();
  while (!writerQuit_) {
    uint64_t r = read_.load(), w = wrote_.load(std::memory_order_acquire);
    if (nr && w > r) {
      size_t at = size_t(r % ringFrames_);
      size_t n = size_t(std::min<uint64_t>(w - r, ringFrames_ - at));
      {
        std::lock_guard<std::mutex> g(fileMu());
        std::fwrite(ring_.data() + at * nr, sizeof(float) * nr, n, file_);
      }
      read_.store(r + n, std::memory_order_release);
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool AudioEngine::prepare(uint32_t frames, uint32_t channels, uint32_t outStart, std::string& err) {
  if (!stream_) { err = "no sound card open"; return false; }
  if (running_ || startReq_) { err = "playing"; return false; }
  if (channels > 2 || (channels && outStart >= outCh_)) { err = "bad channels"; return false; }
  if (uint64_t(frames) * channels > kMaxPlayFloats) { err = "too long"; return false; }
  play_.assign(size_t(frames) * channels, 0.f);
  playFrames_ = frames;
  playCh_ = channels;
  playOut_ = outStart;
  return true;
}

bool AudioEngine::fill(uint32_t offset, uint32_t frames, uint32_t channels, const uint8_t* planar, std::string& err) {
  if (running_ || startReq_) { err = "playing"; return false; }
  if (channels != playCh_ || uint64_t(offset) + frames > playFrames_) { err = "bad block"; return false; }
  for (uint32_t k = 0; k < channels; k++)
    std::memcpy(play_.data() + size_t(k) * playFrames_ + offset, planar + size_t(k) * frames * 4, size_t(frames) * 4);
  return true;
}

bool AudioEngine::start(std::string& err) {
  if (!stream_) { err = "no sound card open"; return false; }
  if (running_ || startReq_) { err = "playing"; return false; }
  {
    std::lock_guard<std::mutex> g(fileMu());
    std::fflush(file_);
    if (seek64(file_, 0) != 0) { err = "take file"; return false; }
    // a new take: the file starts over (it is only ever read up to takeFrames_)
  }
  wrote_ = 0;
  read_ = 0;
  lost_ = 0;
  takeFrames_ = 0;
  stopReq_ = false;
  startReq_.store(true, std::memory_order_release);
  for (int i = 0; i < 300 && !running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!running_) {
    startReq_ = false;
    err = "the sound card does not run";
    return false;
  }
  return true;
}

bool AudioEngine::stop(uint64_t& frames, uint64_t& lost, std::string& err) {
  if (!stream_) { err = "no sound card open"; return false; }
  if (!running_) { err = "not playing"; return false; }
  stopReq_.store(true, std::memory_order_release);
  for (int i = 0; i < 100 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  running_ = false;   // a card that stopped calling back: it will not record any more anyway
  stopReq_ = false;
  // everything recorded reaches the file
  for (int i = 0; i < 500 && read_.load() < wrote_.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  {
    std::lock_guard<std::mutex> g(fileMu());
    std::fflush(file_);
  }
  takeFrames_ = read_.load();
  frames = takeFrames_;
  lost = lost_.load();
  return true;
}

bool AudioEngine::readTake(uint64_t offset, uint32_t frames, std::vector<float>& planar, uint32_t& got, std::string& err) {
  const size_t nr = rec_.size();
  if (!stream_ || !nr) { err = "no take"; return false; }
  if (running_) { err = "recording"; return false; }
  if (offset > takeFrames_) { err = "past the end"; return false; }
  got = uint32_t(std::min<uint64_t>(frames, takeFrames_ - offset));
  std::vector<float> inter(size_t(got) * nr);
  {
    std::lock_guard<std::mutex> g(fileMu());
    if (seek64(file_, offset * nr * sizeof(float)) != 0 ||
        std::fread(inter.data(), sizeof(float) * nr, got, file_) != got) {
      err = "take file";
      return false;
    }
    seek64(file_, 0);
  }
  planar.assign(size_t(got) * nr, 0.f);
  for (uint32_t f = 0; f < got; f++)
    for (size_t k = 0; k < nr; k++) planar[k * got + f] = inter[size_t(f) * nr + k];
  return true;
}

std::vector<float> AudioEngine::takePeaks() {
  std::vector<float> out;
  if (!stream_) return out;
  for (uint32_t c = 0; c < inCh_ && c < kMaxRecordChannels; c++) out.push_back(peaks_[c].exchange(0.f));
  return out;
}
}
