// oros-bridge-asio.dll: ASIO sound cards for orOS Bridge.
//
// Copyright (C) 2026 Christos Koulaxizis
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, version 3 of the License (or, at your option, any
// later version). It is distributed WITHOUT ANY WARRANTY; see asio/LICENSE.
//
// It is built with the Steinberg ASIO SDK 2.3.4, used under its GPLv3
// licence. ASIO is a trademark and software of Steinberg Media Technologies
// GmbH. The MIT bridge loads this library only when it lies beside the exe;
// the whole boundary is src/audio_module.h.
//
// ASIO allows one driver per process and wants every call on the thread that
// loaded it, so one worker thread (COM single-threaded apartment, with a
// hidden window as the driver's sysRef) makes every driver call; the
// driver's own thread calls bufferSwitch, which only converts samples and
// calls the bridge.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include "../src/audio_module.h"

extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace {

// ---------- the worker thread ----------
class Worker {
public:
  static Worker& get() {
    static Worker* w = new Worker();   // lives until the process ends
    return *w;
  }
  void run(const std::function<void()>& f) {
    if (GetCurrentThreadId() == tid_) { f(); return; }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
      std::lock_guard<std::mutex> g(mu_);
      q_.push_back([&] {
        f();
        std::lock_guard<std::mutex> g2(m);
        done = true;
        cv.notify_one();
      });
    }
    SetEvent(ev_);
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return done; });
  }
  HWND window() const { return wnd_; }

private:
  Worker() {
    ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::mutex m;
    std::condition_variable cv;
    bool up = false;
    std::thread([&] {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      tid_ = GetCurrentThreadId();
      wnd_ = CreateWindowExW(0, L"STATIC", L"orOS Bridge ASIO", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
      {
        std::lock_guard<std::mutex> g(m);
        up = true;
        cv.notify_one();
      }
      loop();
    }).detach();
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return up; });
  }
  void loop() {
    for (;;) {
      MsgWaitForMultipleObjects(1, &ev_, FALSE, INFINITE, QS_ALLINPUT);
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      for (;;) {
        std::function<void()> f;
        {
          std::lock_guard<std::mutex> g(mu_);
          if (q_.empty()) break;
          f = std::move(q_.front());
          q_.pop_front();
        }
        f();
      }
    }
  }
  HANDLE ev_ = nullptr;
  HWND wnd_ = nullptr;
  DWORD tid_ = 0;
  std::mutex mu_;
  std::deque<std::function<void()>> q_;
};

void setErr(char* err, uint32_t cap, const std::string& s) {
  if (err && cap) std::snprintf(err, cap, "%s", s.c_str());
}

// ---------- samples ----------
bool supported(ASIOSampleType t) {
  switch (t) {
    case ASIOSTInt16LSB: case ASIOSTInt24LSB: case ASIOSTInt32LSB: case ASIOSTFloat32LSB: case ASIOSTFloat64LSB:
    case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
      return true;
    default:
      return false;
  }
}

int32_t toInt(float v, double scale) {
  double d = std::clamp(double(v), -1.0, 1.0) * scale;
  return int32_t(std::lrint(std::clamp(d, -scale - 1.0, scale)));
}

// one channel of the card's buffer -> every frame's sample `ch` of `dst` (stride = channels)
void readNative(ASIOSampleType t, const void* src, float* dst, uint32_t stride, uint32_t frames) {
  const auto* b = static_cast<const uint8_t*>(src);
  for (uint32_t f = 0; f < frames; f++) {
    float v = 0.f;
    switch (t) {
      case ASIOSTInt16LSB: { int16_t x; std::memcpy(&x, b + f * 2, 2); v = float(x / 32768.0); break; }
      case ASIOSTInt24LSB: {
        int32_t x = int32_t(uint32_t(b[f * 3]) << 8 | uint32_t(b[f * 3 + 1]) << 16 | uint32_t(b[f * 3 + 2]) << 24) >> 8;
        v = float(x / 8388608.0);
        break;
      }
      case ASIOSTInt32LSB: { int32_t x; std::memcpy(&x, b + f * 4, 4); v = float(x / 2147483648.0); break; }
      case ASIOSTInt32LSB16: { int32_t x; std::memcpy(&x, b + f * 4, 4); v = float(x / 32768.0); break; }
      case ASIOSTInt32LSB18: { int32_t x; std::memcpy(&x, b + f * 4, 4); v = float(x / 131072.0); break; }
      case ASIOSTInt32LSB20: { int32_t x; std::memcpy(&x, b + f * 4, 4); v = float(x / 524288.0); break; }
      case ASIOSTInt32LSB24: { int32_t x; std::memcpy(&x, b + f * 4, 4); v = float(x / 8388608.0); break; }
      case ASIOSTFloat32LSB: std::memcpy(&v, b + f * 4, 4); break;
      case ASIOSTFloat64LSB: { double x; std::memcpy(&x, b + f * 8, 8); v = float(x); break; }
      default: break;
    }
    dst[size_t(f) * stride] = v;
  }
}

