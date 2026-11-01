// Scanners on Linux through SANE. libsane is loaded at run time (dlopen), so
// the bridge holds no SANE code and needs no SANE to build or run: without it
// the scanner list says "nosane". The few types below are written from the
// SANE standard (the interface every frontend and backend shares), version 1.
// SANE is not thread safe, so every call happens under one lock; scanner.cpp
// already keeps listing and scanning apart.
#include "scanner.h"
#if !defined(_WIN32)
#include <dlfcn.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace ob {
namespace {

// ---- the SANE standard, version 1 ----
using SANE_Int = int;
using SANE_Word = int;
using SANE_Bool = int;
using SANE_Byte = unsigned char;
using SANE_String_Const = const char*;
using SANE_Handle = void*;
using SANE_Status = int;
struct SANE_Device { SANE_String_Const name, vendor, model, type; };
struct SANE_Range { SANE_Word min, max, quant; };
struct SANE_Option_Descriptor {
  SANE_String_Const name, title, desc;
  int type, unit;
  SANE_Int size, cap;
  int constraint_type;
  union {
    const SANE_String_Const* string_list;
    const SANE_Word* word_list;   // [count, values…]
    const SANE_Range* range;
  } constraint;
};
struct SANE_Parameters {
  int format;
  SANE_Bool last_frame;
  SANE_Int bytes_per_line, pixels_per_line, lines, depth;
};
enum { ST_GOOD = 0, ST_UNSUPPORTED, ST_CANCELLED, ST_BUSY, ST_INVAL, ST_EOF, ST_JAMMED, ST_NO_DOCS,
       ST_COVER_OPEN, ST_IO_ERROR, ST_NO_MEM, ST_ACCESS_DENIED };
enum { TYPE_BOOL = 0, TYPE_INT, TYPE_FIXED, TYPE_STRING, TYPE_BUTTON, TYPE_GROUP };
enum { CONSTRAINT_NONE = 0, CONSTRAINT_RANGE, CONSTRAINT_WORD_LIST, CONSTRAINT_STRING_LIST };
enum { ACTION_GET = 0, ACTION_SET = 1 };
enum { FRAME_GRAY = 0, FRAME_RGB = 1 };
constexpr SANE_Int CAP_SOFT_SELECT = 1, CAP_INACTIVE = 32;

struct Sane {
  SANE_Status (*init)(SANE_Int*, void*);
  SANE_Status (*get_devices)(const SANE_Device***, SANE_Bool);
  SANE_Status (*open)(SANE_String_Const, SANE_Handle*);
  void (*close)(SANE_Handle);
  const SANE_Option_Descriptor* (*get_option_descriptor)(SANE_Handle, SANE_Int);
  SANE_Status (*control_option)(SANE_Handle, SANE_Int, int, void*, SANE_Int*);
  SANE_Status (*get_parameters)(SANE_Handle, SANE_Parameters*);
  SANE_Status (*start)(SANE_Handle);
  SANE_Status (*read)(SANE_Handle, SANE_Byte*, SANE_Int, SANE_Int*);
  void (*cancel)(SANE_Handle);
  SANE_String_Const (*strstatus)(SANE_Status);
  bool ok = false;
  std::string why;
};

std::mutex gMu;

Sane& sane() {
  static Sane s = [] {
    Sane x{};
    void* h = dlopen("libsane.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libsane.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { x.why = "SANE is not installed (libsane1)"; return x; }
    bool all = true;
    auto get = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(h, name));
      if (!fn) all = false;
    };
    get(x.init, "sane_init");
    get(x.get_devices, "sane_get_devices");
    get(x.open, "sane_open");
    get(x.close, "sane_close");
    get(x.get_option_descriptor, "sane_get_option_descriptor");
    get(x.control_option, "sane_control_option");
    get(x.get_parameters, "sane_get_parameters");
    get(x.start, "sane_start");
    get(x.read, "sane_read");
    get(x.cancel, "sane_cancel");
    get(x.strstatus, "sane_strstatus");
    if (!all) { x.why = "libsane is missing functions"; return x; }
    SANE_Int version = 0;
    SANE_Status st = x.init(&version, nullptr);
    if (st != ST_GOOD) { x.why = std::string("SANE did not start: ") + x.strstatus(st); return x; }
    x.ok = true;   // never sane_exit: the bridge keeps SANE until it ends
    return x;
  }();
  return s;
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}
bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

