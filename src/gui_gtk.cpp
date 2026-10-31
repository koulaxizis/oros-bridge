// The bridge's window and tray icon on Linux (0.7.0), the same as on Windows
// (gui_win32.cpp): GTK 3 and an AppIndicator (Ayatana or the older one),
// both loaded at run time. Without GTK or without a display the bridge is
// the command-line program it was before; without an AppIndicator it has a
// window and no tray icon (closing then minimises it, Quit is a button).
// GLib's loop runs inside MainLoop (MainLoop::Guest), so plugin calls, X11
// editors and this window share the main thread. See gui.h and DESIGN.md.
#include "gui.h"
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "gui_icon.h"   // kIconPng, made by CMake from res/oros-bridge.png
#include "mainloop.h"
#include "scan.h"

namespace ob {
namespace {
namespace fs = std::filesystem;

// ---------- the few GLib, GObject, GTK and AppIndicator calls we use ----------
using gboolean = int;
using gpointer = void*;
using GCallback = void (*)();
struct GPollFD { int fd; unsigned short events, revents; };   // the same layout as struct pollfd
using W = void*;   // a GtkWidget* (or another GObject*)

struct Api {
  // glib / gobject
  void* (*g_main_context_default)();
  gboolean (*g_main_context_acquire)(void*);
  gboolean (*g_main_context_prepare)(void*, int*);
  int (*g_main_context_query)(void*, int, int*, GPollFD*, int);
  gboolean (*g_main_context_check)(void*, int, GPollFD*, int);
  void (*g_main_context_dispatch)(void*);
  unsigned long (*g_signal_connect_data)(gpointer, const char*, GCallback, gpointer, void*, int);
  // gtk
  gboolean (*gtk_init_check)(int*, char***);
  W (*gtk_window_new)(int);
  void (*gtk_window_set_title)(W, const char*);
  void (*gtk_window_set_resizable)(W, gboolean);
  gboolean (*gtk_window_set_icon_from_file)(W, const char*, void*);
  void (*gtk_window_present)(W);
  void (*gtk_window_iconify)(W);
  void (*gtk_window_set_urgency_hint)(W, gboolean);
  void (*gtk_window_set_wmclass)(W, const char*, const char*);
  void (*gtk_container_add)(W, W);
  void (*gtk_container_set_border_width)(W, unsigned);
  W (*gtk_box_new)(int, int);
  void (*gtk_box_pack_start)(W, W, gboolean, gboolean, unsigned);
  void (*gtk_box_pack_end)(W, W, gboolean, gboolean, unsigned);
  W (*gtk_label_new)(const char*);
  void (*gtk_label_set_text)(W, const char*);
  void (*gtk_label_set_markup)(W, const char*);
  void (*gtk_label_set_xalign)(W, float);
  void (*gtk_label_set_line_wrap)(W, gboolean);
  void (*gtk_label_set_selectable)(W, gboolean);
  W (*gtk_button_new_with_label)(const char*);
  W (*gtk_check_button_new_with_label)(const char*);
  void (*gtk_toggle_button_set_active)(W, gboolean);
  gboolean (*gtk_toggle_button_get_active)(W);
  void (*gtk_widget_set_sensitive)(W, gboolean);
  void (*gtk_widget_show_all)(W);
  void (*gtk_widget_show)(W);
  void (*gtk_widget_hide)(W);
  void (*gtk_widget_destroy)(W);
  void (*gtk_widget_set_size_request)(W, int, int);
  void (*gtk_widget_set_can_focus)(W, gboolean);
  W (*gtk_message_dialog_new)(W, int, int, int, const char*, ...);
  W (*gtk_menu_new)();
  W (*gtk_menu_item_new_with_label)(const char*);
  W (*gtk_check_menu_item_new_with_label)(const char*);
  void (*gtk_check_menu_item_set_active)(W, gboolean);
  gboolean (*gtk_check_menu_item_get_active)(W);
  W (*gtk_separator_menu_item_new)();
  void (*gtk_menu_shell_append)(W, W);
  // app indicator (optional)
  void* (*app_indicator_new_with_path)(const char*, const char*, int, const char*) = nullptr;
  void (*app_indicator_set_status)(void*, int) = nullptr;
  void (*app_indicator_set_menu)(void*, W) = nullptr;
  void (*app_indicator_set_title)(void*, const char*) = nullptr;
  void (*app_indicator_set_secondary_activate_target)(void*, W) = nullptr;