void writeNative(ASIOSampleType t, const float* src, uint32_t stride, void* dst, uint32_t frames) {
  auto* b = static_cast<uint8_t*>(dst);
  for (uint32_t f = 0; f < frames; f++) {
    float v = src[size_t(f) * stride];
    switch (t) {
      case ASIOSTInt16LSB: { int16_t x = int16_t(toInt(v, 32767.0)); std::memcpy(b + f * 2, &x, 2); break; }
      case ASIOSTInt24LSB: {
        int32_t x = toInt(v, 8388607.0);
        b[f * 3] = uint8_t(x);
        b[f * 3 + 1] = uint8_t(x >> 8);
        b[f * 3 + 2] = uint8_t(x >> 16);
        break;
      }
      case ASIOSTInt32LSB: { int32_t x = toInt(v, 2147483647.0); std::memcpy(b + f * 4, &x, 4); break; }
      case ASIOSTInt32LSB16: { int32_t x = toInt(v, 32767.0); std::memcpy(b + f * 4, &x, 4); break; }
      case ASIOSTInt32LSB18: { int32_t x = toInt(v, 131071.0); std::memcpy(b + f * 4, &x, 4); break; }
      case ASIOSTInt32LSB20: { int32_t x = toInt(v, 524287.0); std::memcpy(b + f * 4, &x, 4); break; }
      case ASIOSTInt32LSB24: { int32_t x = toInt(v, 8388607.0); std::memcpy(b + f * 4, &x, 4); break; }
      case ASIOSTFloat32LSB: std::memcpy(b + f * 4, &v, 4); break;
      case ASIOSTFloat64LSB: { double x = v; std::memcpy(b + f * 8, &x, 8); break; }
      default: break;
    }
  }
}

// ---------- the open stream (one per process: ASIO loads one driver) ----------
struct Stream {
  oros_audio_cb cb = nullptr;
  void* user = nullptr;
  long inCh = 0, outCh = 0, frames = 0;
  std::vector<ASIOBufferInfo> bufs;
  std::vector<ASIOSampleType> types;
  std::vector<float> in, out;
  bool postOutput = false;
  std::atomic<bool> started{false};
};
std::atomic<Stream*> gStream{nullptr};   // set and cleared on the worker thread; read by the driver's thread while started
ASIOCallbacks gCallbacks;

void process(long index) {
  Stream* s = gStream.load(std::memory_order_acquire);
  if (!s || !s->started.load(std::memory_order_acquire)) return;
  for (long c = 0; c < s->inCh; c++)
    readNative(s->types[c], s->bufs[c].buffers[index], s->in.data() + c, uint32_t(s->inCh), uint32_t(s->frames));
  s->cb(s->user, s->in.data(), uint32_t(s->inCh), s->out.data(), uint32_t(s->outCh), uint32_t(s->frames));
  for (long c = 0; c < s->outCh; c++)
    writeNative(s->types[s->inCh + c], s->out.data() + c, uint32_t(s->outCh), s->bufs[s->inCh + c].buffers[index], uint32_t(s->frames));
  if (s->postOutput) ASIOOutputReady();
}

void onBufferSwitch(long index, ASIOBool) { process(index); }
ASIOTime* onBufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) {
  process(index);
  return t;
}
void onSampleRate(ASIOSampleRate) {}
long onMessage(long selector, long value, void*, double*) {
  switch (selector) {
    case kAsioSelectorSupported:
      return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioResyncRequest ||
             value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo;
    case kAsioEngineVersion: return 2;
    case kAsioResetRequest: return 1;   // the take goes on; the next open sees the driver's new settings
    case kAsioResyncRequest: return 1;
    case kAsioLatenciesChanged: return 1;
    case kAsioSupportsTimeInfo: return 1;
    default: return 0;
  }
}