// The standard's names and the ones backends use instead
std::string sourceKind(const std::string& raw) {
  auto s = lower(raw);
  if (has(s, "duplex")) return "adf-duplex";
  if (has(s, "adf") || has(s, "feeder") || has(s, "document")) return "adf";
  if (has(s, "flatbed") || has(s, "platen") || has(s, "glass") || s == "normal") return "flatbed";
  return "";
}
std::string modeKind(const std::string& raw) {
  auto s = lower(raw);
  if (has(s, "color") || has(s, "colour")) return "color";
  if (has(s, "gray") || has(s, "grey")) return "gray";
  if (has(s, "lineart") || has(s, "binary") || has(s, "black") || has(s, "threshold")) return "lineart";
  return "";
}

ScanError statusError(SANE_Status st) {
  std::string t = sane().strstatus(st);
  switch (st) {
    case ST_BUSY: return {"busy", t};
    case ST_NO_DOCS: return {"nopaper", t};
    case ST_JAMMED: return {"jammed", t};
    case ST_COVER_OPEN: return {"cover", t};
    case ST_ACCESS_DENIED: return {"denied", t};
    case ST_UNSUPPORTED: return {"unsupported", t};
    case ST_INVAL: return {"unsupported", t};
    default: return {"other", t};
  }
}

// One open device, closed when it goes
struct Device {
  SANE_Handle h = nullptr;
  ~Device() { if (h) sane().close(h); }
  int find(const char* name, const SANE_Option_Descriptor** out) {
    for (SANE_Int i = 1; i < 1000; i++) {
      auto* d = sane().get_option_descriptor(h, i);
      if (!d) break;
      if (d->name && std::strcmp(d->name, name) == 0) {
        if (d->cap & CAP_INACTIVE) return -1;
        *out = d;
        return i;
      }
    }
    return -1;
  }
  std::vector<std::string> strings(const char* name) {
    const SANE_Option_Descriptor* d;
    std::vector<std::string> v;
    if (find(name, &d) < 0 || d->type != TYPE_STRING || d->constraint_type != CONSTRAINT_STRING_LIST) return v;
    for (auto* p = d->constraint.string_list; p && *p; p++) v.push_back(*p);
    return v;
  }
  bool setString(const char* name, const std::string& value) {
    const SANE_Option_Descriptor* d;
    int i = find(name, &d);
    if (i < 0 || d->type != TYPE_STRING || !(d->cap & CAP_SOFT_SELECT)) return false;
    std::vector<char> buf(std::max<size_t>(size_t(std::max(d->size, 1)), value.size() + 1), 0);
    std::memcpy(buf.data(), value.data(), value.size());
    SANE_Int info = 0;
    return sane().control_option(h, i, ACTION_SET, buf.data(), &info) == ST_GOOD;
  }
};

// The resolutions the scanner takes, in the steps people know
std::vector<uint32_t> resolutions(Device& dev) {
  static const uint32_t kUsual[] = {75, 100, 150, 200, 300, 400, 600, 1200};
  const SANE_Option_Descriptor* d;
  std::vector<uint32_t> v;
  if (dev.find("resolution", &d) < 0 || (d->type != TYPE_INT && d->type != TYPE_FIXED)) return {300};
  auto dpi = [&](SANE_Word w) { return d->type == TYPE_FIXED ? uint32_t(w >> 16) : uint32_t(w); };
  if (d->constraint_type == CONSTRAINT_WORD_LIST && d->constraint.word_list) {
    for (SANE_Int k = 1; k <= d->constraint.word_list[0]; k++) {
      uint32_t r = dpi(d->constraint.word_list[k]);
      if (r >= 50 && r <= 1200) v.push_back(r);
    }
  } else if (d->constraint_type == CONSTRAINT_RANGE && d->constraint.range) {
    uint32_t lo = dpi(d->constraint.range->min), hi = dpi(d->constraint.range->max);
    for (auto r : kUsual)
      if (r >= lo && r <= hi) v.push_back(r);
  }
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  if (v.empty()) v.push_back(300);
  return v;
}

