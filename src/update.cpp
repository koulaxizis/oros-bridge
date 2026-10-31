// New versions of the bridge (0.8.0). See update.h and DESIGN.md decision 15.
#include "update.h"
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>
#include "mainloop.h"
#include "scan.h"
#include "sha256.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace ob {
namespace update {
namespace {

const char* kApi = "https://api.github.com/repos/koulaxizis/oros-bridge/releases/latest";
const char* kDownload = "https://github.com/koulaxizis/oros-bridge/releases/download/";
const char* kPage = "https://github.com/koulaxizis/oros-bridge/releases/tag/";
constexpr uintmax_t kMaxAsset = uintmax_t(512) << 20;   // the AppImage is the largest, far below
constexpr uintmax_t kMaxSmall = 1 << 20;                // the release JSON, SHA256SUMS.txt

std::string selfFile;            // the program as it was at start (later it may be replaced)
std::vector<std::string> args;   // the options it was started with
bool isBundled = false;

const std::regex& tagRe() {
  static const std::regex re(R"(^v(\d+)\.(\d+)\.(\d+)$)");
  return re;
}

std::string apiUrl() {
  const char* t = std::getenv("OROS_BRIDGE_UPDATE_URL");
  return t && *t ? std::string(t) + "/latest" : kApi;
}
std::string assetUrl(const std::string& tag, const std::string& name) {
  const char* t = std::getenv("OROS_BRIDGE_UPDATE_URL");
  return (t && *t ? std::string(t) + "/download/" : std::string(kDownload)) + tag + "/" + name;
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

bool writable(const fs::path& dir) {
  std::error_code ec;
  auto probe = dir / (".oros-bridge-write-" + randomHex(4));
  { std::ofstream f(probe, std::ios::binary); if (!f) return false; }
  fs::remove(probe, ec);
  return true;
}

fs::path cacheDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH + 1];
  DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  return fs::path(std::wstring(buf, n)) / "oros-bridge-update";
#else
  const char* x = std::getenv("XDG_CACHE_HOME");
  const char* h = std::getenv("HOME");
  fs::path base = x && *x == '/' ? fs::path(x) : fs::path(h ? h : "/tmp") / ".cache";
  return base / "oros-bridge" / "update";
#endif
}

// ---------- downloads ----------
#ifdef _WIN32
std::wstring wide(const std::string& s) { return fs::u8path(s).wstring(); }

bool download(const std::string& url, const fs::path& file, uintmax_t max, std::string& detail) {
  std::wstring w = wide(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256], path[2048], extra[1024];
  uc.lpszHostName = host; uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
  uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) { detail = "bad address " + url; return false; }
  const std::wstring agentName = wide("orOS-Bridge/" OROS_BRIDGE_VERSION);
  const wchar_t* agent = agentName.c_str();
  HINTERNET s = WinHttpOpen(agent, 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, Windows 8.1+ */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) { detail = "WinHttpOpen failed"; return false; }
  WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
  bool ok = false;
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", (std::wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                  : nullptr;
  if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      detail = "HTTP " + std::to_string(status) + " from " + url;
    } else {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      uintmax_t total = 0;
      ok = bool(out);
      std::vector<char> buf(1 << 16);
      while (ok) {
        DWORD got = 0;
        if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &got)) { ok = false; detail = "the download broke off"; break; }
        if (got == 0) break;
        total += got;
        if (total > max) { ok = false; detail = "too large"; break; }
        out.write(buf.data(), got);
      }
      out.close();
      if (ok && !out) { ok = false; detail = "could not write " + file.u8string(); }
    }
  } else if (detail.empty()) {
    detail = "no connection to " + url + " (" + std::to_string(GetLastError()) + ")";
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return ok;
}
#else
// libcurl, loaded at run time (the program has no build dependency on it);
// without it the curl or wget command
struct Curl {
  void* (*init)() = nullptr;
  int (*setopt)(void*, int, ...) = nullptr;
  int (*perform)(void*) = nullptr;
  int (*getinfo)(void*, int, ...) = nullptr;
  void (*cleanup)(void*) = nullptr;
  const char* (*strerror)(int) = nullptr;
  bool load() {
    static bool tried = false, ok = false;
    if (tried) return ok;
    tried = true;
    for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4"}) {
      void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
      if (!lib) continue;
      init = reinterpret_cast<decltype(init)>(dlsym(lib, "curl_easy_init"));
      setopt = reinterpret_cast<decltype(setopt)>(dlsym(lib, "curl_easy_setopt"));
      perform = reinterpret_cast<decltype(perform)>(dlsym(lib, "curl_easy_perform"));
      getinfo = reinterpret_cast<decltype(getinfo)>(dlsym(lib, "curl_easy_getinfo"));
      cleanup = reinterpret_cast<decltype(cleanup)>(dlsym(lib, "curl_easy_cleanup"));
      strerror = reinterpret_cast<decltype(strerror)>(dlsym(lib, "curl_easy_strerror"));
      if (init && setopt && perform && getinfo && cleanup && strerror) return ok = true;
    }
    return false;
  }
};
Curl curl;
// option numbers from curl.h (stable ABI)
constexpr int kOptWriteData = 10001, kOptUrl = 10002, kOptUserAgent = 10018, kOptNoProxy = 10177;
constexpr int kOptWriteFunction = 20011, kOptTimeout = 13, kOptConnectTimeout = 78, kOptFollow = 52, kOptMaxRedirs = 68;
constexpr int kOptFailOnError = 45, kOptNoSignal = 99, kInfoResponseCode = 0x200002;

