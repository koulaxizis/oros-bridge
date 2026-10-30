#pragma once
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ob {
// A shared library loaded from a plugin file (the .clap file itself).
class DynLib {
public:
  explicit DynLib(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    h_ = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    h_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
  }
  ~DynLib() {
    if (!h_) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)h_);
#else
    dlclose(h_);
#endif
  }
  DynLib(const DynLib&) = delete;
  DynLib& operator=(const DynLib&) = delete;
  bool ok() const { return h_ != nullptr; }
  void* symbol(const char* name) const {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)h_, name);
#else
    return dlsym(h_, name);
#endif
  }
  static std::string lastError() {
#ifdef _WIN32
    return "error " + std::to_string(GetLastError());
#else
    const char* e = dlerror();
    return e ? e : "unknown";
#endif
  }

private:
  void* h_ = nullptr;
};
}
