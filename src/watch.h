#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "config.h"

namespace ob {

// Folders on this computer that orOS hears about (0.11.0, DESIGN decision 19).
// Only the person at this computer adds a folder (the bridge's window, the
// system's folder chooser that a page can only ask to open, or --watch); a
// page can list them, take one off, hear about new files, read a file it was
// told about and print it. Nothing else on the disk is reachable.
struct WatchedFile {
  std::string key;      // 16 hex: SHA-256 of folder, name, size and time
  std::string folder;   // the watched folder (as added)
  std::string name;     // the file's name in it (no folders below)
  std::string kind;     // "pdf", "image" or "other"
  uint64_t size = 0;
  int64_t mtime = 0;    // ms since 1970
  int64_t seen = 0;     // ms since 1970, when the bridge reported it
};
void to_json(nlohmann::json& j, const WatchedFile& f);

namespace watch {
constexpr size_t kMaxFolders = 16;
constexpr size_t kRecent = 100;                       // files a page can still ask about
constexpr uint64_t kMaxRead = 512ull * 1024 * 1024;   // files larger are listed, never read
constexpr uint32_t kMaxChunk = 4u * 1024 * 1024;

// The kind from the name: .pdf, the usual photo formats, everything else.
std::string kindOf(const std::string& name);
// Temporary and system files that never count (downloads in progress,
// Office lock files, hidden files, Thumbs.db).
bool ignored(const std::string& name);

void start(Config& cfg);   // the thread that looks every 2 s
std::vector<std::string> folders();
// What the folder list looks like now (the window redraws when it changes).
int version();
// adds a folder (it must exist); its files now are the baseline, never reported
bool add(const std::string& path, std::string& err);
bool remove(const std::string& path);
std::vector<WatchedFile> recent();
bool find(const std::string& key, WatchedFile& out);
// Reads part of a reported file, after checking it is still the same file
// (size and time) and still a plain file directly in its folder.
// err: "notfound", "changed", "toolarge", "unreadable"
bool read(const std::string& key, uint64_t offset, uint32_t length, std::vector<uint8_t>& out, uint64_t& size, std::string& err);
// Prints a reported file with the system (Windows: the file type's print
// command; Linux: CUPS lp). err: "notfound", "changed", "noprinter", "failed"
bool print(const std::string& key, std::string& err);
void setTestPrintDir(const std::string& dir);   // --test-print: copy instead (tests)

// A page that listens: called on the watcher's thread for each new file.
int listen(std::function<void(const WatchedFile&)> cb);
void unlisten(int id);

// The system's folder chooser, shown on this computer; "" when the person
// cancelled. err: "nodialog" (no chooser on this system), "busy" (one is open)
bool pick(const std::string& lang, std::string& path, std::string& err);
// The same without waiting (the window's own button): done(1, path) chosen,
// done(0, "") cancelled, done(-1, "") no chooser, done(-2, "") one is open.
// done may run on any thread.
void pickAsync(const std::string& lang, std::function<void(int, std::string)> done);
// The window's own chooser (GTK, on the main loop), used instead of zenity/kdialog.
void setPicker(std::function<void(const std::string&, std::function<void(int, std::string)>)> p);
void setPickTest(const std::string& file);   // --test-pick: the chooser answers what FILE names, empty = cancelled (tests)
}
}