struct Sink { std::ofstream* out; uintmax_t total, max; bool tooLarge; };
size_t sinkWrite(char* p, size_t size, size_t n, void* ud) {
  auto* s = static_cast<Sink*>(ud);
  s->total += size * n;
  if (s->total > s->max) { s->tooLarge = true; return 0; }
  s->out->write(p, std::streamsize(size * n));
  return *s->out ? size * n : 0;
}

int run(const std::vector<std::string>& argv) {
  std::vector<char*> a;
  for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
  a.push_back(nullptr);
  pid_t pid;
  if (posix_spawnp(&pid, a[0], nullptr, nullptr, a.data(), environ) != 0) return -1;
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
bool onPath(const std::string& prog) {
  const char* p = std::getenv("PATH");
  std::stringstream ss(p ? p : "/usr/bin:/bin");
  std::string dir;
  while (std::getline(ss, dir, ':'))
    if (!dir.empty() && access((dir + "/" + prog).c_str(), X_OK) == 0) return true;
  return false;
}

bool download(const std::string& url, const fs::path& file, uintmax_t max, std::string& detail) {
  std::error_code ec;
  if (curl.load()) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) { detail = "could not write " + file.string(); return false; }
    Sink sink{&out, 0, max, false};
    void* h = curl.init();
    if (!h) { detail = "curl_easy_init failed"; return false; }
    curl.setopt(h, kOptUrl, url.c_str());
    curl.setopt(h, kOptUserAgent, "orOS-Bridge/" OROS_BRIDGE_VERSION);
    curl.setopt(h, kOptFollow, 1L);
    curl.setopt(h, kOptMaxRedirs, 10L);
    curl.setopt(h, kOptFailOnError, 1L);
    curl.setopt(h, kOptNoSignal, 1L);
    curl.setopt(h, kOptConnectTimeout, 15L);
    curl.setopt(h, kOptTimeout, 600L);
    curl.setopt(h, kOptNoProxy, "127.0.0.1,localhost");
    curl.setopt(h, kOptWriteFunction, &sinkWrite);
    curl.setopt(h, kOptWriteData, &sink);
    int rc = curl.perform(h);
    long status = 0;
    curl.getinfo(h, kInfoResponseCode, &status);
    curl.cleanup(h);
    out.close();
    if (rc == 0 && out) return true;
    detail = sink.tooLarge ? "too large" : status >= 400 ? "HTTP " + std::to_string(status) + " from " + url
                                                          : std::string(curl.strerror(rc)) + " (" + url + ")";
    fs::remove(file, ec);
    return false;
  }
  // no libcurl: the commands (no shell, the address is one argument)
  int rc = -1;
  if (onPath("curl")) rc = run({"curl", "-fsSL", "--max-time", "600", "--noproxy", "127.0.0.1,localhost", "-o", file.string(), url});
  else if (onPath("wget")) rc = run({"wget", "-q", "-O", file.string(), url});
  else { detail = "neither libcurl nor curl nor wget is installed"; return false; }
  if (rc == 0 && fs::file_size(file, ec) <= max && !ec) return true;
  detail = "could not download " + url;
  fs::remove(file, ec);
  return false;
}
#endif