  bool loaded = false, tried = false;
  bool load() {
    if (tried) return loaded;
    tried = true;
    void* gtk = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!gtk) return false;
    bool ok = true;
    auto get = [&](void* lib, auto& f, const char* name) {
      f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, name));
      ok = ok && f;
    };
#define G(name) get(gtk, name, #name)   // GTK pulls in GLib and GObject: their symbols resolve through it
    G(g_main_context_default); G(g_main_context_acquire); G(g_main_context_prepare); G(g_main_context_query);
    G(g_main_context_check); G(g_main_context_dispatch); G(g_signal_connect_data);
    G(gtk_init_check); G(gtk_window_new); G(gtk_window_set_title); G(gtk_window_set_resizable);
    G(gtk_window_set_icon_from_file); G(gtk_window_present); G(gtk_window_iconify); G(gtk_window_set_urgency_hint);
    G(gtk_window_set_wmclass); G(gtk_container_add); G(gtk_container_set_border_width); G(gtk_box_new);
    G(gtk_box_pack_start); G(gtk_box_pack_end); G(gtk_label_new); G(gtk_label_set_text); G(gtk_label_set_markup);
    G(gtk_label_set_xalign); G(gtk_label_set_line_wrap); G(gtk_label_set_selectable); G(gtk_button_new_with_label);
    G(gtk_check_button_new_with_label); G(gtk_toggle_button_set_active); G(gtk_toggle_button_get_active);
    G(gtk_widget_set_sensitive); G(gtk_widget_show_all); G(gtk_widget_show); G(gtk_widget_hide); G(gtk_widget_destroy);
    G(gtk_widget_set_size_request); G(gtk_widget_set_can_focus); G(gtk_message_dialog_new); G(gtk_menu_new); G(gtk_menu_item_new_with_label);
    G(gtk_check_menu_item_new_with_label); G(gtk_check_menu_item_set_active); G(gtk_check_menu_item_get_active);
    G(gtk_separator_menu_item_new); G(gtk_menu_shell_append);
#undef G
    if (!ok) return false;
    for (const char* name : {"libayatana-appindicator3.so.1", "libappindicator3.so.1"}) {
      void* ai = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
      if (!ai) continue;
      bool aok = true;
      auto aget = [&](auto& f, const char* n) {
        f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(ai, n));
        aok = aok && f;
      };
      aget(app_indicator_new_with_path, "app_indicator_new_with_path");
      aget(app_indicator_set_status, "app_indicator_set_status");
      aget(app_indicator_set_menu, "app_indicator_set_menu");
      aget(app_indicator_set_title, "app_indicator_set_title");
      aget(app_indicator_set_secondary_activate_target, "app_indicator_set_secondary_activate_target");
      if (aok) break;
      app_indicator_new_with_path = nullptr;
    }
    loaded = true;
    return true;
  }
  bool tray() const { return app_indicator_new_with_path != nullptr; }
};
Api a;

constexpr int kWindowToplevel = 0, kVertical = 1, kHorizontal = 0;
constexpr int kDialogModal = 1 << 0, kDialogDestroyWithParent = 1 << 1;
constexpr int kMessageQuestion = 2, kMessageError = 3, kButtonsOk = 1, kButtonsYesNo = 4, kResponseYes = -8;
constexpr int kIndicatorApplicationStatus = 0, kIndicatorActive = 1;

template <class F>
void on(W w, const char* signal, F f, gpointer data = nullptr) {
  a.g_signal_connect_data(w, signal, reinterpret_cast<GCallback>(f), data, nullptr, 0);
}

