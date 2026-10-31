// "orOS Test ASIO": a loopback ASIO driver for the tests only (never shipped).
// 4 inputs, 2 outputs, 32-bit integer samples, a 240-frame buffer. Input 1/2
// is output 1/2 again kDelay frames later, input 3/4 the same at half level;
// the latencies it reports add up to kDelay. Its own thread calls the host
// in real time, as a sound card would.
//
// Copyright (C) 2026 Christos Koulaxizis. GPLv3, see asio/LICENSE. Built with
// the Steinberg ASIO SDK 2.3.4 (GPLv3). ASIO is a trademark and software of
// Steinberg Media Technologies GmbH.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

// {F20028E5-5EE8-4D40-AE0B-AB0066A32676}; tests/protocol.test.js registers it
static const CLSID kClsid = {0xf20028e5, 0x5ee8, 0x4d40, {0xae, 0x0b, 0xab, 0x00, 0x66, 0xa3, 0x26, 0x76}};

namespace {
constexpr long kIn = 4, kOut = 2, kFrames = 240, kDelay = 480;

void setSamples(ASIOSamples& s, uint64_t p) {
#if NATIVE_INT64
  s = ASIOSamples(p);
#else
  s.hi = static_cast<unsigned long>(p >> 32);
  s.lo = static_cast<unsigned long>(p & 0xffffffffu);
#endif
}

class TestAsio final : public IASIO {
public:
  // IUnknown; ASIO asks for its CLSID as the interface
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == kClsid) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (!n) delete this;
    return n;
  }

  ASIOBool init(void*) override { return ASIOTrue; }
  void getDriverName(char* name) override { std::strcpy(name, "orOS Test ASIO"); }
  long getDriverVersion() override { return 1; }
  void getErrorMessage(char* s) override { std::strcpy(s, ""); }
  ASIOError start() override {
    if (!cb_) return ASE_NotPresent;
    if (running_) return ASE_OK;
    running_ = true;
    th_ = std::thread([this] { run(); });
    return ASE_OK;
  }
  ASIOError stop() override {
    if (running_) {
      running_ = false;
      th_.join();
    }
    return ASE_OK;
  }
  ASIOError getChannels(long* in, long* out) override {
    *in = kIn;
    *out = kOut;
    return ASE_OK;
  }
  ASIOError getLatencies(long* in, long* out) override {
    *in = kDelay - kFrames;
    *out = kFrames;
    return ASE_OK;
  }
  ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
    *mn = *mx = *pref = kFrames;
    *gran = 0;
    return ASE_OK;
  }
  ASIOError canSampleRate(ASIOSampleRate r) override { return r == 44100.0 || r == 48000.0 ? ASE_OK : ASE_NoClock; }
  ASIOError getSampleRate(ASIOSampleRate* r) override {
    *r = rate_;
    return ASE_OK;
  }
  ASIOError setSampleRate(ASIOSampleRate r) override {
    if (canSampleRate(r) != ASE_OK) return ASE_NoClock;
    rate_ = r;
    return ASE_OK;
  }
  ASIOError getClockSources(ASIOClockSource* c, long* n) override {
    std::memset(c, 0, sizeof *c);
    c->isCurrentSource = ASIOTrue;
    std::strcpy(c->name, "Internal");
    *n = 1;
    return ASE_OK;
  }
  ASIOError setClockSource(long) override { return ASE_OK; }
  ASIOError getSamplePosition(ASIOSamples* s, ASIOTimeStamp* t) override {
    setSamples(*s, pos_.load());
    std::memset(t, 0, sizeof *t);
    return ASE_OK;
  }
  ASIOError getChannelInfo(ASIOChannelInfo* info) override {
    if (info->channel < 0 || info->channel >= (info->isInput ? kIn : kOut)) return ASE_InvalidParameter;
    info->isActive = ASIOTrue;
    info->channelGroup = 0;
    info->type = ASIOSTInt32LSB;
    std::snprintf(info->name, sizeof info->name, "%s %ld", info->isInput ? "In" : "Out", info->channel + 1);
    return ASE_OK;
  }
  ASIOError createBuffers(ASIOBufferInfo* infos, long n, long size, ASIOCallbacks* cb) override {
    if (size != kFrames || n <= 0) return ASE_InvalidMode;
    disposeBuffers();
    for (long i = 0; i < n; i++) {
      ASIOBufferInfo& b = infos[i];
      if (b.channelNum < 0 || b.channelNum >= (b.isInput ? kIn : kOut)) return ASE_InvalidParameter;
      for (int h = 0; h < 2; h++) {
        mem_.emplace_back(size_t(kFrames), 0);
        b.buffers[h] = mem_.back().data();
      }
    }
    infos_.assign(infos, infos + n);
    cb_ = cb;
    timeInfo_ = cb->asioMessage && cb->asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1 &&
                cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    return ASE_OK;
  }
  ASIOError disposeBuffers() override {
    stop();
    mem_.clear();
    infos_.clear();
    cb_ = nullptr;
    return ASE_OK;
  }
  ASIOError controlPanel() override { return ASE_OK; }
  ASIOError future(long, void*) override { return ASE_InvalidParameter; }
  ASIOError outputReady() override { return ASE_OK; }

private:
  void run() {
    std::vector<int32_t> hist(size_t(kDelay + kFrames) * kOut, 0);
    const size_t n = hist.size() / kOut;
    auto next = std::chrono::steady_clock::now();
    const auto step = std::chrono::nanoseconds(int64_t(kFrames * 1e9 / rate_));
    long index = 0;
    while (running_) {
      uint64_t t0 = pos_.load();
      for (auto& b : infos_) {
        if (!b.isInput) continue;
        auto* dst = static_cast<int32_t*>(b.buffers[index]);
        for (long f = 0; f < kFrames; f++) {
          uint64_t t = t0 + f;
          int32_t v = t >= uint64_t(kDelay) ? hist[((t - kDelay) % n) * kOut + (b.channelNum & 1)] : 0;
          dst[f] = b.channelNum < 2 ? v : v / 2;
        }
      }
      if (timeInfo_) {
        ASIOTime tm;
        std::memset(&tm, 0, sizeof tm);
        tm.timeInfo.sampleRate = rate_;
        tm.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        setSamples(tm.timeInfo.samplePosition, t0);
        cb_->bufferSwitchTimeInfo(&tm, index, ASIOTrue);
      } else {
        cb_->bufferSwitch(index, ASIOTrue);
      }
      for (auto& b : infos_) {
        if (b.isInput) continue;
        auto* src = static_cast<const int32_t*>(b.buffers[index]);
        for (long f = 0; f < kFrames; f++) hist[((t0 + f) % n) * kOut + b.channelNum] = src[f];
      }
      pos_ = t0 + kFrames;
      index ^= 1;
      next += step;
      std::this_thread::sleep_until(next);
    }
  }
  std::atomic<ULONG> refs_{1};
  ASIOSampleRate rate_ = 48000.0;
  std::vector<std::vector<int32_t>> mem_;
  std::vector<ASIOBufferInfo> infos_;
  ASIOCallbacks* cb_ = nullptr;
  bool timeInfo_ = false;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> pos_{0};
  std::thread th_;
};

class Factory : public IClassFactory {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *ppv = this;
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* d = new TestAsio();
    HRESULT hr = d->QueryInterface(riid, ppv);
    d->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
Factory gFactory;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
  if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
  return gFactory.QueryInterface(riid, ppv);
}
STDAPI DllCanUnloadNow() { return S_FALSE; }