// "<hex>  <name>" lines, as sha256sum and the release workflow write them
std::map<std::string, std::string> parseSums(const std::string& text) {
  std::map<std::string, std::string> m;
  std::stringstream ss(text);
  std::string line;
  static const std::regex re(R"(^([0-9a-fA-F]{64})\s+\*?(?:\./)?(\S.*?)\s*$)");
  while (std::getline(ss, line)) {
    std::smatch sm;
    if (!std::regex_match(line, sm, re)) continue;
    std::string hex = sm[1];
    for (auto& c : hex) c = char(std::tolower(static_cast<unsigned char>(c)));
    m[sm[2]] = hex;
  }
  return m;
}

// ---------- the window's background checks ----------
std::mutex mu;
std::condition_variable wake;
State st;
bool checkSoon = false, checksStarted = false;
}  // namespace

void init(int argc, char** argv) {
  selfFile = selfPath();
#ifndef _WIN32
  const std::string deleted = " (deleted)";
  if (selfFile.size() > deleted.size() && selfFile.compare(selfFile.size() - deleted.size(), deleted.size(), deleted) == 0)
    selfFile.resize(selfFile.size() - deleted.size());
#endif
  args.assign(argv + 1, argv + argc);
  std::error_code ec;
  if (fs::exists(fs::u8path(selfFile).parent_path() / "oros-bridge.bundled", ec)) isBundled = true;
}
void setBundled() { isBundled = true; }
bool bundled() { return isBundled; }

std::string kind() {
  if (isBundled) return "";
  fs::path self = fs::u8path(selfFile);
  std::error_code ec;
#ifdef _WIN32
  return writable(self.parent_path()) ? "exe" : "";
#else
  const char* app = std::getenv("APPIMAGE");
  if (app && *app == '/' && fs::is_regular_file(app, ec)) return writable(fs::path(app).parent_path()) ? "appimage" : "";
  if (self == "/usr/bin/oros-bridge") {
    if (fs::exists("/var/lib/dpkg/info/oros-bridge.list", ec)) return "deb";
    if (fs::exists("/usr/bin/rpm", ec) || fs::exists("/bin/rpm", ec)) return "rpm";
    return "";
  }
  return writable(self.parent_path()) ? "tarball" : "";
#endif
}

bool newer(const std::string& current, const std::string& latestTag) {
  std::smatch c, l;
  std::string cur = "v" + current;
  if (!std::regex_match(cur, c, tagRe()) || !std::regex_match(latestTag, l, tagRe())) return false;
  for (int i = 1; i <= 3; i++) {
    long a = std::stol(c[i]), b = std::stol(l[i]);
    if (a != b) return b > a;
  }
  return false;
}

std::string releasePage(const std::string& tag) { return kPage + tag; }

std::string latestTag(std::string& err) {
  std::error_code ec;
  fs::path dir = cacheDir();
  fs::create_directories(dir, ec);
  fs::path file = dir / ("latest-" + randomHex(4) + ".json");
  bool ok = download(apiUrl(), file, kMaxSmall, err);
  std::string body = ok ? readFile(file) : "";
  fs::remove(file, ec);
  if (!ok) return "";
  try {
    auto j = nlohmann::json::parse(body);
    std::string tag = j.value("tag_name", "");
    if (j.value("draft", false) || j.value("prerelease", false) || !std::regex_match(tag, tagRe())) {
      err = "no usable release";
      return "";
    }
    return tag;
  } catch (...) {
    err = "the release answer is not JSON";
    return "";
  }
}