bool setResolution(Device& dev, uint32_t want) {
  const SANE_Option_Descriptor* d;
  int i = dev.find("resolution", &d);
  if (i < 0) return true;   // a scanner with one resolution
  SANE_Word w = d->type == TYPE_FIXED ? SANE_Word(want << 16) : SANE_Word(want);
  SANE_Int info = 0;
  return sane().control_option(dev.h, i, ACTION_SET, &w, &info) == ST_GOOD;
}

class SaneScanner : public ScannerBackend {
public:
  std::string id() const override { return "sane"; }

  bool list(std::vector<ScannerInfo>& out, ScanError& err) override {
    std::lock_guard<std::mutex> g(gMu);
    auto& s = sane();
    if (!s.ok) { err = {"nosane", s.why}; return false; }
    const SANE_Device** devs = nullptr;
    SANE_Status st = s.get_devices(&devs, 0);
    if (st != ST_GOOD) { err = statusError(st); return false; }
    for (auto** p = devs; p && *p; p++) {
      const SANE_Device* d = *p;
      if (!d->name) continue;
      ScannerInfo info;
      info.id = d->name;
      info.vendor = d->vendor ? d->vendor : "";
      info.model = d->model ? d->model : "";
      info.name = info.vendor.empty() ? info.model : info.vendor + " " + info.model;
      if (info.name.empty()) info.name = d->name;
      Device dev;
      if (s.open(d->name, &dev.h) != ST_GOOD) dev.h = nullptr;
      if (dev.h) {
        for (auto& x : dev.strings("source")) {
          auto k = sourceKind(x);
          if (!k.empty() && std::find(info.sources.begin(), info.sources.end(), k) == info.sources.end()) info.sources.push_back(k);
        }
        for (auto& x : dev.strings("mode")) {
          auto k = modeKind(x);
          if (!k.empty() && std::find(info.modes.begin(), info.modes.end(), k) == info.modes.end()) info.modes.push_back(k);
        }
        info.dpi = resolutions(dev);
      } else {
        info.dpi = {300};   // busy now: what nearly every scanner takes
      }
      if (info.sources.empty()) info.sources = {"flatbed"};
      if (info.modes.empty()) info.modes = {"color"};
      out.push_back(info);
    }
    return true;
  }