// ---------- drivers ----------
// read again from the registry each time (only while no driver is loaded), so a
// driver installed while the bridge runs shows up
std::vector<std::string> driverNames() {
  delete asioDrivers;
  asioDrivers = new AsioDrivers();
  std::vector<std::string> out;
  constexpr long kMax = 64;
  std::vector<std::vector<char>> store(kMax, std::vector<char>(64, 0));
  std::vector<char*> ptrs;
  for (auto& s : store) ptrs.push_back(s.data());
  long n = asioDrivers->getDriverNames(ptrs.data(), kMax);
  for (long i = 0; i < n && i < kMax; i++) out.push_back(ptrs[i]);
  return out;
}

bool loadAndInit(const std::string& name, std::string& err) {
  std::vector<char> buf(name.begin(), name.end());
  buf.push_back(0);
  if (!loadAsioDriver(buf.data())) { err = "the ASIO driver " + name + " could not be loaded"; return false; }
  ASIODriverInfo info{};
  info.asioVersion = 2;
  info.sysRef = Worker::get().window();
  if (ASIOInit(&info) != ASE_OK) {
    err = std::string("the ASIO driver ") + name + " did not start: " + info.errorMessage;
    asioDrivers->removeCurrentDriver();
    return false;
  }
  return true;
}

void unload() {
  ASIOExit();
  if (asioDrivers) asioDrivers->removeCurrentDriver();
}

nlohmann::json gLastList;   // what devices() said last; given again while a stream is open

uint32_t asioDevices(char* json, uint32_t cap, char* err, uint32_t errCap) {
  std::string out;
  Worker::get().run([&] {
    if (gStream) {
      out = gLastList.is_null() ? R"({"inputs":[],"outputs":[]})" : gLastList.dump();
      return;
    }
    nlohmann::json ins = nlohmann::json::array(), outs = nlohmann::json::array();
    for (auto& name : driverNames()) {
      std::string e;
      if (!loadAndInit(name, e)) continue;   // a driver whose card is not there
      long ni = 0, no = 0;
      if (ASIOGetChannels(&ni, &no) == ASE_OK) {
        bool first = ins.empty() && outs.empty();
        if (ni > 0) ins.push_back({{"name", name}, {"channels", ni}, {"default", first}});
        if (no > 0) outs.push_back({{"name", name}, {"channels", no}, {"default", first}});
      }
      unload();
    }
    gLastList = {{"inputs", ins}, {"outputs", outs}};
    out = gLastList.dump();
  });
  uint32_t need = uint32_t(out.size() + 1);
  if (need <= cap) std::memcpy(json, out.c_str(), need);
  (void)err;
  (void)errCap;
  return need;
}

void closeOnWorker(Stream* s) {
  if (s->started.load()) ASIOStop();
  s->started = false;
  if (!s->bufs.empty()) ASIODisposeBuffers();
  unload();
  gStream = nullptr;
  delete s;
}