// ---------- texts (Greek when the language is Greek) ----------
struct Strings {
  const char *ready, *connected, *pairLabel, *hint, *plugins, *noPlugins, *scanning, *folders, *scan, *forget,
      *autostart, *hide, *minimise, *forgetAsk, *open, *quit, *cannotStart;
};
const Strings kEn = {
    "Ready. orOS Studio can connect.",
    "orOS Studio is connected.",
    "Pairing code — type it in orOS Studio:",
    "In Studio: a track's settings › Plugin on this computer.",
    "Plugins found: %d (%d instruments, %d effects)",
    "No plugins found yet.",
    "Looking for plugins…",
    "Plugin folders:",
    "Look for plugins",
    "Forget paired browsers",
    "Start when I log in",
    "Hide",
    "Minimise",
    "Forget every paired browser? orOS Studio will ask for a new code.",
    "Open",
    "Quit",
    "orOS Bridge could not start:",
};
const Strings kEl = {
    "Έτοιμο. Το orOS Studio μπορεί να συνδεθεί.",
    "Το orOS Studio είναι συνδεδεμένο.",
    "Κωδικός σύζευξης — πληκτρολόγησέ τον στο orOS Studio:",
    "Στο Studio: ρυθμίσεις καναλιού › Plugin σε αυτόν τον υπολογιστή.",
    "Plugins που βρέθηκαν: %d (%d όργανα, %d εφέ)",
    "Δεν βρέθηκαν plugins ακόμα.",
    "Αναζήτηση plugins…",
    "Φάκελοι plugins:",
    "Αναζήτηση plugins",
    "Ξέχασε τους browsers",
    "Εκκίνηση κατά τη σύνδεση",
    "Απόκρυψη",
    "Ελαχιστοποίηση",
    "Να ξεχαστούν όλοι οι συνδεδεμένοι browsers; Το orOS Studio θα ζητήσει νέο κωδικό.",
    "Άνοιγμα",
    "Έξοδος",
    "Το orOS Bridge δεν μπόρεσε να ξεκινήσει:",
};
const Strings& strings() {
  for (const char* v : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
    const char* s = std::getenv(v);
    if (s && *s) return std::strncmp(s, "el", 2) == 0 ? kEl : kEn;
  }
  return kEn;
}

std::string home() {
  const char* h = std::getenv("HOME");
  return h ? h : "";
}
std::string xdg(const char* var, const char* fallback) {
  const char* v = std::getenv(var);
  return v && *v == '/' ? std::string(v) : home() + fallback;
}

// ---------- one bridge per user: an abstract socket; a second launch says "show" ----------
int instanceFd = -1;
sockaddr_un instanceAddr(socklen_t& len) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::string name = "oros-bridge-" + std::to_string(getuid());
  std::memcpy(addr.sun_path + 1, name.data(), name.size());   // sun_path[0] = 0: abstract, gone with the process
  len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  return addr;
}

// ---------- the window ----------
struct Gui {
  Config* cfg = nullptr;
  Server* server = nullptr;
  Pairing* pairing = nullptr;
  const Strings* s = &kEn;
  W win = nullptr, status = nullptr, pairLabel = nullptr, code = nullptr, plugins = nullptr, folders = nullptr;
  W scan = nullptr, autostart = nullptr, menuAutostart = nullptr;
  void* indicator = nullptr;
  bool setting = false;   // a toggle changed by us, not by the person
  std::string shownCode = "-", shownPlugins, shownFolders;
  int shownLive = -1;
  std::atomic<bool> scanning{false};
  std::string iconDir, iconFile;
};
Gui g;

std::string spaced(const std::string& code) { return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code; }

// the icon as a file, for the window and the tray (the program may be a lone binary or an AppImage)
void writeIcon() {
  g.iconDir = xdg("XDG_CACHE_HOME", "/.cache") + "/oros-bridge/icons";
  std::string dir = g.iconDir + "/hicolor/256x256/apps";
  g.iconFile = dir + "/oros-bridge.png";
  std::error_code ec;
  if (fs::exists(g.iconFile, ec) && fs::file_size(g.iconFile, ec) == sizeof kIconPng) return;
  fs::create_directories(dir, ec);
  std::ofstream(g.iconFile, std::ios::binary).write(reinterpret_cast<const char*>(kIconPng), sizeof kIconPng);
}

// ---------- start when the person logs in (XDG autostart) ----------
std::string autostartFile() { return xdg("XDG_CONFIG_HOME", "/.config") + "/autostart/oros-bridge.desktop"; }
std::string programPath() {
  const char* app = std::getenv("APPIMAGE");   // inside an AppImage: the AppImage itself, not its mount
  return app && *app ? app : selfPath();
}
bool autostartOn() {
  std::error_code ec;
  return fs::exists(autostartFile(), ec);
}
void setAutostart(bool on) {
  std::error_code ec;
  if (on) {
    fs::create_directories(fs::path(autostartFile()).parent_path(), ec);
    std::ofstream f(autostartFile());
    f << "[Desktop Entry]\nType=Application\nName=orOS Bridge\nComment=Plugins and the sound card for orOS Studio\n"
      << "Exec=\"" << programPath() << "\" --tray\nIcon=oros-bridge\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
  } else {
    fs::remove(autostartFile(), ec);
  }
  g.setting = true;
  bool now = autostartOn();
  if (g.autostart) a.gtk_toggle_button_set_active(g.autostart, now);
  if (g.menuAutostart) a.gtk_check_menu_item_set_active(g.menuAutostart, now);
  g.setting = false;
}

