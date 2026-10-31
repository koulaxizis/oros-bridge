#pragma once
// Scanners (0.9.0, DESIGN decision 16): a flatbed or a multifunction printer
// on this computer scans straight into orOS Scan PDF. WIA 2.0 on Windows
// (scanner_wia.cpp), SANE on Linux (scanner_sane.cpp, libsane loaded at run
// time, so no SANE code is in the bridge), and a test scanner (--test-scanner).
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace ob {

// What a scanner offers. `sources`: "flatbed", "adf", "adf-duplex";
// `modes`: "color", "gray", "lineart"; `dpi`: the resolutions it takes.
struct ScannerInfo {
  std::string id;          // "<backend>:<the backend's own name>"
  std::string name, vendor, model;
  std::vector<std::string> sources, modes;
  std::vector<uint32_t> dpi;
};
void to_json(nlohmann::json& j, const ScannerInfo& s);

struct ScanRequest {
  std::string source = "flatbed", mode = "color";
  uint32_t dpi = 300;
};

// Where the pages go. Every row is 8 bits per sample: 1 channel (grey, line
// art as 0/255) or 3 (RGB). A page's height may be known only at its end.
class ScanSink {
public:
  virtual ~ScanSink() = default;
  virtual void begin(uint32_t width, uint32_t channels) = 0;   // a new page
  virtual void rows(const uint8_t* data, uint32_t n) = 0;      // n whole rows
  virtual void end() = 0;                                      // the page is complete
  virtual void progress(int pct) = 0;                          // of the page being scanned
  virtual bool cancelled() = 0;
};

// A reason the page can show in the person's language, and the details.
struct ScanError {
  std::string code;   // busy, nopaper, jammed, cover, denied, gone, unsupported, nosane, other
  std::string text;
};

class ScannerBackend {
public:
  virtual ~ScannerBackend() = default;
  virtual std::string id() const = 0;   // "wia", "sane", "test"
  // false when the backend is not usable here (err says why)
  virtual bool list(std::vector<ScannerInfo>& out, ScanError& err) = 0;
  // scans one page (flatbed) or every page in the feeder; false on an error
  // (pages already sent stay sent). The id is the backend's own name.
  virtual bool scan(const std::string& id, const ScanRequest& req, ScanSink& sink, ScanError& err) = 0;
};

void enableTestScanner();   // --test-scanner: offers the test scanner (tests)

// The scanners of every backend. The list is kept; `full` looks again
// (network scanners can take seconds to answer). While a scan runs, the
// kept list is returned. `notes` gets one line per backend that is missing.
std::vector<ScannerInfo> listScanners(bool full, std::vector<ScanError>* notes = nullptr);
size_t knownScanners();   // how many the last look found (the bridge's window)
// One scan at a time on this computer: false with code "busy" otherwise.
bool runScan(const std::string& id, const ScanRequest& req, ScanSink& sink, ScanError& err);

// the platform's backend (WIA or SANE), or null
ScannerBackend* platformScanner();
}
