#include "editor.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace ob {
#ifdef _WIN32
namespace {
std::wstring wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

class Win32Editor : public EditorWindow {
public:
  HWND hwnd = nullptr;
  bool resizable = false;
  std::function<void()> onClosed;
  std::function<void(int, int)> onResized;
  bool inResize = false;

  static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Win32Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
      case WM_CLOSE:
        if (self && self->onClosed) { auto f = self->onClosed; f(); }   // the owner destroys us
        return 0;
      case WM_SIZE:
        if (self && self->onResized && !self->inResize) self->onResized(LOWORD(lp), HIWORD(lp));
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
  }
  static void registerClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"OrosBridgeEditor";
    RegisterClassExW(&wc);
    done = true;
  }
  DWORD style() const { return resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX); }

  ~Win32Editor() override {
    if (hwnd) DestroyWindow(hwnd);
  }
  void* nativeHandle() override { return hwnd; }
  void resize(int w, int h) override {
    RECT r{0, 0, w, h};
    AdjustWindowRectEx(&r, style(), FALSE, 0);
    inResize = true;
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    inResize = false;
  }
  void show() override {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
  }
};
}

bool EditorWindow::supported() { return true; }

std::unique_ptr<EditorWindow> EditorWindow::create(const std::string& title, int width, int height, bool resizable,
                                                   std::function<void()> onClosed,
                                                   std::function<void(int, int)> onResized) {
  Win32Editor::registerClass();
  auto w = std::make_unique<Win32Editor>();
  w->resizable = resizable;
  w->onClosed = std::move(onClosed);
  w->onResized = std::move(onResized);
  RECT r{0, 0, width > 0 ? width : 400, height > 0 ? height : 300};
  AdjustWindowRectEx(&r, w->style(), FALSE, 0);
  w->hwnd = CreateWindowExW(0, L"OrosBridgeEditor", wide(title).c_str(), w->style(), CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!w->hwnd) return nullptr;
  SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w.get()));
  return w;
}
#else
bool EditorWindow::supported() { return false; }
std::unique_ptr<EditorWindow> EditorWindow::create(const std::string&, int, int, bool, std::function<void()>,
                                                   std::function<void(int, int)>) {
  return nullptr;
}
#endif
}
