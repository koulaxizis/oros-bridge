#include "mainloop.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <map>
#endif

namespace ob {
namespace {
std::mutex mu;
std::condition_variable cv;
std::deque<std::function<void()>> tasks;
std::atomic<bool> quitting{false};
#ifdef _WIN32
HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#else
int wakeFd[2] = {-1, -1};
void openWake() {
  if (wakeFd[0] >= 0) return;
  if (pipe(wakeFd) != 0) return;
  for (int fd : wakeFd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
}
void signalWake() {
  if (wakeFd[1] < 0) return;
  char c = 1;
  ssize_t r = write(wakeFd[1], &c, 1);   // a full pipe already wakes the loop
  (void)r;
}

using Clock = std::chrono::steady_clock;
struct FdWatch { int fd; short events; MainLoop::FdCallback cb; };
struct Timer { int periodMs; Clock::time_point due; std::function<void()> cb; };
std::map<int, FdWatch> fdWatches;
std::map<int, Timer> timers;
int nextId = 1;
MainLoop::Guest* guest = nullptr;
#endif

bool runTasks() {
  for (;;) {
    std::function<void()> t;
    {
      std::lock_guard<std::mutex> g(mu);
      if (tasks.empty()) return !quitting;
      t = std::move(tasks.front());
      tasks.pop_front();
    }
    t();
  }
}
}

MainLoop& MainLoop::get() {
  static MainLoop loop;
  return loop;
}

MainLoop::MainLoop() : mainId_(std::this_thread::get_id()) {
#ifndef _WIN32
  openWake();
#endif
}

void MainLoop::post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> g(mu);
    tasks.push_back(std::move(task));
  }
#ifdef _WIN32
  SetEvent(wake);
#else
  signalWake();
#endif
  cv.notify_one();
}

void MainLoop::quit() {
  quitting = true;
  post([] {});
}

void MainLoop::run() {
  mainId_ = std::this_thread::get_id();
#ifdef _WIN32
  // tasks and window messages (plugin editors) on the same thread
  while (runTasks()) {
    DWORD r = MsgWaitForMultipleObjectsEx(1, &wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (r == WAIT_OBJECT_0 + 1) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }
#else
  // tasks, the wake pipe, watched descriptors, timers and the guest loop in one poll()
  std::vector<pollfd> fds;
  std::vector<int> ids;
  while (runTasks()) {
    fds.clear();
    ids.clear();
    fds.push_back({wakeFd[0], POLLIN, 0});
    for (auto& [id, w] : fdWatches) {
      fds.push_back({w.fd, w.events, 0});
      ids.push_back(id);
    }
    size_t guestAt = fds.size();
    int timeout = guest ? guest->prepare(fds) : -1;
    auto now = Clock::now();
    for (auto& [id, t] : timers) {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.due - now).count();
      int left = int(std::clamp<long long>(ms < 0 ? 0 : ms + 1, 0, 60000));
      timeout = timeout < 0 ? left : std::min(timeout, left);
    }
    int r = poll(fds.data(), nfds_t(fds.size()), timeout);
    if (r < 0 && errno != EINTR) break;
    if (fds[0].revents) {
      char buf[64];
      while (read(wakeFd[0], buf, sizeof buf) > 0) {}
    }
    for (size_t i = 0; i < ids.size(); i++) {
      short ev = fds[i + 1].revents;
      if (!ev) continue;
      auto it = fdWatches.find(ids[i]);   // a callback may have removed it
      if (it == fdWatches.end()) continue;
      auto cb = it->second.cb;
      cb(it->second.fd, ev);
    }
    if (guest) guest->dispatch(fds.data() + guestAt, fds.size() - guestAt);
    now = Clock::now();
    std::vector<int> due;
    for (auto& [id, t] : timers)
      if (t.due <= now) due.push_back(id);
    for (int id : due) {
      auto it = timers.find(id);
      if (it == timers.end()) continue;
      auto& t = it->second;
      t.due += std::chrono::milliseconds(t.periodMs);
      if (t.due <= now) t.due = now + std::chrono::milliseconds(t.periodMs);   // late: no burst
      auto cb = t.cb;
      cb();
    }
  }
#endif
}

#ifndef _WIN32
int MainLoop::addFd(int fd, short events, FdCallback cb) {
  int id = nextId++;
  fdWatches[id] = {fd, events, std::move(cb)};
  return id;
}
void MainLoop::removeFd(int id) { fdWatches.erase(id); }
int MainLoop::addTimer(int periodMs, std::function<void()> cb) {
  int id = nextId++;
  periodMs = std::max(1, periodMs);
  timers[id] = {periodMs, Clock::now() + std::chrono::milliseconds(periodMs), std::move(cb)};
  return id;
}
void MainLoop::removeTimer(int id) { timers.erase(id); }
void MainLoop::setGuest(Guest* g) { guest = g; }
#endif
}