[[noreturn]] void quit() { std::_Exit(0); }   // as Ctrl+C did: the system frees the plugins

void showWindow() {
  a.gtk_widget_show(g.win);
  a.gtk_window_present(g.win);
}
void hideWindow() {
  if (a.tray()) a.gtk_widget_hide(g.win);
  else a.gtk_window_iconify(g.win);   // no tray icon to come back from
}

void refreshStatus() {
  int live = g.server->connections();
  if ((live > 0) != (g.shownLive > 0) || g.shownLive < 0)
    a.gtk_label_set_markup(g.status, (std::string("<b>") + (live > 0 ? g.s->connected : g.s->ready) + "</b>").c_str());
  g.shownLive = live;
  std::string code = g.pairing->currentCode();
  if (code == g.shownCode) return;
  g.shownCode = code;
  if (code.empty()) {
    a.gtk_label_set_text(g.pairLabel, g.s->hint);
    a.gtk_widget_hide(g.code);
  } else {
    a.gtk_label_set_text(g.pairLabel, g.s->pairLabel);
    // digits and a space only, so no markup escaping is needed
    a.gtk_label_set_markup(g.code, ("<span size=\"xx-large\" weight=\"bold\">" + spaced(code) + "</span>").c_str());
    a.gtk_widget_show(g.code);
  }
}
void refreshPlugins() {
  bool scanning = g.scanning;
  std::string text;
  if (scanning) {
    text = g.s->scanning;
  } else {
    auto list = g.cfg->plugins();
    int inst = 0;
    for (auto& p : list) inst += p.instrument ? 1 : 0;
    char buf[256];
    std::snprintf(buf, sizeof buf, g.s->plugins, int(list.size()), inst, int(list.size()) - inst);
    text = list.empty() ? g.s->noPlugins : buf;
  }
  if (text != g.shownPlugins) {
    g.shownPlugins = text;
    a.gtk_label_set_text(g.plugins, text.c_str());
  }
  a.gtk_widget_set_sensitive(g.scan, !scanning);
  std::string f;
  for (auto& d : g.cfg->folders()) f += (f.empty() ? "" : "\n") + d;
  if (f != g.shownFolders) {
    g.shownFolders = f;
    a.gtk_label_set_text(g.folders, f.c_str());
  }
}
void startScan() {
  if (g.scanning.exchange(true)) return;
  refreshPlugins();
  Config* cfg = g.cfg;
  std::thread([cfg] {
    try {
      scanPlugins(*cfg, false, [](const std::string&) {});
    } catch (...) {
    }
    MainLoop::get().post([] {
      g.scanning = false;
      refreshPlugins();
    });
  }).detach();
}
void askForget() {
  // not gtk_dialog_run: a nested loop would stop MainLoop (and every plugin call) until it is answered
  W d = a.gtk_message_dialog_new(g.win, kDialogModal | kDialogDestroyWithParent, kMessageQuestion, kButtonsYesNo, "%s", g.s->forgetAsk);
  a.gtk_window_set_title(d, "orOS Bridge");
  on(d, "response", +[](W dlg, int response, gpointer) {
    if (response == kResponseYes) g.cfg->forgetTokens();
    a.gtk_widget_destroy(dlg);
  });
  a.gtk_widget_show_all(d);
}

W label(const char* text, bool wrap = false) {
  W l = a.gtk_label_new(text);
  a.gtk_label_set_xalign(l, 0);
  if (wrap) a.gtk_label_set_line_wrap(l, 1);
  return l;
}