bool install(const std::string& tag, std::string& err, std::string& detail) {
  std::error_code ec;
  std::string k = kind();
  if (k.empty()) { err = "manual"; detail = isBundled ? "orOS updates this copy of the bridge" : "this copy cannot update itself"; return false; }
  if (!std::regex_match(tag, tagRe())) { err = "download"; detail = "bad tag " + tag; return false; }
  fs::path self = fs::u8path(selfFile);
  fs::path work = k == "deb" || k == "rpm" ? cacheDir() : (k == "appimage" ? fs::path(std::getenv("APPIMAGE")) : self).parent_path();
  fs::create_directories(work, ec);

  fs::path sumsFile = work / (".oros-bridge-sums-" + randomHex(4));
  if (!download(assetUrl(tag, "SHA256SUMS.txt"), sumsFile, kMaxSmall, detail)) { err = "download"; return false; }
  auto sums = parseSums(readFile(sumsFile));
  fs::remove(sumsFile, ec);
  // one file of the release to dest, checked against SHA256SUMS.txt
  auto fetch = [&](const std::string& asset, const fs::path& dest) {
    auto want = sums.find(asset);
    if (want == sums.end()) { err = "checksum"; detail = asset + " is not in SHA256SUMS.txt"; return false; }
    if (!download(assetUrl(tag, asset), dest, kMaxAsset, detail)) { err = "download"; fs::remove(dest, ec); return false; }
    if (sha256File(dest.u8string()) != want->second) {
      err = "checksum";
      detail = asset + " does not match its SHA-256 in SHA256SUMS.txt";
      fs::remove(dest, ec);
      return false;
    }
    return true;
  };
  auto swapIn = [&](const fs::path& from, const fs::path& to) {
    fs::rename(from, to, ec);
    if (ec) { err = "files"; detail = "could not replace " + to.u8string() + ": " + ec.message(); fs::remove(from, ec); return false; }
    return true;
  };

#ifdef _WIN32
  // a running exe (and a loaded dll) can be renamed, not overwritten: the old
  // ones become .old (removed at the next start), the new ones take their names
  fs::path dll = self.parent_path() / "oros-bridge-asio.dll";
  bool withDll = fs::exists(dll, ec);
  // fs::path += (not a std::string): a UTF-8 string would be read in the ANSI code page
  auto plus = [](fs::path p, const char* ext) { p += ext; return p; };
  fs::path newExe = plus(self, ".new"), newDll = plus(dll, ".new");
  if (!fetch("oros-bridge-windows-x64.exe", newExe)) return false;
  if (withDll && !fetch("oros-bridge-asio.dll", newDll)) { fs::remove(newExe, ec); return false; }
  fs::path oldExe = plus(self, ".old"), oldDll = plus(dll, ".old");
  fs::remove(oldExe, ec);
  fs::remove(oldDll, ec);
  fs::rename(self, oldExe, ec);
  if (ec) { err = "files"; detail = "could not move " + self.u8string() + ": " + ec.message(); fs::remove(newExe, ec); fs::remove(newDll, ec); return false; }
  if (!swapIn(newExe, self)) { fs::rename(oldExe, self, ec); fs::remove(newDll, ec); return false; }
  if (withDll) {
    fs::rename(dll, oldDll, ec);
    if (ec || !swapIn(newDll, dll)) {
      // put the old pair back, so exe and module stay of one version
      if (!fs::exists(dll, ec)) fs::rename(oldDll, dll, ec);
      fs::remove(self, ec);
      fs::rename(oldExe, self, ec);
      fs::remove(newDll, ec);
      if (err.empty()) { err = "files"; detail = "could not replace " + dll.u8string(); }
      return false;
    }
  }
  return true;
#else
  if (k == "appimage" || k == "tarball") {
    fs::path target = k == "appimage" ? fs::path(std::getenv("APPIMAGE")) : self;
    fs::path tmp = target.string() + ".new";
    if (k == "appimage") {
      if (!fetch("oros-bridge-linux-x64.AppImage", tmp)) return false;
    } else {
      fs::path tgz = work / (".oros-bridge-update-" + randomHex(4) + ".tar.gz");
      fs::path dir = work / (".oros-bridge-update-" + randomHex(4));
      if (!fetch("oros-bridge-linux-x64.tar.gz", tgz)) return false;
      fs::create_directories(dir, ec);
      int rc = run({"tar", "-xzf", tgz.string(), "-C", dir.string()});
      fs::remove(tgz, ec);
      fs::rename(dir / "oros-bridge" / "oros-bridge", tmp, ec);
      fs::remove_all(dir, ec);
      if (rc != 0 || !fs::is_regular_file(tmp, ec)) { err = "files"; detail = "the tar.gz has no oros-bridge/oros-bridge"; fs::remove(tmp, ec); return false; }
    }
    chmod(tmp.c_str(), 0755);
    return swapIn(tmp, target);
  }
  // a package: the system's package tool installs it, as root through pkexec
  fs::path pkg = work / (k == "deb" ? "oros-bridge-linux-x64.deb" : "oros-bridge-linux-x64.rpm");
  if (!fetch(pkg.filename().string(), pkg)) return false;
  chmod(pkg.c_str(), 0644);
  std::vector<std::string> cmd;
  if (k == "deb") cmd = onPath("apt-get") ? std::vector<std::string>{"pkexec", "apt-get", "install", "-y", pkg.string()}
                                         : std::vector<std::string>{"pkexec", "dpkg", "-i", pkg.string()};
  else cmd = {"pkexec", "rpm", "-U", pkg.string()};
  if (!onPath("pkexec")) { err = "install"; detail = "pkexec is not installed"; return false; }
  int rc = run(cmd);
  fs::remove(pkg, ec);
  if (rc != 0) { err = "install"; detail = cmd[1] + " ended with " + std::to_string(rc) + " (or the password was not given)"; return false; }
  return true;
#endif
}

