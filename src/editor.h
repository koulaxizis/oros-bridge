#pragma once
#include <functional>
#include <memory>
#include <string>

namespace ob {

// A top-level window that holds a plugin's editor. Windows only for now; on
// other systems create() returns null and the editor is not offered.
class EditorWindow {
public:
  virtual ~EditorWindow() = default;
  virtual void* nativeHandle() = 0;                  // HWND
  virtual void resize(int width, int height) = 0;    // the client area
  virtual void show() = 0;
  // onClosed: the person closed it; onResized: the person dragged its edge
  static std::unique_ptr<EditorWindow> create(const std::string& title, int width, int height, bool resizable,
                                              std::function<void()> onClosed,
                                              std::function<void(int, int)> onResized);
  static bool supported();
};
}
