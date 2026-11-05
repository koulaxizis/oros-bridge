// Watched folders (0.11.0): see watch.h and DESIGN.md decision 19.
#include "watch.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include "sha256.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ob {
namespace fs = std::filesystem;

void to_json(nlohmann::json& j, const WatchedFile& f) {
  j = {{"key", f.key}, {"folder", f.folder}, {"name", f.name}, {"kind", f.kind},
       {"size", f.size}, {"mtime", f.mtime}, {"seen", f.seen}};
}

namespace watch {
namespace {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
  for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}
bool endsWith(const std::string& s, const char* t) {
  size_t n = std::strlen(t);
  return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
}

#ifdef _WIN32
std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
#endif

// A plain file (not a link, a folder or a device): its size and time.
bool statFile(const fs::path& p, uint64_t& size, int64_t& mtime) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &a)) return false;
  if (a.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return false;
  size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  ULARGE_INTEGER t;
  t.LowPart = a.ftLastWriteTime.dwLowDateTime;
  t.HighPart = a.ftLastWriteTime.dwHighDateTime;
  mtime = int64_t(t.QuadPart / 10000) - 11644473600000LL;   // 100 ns since 1601 -> ms since 1970
  return true;
#else
  struct stat st;
  if (lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  size = uint64_t(st.st_size);
  mtime = int64_t(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
  return true;
#endif
}

enum class State { known, pending, reported };
struct Entry {
  uint64_t size = 0;
  int64_t mtime = 0;
  State state = State::known;
};
struct Folder {
  std::string path;
  int64_t upto = 0;     // files newer than this, found at start, were added while the bridge was off
  bool primed = false;  // looked once
  std::map<std::string, Entry> files;
};

std::mutex mu;
Config* cfg = nullptr;
std::vector<Folder> list;
std::vector<WatchedFile> ring;            // the last kRecent reported files, oldest first
std::map<int, std::function<void(const WatchedFile&)>> listeners;
int nextListener = 1;
std::atomic<int> ver{1};
std::string testPrintDir, testPick;
std::atomic<bool> picking{false};
int64_t savedAt = 0;
bool started = false;

void persist() {   // with mu held
  if (!cfg) return;
  auto j = nlohmann::json::array();
  for (auto& f : list) j.push_back({{"path", f.path}, {"upto", f.upto}});
  cfg->setWatchList(j);
  savedAt = nowMs();
}

std::string keyOf(const std::string& folder, const std::string& name, uint64_t size, int64_t mtime) {
  std::string s = folder + "\n" + name + "\n" + std::to_string(size) + "\n" + std::to_string(mtime);
  return sha256Hex(s).substr(0, 16);
}

struct Seen { std::string name; uint64_t size; int64_t mtime; };
// The folder's plain files right now (no folders below, at most 20000).
bool listFolder(const std::string& folder, std::vector<Seen>& out) {
  std::error_code ec;
  fs::path dir = fs::u8path(folder);
  if (!fs::is_directory(dir, ec)) return false;
  fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  if (ec) return false;
  for (; it != end && out.size() < 20000; it.increment(ec)) {
    if (ec) break;
    std::string name = it->path().filename().u8string();
    if (ignored(name)) continue;
    Seen s{name, 0, 0};
    if (!statFile(it->path(), s.size, s.mtime)) continue;
    out.push_back(std::move(s));
  }
  return true;
}

void report(Folder& f, const std::string& name, const Entry& e, std::vector<WatchedFile>& news) {
  WatchedFile w;
  w.folder = f.path;
  w.name = name;
  w.kind = kindOf(name);
  w.size = e.size;
  w.mtime = e.mtime;
  w.seen = nowMs();
  w.key = keyOf(f.path, name, e.size, e.mtime);
  ring.erase(std::remove_if(ring.begin(), ring.end(), [&](const WatchedFile& r) { return r.key == w.key; }), ring.end());
  ring.push_back(w);
  while (ring.size() > kRecent) ring.erase(ring.begin());
  news.push_back(w);
}

void look() {
  std::vector<std::string> paths;
  {
    std::lock_guard<std::mutex> g(mu);
    for (auto& f : list) paths.push_back(f.path);
  }
  for (auto& path : paths) {
    std::vector<Seen> seen;
    bool ok = listFolder(path, seen);
    int64_t lookedAt = nowMs();
    std::vector<WatchedFile> news;
    {
      std::lock_guard<std::mutex> g(mu);
      auto fi = std::find_if(list.begin(), list.end(), [&](const Folder& f) { return f.path == path; });
      if (fi == list.end() || !ok) continue;   // taken off meanwhile, or not there now (a USB stick): kept as it was
      Folder& f = *fi;
      std::set<std::string> here;
      for (auto& s : seen) {
        here.insert(s.name);
        auto e = f.files.find(s.name);
        if (e == f.files.end()) {
          Entry n{s.size, s.mtime, State::pending};
          if (!f.primed && s.mtime <= f.upto) n.state = State::known;   // there before: the baseline
          f.files.emplace(s.name, n);
          continue;
        }
        Entry& x = e->second;
        bool same = x.size == s.size && x.mtime == s.mtime;
        if (!same) {   // still being written, or written again
          x.size = s.size;
          x.mtime = s.mtime;
          if (x.state == State::reported) x.state = State::pending;   // the same name with new content (a scanner's scan.pdf)
          continue;
        }
        if (x.state == State::pending && x.size > 0) {   // the same size and time twice: finished
          x.state = State::reported;
          report(f, s.name, x, news);
        }
      }
      for (auto it = f.files.begin(); it != f.files.end();) it = here.count(it->first) ? std::next(it) : f.files.erase(it);
      f.primed = true;
      f.upto = lookedAt - 2000;   // a file written during this look is newer than this
      if (!news.empty() || lookedAt - savedAt > 60000) persist();
      for (auto& w : news)
        for (auto& l : listeners) l.second(w);
    }
  }
}

int intervalMs() {
  const char* e = std::getenv("OROS_BRIDGE_WATCH_MS");   // tests look faster
  int v = e ? std::atoi(e) : 0;
  return v >= 50 && v <= 60000 ? v : 2000;
}

#ifndef _WIN32
// Runs a program (no shell) and gives its exit code, -1 when it could not start;
// out gets what it wrote, when asked.
int runProgram(const std::vector<std::string>& argv, std::string* out) {
  int pipefd[2] = {-1, -1};
  if (out && pipe(pipefd) != 0) return -1;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (out) {
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
  }
  std::vector<char*> a;
  for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
  a.push_back(nullptr);
  pid_t pid;
  int rc = posix_spawnp(&pid, a[0], &fa, nullptr, a.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (out) close(pipefd[1]);
  if (rc != 0) {
    if (out) close(pipefd[0]);
    return -1;
  }
  if (out) {
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0 && out->size() < 65536) out->append(buf, size_t(n));
    close(pipefd[0]);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  if (!WIFEXITED(st)) return -1;
  return WEXITSTATUS(st);
}
#endif

std::function<void(const std::string&, std::function<void(int, std::string)>)> picker;   // the window's own (GTK)
}  // namespace

std::string kindOf(const std::string& name) {
  std::string n = lower(name);
  if (endsWith(n, ".pdf")) return "pdf";
  for (const char* e : {".jpg", ".jpeg", ".png", ".gif", ".webp", ".heic", ".heif", ".bmp", ".tif", ".tiff", ".avif"})
    if (endsWith(n, e)) return "image";
  return "other";
}

bool ignored(const std::string& name) {
  if (name.empty() || name[0] == '.' || name.rfind("~$", 0) == 0) return true;
  std::string n = lower(name);
  if (n == "thumbs.db" || n == "desktop.ini") return true;
  for (const char* e : {".tmp", ".temp", ".part", ".partial", ".crdownload", ".download", ".opdownload", ".swp", "~"})
    if (endsWith(n, e)) return true;
  return false;
}

void start(Config& c) {
  {
    std::lock_guard<std::mutex> g(mu);
    if (started) return;
    started = true;
    cfg = &c;
    for (auto& j : c.watchList()) {
      if (!j.is_object() || !j.contains("path") || !j["path"].is_string()) continue;
      Folder f;
      f.path = j["path"];
      f.upto = j.value("upto", int64_t(0));
      if (f.path.empty() || list.size() >= kMaxFolders) continue;
      if (std::none_of(list.begin(), list.end(), [&](const Folder& x) { return x.path == f.path; })) list.push_back(f);
    }
  }
  std::thread([] {
    int ms = intervalMs();
    for (;;) {
      look();
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
  }).detach();
}

std::vector<std::string> folders() {
  std::lock_guard<std::mutex> g(mu);
  std::vector<std::string> v;
  for (auto& f : list) v.push_back(f.path);
  return v;
}

int version() { return ver.load(); }

bool add(const std::string& path, std::string& err) {
  if (path.empty() || path.size() > 1024) { err = "bad folder"; return false; }
  std::error_code ec;
  fs::path p = fs::u8path(path);
  if (!p.is_absolute() || !fs::is_directory(p, ec)) { err = "not a folder"; return false; }
  std::string norm = p.lexically_normal().u8string();
  if (norm.size() > 1 && (norm.back() == '/' || norm.back() == '\\') && !(norm.size() == 3 && norm[1] == ':')) norm.pop_back();
  std::lock_guard<std::mutex> g(mu);
  for (auto& f : list)
    if (f.path == norm) return true;
  if (list.size() >= kMaxFolders) { err = "too many folders"; return false; }
  Folder f;
  f.path = norm;
  f.upto = nowMs();   // what is there now is the baseline
  list.push_back(f);
  persist();
  ver++;
  return true;
}

bool remove(const std::string& path) {
  std::lock_guard<std::mutex> g(mu);
  auto n = list.size();
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Folder& f) { return f.path == path; }), list.end());
  if (list.size() == n) return false;
  ring.erase(std::remove_if(ring.begin(), ring.end(), [&](const WatchedFile& r) { return r.folder == path; }), ring.end());
  persist();
  ver++;
  return true;
}

