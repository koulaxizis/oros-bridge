// A test audio module (MIT): what the bridge sees of the ASIO module, on
// every platform. One card "Module loopback", 3 inputs, 2 outputs: input 1/2
// is output 1/2 again kDelay frames later, input 3 is output 2 at a quarter.
// Its own thread calls the bridge in real time.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "../src/audio_module.h"

#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
constexpr uint32_t kDelay = 100, kIn = 3, kOut = 2;

struct Stream {
  oros_audio_cb cb;
  void* user;
  uint32_t rate, period;
  std::atomic<bool> quit{false};
  std::thread th;
  void run() {
    std::vector<float> hist(size_t(kDelay + period) * kOut, 0.f), in(size_t(period) * kIn), out(size_t(period) * kOut);
    const size_t n = hist.size() / kOut;
    uint64_t t0 = 0;
    auto next = std::chrono::steady_clock::now();
    const auto step = std::chrono::nanoseconds(uint64_t(period) * 1000000000ull / rate);
    while (!quit) {
      for (uint32_t f = 0; f < period; f++) {
        uint64_t t = t0 + f;
        for (uint32_t c = 0; c < kIn; c++) {
          float v = t >= kDelay ? hist[((t - kDelay) % n) * kOut + (c == 0 ? 0 : 1)] : 0.f;
          in[size_t(f) * kIn + c] = c == 2 ? v * 0.25f : v;
        }
      }
      cb(user, in.data(), kIn, out.data(), kOut, period);
      for (uint32_t f = 0; f < period; f++)
        for (uint32_t c = 0; c < kOut; c++) hist[((t0 + f) % n) * kOut + c] = out[size_t(f) * kOut + c];
      t0 += period;
      next += step;
      std::this_thread::sleep_until(next);
    }
  }
};

uint32_t devices(char* json, uint32_t cap, char*, uint32_t) {
  const char* s = R"({"inputs":[{"name":"Module loopback","channels":3,"default":true}],"outputs":[{"name":"Module loopback","channels":2,"default":true}]})";
  uint32_t need = uint32_t(std::strlen(s) + 1);
  if (need <= cap) std::memcpy(json, s, need);
  return need;
}

void* open(const char* input, const char* output, uint32_t sr, uint32_t period, oros_audio_cb cb, void* user,
           oros_audio_stream_info* info, char* err, uint32_t errCap) {
  for (const char* n : {input, output})
    if (n && *n && std::string(n) != "Module loopback") {
      std::snprintf(err, errCap, "no such card: %s", n);
      return nullptr;
    }
  auto* s = new Stream{cb, user, sr ? sr : 44100, period < 16 ? 16 : period};
  *info = {s->rate, kIn, kOut, s->period, kDelay};
  s->th = std::thread([s] { s->run(); });
  return s;
}

void close(void* h) {
  auto* s = static_cast<Stream*>(h);
  s->quit = true;
  s->th.join();
  delete s;
}

const oros_audio_module kModule = {OROS_AUDIO_MODULE_ABI, "modtest", "Module loopback (test)", &devices, &open, &close};
}

EXPORT const oros_audio_module* oros_audio_module_v1(uint32_t abi) { return abi == OROS_AUDIO_MODULE_ABI ? &kModule : nullptr; }
