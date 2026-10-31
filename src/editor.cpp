#include "editor.h"
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
// Xlib names these as macros; here they are function pointers of the same name
#undef ConnectionNumber
#undef DefaultScreen
#undef RootWindow
#undef BlackPixel
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include "mainloop.h"
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
#elif defined(__linux__)
// X11 (0.7.0): a top-level window whose id is handed to the plugin
// (kPlatformTypeX11EmbedWindowID, CLAP_WINDOW_API_X11). libX11 is loaded at
// run time, so the bridge still starts on a computer without a display
// (it then offers no editors). Under Wayland it works through XWayland.
namespace {
struct X {
  void* lib = nullptr;
  Display* (*OpenDisplay)(const char*);
  int (*ConnectionNumber)(Display*);
  int (*DefaultScreen)(Display*);
  Window (*RootWindow)(Display*, int);
  unsigned long (*BlackPixel)(Display*, int);
  Window (*CreateSimpleWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
  int (*DestroyWindow)(Display*, Window);
  int (*MapRaised)(Display*, Window);
  int (*ResizeWindow)(Display*, Window, unsigned, unsigned);
  int (*SelectInput)(Display*, Window, long);
  Atom (*InternAtom)(Display*, const char*, Bool);
  Status (*SetWMProtocols)(Display*, Window, Atom*, int);
  int (*ChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int);
  XSizeHints* (*AllocSizeHints)();
  void (*SetWMNormalHints)(Display*, Window, XSizeHints*);
  int (*SetClassHint)(Display*, Window, XClassHint*);
  int (*Free)(void*);
  int (*Flush)(Display*);
  int (*Sync)(Display*, Bool);
  int (*Pending)(Display*);
  int (*NextEvent)(Display*, XEvent*);
  Display* dpy = nullptr;
  Atom wmDelete = 0, wmProtocols = 0, netName = 0, utf8 = 0;
  int fdWatch = 0;
  bool tried = false;

  bool load() {
    if (lib) return true;
    lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
    bool ok = true;
    auto get = [&](auto& f, const char* name) {
      f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, name));
      ok = ok && f;
    };
    get(OpenDisplay, "XOpenDisplay");
    get(ConnectionNumber, "XConnectionNumber");
    get(DefaultScreen, "XDefaultScreen");
    get(RootWindow, "XRootWindow");
    get(BlackPixel, "XBlackPixel");
    get(CreateSimpleWindow, "XCreateSimpleWindow");
    get(DestroyWindow, "XDestroyWindow");
    get(MapRaised, "XMapRaised");
    get(ResizeWindow, "XResizeWindow");
    get(SelectInput, "XSelectInput");
    get(InternAtom, "XInternAtom");
    get(SetWMProtocols, "XSetWMProtocols");
    get(ChangeProperty, "XChangeProperty");
    get(AllocSizeHints, "XAllocSizeHints");
    get(SetWMNormalHints, "XSetWMNormalHints");
    get(SetClassHint, "XSetClassHint");
    get(Free, "XFree");
    get(Flush, "XFlush");
    get(Sync, "XSync");
    get(Pending, "XPending");
    get(NextEvent, "XNextEvent");
    if (!ok) { dlclose(lib); lib = nullptr; }
    return ok;
  }
  Display* display();   // main thread
};
X x;

class X11Editor;
std::map<Window, X11Editor*> windows;

class X11Editor : public EditorWindow {
public:
  Window win = 0;
  bool resizable = false;
  int width = 0, height = 0;   // what the window has, or was last asked to have
  std::function<void()> onClosed;
  std::function<void(int, int)> onResized;

