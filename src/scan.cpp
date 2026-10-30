#include "scan.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <thread>
#include "plugin.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ob {

std::string selfPath() {
#ifdef _WIN32
  wchar_t buf[32768];
  DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
  return fs::path(std::wstring(buf, n)).u8string();
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec).string();
#endif
}

namespace {
constexpr int kTimeoutMs = 30000;
constexpr size_t kMaxOutput = 1 << 20;

// Runs "<self> --describe <format> <path>", returns its stdout, "" on failure.
std::string runChild(const std::string& format, const std::string& path) {
#ifdef _WIN32
  auto wide = [](const std::string& s) { return fs::u8path(s).wstring(); };
  std::wstring cmd = L"\"" + wide(selfPath()) + L"\" --describe " + wide(format) + L" \"" + wide(path) + L"\"";
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) { CloseHandle(rd); return ""; }
  std::string out;
  std::thread reader([&] {
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
      if (out.size() < kMaxOutput) out.append(buf, n);
    }
  });
  bool done = WaitForSingleObject(pi.hProcess, kTimeoutMs) == WAIT_OBJECT_0;
  if (!done) TerminateProcess(pi.hProcess, 1);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  reader.join();
  CloseHandle(rd);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return done && code == 0 ? out : "";
#else
  int fd[2];
  if (pipe(fd)) return "";
  pid_t pid = fork();
  if (pid < 0) { close(fd[0]); close(fd[1]); return ""; }
  if (pid == 0) {
    dup2(fd[1], 1);
    close(fd[0]);
    close(fd[1]);
    auto self = selfPath();
    execl(self.c_str(), self.c_str(), "--describe", format.c_str(), path.c_str(), (char*)nullptr);
    _exit(127);
  }
  close(fd[1]);
  std::string out;
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
  bool timedOut = false;
  for (;;) {
    int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count());
    if (left <= 0) { timedOut = true; break; }
    pollfd p{fd[0], POLLIN, 0};
    if (poll(&p, 1, left) <= 0) continue;
    char buf[4096];
    ssize_t n = read(fd[0], buf, sizeof buf);
    if (n <= 0) break;
    if (out.size() < kMaxOutput) out.append(buf, size_t(n));
  }
  close(fd[0]);
  if (timedOut) kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  return !timedOut && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? out : "";
#endif
}

int64_t stampOf(const fs::path& p) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
    // a .vst3 bundle: the newest file inside
    int64_t best = 0;
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      auto t = fs::last_write_time(it->path(), ec);
      if (!ec) best = std::max<int64_t>(best, t.time_since_epoch().count());
    }
    return best;
  }
  auto t = fs::last_write_time(p, ec);
  return ec ? 0 : t.time_since_epoch().count();
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

// plugin files under a folder: .vst3 (bundle or file) and .clap
void collect(const fs::path& dir, std::vector<std::pair<std::string, fs::path>>& out) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    auto ext = lower(it->path().extension().string());
    if (ext == ".vst3") {
      out.emplace_back("vst3", it->path());
      if (it->is_directory(ec)) it.disable_recursion_pending();   // its insides are not plugins
    } else if (ext == ".clap") {
#ifdef __APPLE__
      out.emplace_back("clap", it->path());
      if (it->is_directory(ec)) it.disable_recursion_pending();
#else
      if (it->is_regular_file(ec)) out.emplace_back("clap", it->path());
#endif
    }
    if (it.depth() > 6) it.disable_recursion_pending();
  }
}
}

std::vector<PluginInfo> scanPlugins(Config& cfg, bool full, std::function<void(const std::string&)> progress) {
  auto old = cfg.plugins();
  auto oldStamps = cfg.stamps();
  std::vector<std::pair<std::string, fs::path>> files;
  for (auto& f : cfg.folders()) collect(fs::u8path(f), files);
  std::vector<PluginInfo> found;
  nlohmann::json stamps = nlohmann::json::object();
  for (auto& [format, p] : files) {
    std::string key = p.u8string();
    int64_t st = stampOf(p);
    stamps[key] = st;
    if (!full && oldStamps.contains(key) && oldStamps[key] == st) {
      for (auto& o : old)
        if (o.path == key) found.push_back(o);
      continue;
    }
    if (progress) progress(key);
    auto text = runChild(format, key);
    try {
      auto j = nlohmann::json::parse(text);
      for (auto& x : j) found.push_back(x.get<PluginInfo>());
    } catch (...) {
      // could not be read (crashed, timed out, not a plugin): left out, tried again when it changes
    }
  }
  // one id once: the first file wins
  std::vector<PluginInfo> unique;
  for (auto& f : found) {
    bool seen = false;
    for (auto& u : unique) seen = seen || u.id == f.id;
    if (!seen) unique.push_back(f);
  }
  cfg.setPlugins(unique, stamps);
  return unique;
}

int describeMain(const std::string& format, const std::string& path) {
  std::string err;
  std::vector<PluginInfo> v;
  if (format == "clap") v = describeClap(path, err);
  else if (format == "vst3") v = describeVst3(path, err);
  else return 2;
  if (v.empty()) {
    std::cerr << "oros-bridge: " << path << ": " << (err.empty() ? "no plugin" : err) << "\n";
    return 1;
  }
  nlohmann::json j = v;
  std::cout << j.dump() << std::flush;
  return 0;
}
}
