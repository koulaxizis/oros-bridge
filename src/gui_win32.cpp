// The bridge's window and tray icon on Windows. See gui.h and DESIGN.md.
#include "gui.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <atomic>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <thread>
#include "mainloop.h"
#include "scan.h"

namespace ob {
namespace {
namespace fs = std::filesystem;

constexpr UINT kTrayMsg = WM_APP + 1;   // tray icon events
constexpr UINT kShowMsg = WM_APP + 2;   // a second launch asks for the window
constexpr UINT_PTR kTimer = 1;
constexpr wchar_t kClass[] = L"OrosBridgeMain";
constexpr wchar_t kTitle[] = L"orOS Bridge";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"orOS Bridge";
enum { ID_SCAN = 101, ID_FORGET, ID_AUTOSTART, ID_HIDE, ID_OPEN, ID_QUIT };

struct Strings {
  const wchar_t *ready, *connected, *pairLabel, *hint, *plugins, *noPlugins, *scanning, *folders,
      *scan, *forget, *autostart, *hide, *forgetAsk, *trayHint, *codeTip, *open, *quit, *cannotStart;
};
const Strings kEn = {
    L"Ready. orOS Studio can connect.",
    L"orOS Studio is connected.",
    L"Pairing code — type it in orOS Studio:",
    L"In Studio: a track's settings › Plugin on this computer.",
    L"Plugins found: %d (%d instruments, %d effects)",
    L"No plugins found yet.",
    L"Looking for plugins…",
    L"Plugin folders:",
    L"Look for plugins",
    L"Forget paired browsers",
    L"Start with Windows",
    L"Hide",
    L"Forget every paired browser? orOS Studio will ask for a new code.",
    L"orOS Bridge keeps running here. Right-click for the menu.",
    L"Pairing code: ",
    L"Open",
    L"Quit",
    L"orOS Bridge could not start:",
};
const Strings kEl = {
    L"Έτοιμο. Το orOS Studio μπορεί να συνδεθεί.",
    L"Το orOS Studio είναι συνδεδεμένο.",
    L"Κωδικός σύζευξης — πληκτρολόγησέ τον στο orOS Studio:",
    L"Στο Studio: ρυθμίσεις καναλιού › Plugin σε αυτόν τον υπολογιστή.",
    L"Plugins που βρέθηκαν: %d (%d όργανα, %d εφέ)",
    L"Δεν βρέθηκαν plugins ακόμα.",
    L"Αναζήτηση plugins…",
    L"Φάκελοι plugins:",
    L"Αναζήτηση plugins",
    L"Ξέχασε τους browsers",
    L"Εκκίνηση με τα Windows",
    L"Απόκρυψη",
    L"Να ξεχαστούν όλοι οι συνδεδεμένοι browsers; Το orOS Studio θα ζητήσει νέο κωδικό.",
    L"Το orOS Bridge συνεχίζει να τρέχει εδώ. Δεξί κλικ για το μενού.",
    L"Κωδικός σύζευξης: ",
    L"Άνοιγμα",
    L"Έξοδος",
    L"Το orOS Bridge δεν μπόρεσε να ξεκινήσει:",
};

const Strings& strings() {
  return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GREEK ? kEl : kEn;
}

std::wstring wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

struct Gui {
  Config* cfg = nullptr;
  Server* server = nullptr;
  Pairing* pairing = nullptr;
  const Strings* s = &kEn;
  HWND hwnd = nullptr;
  HWND status = nullptr, pairLabel = nullptr, code = nullptr, plugins = nullptr, folders = nullptr;
  HWND scan = nullptr, autostart = nullptr;
  HFONT font = nullptr, bold = nullptr, big = nullptr;
  HICON icon = nullptr, iconSm = nullptr;
  NOTIFYICONDATAW nid{};
  UINT taskbarCreated = 0;
  int dpi = 96;
  // what the window shows now, so the timer only touches what changed
  std::string shownCode = "-";
  int shownLive = -1;
  std::wstring shownPlugins, shownFolders;
  std::atomic<bool> scanning{false};
};
Gui g;

int px(int dip) { return MulDiv(dip, g.dpi, 96); }

std::wstring spaced(const std::string& code) {
  return wide(code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code);
}

// ---------- start with Windows (HKCU Run) ----------
std::wstring runCommand() { return L"\"" + wide(selfPath()) + L"\" --tray"; }

bool autostartOn() {
  wchar_t buf[2048];
  DWORD n = sizeof buf;
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS;
}

void setAutostart(bool on) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
  if (on) {
    auto c = runCommand();
    RegSetValueExW(k, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(c.c_str()), DWORD((c.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(k, kRunValue);
  }
  RegCloseKey(k);
  if (g.autostart) SendMessageW(g.autostart, BM_SETCHECK, autostartOn() ? BST_CHECKED : BST_UNCHECKED, 0);
}

// ---------- tray ----------
void addTray() {
  g.nid = {};
  g.nid.cbSize = sizeof(g.nid);
  g.nid.hWnd = g.hwnd;
  g.nid.uID = 1;
  g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g.nid.uCallbackMessage = kTrayMsg;
  g.nid.hIcon = g.iconSm;
  lstrcpynW(g.nid.szTip, kTitle, ARRAYSIZE(g.nid.szTip));
  Shell_NotifyIconW(NIM_ADD, &g.nid);
}

void balloon(const std::wstring& text) {
  NOTIFYICONDATAW n = g.nid;
  n.uFlags = NIF_INFO;
  lstrcpynW(n.szInfo, text.c_str(), ARRAYSIZE(n.szInfo));
  lstrcpynW(n.szInfoTitle, kTitle, ARRAYSIZE(n.szInfoTitle));
  n.dwInfoFlags = NIIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

[[noreturn]] void quit() {
  Shell_NotifyIconW(NIM_DELETE, &g.nid);
  std::_Exit(0);   // as Ctrl+C did: the system frees the plugins
}

// ---------- window ----------
void showWindow() {
  if (IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
  ShowWindow(g.hwnd, SW_SHOW);
  SetForegroundWindow(g.hwnd);   // from the background Windows flashes the taskbar instead
}

void hideToTray() {
  ShowWindow(g.hwnd, SW_HIDE);
  // the first time ever, say where the bridge went
  std::error_code ec;
  auto mark = fs::u8path(g.cfg->dir()) / "tray-hint-shown";
  if (!fs::exists(mark, ec)) {
    balloon(g.s->trayHint);
    fs::create_directories(mark.parent_path(), ec);
    std::ofstream(mark).put('1');
  }
}

void refreshStatus() {
  int live = g.server->connections();
  if ((live > 0) != (g.shownLive > 0) || g.shownLive < 0)
    SetWindowTextW(g.status, live > 0 ? g.s->connected : g.s->ready);
  g.shownLive = live;
  std::string code = g.pairing->currentCode();
  if (code == g.shownCode) return;
  g.shownCode = code;
  if (code.empty()) {
    SetWindowTextW(g.pairLabel, g.s->hint);
    ShowWindow(g.code, SW_HIDE);
  } else {
    SetWindowTextW(g.pairLabel, g.s->pairLabel);
    SetWindowTextW(g.code, spaced(code).c_str());
    ShowWindow(g.code, SW_SHOW);
  }
}

void refreshPlugins() {
  bool scanning = g.scanning;
  std::wstring text;
  if (scanning) {
    text = g.s->scanning;
  } else {
    auto list = g.cfg->plugins();
    int inst = 0;
    for (auto& p : list) inst += p.instrument ? 1 : 0;
    wchar_t buf[256];
    std::swprintf(buf, 256, g.s->plugins, int(list.size()), inst, int(list.size()) - inst);
    text = list.empty() ? g.s->noPlugins : buf;
  }
  if (text != g.shownPlugins) {
    g.shownPlugins = text;
    SetWindowTextW(g.plugins, text.c_str());
  }
  EnableWindow(g.scan, !scanning);
  std::wstring f;
  for (auto& d : g.cfg->folders()) f += (f.empty() ? L"" : L"\r\n") + wide(d);
  if (f != g.shownFolders) {
    g.shownFolders = f;
    SetWindowTextW(g.folders, f.c_str());
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

void trayMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_OPEN, g.s->open);
  AppendMenuW(m, MF_STRING | (autostartOn() ? MF_CHECKED : 0), ID_AUTOSTART, g.s->autostart);
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_QUIT, g.s->quit);
  SetMenuDefaultItem(m, ID_OPEN, FALSE);
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g.hwnd);   // so the menu closes when the person clicks elsewhere
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g.hwnd, nullptr);
  PostMessageW(g.hwnd, WM_NULL, 0, 0);
  DestroyMenu(m);
  if (cmd == ID_OPEN) showWindow();
  else if (cmd == ID_AUTOSTART) setAutostart(!autostartOn());
  else if (cmd == ID_QUIT) quit();
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (g.taskbarCreated && msg == g.taskbarCreated) {   // Explorer restarted
    addTray();
    return 0;
  }
  switch (msg) {
    case WM_TIMER:
      refreshStatus();
      refreshPlugins();
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case ID_SCAN: startScan(); break;
        case ID_FORGET:
          if (MessageBoxW(h, g.s->forgetAsk, kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) g.cfg->forgetTokens();
          break;
        case ID_AUTOSTART:
          if (HIWORD(wp) == BN_CLICKED) setAutostart(SendMessageW(g.autostart, BM_GETCHECK, 0, 0) == BST_CHECKED);
          break;
        case ID_HIDE: hideToTray(); break;
      }
      return 0;
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) hideToTray();
      return 0;
    case WM_CLOSE:   // closing keeps the bridge running in the tray; Quit is in its menu
      hideToTray();
      return 0;
    case WM_ENDSESSION:
      if (wp) Shell_NotifyIconW(NIM_DELETE, &g.nid);
      return 0;
    case kShowMsg:
      showWindow();
      return 0;
    case kTrayMsg:
      switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: showWindow(); break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: trayMenu(); break;
      }
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f, DWORD ex = 0) {
  HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), g.hwnd,
                           reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
  return c;
}

void createWindow() {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  HDC dc = GetDC(nullptr);
  g.dpi = GetDeviceCaps(dc, LOGPIXELSY);
  ReleaseDC(nullptr, dc);

  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
  LOGFONTW lf = ncm.lfMessageFont;
  g.font = CreateFontIndirectW(&lf);
  lf.lfWeight = FW_SEMIBOLD;
  g.bold = CreateFontIndirectW(&lf);
  lf.lfHeight = -px(34);
  lf.lfWeight = FW_BOLD;
  g.big = CreateFontIndirectW(&lf);

  g.icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
  g.iconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = proc;
  wc.hInstance = inst;
  wc.hIcon = g.icon;
  wc.hIconSm = g.iconSm;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  const int W = 460, H = 350, M = 16, inner = W - 2 * M;
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, px(W), px(H)};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  g.hwnd = CreateWindowExW(0, kClass, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, inst, nullptr);

