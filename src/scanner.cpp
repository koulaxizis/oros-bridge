// The scanner list, one scan at a time, and the test scanner (--test-scanner).
#include "scanner.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace ob {

void to_json(nlohmann::json& j, const ScannerInfo& s) {
  j = {{"id", s.id}, {"name", s.name}, {"vendor", s.vendor}, {"model", s.model},
       {"sources", s.sources}, {"modes", s.modes}, {"dpi", s.dpi}};
}

namespace {

// The test scanner: a glass, a feeder of 3 sheets, both sides. A page is
// 2 × 3 inches; every sample is known, so the test checks each one:
//   colour: (x + 2y + 50c + 30p) & 255, grey: (x + y + 30p) & 255,
//   line art: black (0) where (x/8 + y/8) is odd, white (255) elsewhere.
// p counts the pages of this scan from 0.
class TestScanner : public ScannerBackend {
public:
  std::string id() const override { return "test"; }
  bool list(std::vector<ScannerInfo>& out, ScanError&) override {
    ScannerInfo s;
    s.id = "test:loop";
    s.name = "orOS Test Scanner";
    s.vendor = "orOS";
    s.model = "Test Scanner";
    s.sources = {"flatbed", "adf", "adf-duplex"};
    s.modes = {"color", "gray", "lineart"};
    s.dpi = {75, 150, 300, 600};
    out.push_back(s);
    return true;
  }
  bool scan(const std::string& id, const ScanRequest& req, ScanSink& sink, ScanError& err) override {
    if (id != "loop") { err = {"gone", "no such scanner"}; return false; }
    uint32_t pages = req.source == "flatbed" ? 1 : req.source == "adf" ? 3 : 6;
    uint32_t ch = req.mode == "color" ? 3 : 1, w = req.dpi * 2, h = req.dpi * 3;
    std::vector<uint8_t> strip;
    for (uint32_t p = 0; p < pages; p++) {
      sink.begin(w, ch);
      const uint32_t step = 64;
      for (uint32_t y0 = 0; y0 < h; y0 += step) {
        if (sink.cancelled()) return true;
        uint32_t n = std::min(step, h - y0);
        strip.resize(size_t(w) * ch * n);
        size_t k = 0;
        for (uint32_t y = y0; y < y0 + n; y++)
          for (uint32_t x = 0; x < w; x++)
            for (uint32_t c = 0; c < ch; c++) {
              uint32_t v;
              if (req.mode == "color") v = (x + 2 * y + 50 * c + 30 * p) & 255;
              else if (req.mode == "gray") v = (x + y + 30 * p) & 255;
              else v = ((x / 8 + y / 8) & 1) ? 0 : 255;
              strip[k++] = uint8_t(v);
            }
        sink.rows(strip.data(), n);
        sink.progress(int(uint64_t(y0 + n) * 100 / h));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));   // a scanner takes its time
      }
      sink.end();
    }
    return true;
  }
};

bool gTest = false;
std::mutex gListMu, gScanMu;
std::vector<ScannerInfo> gList;
std::vector<ScanError> gNotes;
bool gListed = false;

std::vector<ScannerBackend*> backends() {
  static TestScanner test;
  std::vector<ScannerBackend*> v;
  if (gTest) v.push_back(&test);
  if (auto* p = platformScanner()) v.push_back(p);
  return v;
}
}

void enableTestScanner() { gTest = true; }

std::vector<ScannerInfo> listScanners(bool full, std::vector<ScanError>* notes) {
  std::lock_guard<std::mutex> g(gListMu);
  if (!gListed || full) {
    // a scan holds the scanner: keep the last list rather than wait for it
    std::unique_lock<std::mutex> s(gScanMu, std::try_to_lock);
    if (s.owns_lock() || !gListed) {
      if (!s.owns_lock()) s.lock();
      std::vector<ScannerInfo> all;
      std::vector<ScanError> missing;
      for (auto* b : backends()) {
        std::vector<ScannerInfo> one;
        ScanError e;
        if (b->list(one, e)) {
          for (auto& x : one) {
            if (x.id.rfind(b->id() + ":", 0) != 0) x.id = b->id() + ":" + x.id;
            all.push_back(x);
          }
        } else {
          missing.push_back(e);
        }
      }
      gList = all;
      gNotes = missing;
      gListed = true;
    }
  }
  if (notes) *notes = gNotes;
  return gList;
}

size_t knownScanners() {
  std::lock_guard<std::mutex> g(gListMu);
  return gList.size();
}

bool runScan(const std::string& id, const ScanRequest& req, ScanSink& sink, ScanError& err) {
  auto colon = id.find(':');
  if (colon == std::string::npos) { err = {"gone", "no such scanner"}; return false; }
  std::string bid = id.substr(0, colon), own = id.substr(colon + 1);
  ScannerBackend* b = nullptr;
  for (auto* x : backends())
    if (x->id() == bid) b = x;
  if (!b) { err = {"gone", "no such scanner"}; return false; }
  std::unique_lock<std::mutex> s(gScanMu, std::try_to_lock);
  if (!s.owns_lock()) { err = {"busy", "another scan is running"}; return false; }
  return b->scan(own, req, sink, err);
}
}
