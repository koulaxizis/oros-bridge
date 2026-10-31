#pragma once
// MIDI keyboards for a plugin live on the sound card (0.6.0 wave 3b).
// The bridge reads the port itself, so a note reaches the plugin in the
// card's own callback with no page in between: WinMM on Windows, the raw
// MIDI devices of ALSA (/dev/snd/midiC*D*) on Linux, no extra library.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ob {

// One channel message (note, controller, bend, pressure, program) and when it arrived.
struct MidiStamped {
  int64_t ns = 0;   // steady_clock, nanoseconds
  uint8_t status = 0, d1 = 0, d2 = 0;
};

// What a MIDI port's thread hands to the card's thread: one writer, one
// reader, no lock, no allocation. A full queue drops new messages.
class MidiQueue {
public:
  static constexpr uint32_t kSize = 1024;   // a power of two
  bool push(const MidiStamped& m) {
    uint32_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
    if (w - r >= kSize) return false;
    buf_[w & (kSize - 1)] = m;
    w_.store(w + 1, std::memory_order_release);
    return true;
  }
  bool pop(MidiStamped& m) {
    uint32_t r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
    if (r == w) return false;
    m = buf_[r & (kSize - 1)];
    r_.store(r + 1, std::memory_order_release);
    return true;
  }
  void clear() { r_.store(w_.load()); }

private:
  MidiStamped buf_[kSize];
  std::atomic<uint32_t> w_{0}, r_{0};
};

// Bytes from a port into channel messages: running status, 2- and 3-byte
// messages; system exclusive and real-time bytes are skipped.
class MidiParser {
public:
  // calls out(status, d1, d2) for every complete message
  template <class F>
  void feed(uint8_t b, F&& out) {
    if (b >= 0xF8) return;                       // real time (clock, start, stop…) may come anywhere
    if (b >= 0xF0) { status_ = 0; sysex_ = b == 0xF0; return; }   // system common ends running status
    if (b & 0x80) { status_ = b; n_ = 0; sysex_ = false; return; }
    if (sysex_ || !status_) return;
    data_[n_++] = b;
    uint8_t kind = status_ & 0xF0;
    int need = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
    if (n_ < need) return;
    out(status_, data_[0], need == 2 ? data_[1] : uint8_t(0));
    n_ = 0;
  }

private:
  uint8_t status_ = 0, data_[2] = {0, 0};
  int n_ = 0;
  bool sysex_ = false;
};

inline int64_t midiNow() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// An open MIDI input: its messages go into queue() until it is destroyed.
class MidiInput {
public:
  virtual ~MidiInput() = default;
  MidiQueue& queue() { return queue_; }
  // the test input (--test-audio): messages handed in by the page
  virtual bool inject(const std::vector<uint8_t>& bytes) { (void)bytes; return false; }

protected:
  void received(uint8_t b) {
    parser_.feed(b, [this](uint8_t s, uint8_t d1, uint8_t d2) { queue_.push({midiNow(), s, d1, d2}); });
  }
  MidiQueue queue_;
  MidiParser parser_;
};

// The MIDI inputs of this computer, by name (plus "Test MIDI" with --test-audio).
std::vector<std::string> midiInputs();
std::unique_ptr<MidiInput> openMidiInput(const std::string& name, std::string& err);
void enableTestMidi();
constexpr const char* kTestMidiName = "Test MIDI";
}