  const auto& s = *g.s;
  g.status = add(L"STATIC", s.ready, SS_NOPREFIX, M, 16, inner, 22, 0, g.bold);
  g.pairLabel = add(L"STATIC", s.hint, SS_NOPREFIX, M, 44, inner, 34, 0, g.font);
  g.code = add(L"STATIC", L"", SS_NOPREFIX, M, 78, inner, 50, 0, g.big);
  ShowWindow(g.code, SW_HIDE);
  g.plugins = add(L"STATIC", L"", SS_NOPREFIX, M, 134, inner, 20, 0, g.font);
  add(L"STATIC", s.folders, SS_NOPREFIX, M, 158, inner, 20, 0, g.font);
  g.folders = add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, M, 180, inner, 72, 0, g.font, WS_EX_CLIENTEDGE);
  g.scan = add(L"BUTTON", s.scan, BS_PUSHBUTTON | WS_TABSTOP, M, 264, 180, 30, ID_SCAN, g.font);
  add(L"BUTTON", s.forget, BS_PUSHBUTTON | WS_TABSTOP, M + 188, 264, 220, 30, ID_FORGET, g.font);
  g.autostart = add(L"BUTTON", s.autostart, BS_AUTOCHECKBOX | WS_TABSTOP, M, 308, 280, 24, ID_AUTOSTART, g.font);
  add(L"BUTTON", s.hide, BS_DEFPUSHBUTTON | WS_TABSTOP, M + inner - 110, 305, 110, 30, ID_HIDE, g.font);
}
}  // namespace