  bool scan(const std::string& id, const ScanRequest& req, ScanSink& sink, ScanError& err) override {
    std::lock_guard<std::mutex> g(gMu);
    auto& s = sane();
    if (!s.ok) { err = {"nosane", s.why}; return false; }
    Device dev;
    SANE_Status st = s.open(id.c_str(), &dev.h);
    if (st != ST_GOOD) { dev.h = nullptr; err = statusError(st); return false; }

    // the backend's own words for what was asked
    auto sources = dev.strings("source");
    if (!sources.empty()) {
      std::string pick;
      for (auto& x : sources)
        if (sourceKind(x) == req.source) { pick = x; break; }
      if (pick.empty() || !dev.setString("source", pick)) { err = {"unsupported", "source " + req.source}; return false; }
    } else if (req.source != "flatbed") {
      err = {"unsupported", "source " + req.source};
      return false;
    }
    auto modes = dev.strings("mode");
    if (!modes.empty()) {
      std::string pick;
      for (auto& x : modes)
        if (modeKind(x) == req.mode) { pick = x; break; }
      if (pick.empty() || !dev.setString("mode", pick)) { err = {"unsupported", "mode " + req.mode}; return false; }
    }
    if (!setResolution(dev, req.dpi)) { err = {"unsupported", "resolution " + std::to_string(req.dpi)}; return false; }

    const bool feeder = req.source != "flatbed";
    std::vector<SANE_Byte> buf(1 << 16);
    std::vector<uint8_t> line, rows;
    for (int page = 0;; page++) {
      if (sink.cancelled()) { s.cancel(dev.h); return true; }
      st = s.start(dev.h);
      if (st == ST_NO_DOCS && feeder && page > 0) break;   // the feeder is empty: done
      if (st != ST_GOOD) { s.cancel(dev.h); err = statusError(st); return false; }
      SANE_Parameters p{};
      if (s.get_parameters(dev.h, &p) != ST_GOOD || p.pixels_per_line <= 0 || p.bytes_per_line <= 0) {
        s.cancel(dev.h);
        err = {"other", "the scanner did not say the page size"};
        return false;
      }
      if (p.format != FRAME_GRAY && p.format != FRAME_RGB) {
        s.cancel(dev.h);
        err = {"unsupported", "this scanner sends each colour separately; choose grey"};
        return false;
      }
      if (p.depth != 1 && p.depth != 8 && p.depth != 16) {
        s.cancel(dev.h);
        err = {"unsupported", "bit depth " + std::to_string(p.depth)};
        return false;
      }
      const uint32_t ch = p.format == FRAME_RGB ? 3 : 1, w = uint32_t(p.pixels_per_line);
      const size_t bpl = size_t(p.bytes_per_line);
      sink.begin(w, ch);
      line.clear();
      rows.clear();
      uint32_t y = 0, inRows = 0;
      const uint32_t stripRows = std::max<uint32_t>(1, uint32_t((256u << 10) / (size_t(w) * ch)));
      auto flush = [&] {
        if (inRows) sink.rows(rows.data(), inRows);
        rows.clear();
        inRows = 0;
      };
      for (;;) {
        if (sink.cancelled()) { s.cancel(dev.h); return true; }
        SANE_Int n = 0;
        st = s.read(dev.h, buf.data(), SANE_Int(buf.size()), &n);
        if (st == ST_EOF) break;
        if (st != ST_GOOD) { s.cancel(dev.h); err = statusError(st); return false; }
        line.insert(line.end(), buf.begin(), buf.begin() + n);
        size_t used = 0;
        while (line.size() - used >= bpl) {
          const uint8_t* src = line.data() + used;
          size_t at = rows.size();
          rows.resize(at + size_t(w) * ch);
          uint8_t* dst = rows.data() + at;
          if (p.depth == 8) {
            std::memcpy(dst, src, size_t(w) * ch);
          } else if (p.depth == 16) {   // host byte order; keep the high byte
            for (size_t k = 0; k < size_t(w) * ch; k++) {
              uint16_t v;
              std::memcpy(&v, src + 2 * k, 2);
              dst[k] = uint8_t(v >> 8);
            }
          } else {   // 1 bit: 1 is black
            for (size_t k = 0; k < size_t(w) * ch; k++) dst[k] = (src[k >> 3] & (0x80 >> (k & 7))) ? 0 : 255;
          }
          used += bpl;
          y++;
          if (++inRows >= stripRows) flush();
        }
        line.erase(line.begin(), line.begin() + used);
        if (p.lines > 0) sink.progress(int(std::min<uint64_t>(100, uint64_t(y) * 100 / uint32_t(p.lines))));
      }
      flush();
      sink.end();
      if (!feeder) break;
    }
    s.cancel(dev.h);   // ends the batch (the standard asks for it after the last page)
    return true;
  }
};
}

ScannerBackend* platformScanner() {
  static SaneScanner s;
  return &s;
}
}
#endif
