#include "midi.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <fstream>
#include <regex>
#include <sstream>
#endif

namespace ob {
namespace {
std::atomic<bool> gTestMidi{false};

class TestInput : public MidiInput {
public:
  bool inject(const std::vector<uint8_t>& bytes) override {
    for (auto b : bytes) received(b);
    return true;
  }
};

#ifdef _WIN32
std::string utf8(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// WinMM numbers its inputs; two devices with one name get " (2)", " (3)"…
std::vector<std::pair<std::string, UINT>> winInputs() {
  std::vector<std::pair<std::string, UINT>> out;
  std::map<std::string, int> seen;
  UINT n = midiInGetNumDevs();
  for (UINT i = 0; i < n; i++) {
    MIDIINCAPSW caps{};
    if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
    std::string name = utf8(caps.szPname);
    int k = ++seen[name];
    if (k > 1) name += " (" + std::to_string(k) + ")";
    out.push_back({name, i});
  }
  return out;
}

class WinInput : public MidiInput {
public:
  ~WinInput() override {
    if (h_) {
      midiInStop(h_);
      midiInReset(h_);
      midiInClose(h_);
    }
  }
  bool open(UINT id, std::string& err) {
    if (midiInOpen(&h_, id, DWORD_PTR(&WinInput::proc), DWORD_PTR(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
      h_ = nullptr;
      err = "the MIDI input could not be opened (is another program using it?)";
      return false;
    }
    midiInStart(h_);
    return true;
  }

private:
  static void CALLBACK proc(HMIDIIN, UINT msg, DWORD_PTR self, DWORD_PTR p1, DWORD_PTR) {
    if (msg != MIM_DATA) return;
    auto* in = reinterpret_cast<WinInput*>(self);
    uint8_t st = uint8_t(p1 & 0xFF), d1 = uint8_t((p1 >> 8) & 0x7F), d2 = uint8_t((p1 >> 16) & 0x7F);
    if (st < 0x80 || st >= 0xF0) return;
    in->queue_.push({midiNow(), st, d1, d2});
  }
  HMIDIIN h_ = nullptr;
};
#else
// /proc/asound/cards: " 1 [EIE            ]: USB-Audio - AKAI EIE"
std::map<int, std::string> cardNames() {
  std::map<int, std::string> out;
  std::ifstream f("/proc/asound/cards");
  std::string line;
  std::regex re(R"(^\s*(\d+)\s+\[[^\]]*\]:\s*[^-]*-\s*(.+?)\s*$)");
  std::smatch m;
  while (std::getline(f, line))
    if (std::regex_match(line, m, re)) out[std::stoi(m[1])] = m[2];
  return out;
}

// the raw MIDI devices, "<card name>" or "<card name> <n>" for a card's second port on
std::vector<std::pair<std::string, std::string>> linuxInputs() {
  std::vector<std::pair<std::string, std::string>> out;
  auto names = cardNames();
  std::vector<std::pair<int, int>> devs;
  if (DIR* d = opendir("/dev/snd")) {
    while (auto* e = readdir(d)) {
      int c = 0, n = 0;
      char tail = 0;
      if (std::sscanf(e->d_name, "midiC%dD%d%c", &c, &n, &tail) == 2) devs.push_back({c, n});
    }
    closedir(d);
  }
  std::sort(devs.begin(), devs.end());
  for (auto& [c, n] : devs) {
    std::string base = names.count(c) ? names[c] : "MIDI " + std::to_string(c);
    out.push_back({n ? base + " " + std::to_string(n + 1) : base, "/dev/snd/midiC" + std::to_string(c) + "D" + std::to_string(n)});
  }
  return out;
}

class RawInput : public MidiInput {
public:
  ~RawInput() override {
    quit_ = true;
    if (th_.joinable()) th_.join();
    if (fd_ >= 0) close(fd_);
  }
  bool open(const std::string& path, std::string& err) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
      err = "the MIDI input could not be opened (is another program using it?)";
      return false;
    }
    th_ = std::thread([this] { run(); });
    return true;
  }

private:
  void run() {
    uint8_t buf[256];
    while (!quit_) {
      pollfd p{fd_, POLLIN, 0};
      int r = poll(&p, 1, 50);
      if (r <= 0) continue;
      if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break;   // unplugged
      ssize_t n = read(fd_, buf, sizeof buf);
      for (ssize_t i = 0; i < n; i++) received(buf[i]);
    }
  }
  int fd_ = -1;
  std::atomic<bool> quit_{false};
  std::thread th_;
};
#endif
}

void enableTestMidi() { gTestMidi = true; }

std::vector<std::string> midiInputs() {
  std::vector<std::string> out;
  if (gTestMidi) out.push_back(kTestMidiName);
#ifdef _WIN32
  for (auto& d : winInputs()) out.push_back(d.first);
#else
  for (auto& d : linuxInputs()) out.push_back(d.first);
#endif
  return out;
}

std::unique_ptr<MidiInput> openMidiInput(const std::string& name, std::string& err) {
  if (gTestMidi && name == kTestMidiName) return std::make_unique<TestInput>();
#ifdef _WIN32
  for (auto& d : winInputs()) {
    if (d.first != name) continue;
    auto in = std::make_unique<WinInput>();
    if (!in->open(d.second, err)) return nullptr;
    return in;
  }
#else
  for (auto& d : linuxInputs()) {
    if (d.first != name) continue;
    auto in = std::make_unique<RawInput>();
    if (!in->open(d.second, err)) return nullptr;
    return in;
  }
#endif
  err = "no such MIDI input: " + name;
  return nullptr;
}
}
