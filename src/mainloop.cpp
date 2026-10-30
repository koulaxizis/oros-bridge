#include "mainloop.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ob {
namespace {
std::mutex mu;
std::condition_variable cv;
std::deque<std::function<void()>> tasks;
std::atomic<bool> quitting{false};
#ifdef _WIN32
HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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

MainLoop::MainLoop() : mainId_(std::this_thread::get_id()) {}

void MainLoop::post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> g(mu);
    tasks.push_back(std::move(task));
  }
#ifdef _WIN32
  SetEvent(wake);
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
  while (runTasks()) {
    std::unique_lock<std::mutex> l(mu);
    cv.wait(l, [] { return !tasks.empty() || quitting.load(); });
  }
#endif
}
}