std::vector<WatchedFile> recent() {
  std::lock_guard<std::mutex> g(mu);
  return ring;
}

bool find(const std::string& key, WatchedFile& out) {
  std::lock_guard<std::mutex> g(mu);
  for (auto& r : ring)
    if (r.key == key) { out = r; return true; }
  return false;
}

// still the reported file, directly in a folder that is still watched
static bool current(const std::string& key, WatchedFile& w, fs::path& p, std::string& err) {
  if (!find(key, w)) { err = "notfound"; return false; }
  if (w.name.find('/') != std::string::npos || w.name.find('\\') != std::string::npos || w.name == "..") { err = "notfound"; return false; }
  p = fs::u8path(w.folder) / fs::u8path(w.name);
  uint64_t size;
  int64_t mtime;
  if (!statFile(p, size, mtime)) { err = "notfound"; return false; }
  if (size != w.size || mtime != w.mtime) { err = "changed"; return false; }
  return true;
}

bool read(const std::string& key, uint64_t offset, uint32_t length, std::vector<uint8_t>& out, uint64_t& size, std::string& err) {
  WatchedFile w;
  fs::path p;
  if (!current(key, w, p, err)) return false;
  size = w.size;
  if (w.size > kMaxRead) { err = "toolarge"; return false; }
  if (offset > w.size) offset = w.size;
  uint64_t n = std::min<uint64_t>({uint64_t(length), uint64_t(kMaxChunk), w.size - offset});
  out.resize(size_t(n));
  std::ifstream in(p, std::ios::binary);
  if (!in) { err = "unreadable"; return false; }
  in.seekg(std::streamoff(offset));
  if (n && !in.read(reinterpret_cast<char*>(out.data()), std::streamsize(n))) { err = "unreadable"; return false; }
  return true;
}