bool relaunch() {
  std::vector<std::string> a;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--tray") continue;
    if (args[i] == "--after-update") { i++; continue; }
    a.push_back(args[i]);
  }
#ifdef _WIN32
  a.push_back("--after-update");
  a.push_back(std::to_string(GetCurrentProcessId()));
  auto quote = [](const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
      if (c == L'\\') { slashes++; continue; }
      if (c == L'"') q.append(slashes * 2 + 1, L'\\');
      else q.append(slashes, L'\\');
      slashes = 0;
      q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
  };
  std::wstring exe = fs::u8path(selfFile).wstring();
  std::wstring cmd = quote(exe);
  for (auto& s : a) cmd += L" " + quote(wide(s));
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  const char* app = std::getenv("APPIMAGE");
  std::string exe = kind() == "appimage" && app ? app : selfFile;
  a.insert(a.begin(), exe);
  a.push_back("--after-update");
  a.push_back(std::to_string(getpid()));
  std::vector<char*> v;
  for (auto& s : a) v.push_back(const_cast<char*>(s.c_str()));
  v.push_back(nullptr);
  pid_t pid;
  return posix_spawn(&pid, exe.c_str(), nullptr, nullptr, v.data(), environ) == 0;
#endif
}

void waitForOld(long pid) {
  if (pid <= 0) return;
#ifdef _WIN32
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
  if (!h) return;
  WaitForSingleObject(h, 15000);
  CloseHandle(h);
#else
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < end) {
    if (kill(pid_t(pid), 0) != 0) return;
    // a zombie waiting for its parent counts as gone
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string s((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    auto p = s.rfind(')');
    if (p != std::string::npos && p + 2 < s.size() && s[p + 2] == 'Z') return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#endif
}

void cleanOld() {
#ifdef _WIN32
  std::error_code ec;
  fs::path self = fs::u8path(selfFile);
  fs::path old = self;
  old += ".old";
  fs::remove(old, ec);
  fs::remove((self.parent_path() / "oros-bridge-asio.dll.old"), ec);
#endif
}

void openPage(const std::string& url) {
#ifdef _WIN32
  ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  std::thread([url] { run({"xdg-open", url}); }).detach();
#endif
}

// ---------- the window's background checks ----------
void startChecks(Config& cfg) {
  if (isBundled) return;
  {
    std::lock_guard<std::mutex> g(mu);
    if (checksStarted) return;
    checksStarted = true;
    st.canInstall = !kind().empty();
  }
  Config* c = &cfg;
  std::thread([c] {
    const char* d = std::getenv("OROS_BRIDGE_UPDATE_DELAY");   // seconds before the first check (tests)
    auto first = std::chrono::seconds(d && *d ? std::atoi(d) : 60);
    std::unique_lock<std::mutex> lk(mu);
    wake.wait_for(lk, first, [] { return checkSoon; });
    for (;;) {
      checkSoon = false;
      lk.unlock();
      std::string tag, err;
      if (c->updateCheck()) tag = latestTag(err);   // offline or GitHub busy: next time
      lk.lock();
      if (!tag.empty()) st.tag = newer(OROS_BRIDGE_VERSION, tag) ? tag : "";
      if (!c->updateCheck()) st.tag.clear();
      wake.wait_for(lk, std::chrono::hours(24), [] { return checkSoon; });
    }
  }).detach();
}

void checkNow() {
  std::lock_guard<std::mutex> g(mu);
  checkSoon = true;
  wake.notify_all();
}

State state() {
  std::lock_guard<std::mutex> g(mu);
  return st;
}

void installAsync(std::function<void()> done) {
  std::string tag;
  {
    std::lock_guard<std::mutex> g(mu);
    if (st.busy || st.tag.empty()) return;
    st.busy = true;
    st.error.clear();
    tag = st.tag;
  }
  std::thread([tag, done] {
    std::string err, detail;
    bool ok = install(tag, err, detail);
    if (!ok) std::fprintf(stderr, "update to %s: %s (%s)\n", tag.c_str(), err.c_str(), detail.c_str());
    MainLoop::get().post([ok, err, done] {
      if (ok && relaunch()) {
        done();
        return;
      }
      std::lock_guard<std::mutex> g(mu);
      st.busy = false;
      st.error = ok ? "files" : err;
    });
  }).detach();
}
}  // namespace update
}  // namespace ob