void createWindow() {
  const auto& s = *g.s;
  g.win = a.gtk_window_new(kWindowToplevel);
  a.gtk_window_set_title(g.win, "orOS Bridge");
  a.gtk_window_set_wmclass(g.win, "oros-bridge", "orOS Bridge");
  a.gtk_window_set_resizable(g.win, 0);
  a.gtk_window_set_icon_from_file(g.win, g.iconFile.c_str(), nullptr);
  a.gtk_container_set_border_width(g.win, 16);
  on(g.win, "delete-event", +[](W, void*, gpointer) -> gboolean {
    hideWindow();   // closing keeps the bridge running; Quit is in the tray menu and on the window
    return 1;
  });
  W box = a.gtk_box_new(kVertical, 8);
  a.gtk_widget_set_size_request(box, 428, -1);
  a.gtk_container_add(g.win, box);
  g.status = label("");
  g.pairLabel = label(s.hint, true);
  g.code = label("");
  g.plugins = label("");
  g.folders = label("");
  a.gtk_label_set_selectable(g.folders, 1);
  a.gtk_widget_set_can_focus(g.folders, 0);   // selectable for copying, but not focused (all selected) at start
  a.gtk_label_set_line_wrap(g.folders, 1);
  for (W w : {g.status, g.pairLabel, g.code, g.plugins}) a.gtk_box_pack_start(box, w, 0, 0, 0);
  a.gtk_box_pack_start(box, label(s.folders), 0, 0, 0);
  a.gtk_box_pack_start(box, g.folders, 0, 0, 0);

  W row = a.gtk_box_new(kHorizontal, 8);
  g.scan = a.gtk_button_new_with_label(s.scan);
  W forget = a.gtk_button_new_with_label(s.forget);
  on(g.scan, "clicked", +[](W, gpointer) { startScan(); });
  on(forget, "clicked", +[](W, gpointer) { askForget(); });
  a.gtk_box_pack_start(row, g.scan, 0, 0, 0);
  a.gtk_box_pack_start(row, forget, 0, 0, 0);
  a.gtk_box_pack_start(box, row, 0, 0, 4);

  W bottom = a.gtk_box_new(kHorizontal, 8);
  g.autostart = a.gtk_check_button_new_with_label(s.autostart);
  on(g.autostart, "toggled", +[](W b, gpointer) {
    if (!g.setting) setAutostart(a.gtk_toggle_button_get_active(b));
  });
  W hide = a.gtk_button_new_with_label(a.tray() ? s.hide : s.minimise);
  W quitB = a.gtk_button_new_with_label(s.quit);
  on(hide, "clicked", +[](W, gpointer) { hideWindow(); });
  on(quitB, "clicked", +[](W, gpointer) { quit(); });
  a.gtk_box_pack_start(bottom, g.autostart, 0, 0, 0);
  a.gtk_box_pack_end(bottom, quitB, 0, 0, 0);
  a.gtk_box_pack_end(bottom, hide, 0, 0, 0);
  a.gtk_box_pack_start(box, bottom, 0, 0, 0);
  a.gtk_widget_show_all(box);
  a.gtk_widget_hide(g.code);
}

void createTray() {
  if (!a.tray()) return;
  const auto& s = *g.s;
  W menu = a.gtk_menu_new();
  W open = a.gtk_menu_item_new_with_label(s.open);
  g.menuAutostart = a.gtk_check_menu_item_new_with_label(s.autostart);
  W quitI = a.gtk_menu_item_new_with_label(s.quit);
  on(open, "activate", +[](W, gpointer) { showWindow(); });
  on(g.menuAutostart, "toggled", +[](W m, gpointer) {
    if (!g.setting) setAutostart(a.gtk_check_menu_item_get_active(m));
  });
  on(quitI, "activate", +[](W, gpointer) { quit(); });
  for (W w : {open, g.menuAutostart, a.gtk_separator_menu_item_new(), quitI}) a.gtk_menu_shell_append(menu, w);
  a.gtk_widget_show_all(menu);
  g.indicator = a.app_indicator_new_with_path("oros-bridge", "oros-bridge", kIndicatorApplicationStatus, g.iconDir.c_str());
  if (!g.indicator) return;
  a.app_indicator_set_title(g.indicator, "orOS Bridge");
  a.app_indicator_set_menu(g.indicator, menu);
  a.app_indicator_set_secondary_activate_target(g.indicator, open);   // middle click opens the window
  a.app_indicator_set_status(g.indicator, kIndicatorActive);
}