void setTestPrintDir(const std::string& dir) { testPrintDir = dir; }
void setPickTest(const std::string& path) { testPick = path; }

bool print(const std::string& key, std::string& err) {
  WatchedFile w;
  fs::path p;
  if (!current(key, w, p, err)) return false;
  if (!testPrintDir.empty()) {
    std::error_code ec;
    fs::copy_file(p, fs::u8path(testPrintDir) / fs::u8path(w.name), fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "failed"; return false; }
    return true;
  }
#ifdef _WIN32
  std::wstring file = p.wstring();
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof sei;
  sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  sei.lpVerb = L"print";
  sei.lpFile = file.c_str();
  sei.nShow = SW_SHOWMINNOACTIVE;
  if (!ShellExecuteExW(&sei)) {
    DWORD e = GetLastError();
    err = (e == ERROR_NO_ASSOCIATION || e == ERROR_FILE_NOT_FOUND) ? "noapp" : "failed";
    return false;
  }
  return true;
#else
  int rc = runProgram({"lp", "--", p.u8string()}, nullptr);
  if (rc == -1 || rc == 127) { err = "noapp"; return false; }   // no CUPS on this computer
  if (rc != 0) { err = "failed"; return false; }
  return true;
#endif
}

int listen(std::function<void(const WatchedFile&)> cb) {
  std::lock_guard<std::mutex> g(mu);
  int id = nextListener++;
  listeners[id] = std::move(cb);
  return id;
}