void* asioOpen(const char* input, const char* output, uint32_t sampleRate, uint32_t, oros_audio_cb cb, void* user,
           oros_audio_stream_info* info, char* err, uint32_t errCap) {
  void* result = nullptr;
  Worker::get().run([&] {
    if (gStream) { setErr(err, errCap, "ASIO is already in use by another page"); return; }
    std::string in = input ? input : "", out = output ? output : "";
    if (!in.empty() && !out.empty() && in != out) {
      setErr(err, errCap, "with ASIO one card records and plays: choose the same card for input and output");
      return;
    }
    std::string name = in.empty() ? out : in;
    auto names = driverNames();
    if (name.empty()) {
      if (names.empty()) { setErr(err, errCap, "no ASIO driver is installed"); return; }
      name = names.front();
    } else if (std::find(names.begin(), names.end(), name) == names.end()) {
      setErr(err, errCap, "no such ASIO driver: " + name);
      return;
    }
    std::string e;
    if (!loadAndInit(name, e)) { setErr(err, errCap, e); return; }
    auto* s = new Stream();
    s->cb = cb;
    s->user = user;
    gStream = s;
    auto fail = [&](const std::string& why) {
      setErr(err, errCap, why);
      closeOnWorker(s);
    };
    if (ASIOGetChannels(&s->inCh, &s->outCh) != ASE_OK) return fail("the card did not say its channels");
    ASIOSampleRate rate = 0;
    if (sampleRate) {
      if (ASIOCanSampleRate(ASIOSampleRate(sampleRate)) != ASE_OK)
        return fail("the card cannot run at " + std::to_string(sampleRate) + " Hz");
      if (ASIOSetSampleRate(ASIOSampleRate(sampleRate)) != ASE_OK)
        return fail("the card did not switch to " + std::to_string(sampleRate) + " Hz");
    }
    if (ASIOGetSampleRate(&rate) != ASE_OK || rate <= 0) {
      for (double r : {48000.0, 44100.0})
        if (ASIOCanSampleRate(r) == ASE_OK && ASIOSetSampleRate(r) == ASE_OK) { rate = r; break; }
      if (rate <= 0) return fail("the card did not say its sample rate");
    }
    // the size set in the driver's own control panel: ASIO players expect it
    long mn = 0, mx = 0, pref = 0, gran = 0;
    if (ASIOGetBufferSize(&mn, &mx, &pref, &gran) != ASE_OK || pref <= 0) return fail("the card did not say its buffer size");
    s->frames = pref;
    long total = s->inCh + s->outCh;
    if (total <= 0) return fail("the card has no channels");
    s->bufs.resize(size_t(total));
    s->types.resize(size_t(total));
    for (long i = 0; i < total; i++) {
      ASIOBufferInfo& b = s->bufs[size_t(i)];
      b.isInput = i < s->inCh ? ASIOTrue : ASIOFalse;
      b.channelNum = i < s->inCh ? i : i - s->inCh;
      b.buffers[0] = b.buffers[1] = nullptr;
      ASIOChannelInfo ci{};
      ci.channel = b.channelNum;
      ci.isInput = b.isInput;
      if (ASIOGetChannelInfo(&ci) != ASE_OK) { s->bufs.clear(); return fail("the card did not describe its channels"); }
      if (!supported(ci.type)) { s->bufs.clear(); return fail("this card's sample format is not supported (type " + std::to_string(ci.type) + ")"); }
      s->types[size_t(i)] = ci.type;
    }
    gCallbacks.bufferSwitch = &onBufferSwitch;
    gCallbacks.sampleRateDidChange = &onSampleRate;
    gCallbacks.asioMessage = &onMessage;
    gCallbacks.bufferSwitchTimeInfo = &onBufferSwitchTimeInfo;
    if (ASIOCreateBuffers(s->bufs.data(), total, s->frames, &gCallbacks) != ASE_OK) {
      s->bufs.clear();
      return fail("the card refused the buffers");
    }
    s->in.assign(size_t(s->frames) * size_t(std::max<long>(s->inCh, 1)), 0.f);
    s->out.assign(size_t(s->frames) * size_t(std::max<long>(s->outCh, 1)), 0.f);
    for (long i = 0; i < s->outCh; i++)
      for (int h = 0; h < 2; h++) writeNative(s->types[size_t(s->inCh + i)], s->out.data(), 1, s->bufs[size_t(s->inCh + i)].buffers[h], uint32_t(s->frames));
    s->postOutput = ASIOOutputReady() == ASE_OK;
    long li = 0, lo = 0;
    if (ASIOGetLatencies(&li, &lo) != ASE_OK) li = lo = s->frames;
    s->started = true;   // before start: the driver may call bufferSwitch at once
    if (ASIOStart() != ASE_OK) {
      s->started = false;
      return fail("the card did not start");
    }
    info->sampleRate = uint32_t(std::lrint(rate));
    info->inChannels = uint32_t(s->inCh);
    info->outChannels = uint32_t(s->outCh);
    info->period = uint32_t(s->frames);
    info->latency = uint32_t(std::max<long>(li, 0) + std::max<long>(lo, 0));
    result = s;
  });
  return result;
}

void asioClose(void* h) {
  if (!h) return;
  Worker::get().run([&] {
    if (gStream.load() == h) closeOnWorker(static_cast<Stream*>(h));
  });
}

const oros_audio_module kModule = {OROS_AUDIO_MODULE_ABI, "asio", "ASIO", &asioDevices, &asioOpen, &asioClose};
}

extern "C" __declspec(dllexport) const oros_audio_module* oros_audio_module_v1(uint32_t hostAbi) {
  return hostAbi == OROS_AUDIO_MODULE_ABI ? &kModule : nullptr;
}
