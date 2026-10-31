#pragma once
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

namespace ob {

// A top-level window that holds a plugin's editor: Win32, or X11 on Linux
// (0.7.0, needs a display). Elsewhere create() returns null and the editor
// is not offered.
class EditorWindow {
public:
  virtual ~EditorWindow() = default;
  virtual void* nativeHandle() = 0;                  // HWND, or the X11 Window id
  virtual void resize(int width, int height) = 0;    // the client area
  virtual void show() = 0;
  // onClosed: the person closed it; onResized: the person dragged its edge
  static std::unique_ptr<EditorWindow> create(const std::string& title, int width, int height, bool resizable,
                                              std::function<void()> onClosed,
                                              std::function<void(int, int)> onResized);
  static bool supported();
};
}