void unlisten(int id) {
  std::lock_guard<std::mutex> g(mu);   // the watcher calls listeners with mu held: none runs after this
  listeners.erase(id);
}

void setPicker(std::function<void(const std::string&, std::function<void(int, std::string)>)> p) { picker = std::move(p); }

// ---------- the system's folder chooser ----------
static const char* pickTitle(const std::string& lang) {
  return lang == "el" ? "Φάκελος που θα παρακολουθεί το orOS" : "A folder for orOS to watch";
}

#ifdef _WIN32
// IFileOpenDialog on a thread of its own (a modal loop on the main thread
// would hold up the plugins' calls), owned by a hidden topmost window so it
// comes up above the browser that asked for it.
static int pickWin(const std::string& lang, std::string& path) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool uninit = SUCCEEDED(hr);
  int status = -1;
  IFileOpenDialog* d = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    d->SetTitle(wide(pickTitle(lang)).c_str());
    HWND owner = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"orOS Bridge", WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    hr = d->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) status = 0;
    else if (SUCCEEDED(hr)) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(d->GetResult(&item))) {
        PWSTR s = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &s)) && s) {
          path = narrow(s);
          CoTaskMemFree(s);
          status = 1;
        }
        item->Release();
      }
    }
    if (owner) DestroyWindow(owner);
    d->Release();
  }
  if (uninit) CoUninitialize();
  return status;
}
#else
// zenity (GNOME), kdialog (KDE) or yad, without a shell
static int pickCommand(const std::string& lang, std::string& path) {
  if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) return -1;
  std::string title = pickTitle(lang);
  const char* home = std::getenv("HOME");
  std::string start = home ? std::string(home) + "/" : "/";
  std::vector<std::vector<std::string>> tries = {
      {"zenity", "--file-selection", "--directory", "--title=" + title, "--filename=" + start},
      {"kdialog", "--getexistingdirectory", start, "--title", title},
      {"yad", "--file", "--directory", "--title=" + title, "--filename=" + start},
  };
  for (auto& t : tries) {
    std::string out;
    int rc = runProgram(t, &out);
    if (rc == -1 || rc == 127) continue;   // not installed
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (rc != 0 || out.empty()) return 0;  // cancelled
    path = out;
    return 1;
  }
  return -1;
}
#endif

void pickAsync(const std::string& lang, std::function<void(int, std::string)> done) {
  if (picking.exchange(true)) { done(-2, ""); return; }
  auto finish = [done](int st, std::string p) {
    picking = false;
    done(st, std::move(p));
  };
  if (!testPick.empty()) {   // the tests: the file names the folder "chosen", empty = cancelled
    std::ifstream in(fs::u8path(testPick), std::ios::binary);
    std::string p((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
    finish(p.empty() ? 0 : 1, p);
    return;
  }
  if (picker) { picker(lang, finish); return; }
  std::thread([lang, finish] {
    std::string p;
#ifdef _WIN32
    int st = pickWin(lang, p);
#else
    int st = pickCommand(lang, p);
#endif
    finish(st, p);
  }).detach();
}

bool pick(const std::string& lang, std::string& path, std::string& err) {
  auto pr = std::make_shared<std::promise<std::pair<int, std::string>>>();
  auto fut = pr->get_future();
  pickAsync(lang, [pr](int st, std::string p) { pr->set_value({st, std::move(p)}); });
  auto r = fut.get();
  if (r.first == -2) { err = "busy"; return false; }
  if (r.first < 0) { err = "nodialog"; return false; }
  path = r.first == 1 ? r.second : "";
  return true;
}
}  // namespace watch
}  // namespace ob
