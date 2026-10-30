#pragma once
#include <functional>
#include <future>
#include <thread>

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

private:
  MainLoop();
  std::thread::id mainId_;
};
}
