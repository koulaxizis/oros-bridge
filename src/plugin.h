#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "config.h"

namespace ob {

// A MIDI message at a frame of the block (note on/off, controller, bend…).
struct MidiEvent {
  uint32_t offset;
  uint8_t status, d1, d2;
};

// A loaded plugin. Every call happens on the main thread (MainLoop), except
// process() and stopProcessing() of a plugin activated for real time: those
// run on the sound card's thread (live, see audio.h LiveFx), which must not
// allocate, lock or wait. Otherwise the bridge renders ahead of time
// (freeze): process() is called on the main thread block after block as fast
// as the plugin goes, never against a sound card clock.
class Plugin {
public:
  virtual ~Plugin() = default;
  // sampleRate and the largest block process() will get; false when it refuses.
  // realtime: process() will run on a sound card's thread (VST 3 kRealtime;
  // CLAP starts processing on that thread at the first process()).
  virtual bool activate(double sampleRate, uint32_t maxFrames, bool realtime, std::string& err) = 0;
  // real time only, on the sound card's thread: the last call before the
  // plugin leaves that thread (CLAP stop_processing)
  virtual void stopProcessing() {}
  virtual uint32_t inputs() const = 0;     // channels of the main input (0 for most instruments)
  virtual uint32_t outputs() const = 0;    // channels of the main output
  virtual uint32_t latency() = 0;          // frames the output lags the input
  virtual uint32_t tail() = 0;             // frames of sound after the input stops (capped)
  // in: inputs() channels, out: outputs() channels, frames <= maxFrames;
  // allocates nothing (real time), at most kMaxEvents events
  virtual bool process(const float* const* in, float* const* out, uint32_t frames,
                       const std::vector<MidiEvent>& events) = 0;
  virtual void reset() = 0;                // forget the sound so far (a new render)
  virtual bool getState(std::vector<uint8_t>& out) = 0;
  virtual bool setState(const std::vector<uint8_t>& in) = 0;
  // the plugin's own window; onClosed runs when the person closes it
  virtual bool hasEditor() = 0;
  virtual bool openEditor(const std::string& title, std::function<void()> onClosed, std::string& err) = 0;
  virtual void closeEditor() = 0;
  // the state changed since the last getState (the person turned a knob)
  virtual bool dirty() = 0;
};

std::unique_ptr<Plugin> openClap(const PluginInfo& info, std::string& err);
std::unique_ptr<Plugin> openVst3(const PluginInfo& info, std::string& err);
inline std::unique_ptr<Plugin> openPlugin(const PluginInfo& info, std::string& err) {
  if (info.format == "clap") return openClap(info, err);
  if (info.format == "vst3") return openVst3(info, err);
  err = "unknown format";
  return nullptr;
}

// What one file holds (called in a child process, see scan.cpp).
std::vector<PluginInfo> describeClap(const std::string& path, std::string& err);
std::vector<PluginInfo> describeVst3(const std::string& path, std::string& err);

constexpr uint32_t kMaxChannels = 8;
constexpr uint32_t kMaxEvents = 1024;   // per process() call; more are dropped
constexpr uint32_t kMaxTail = 30 * 192000;   // 30 s at 192 kHz
}