// GLib's loop, served by MainLoop's poll()
class GlibGuest : public MainLoop::Guest {
public:
  explicit GlibGuest(void* ctx) : ctx_(ctx) {}
  int prepare(std::vector<pollfd>& fds) override {
    bool ready = a.g_main_context_prepare(ctx_, &prio_);
    int timeout = -1;
    for (;;) {
      n_ = a.g_main_context_query(ctx_, prio_, &timeout, gfds_.data(), int(gfds_.size()));
      if (n_ <= int(gfds_.size())) break;
      gfds_.resize(size_t(n_));
    }
    if (ready) timeout = 0;
    for (int i = 0; i < n_; i++) fds.push_back({gfds_[size_t(i)].fd, short(gfds_[size_t(i)].events), 0});
    return timeout;
  }
  void dispatch(const pollfd* fds, size_t n) override {
    for (size_t i = 0; i < n && int(i) < n_; i++) gfds_[i].revents = (unsigned short)fds[i].revents;
    if (a.g_main_context_check(ctx_, prio_, gfds_.data(), n_)) a.g_main_context_dispatch(ctx_);
  }

private:
  void* ctx_;
  int prio_ = 0, n_ = 0;
  std::vector<GPollFD> gfds_ = std::vector<GPollFD>(16);
};

bool gtkReady() {
  static int ready = -1;
  if (ready < 0) ready = a.load() && a.gtk_init_check(nullptr, nullptr) ? 1 : 0;
  return ready == 1;
}
}  // namespace

bool guiAvailable() {
  const char* x = std::getenv("DISPLAY");
  const char* w = std::getenv("WAYLAND_DISPLAY");
  return ((x && *x) || (w && *w)) && gtkReady();
}

bool guiAlreadyRunning() {
  socklen_t len;
  sockaddr_un addr = instanceAddr(len);
  instanceFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (instanceFd < 0) return false;
  if (bind(instanceFd, reinterpret_cast<sockaddr*>(&addr), len) == 0) {
    listen(instanceFd, 4);
    return false;   // we are the first; guiRun() answers the others
  }
  close(instanceFd);
  instanceFd = -1;
  int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (c < 0) return false;
  bool sent = connect(c, reinterpret_cast<sockaddr*>(&addr), len) == 0 && write(c, "show\n", 5) == 5;
  close(c);
  return sent;
}

void guiPairingCode(const std::string&) {
  MainLoop::get().post([] {
    if (!g.win) return;
    refreshStatus();
    showWindow();
    a.gtk_window_set_urgency_hint(g.win, 1);
  });
}

void guiError(const std::string& msg) {
  if (!gtkReady()) return;
  std::string text = std::string(strings().cannotStart) + "\n\n" + msg;
  W d = a.gtk_message_dialog_new(nullptr, kDialogModal, kMessageError, kButtonsOk, "%s", text.c_str());
  a.gtk_window_set_title(d, "orOS Bridge");
  static bool done;
  done = false;
  on(d, "response", +[](W, int, gpointer) { done = true; });
  a.gtk_widget_show_all(d);
  void* ctx = a.g_main_context_default();
  GlibGuest guest(ctx);
  std::vector<pollfd> fds;
  while (!done) {   // before MainLoop runs: a small loop of our own
    fds.clear();
    int t = guest.prepare(fds);
    poll(fds.data(), nfds_t(fds.size()), t);
    guest.dispatch(fds.data(), fds.size());
  }
  a.gtk_widget_destroy(d);
}

int guiRun(Config& cfg, Server& server, Pairing& pairing, bool hidden) {
  g.cfg = &cfg;
  g.server = &server;
  g.pairing = &pairing;
  g.s = &strings();
  void* ctx = a.g_main_context_default();
  a.g_main_context_acquire(ctx);
  static GlibGuest guest(ctx);
  MainLoop::get().setGuest(&guest);
  writeIcon();
  createWindow();
  createTray();
  if (autostartOn()) setAutostart(true);   // follow the program if it was moved
  else setAutostart(false);
  refreshStatus();
  refreshPlugins();
  MainLoop::get().addTimer(500, [] {
    refreshStatus();
    refreshPlugins();
  });
  if (instanceFd >= 0)
    MainLoop::get().addFd(instanceFd, POLLIN, [](int fd, short) {
      int c = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
      if (c < 0) return;
      char buf[16];
      ssize_t n = read(c, buf, sizeof buf);   // a short local write: "show\n"
      close(c);
      if (n > 0) showWindow();
    });
  // without a tray icon a hidden window could not be found again: --tray then starts minimised
  if (!hidden) showWindow();
  else if (!a.tray()) {
    a.gtk_widget_show(g.win);
    a.gtk_window_iconify(g.win);
  }
  MainLoop::get().run();
  quit();
}
}
