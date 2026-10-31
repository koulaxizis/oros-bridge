#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

namespace ob {

// ---------- the sound card ----------
// A backend is one way of reaching sound cards: WASAPI on Windows; JACK,
// PulseAudio/PipeWire and ALSA on Linux (miniaudio, audio_ma.cpp). A separate
// library can add one more (audio_module.h): the ASIO module, asio/. "test" is a loopback card for
// the tests (--test-audio).

// One period of sound: `in` holds inCh interleaved floats per frame, `out`
// outCh; the callback fills `out` completely. It runs on the
// card's own thread and must not allocate, lock or wait.
using AudioCallback = std::function<void(const float* in, uint32_t inCh, float* out, uint32_t outCh, uint32_t frames)>;

struct StreamSpec {
  std::string input, output;   // device names from devices(); "" = the system's default
  uint32_t sampleRate = 0;     // 0 = the card's own rate
  uint32_t period = 256;       // frames per callback (a wish; the card may choose another)
  bool exclusive = false;      // WASAPI exclusive mode
};

// A running duplex stream. Closing (destroying) it stops the callback first.
class AudioStream {
public:
  virtual ~AudioStream() = default;
  uint32_t sampleRate = 0, inChannels = 0, outChannels = 0, period = 0;
  uint32_t latency = 0;   // frames from an output sample to its return on an input (best estimate)
};

class AudioBackend {
public:
  virtual ~AudioBackend() = default;
  virtual std::string id() const = 0;      // "wasapi", "jack", "pulse", "alsa", "test"
  virtual std::string label() const = 0;   // shown to the person
  virtual bool canExclusive() const { return false; }
  // {"inputs": [{name, channels, default}], "outputs": [...]}; false when the backend is not there now
  virtual bool devices(nlohmann::json& out, std::string& err) = 0;
  virtual std::unique_ptr<AudioStream> open(const StreamSpec& spec, AudioCallback cb, std::string& err) = 0;
};

// The backends of this system, in the order they are offered.
std::vector<AudioBackend*> audioBackends();
AudioBackend* findAudioBackend(const std::string& id);
void enableTestAudio();   // adds the loopback backend (tests only)
// also offer the sound system in this library (audio_module.h); before the first audioBackends()
void addAudioModule(const std::string& path);

class Plugin;

// ---------- a plugin live on an input (0.5.0) ----------
// While the card runs, the chosen inputs go through the plugin in the card's
// own callback and its sound is added to the outputs, so the person hears
// themselves through it (an amp, a reverb) with only the card's buffers and
// the plugin's latency in between. The plugin is a second instance activated
// for real time; the session opens and closes it on the main thread, and its
// process() runs only here. A take records the dry inputs unless `wet`: then
// the take's channels that are the plugin's inputs hold its sound instead
// (not moved by its latency; the page knows it).
struct LiveFx {
  Plugin* plugin = nullptr;        // owned by the session; process() on the card's thread only
  std::vector<uint32_t> inputs;    // 1 or 2 card inputs
  uint32_t out = 0;                // first output channel
  bool wet = false;
  uint32_t maxFrames = 0, pluginIns = 0, pluginOuts = 0;
  std::atomic<float> gain{1.f};
  std::atomic<bool> on{false};
  // the card's thread reports: its loudest output since the last look, too
  // slow (turned itself off), failed
  std::atomic<float> peak{0.f};
  std::atomic<bool> slow{false}, failed{false};
  // planar buffers, made before the plugin goes live
  std::vector<std::vector<float>> inBuf, outBuf;
  std::vector<const float*> inPtr;
  std::vector<float*> outPtr;
  // engine side
  std::atomic<bool> stopReq{false}, stopped{false};
  uint32_t late = 0;               // periods in a row the plugin took longer than the period
  void prepare(uint32_t frames, uint32_t ins, uint32_t outs);
};

// ---------- a take ----------
// One open card for one page: it plays what the page sent (prepared ahead,
// so the page never feeds the card in real time) and records chosen inputs
// from the same callback, so a recorded frame and the frame played with it
// share one clock. Recorded sound goes to a file through a writer thread.
class AudioEngine {
public:
  AudioEngine() = default;
  ~AudioEngine();
  bool open(AudioBackend& b, const StreamSpec& spec, const std::vector<uint32_t>& record, std::string& err);
  void close();
  bool isOpen() const { return stream_ != nullptr; }
  const AudioStream& stream() const { return *stream_; }
  const std::vector<uint32_t>& recordChannels() const { return rec_; }

  // the sound to play from the start, planar; only while stopped
  bool prepare(uint32_t frames, uint32_t channels, uint32_t outStart, std::string& err);
  bool fill(uint32_t offset, uint32_t frames, uint32_t channels, const uint8_t* planar, std::string& err);
  // start: from the next period on, play from frame 0 and record
  bool start(std::string& err);
  // stop: the take is complete (frames recorded, frames lost because the disk was slow)
  bool stop(uint64_t& frames, uint64_t& lost, std::string& err);
  bool running() const { return running_.load(); }
  uint64_t position() const { return pos_.load(); }
  // planar float32 of the last take, channels = recordChannels().size()
  bool readTake(uint64_t offset, uint32_t frames, std::vector<float>& planar, uint32_t& got, std::string& err);
  // the loudest sample of every input channel since the last call (for meters)
  std::vector<float> takePeaks();
  // put a plugin live on the inputs (nullptr takes it off); only while stopped.
  // Taking it off waits until the card's thread has let go of it.
  bool setLive(LiveFx* fx, std::string& err);
  LiveFx* live() const { return live_.load(); }

private:
  void callback(const float* in, uint32_t inCh, float* out, uint32_t outCh, uint32_t frames);
  void runLive(LiveFx& fx, const float* in, uint32_t inCh, float* out, uint32_t outCh, uint32_t frames);
  void detachLive();
  void writer();
  std::unique_ptr<AudioStream> stream_;
  std::vector<uint32_t> rec_;
  uint32_t inCh_ = 0, outCh_ = 0;
  // playing
  std::vector<float> play_;          // planar, playCh_ x playFrames_
  uint32_t playCh_ = 0, playOut_ = 0;
  uint64_t playFrames_ = 0;
  // recording: the callback writes a ring, the writer thread empties it into the file
  std::vector<float> ring_;
  size_t ringFrames_ = 0;
  std::atomic<uint64_t> wrote_{0}, read_{0}, lost_{0};
  std::atomic<bool> startReq_{false}, running_{false}, stopReq_{false}, writerQuit_{false};
  std::atomic<uint64_t> pos_{0}, calls_{0};
  std::thread writerThread_;
  std::FILE* file_ = nullptr;
  std::string path_;
  uint64_t takeFrames_ = 0;
  std::unique_ptr<std::atomic<float>[]> peaks_;
  std::atomic<LiveFx*> live_{nullptr};
  std::atomic<bool> inCallback_{false};
  std::vector<float> wet_;           // the plugin's sound per frame, for a wet take (interleaved, pluginOuts)
};

constexpr uint32_t kMaxRecordChannels = 32;
constexpr uint32_t kLiveLatePeriods = 8;     // a live plugin this many periods late in a row turns off
constexpr uint64_t kMaxPlayFloats = 1ull << 28;   // 1 GiB of sound to play
}
