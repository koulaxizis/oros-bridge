// Scanners on Windows through WIA 2.0 (IWiaDevMgr2, Windows Vista and later):
// every scanner driver speaks it, and it is part of Windows. Each call runs on
// its own short-lived thread in the multithreaded apartment, so the WIA
// service's callbacks never wait for a message loop. A page comes as a BMP,
// read here into 8-bit rows.
#include "scanner.h"
#ifdef _WIN32
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <sti.h>
#include <wia.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace ob {
namespace {

std::string narrow(const wchar_t* w) {
  if (!w) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
std::wstring wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return L"";
  std::wstring w(size_t(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

template <class T> struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { if (p) p->Release(); }
  T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

// Runs f on a thread of its own in the multithreaded apartment
template <class F> void inMta(F&& f) {
  std::thread t([&] {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    f();
    if (SUCCEEDED(hr)) CoUninitialize();
  });
  t.join();
}

ScanError hrError(HRESULT hr) {
  char t[64];
  std::snprintf(t, sizeof t, "WIA error 0x%08lx", (unsigned long)hr);
  switch (hr) {
    case WIA_ERROR_BUSY: return {"busy", t};
    case WIA_ERROR_PAPER_EMPTY: return {"nopaper", t};
    case WIA_ERROR_PAPER_JAM: return {"jammed", t};
    case WIA_ERROR_COVER_OPEN: return {"cover", t};
    case WIA_ERROR_OFFLINE:
    case WIA_S_NO_DEVICE_AVAILABLE: return {"gone", t};
    case E_ACCESSDENIED: return {"denied", t};
    default: return {"other", t};
  }
}

bool readLong(IWiaPropertyStorage* ps, PROPID id, LONG& out) {
  PROPSPEC sp{};
  sp.ulKind = PRSPEC_PROPID;
  sp.propid = id;
  PROPVARIANT v;
  PropVariantInit(&v);
  bool ok = ps->ReadMultiple(1, &sp, &v) == S_OK && v.vt == VT_I4;
  if (ok) out = v.lVal;
  PropVariantClear(&v);
  return ok;
}
std::string readText(IWiaPropertyStorage* ps, PROPID id) {
  PROPSPEC sp{};
  sp.ulKind = PRSPEC_PROPID;
  sp.propid = id;
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string s;
  if (ps->ReadMultiple(1, &sp, &v) == S_OK && v.vt == VT_BSTR) s = narrow(v.bstrVal);
  PropVariantClear(&v);
  return s;
}
bool readGuid(IWiaPropertyStorage* ps, PROPID id, GUID& out) {
  PROPSPEC sp{};
  sp.ulKind = PRSPEC_PROPID;
  sp.propid = id;
  PROPVARIANT v;
  PropVariantInit(&v);
  bool ok = ps->ReadMultiple(1, &sp, &v) == S_OK && v.vt == VT_CLSID && v.puuid;
  if (ok) out = *v.puuid;
  PropVariantClear(&v);
  return ok;
}
HRESULT writeLong(IWiaPropertyStorage* ps, PROPID id, LONG value) {
  PROPSPEC sp{};
  sp.ulKind = PRSPEC_PROPID;
  sp.propid = id;
  PROPVARIANT v;
  PropVariantInit(&v);
  v.vt = VT_I4;
  v.lVal = value;
  return ps->WriteMultiple(1, &sp, &v, WIA_IPA_FIRST);
}
HRESULT writeGuid(IWiaPropertyStorage* ps, PROPID id, const GUID& g) {
  PROPSPEC sp{};
  sp.ulKind = PRSPEC_PROPID;
  sp.propid = id;
  PROPVARIANT v;
  PropVariantInit(&v);
  GUID copy = g;
  v.vt = VT_CLSID;
  v.puuid = &copy;
  return ps->WriteMultiple(1, &sp, &v, WIA_IPA_FIRST);
}
// The values a property may take: a list, or a range (min, max, step)
bool validLongs(IWiaPropertyStorage* ps, PROPID id, std::vector<LONG>& list, LONG& lo, LONG& hi, bool& isRange) {
  PROPSPEC sp{};
  sp.ulKind = PRSPEC_PROPID;
  sp.propid = id;
  ULONG flags = 0;
  PROPVARIANT v;
  PropVariantInit(&v);
  bool ok = false;
  if (ps->GetPropertyAttributes(1, &sp, &flags, &v) == S_OK && v.vt == (VT_VECTOR | VT_I4)) {
    if ((flags & WIA_PROP_LIST) && v.cal.cElems > WIA_LIST_VALUES) {
      for (ULONG i = WIA_LIST_VALUES; i < v.cal.cElems; i++) list.push_back(v.cal.pElems[i]);
      isRange = false;
      ok = true;
    } else if ((flags & WIA_PROP_RANGE) && v.cal.cElems > WIA_RANGE_MAX) {
      lo = v.cal.pElems[WIA_RANGE_MIN];
      hi = v.cal.pElems[WIA_RANGE_MAX];
      isRange = true;
      ok = true;
    }
  }
  PropVariantClear(&v);
  return ok;
}

Com<IWiaDevMgr2>* newManager(HRESULT& hr) {
  auto* m = new Com<IWiaDevMgr2>();
  hr = CoCreateInstance(CLSID_WiaDevMgr2, nullptr, CLSCTX_LOCAL_SERVER, IID_IWiaDevMgr2, (void**)m->put());
  if (FAILED(hr)) { delete m; return nullptr; }
  return m;
}

// The scanner's items: the glass and the feeder
struct Items {
  Com<IWiaItem2> root, flatbed, feeder;
  bool duplex = false;
};
HRESULT openItems(IWiaDevMgr2* mgr, const std::string& id, Items& it) {
  BSTR bid = SysAllocString(wide(id).c_str());
  HRESULT hr = mgr->CreateDevice(0, bid, it.root.put());
  SysFreeString(bid);
  if (FAILED(hr)) return hr;
  Com<IEnumWiaItem2> en;
  if (FAILED(hr = it.root->EnumChildItems(nullptr, en.put()))) return hr;
  for (;;) {
    IWiaItem2* child = nullptr;
    ULONG got = 0;
    if (en->Next(1, &child, &got) != S_OK || !got || !child) break;
    Com<IWiaItem2> c;
    *c.put() = child;
    Com<IWiaPropertyStorage> ps;
    if (FAILED(c->QueryInterface(IID_IWiaPropertyStorage, (void**)ps.put()))) continue;
    GUID cat{};
    if (!readGuid(ps.p, WIA_IPA_ITEM_CATEGORY, cat)) continue;
    if (cat == WIA_CATEGORY_FLATBED && !it.flatbed) {
      it.flatbed.p = c.p;
      c.p = nullptr;
    } else if (cat == WIA_CATEGORY_FEEDER && !it.feeder) {
      // duplex: the feeder item says so (WIA 2.0), else the device root (the same property id)
      LONG caps = 0;
      bool known = readLong(ps.p, WIA_DPS_DOCUMENT_HANDLING_CAPABILITIES, caps);
      if (!known) {
        Com<IWiaPropertyStorage> rps;
        if (SUCCEEDED(it.root->QueryInterface(IID_IWiaPropertyStorage, (void**)rps.put())))
          known = readLong(rps.p, WIA_DPS_DOCUMENT_HANDLING_CAPABILITIES, caps);
      }
      if (known) it.duplex = (caps & DUP) != 0;
      it.feeder.p = c.p;
      c.p = nullptr;
    }
  }
  return S_OK;
}

void describe(IWiaItem2* item, ScannerInfo& info) {
  static const uint32_t kUsual[] = {75, 100, 150, 200, 300, 400, 600, 1200};
  Com<IWiaPropertyStorage> ps;
  if (FAILED(item->QueryInterface(IID_IWiaPropertyStorage, (void**)ps.put()))) return;
  std::vector<LONG> list;
  LONG lo = 0, hi = 0;
  bool range = false;
  if (validLongs(ps.p, WIA_IPS_XRES, list, lo, hi, range)) {
    if (range) {
      for (auto r : kUsual)
        if (LONG(r) >= lo && LONG(r) <= hi) info.dpi.push_back(r);
    } else {
      for (auto r : list)
        if (r >= 50 && r <= 1200) info.dpi.push_back(uint32_t(r));
    }
  }
  list.clear();
  if (validLongs(ps.p, WIA_IPA_DATATYPE, list, lo, hi, range) && !range) {
    for (auto d : list) {
      const char* k = d == WIA_DATA_COLOR ? "color" : d == WIA_DATA_GRAYSCALE ? "gray" : d == WIA_DATA_THRESHOLD ? "lineart" : nullptr;
      if (k && std::find(info.modes.begin(), info.modes.end(), k) == info.modes.end()) info.modes.push_back(k);
    }
  }
}

// Receives each page as a BMP in memory
class Receiver : public IWiaTransferCallback {
public:
  explicit Receiver(ScanSink& sink) : sink_(sink) {}
  std::vector<Com<IStream>*> pages;
  ~Receiver() { for (auto* p : pages) delete p; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override { return --ref_; }   // lives on the scan's stack
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IWiaTransferCallback) {
      *out = static_cast<IWiaTransferCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE TransferCallback(LONG, WiaTransferParams* p) override {
    if (p && p->lMessage == WIA_TRANSFER_MSG_STATUS) sink_.progress(int(std::clamp<LONG>(p->lPercentComplete, 0, 100)));
    return sink_.cancelled() ? S_FALSE : S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNextStream(LONG, BSTR, BSTR, IStream** out) override {
    *out = nullptr;
    if (sink_.cancelled()) return S_FALSE;
    auto* s = new Com<IStream>();
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, s->put());
    if (FAILED(hr)) { delete s; return hr; }
    pages.push_back(s);
    s->p->AddRef();
    *out = s->p;
    return S_OK;
  }

private:
  ScanSink& sink_;
  std::atomic<ULONG> ref_{1};
};

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// A BMP (file header, BITMAPINFOHEADER or later, 1/4/8-bit with a palette, 24 or 32-bit) → rows
bool sendBmp(IStream* s, bool wantGray, ScanSink& sink, ScanError& err) {
  HGLOBAL hg = nullptr;
  STATSTG st{};
  if (FAILED(GetHGlobalFromStream(s, &hg)) || FAILED(s->Stat(&st, STATFLAG_NONAME))) { err = {"other", "no picture"}; return false; }
  size_t n = size_t(st.cbSize.QuadPart);
  const uint8_t* b = (const uint8_t*)GlobalLock(hg);
  if (!b) { err = {"other", "no picture"}; return false; }
  struct Unlock { HGLOBAL h; ~Unlock() { GlobalUnlock(h); } } unlock{hg};
  if (n < 54 || b[0] != 'B' || b[1] != 'M') { err = {"unsupported", "the scanner did not send a BMP"}; return false; }
  uint32_t off = le32(b + 10), hsz = le32(b + 14);
  int32_t w = int32_t(le32(b + 18)), h = int32_t(le32(b + 22));
  uint16_t bpp = le16(b + 28);
  uint32_t comp = le32(b + 30), used = le32(b + 46);
  bool topDown = h < 0;
  uint32_t W = uint32_t(w), H = uint32_t(topDown ? -h : h);
  if (w <= 0 || H == 0 || (comp != 0 && comp != 3) || !(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32)) {
    err = {"unsupported", "BMP " + std::to_string(bpp) + " bit"};
    return false;
  }
  size_t stride = ((size_t(W) * bpp + 31) / 32) * 4;
  if (off > n || stride * H > n - off) {   // some drivers leave the height out of a feeder page; use what came
    H = off <= n ? uint32_t((n - off) / stride) : 0;
    if (!H) { err = {"other", "the picture is cut short"}; return false; }
  }
  // the palette, as grey or colour
  uint8_t pal[256][3] = {};
  bool palGray = true;
  if (bpp <= 8) {
    uint32_t count = used ? std::min<uint32_t>(used, 256) : (1u << bpp);
    const uint8_t* q = b + 14 + hsz;
    for (uint32_t i = 0; i < count && q + 4 * i + 3 < b + n; i++) {
      pal[i][0] = q[4 * i + 2];
      pal[i][1] = q[4 * i + 1];
      pal[i][2] = q[4 * i];
      if (pal[i][0] != pal[i][1] || pal[i][1] != pal[i][2]) palGray = false;
    }
  }
  const uint32_t ch = (bpp <= 8 && palGray) || wantGray ? 1 : 3;
  sink.begin(W, ch);
  const uint32_t stripRows = std::max<uint32_t>(1, uint32_t((256u << 10) / (size_t(W) * ch)));
  std::vector<uint8_t> rows;
  rows.reserve(size_t(W) * ch * stripRows);
  uint32_t inRows = 0;
  for (uint32_t y = 0; y < H; y++) {
    const uint8_t* src = b + off + stride * (topDown ? y : H - 1 - y);
    for (uint32_t x = 0; x < W; x++) {
      uint8_t r, g, bl;
      if (bpp == 24 || bpp == 32) {
        const uint8_t* px = src + size_t(x) * (bpp / 8);
        bl = px[0]; g = px[1]; r = px[2];
      } else {
        uint32_t idx = bpp == 8 ? src[x] : bpp == 4 ? (src[x >> 1] >> ((x & 1) ? 0 : 4)) & 15 : (src[x >> 3] >> (7 - (x & 7))) & 1;
        r = pal[idx][0]; g = pal[idx][1]; bl = pal[idx][2];
      }
      if (ch == 1) rows.push_back(uint8_t((r * 299 + g * 587 + bl * 114 + 500) / 1000));
      else { rows.push_back(r); rows.push_back(g); rows.push_back(bl); }
    }
    if (++inRows >= stripRows || y + 1 == H) {
      sink.rows(rows.data(), inRows);
      rows.clear();
      inRows = 0;
    }
    if (sink.cancelled()) return true;
  }
  sink.end();
  return true;
}

class WiaScanner : public ScannerBackend {
public:
  std::string id() const override { return "wia"; }

  bool list(std::vector<ScannerInfo>& out, ScanError& err) override {
    bool ok = false;
    inMta([&] {
      HRESULT hr;
      std::unique_ptr<Com<IWiaDevMgr2>> mgr(newManager(hr));
      if (!mgr) { err = hrError(hr); return; }
      Com<IEnumWIA_DEV_INFO> en;
      if (FAILED(hr = (*mgr)->EnumDeviceInfo(WIA_DEVINFO_ENUM_LOCAL, en.put()))) { err = hrError(hr); return; }
      ok = true;
      for (;;) {
        IWiaPropertyStorage* raw = nullptr;
        ULONG got = 0;
        if (en->Next(1, &raw, &got) != S_OK || !got || !raw) break;
        Com<IWiaPropertyStorage> ps;
        *ps.put() = raw;
        LONG type = 0;
        if (!readLong(ps.p, WIA_DIP_DEV_TYPE, type) || GET_STIDEVICE_TYPE(type) != StiDeviceTypeScanner) continue;
        ScannerInfo info;
        info.id = readText(ps.p, WIA_DIP_DEV_ID);
        info.name = readText(ps.p, WIA_DIP_DEV_NAME);
        info.vendor = readText(ps.p, WIA_DIP_VEND_DESC);
        info.model = info.name;
        if (info.id.empty()) continue;
        if (info.name.empty()) info.name = info.id;
        Items it;
        if (SUCCEEDED(openItems(mgr->p, info.id, it))) {
          if (it.flatbed) info.sources.push_back("flatbed");
          if (it.feeder) {
            info.sources.push_back("adf");
            if (it.duplex) info.sources.push_back("adf-duplex");
          }
          IWiaItem2* first = it.flatbed ? it.flatbed.p : it.feeder.p;
          if (first) describe(first, info);
        }
        if (info.sources.empty()) info.sources = {"flatbed"};
        if (info.modes.empty()) info.modes = {"color", "gray", "lineart"};
        if (info.dpi.empty()) info.dpi = {300};
        out.push_back(info);
      }
    });
    return ok;
  }

  bool scan(const std::string& id, const ScanRequest& req, ScanSink& sink, ScanError& err) override {
    bool ok = false;
    inMta([&] {
      HRESULT hr;
      std::unique_ptr<Com<IWiaDevMgr2>> mgr(newManager(hr));
      if (!mgr) { err = hrError(hr); return; }
      Items it;
      if (FAILED(hr = openItems(mgr->p, id, it))) { err = hrError(hr); return; }
      const bool feeder = req.source != "flatbed";
      IWiaItem2* item = feeder ? it.feeder.p : it.flatbed.p;
      if (!item && !feeder) item = it.root.p;   // a scanner with no child items scans from its root
      if (!item || (req.source == "adf-duplex" && !it.duplex)) { err = {"unsupported", "source " + req.source}; return; }
      Com<IWiaPropertyStorage> ps;
      if (FAILED(hr = item->QueryInterface(IID_IWiaPropertyStorage, (void**)ps.put()))) { err = hrError(hr); return; }
      LONG type = req.mode == "color" ? WIA_DATA_COLOR : req.mode == "gray" ? WIA_DATA_GRAYSCALE : WIA_DATA_THRESHOLD;
      LONG depth = req.mode == "color" ? 24 : req.mode == "gray" ? 8 : 1;
      if (FAILED(hr = writeLong(ps.p, WIA_IPA_DATATYPE, type))) { err = {"unsupported", "mode " + req.mode}; return; }
      writeLong(ps.p, WIA_IPA_DEPTH, depth);   // some drivers set it from the data type themselves
      if (FAILED(writeLong(ps.p, WIA_IPS_XRES, LONG(req.dpi))) || FAILED(writeLong(ps.p, WIA_IPS_YRES, LONG(req.dpi)))) {
        err = {"unsupported", "resolution " + std::to_string(req.dpi)};
        return;
      }
      writeGuid(ps.p, WIA_IPA_FORMAT, WiaImgFmt_BMP);
      writeLong(ps.p, WIA_IPA_TYMED, TYMED_FILE);
      if (feeder) {
        writeLong(ps.p, WIA_IPS_DOCUMENT_HANDLING_SELECT, req.source == "adf-duplex" ? (DUPLEX | FRONT_FIRST) : FRONT_ONLY);
        writeLong(ps.p, WIA_IPS_PAGES, ALL_PAGES);
      }
      Com<IWiaTransfer> tr;
      if (FAILED(hr = item->QueryInterface(IID_IWiaTransfer, (void**)tr.put()))) { err = hrError(hr); return; }
      Receiver rx(sink);
      hr = tr->Download(0, &rx);
      if (sink.cancelled()) { ok = true; return; }
      // the feeder ran out after the last page: that is the end, not an error
      bool emptyAfterPages = feeder && hr == WIA_ERROR_PAPER_EMPTY && !rx.pages.empty();
      if (FAILED(hr) && !emptyAfterPages) { err = hrError(hr); return; }
      if (rx.pages.empty()) { err = feeder ? ScanError{"nopaper", "no pages"} : ScanError{"other", "no picture came"}; return; }
      for (auto* p : rx.pages) {
        if (!sendBmp(p->p, req.mode != "color", sink, err)) return;
        if (sink.cancelled()) break;
      }
      ok = true;
    });
    return ok;
  }
};
}

ScannerBackend* platformScanner() {
  static WiaScanner s;
  return &s;
}
}
#endif