  ~X11Editor() override {
    if (!win) return;
    windows.erase(win);
    x.DestroyWindow(x.dpy, win);
    x.Flush(x.dpy);
  }
  void* nativeHandle() override { return reinterpret_cast<void*>(uintptr_t(win)); }
  void fixSize(int w, int h) {
    if (resizable) return;
    XSizeHints* hints = x.AllocSizeHints();
    if (!hints) return;
    hints->flags = PMinSize | PMaxSize;
    hints->min_width = hints->max_width = w;
    hints->min_height = hints->max_height = h;
    x.SetWMNormalHints(x.dpy, win, hints);
    x.Free(hints);
  }
  void resize(int w, int h) override {
    if (w <= 0 || h <= 0) return;
    width = w;
    height = h;
    fixSize(w, h);
    x.ResizeWindow(x.dpy, win, unsigned(w), unsigned(h));
    x.Flush(x.dpy);
  }
  void show() override {
    x.MapRaised(x.dpy, win);
    x.Flush(x.dpy);
  }
  void event(XEvent& e) {
    if (e.type == ClientMessage && Atom(e.xclient.data.l[0]) == x.wmDelete) {
      if (onClosed) { auto f = onClosed; f(); }   // the owner destroys us
    } else if (e.type == ConfigureNotify) {
      int w = e.xconfigure.width, h = e.xconfigure.height;
      if (w == width && h == height) return;   // our own resize, or a move
      width = w;
      height = h;
      if (onResized) onResized(w, h);
    }
  }
};

void readEvents() {
  while (x.dpy && x.Pending(x.dpy) > 0) {
    XEvent e;
    x.NextEvent(x.dpy, &e);
    auto it = windows.find(e.xany.window);
    if (it != windows.end()) it->second->event(e);
  }
}

Display* X::display() {
  if (dpy || tried) return dpy;
  tried = true;
  if (!load()) return nullptr;
  dpy = OpenDisplay(nullptr);
  if (!dpy) return nullptr;
  wmDelete = InternAtom(dpy, "WM_DELETE_WINDOW", False);
  wmProtocols = InternAtom(dpy, "WM_PROTOCOLS", False);
  netName = InternAtom(dpy, "_NET_WM_NAME", False);
  utf8 = InternAtom(dpy, "UTF8_STRING", False);
  fdWatch = MainLoop::get().addFd(ConnectionNumber(dpy), POLLIN, [](int, short) { readEvents(); });
  return dpy;
}
}

bool EditorWindow::supported() {
  const char* d = std::getenv("DISPLAY");
  if (!d || !*d) return false;
  static int loadable = -1;
  if (loadable < 0) {
    void* h = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (!h) h = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    loadable = h ? 1 : 0;   // stays loaded; X::load() opens the same one
  }
  return loadable == 1;
}

std::unique_ptr<EditorWindow> EditorWindow::create(const std::string& title, int width, int height, bool resizable,
                                                   std::function<void()> onClosed,
                                                   std::function<void(int, int)> onResized) {
  Display* d = x.display();
  if (!d) return nullptr;
  auto w = std::make_unique<X11Editor>();
  w->resizable = resizable;
  w->onClosed = std::move(onClosed);
  w->onResized = std::move(onResized);
  w->width = width > 0 ? width : 400;
  w->height = height > 0 ? height : 300;
  int scr = x.DefaultScreen(d);
  w->win = x.CreateSimpleWindow(d, x.RootWindow(d, scr), 0, 0, unsigned(w->width), unsigned(w->height), 0,
                                x.BlackPixel(d, scr), x.BlackPixel(d, scr));
  if (!w->win) return nullptr;
  windows[w->win] = w.get();
  x.SelectInput(d, w->win, StructureNotifyMask);
  x.SetWMProtocols(d, w->win, &x.wmDelete, 1);
  x.ChangeProperty(d, w->win, x.netName, x.utf8, 8, PropModeReplace,
                   reinterpret_cast<const unsigned char*>(title.c_str()), int(title.size()));
  XClassHint cls{const_cast<char*>("oros-bridge"), const_cast<char*>("orOS Bridge")};
  x.SetClassHint(d, w->win, &cls);
  w->fixSize(w->width, w->height);
  x.Sync(d, False);   // the plugin talks to the X server over its own connection: the window must exist there
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