bool guiAlreadyRunning() {
  // held until the process ends
  HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\orOS-Bridge");
  if (!m || GetLastError() != ERROR_ALREADY_EXISTS) return false;
  CloseHandle(m);
  if (HWND w = FindWindowW(kClass, nullptr)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    AllowSetForegroundWindow(pid);
    PostMessageW(w, kShowMsg, 0, 0);
  }
  return true;
}

void guiPairingCode(const std::string& code) {
  MainLoop::get().post([code] {
    if (!g.hwnd) return;
    bool wasHidden = !IsWindowVisible(g.hwnd) || IsIconic(g.hwnd);
    refreshStatus();
    showWindow();
    if (wasHidden) balloon(g.s->codeTip + spaced(code));
  });
}

void guiError(const std::string& msg) {
  std::wstring text = std::wstring(strings().cannotStart) + L"\n\n" + wide(msg);
  MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | MB_ICONERROR);
}

int guiRun(Config& cfg, Server& server, Pairing& pairing, bool hidden) {
  g.cfg = &cfg;
  g.server = &server;
  g.pairing = &pairing;
  g.s = &strings();
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  // Our window follows the system DPI so its text is sharp; the process stays
  // DPI-unaware so plugin editors look as they always did (Windows scales them).
  using SetCtx = HANDLE(WINAPI*)(HANDLE);
  auto setCtx = reinterpret_cast<SetCtx>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
  HANDLE old = setCtx ? setCtx(reinterpret_cast<HANDLE>(INT_PTR(-2))) : nullptr;   // SYSTEM_AWARE
  createWindow();
  if (old) setCtx(old);

  g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  addTray();
  if (autostartOn()) setAutostart(true);   // follow the program if it was moved
  SendMessageW(g.autostart, BM_SETCHECK, autostartOn() ? BST_CHECKED : BST_UNCHECKED, 0);
  refreshStatus();
  refreshPlugins();
  SetTimer(g.hwnd, kTimer, 500, nullptr);
  if (!hidden) showWindow();
  MainLoop::get().run();
  quit();
}
}
