#pragma once
#include <functional>
#include <future>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <poll.h>
#endif

namespace ob {

// Every call into a plugin happens on the main thread, the one that also
// runs the editor windows (plugins expect that). Network threads hand their
// work over with call() and wait for the answer.
class MainLoop {
public:
  static MainLoop& get();
  void post(std::function<void()> task);
  template <class F>
  auto call(F f) -> decltype(f()) {
    if (onMainThread()) return f();
    std::packaged_task<decltype(f())()> task(std::move(f));
    auto fut = task.get_future();
    auto* p = new std::packaged_task<decltype(f())()>(std::move(task));
    post([p] { (*p)(); delete p; });
    return fut.get();
  }
  bool onMainThread() const { return std::this_thread::get_id() == mainId_; }
  void run();      // until quit()
  void quit();

#ifndef _WIN32
  // Linux (0.7.0): the X11 connection and the plugins' own file descriptors
  // and timers (VST 3 IRunLoop, CLAP posix-fd and timer support) are served
  // by this loop too. Main thread only; an id is never reused, removing an
  // unknown id does nothing, and a callback may add or remove any of them.
  using FdCallback = std::function<void(int fd, short revents)>;
  int addFd(int fd, short events, FdCallback cb);
  void removeFd(int id);
  int addTimer(int periodMs, std::function<void()> cb);
  void removeTimer(int id);
  // another event loop that shares the wait (GLib's, for the GTK window):
  // prepare() adds its descriptors and returns its timeout (-1 for none),
  // dispatch() gets them back with their revents.
  struct Guest {
    virtual ~Guest() = default;
    virtual int prepare(std::vector<pollfd>& fds) = 0;
    virtual void dispatch(const pollfd* fds, size_t n) = 0;
  };
  void setGuest(Guest* g);
#endif

private:
  MainLoop();
  std::thread::id mainId_;
};
}
