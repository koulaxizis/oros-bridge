#include "server.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include "base64.h"
#include "session.h"
#include "sha1.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
#endif

namespace ob {

// ---------- pairing ----------
void Pairing::start() {
  std::string code;
  {
    std::lock_guard<std::mutex> g(mu_);
    uint8_t b[4];
    randomBytes(b, 4);
    uint32_t n = (uint32_t(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3]) % 1000000;
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06u", n);
    code_ = buf;
    wrong_ = 0;
    code = code_;
  }
  if (show_) show_(code);
}

bool Pairing::active() {
  std::lock_guard<std::mutex> g(mu_);
  return !code_.empty();
}

std::string Pairing::currentCodeForTests() {
  std::lock_guard<std::mutex> g(mu_);
  return code_;
}

std::string Pairing::tryCode(const std::string& code) {
  std::lock_guard<std::mutex> g(mu_);
  if (code_.empty() || code.size() != code_.size()) {
    if (!code_.empty() && ++wrong_ >= 3) code_.clear();
    return "";
  }
  unsigned diff = 0;
  for (size_t i = 0; i < code.size(); i++) diff |= unsigned(code[i] ^ code_[i]);
  if (diff) {
    if (++wrong_ >= 3) code_.clear();
    return "";
  }
  code_.clear();
  auto token = randomHex(32);
  cfg_.addToken(token);
  return token;
}

// ---------- checks ----------
static std::string lowerStr(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

bool originAllowed(const std::vector<std::string>& allowed, const std::string& origin) {
  if (origin.empty()) return false;
  auto o = lowerStr(origin);
  for (auto& a : allowed)
    if (lowerStr(a) == o) return true;
  return false;
}

bool hostAllowed(const std::string& host, uint16_t port) {
  auto h = lowerStr(host);
  auto p = ":" + std::to_string(port);
  return h == "127.0.0.1" + p || h == "localhost" + p;
}

std::string wsAccept(const std::string& key) {
  auto h = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  return base64Encode(h.data(), h.size());
}

// ---------- sockets ----------
namespace {
bool sendAll(intptr_t s, const uint8_t* p, size_t n) {
  while (n) {
    int k = ::send((decltype(socket(0, 0, 0)))s, (const char*)p, int(std::min<size_t>(n, 1 << 20)), 0);
    if (k <= 0) return false;
    p += k;
    n -= size_t(k);
  }
  return true;
}
bool recvAll(intptr_t s, uint8_t* p, size_t n) {
  while (n) {
    int k = ::recv((decltype(socket(0, 0, 0)))s, (char*)p, int(std::min<size_t>(n, 1 << 20)), 0);
    if (k <= 0) return false;
    p += k;
    n -= size_t(k);
  }
  return true;
}

constexpr size_t kMaxHeader = 16 * 1024;
constexpr uint64_t kMaxMessage = 64ull << 20;

struct Request {
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string get(const std::string& name) const {
    for (auto& h : headers)
      if (lowerStr(h.first) == name) return h.second;
    return "";
  }
};

bool readRequest(intptr_t s, Request& r) {
  std::string buf;
  char c;
  while (buf.size() < kMaxHeader) {
    if (!recvAll(s, (uint8_t*)&c, 1)) return false;
    buf += c;
    if (buf.size() >= 4 && buf.compare(buf.size() - 4, 4, "\r\n\r\n") == 0) break;
  }
  if (buf.size() >= kMaxHeader) return false;
  size_t pos = buf.find("\r\n");
  auto line = buf.substr(0, pos);
  auto sp1 = line.find(' '), sp2 = line.rfind(' ');
  if (sp1 == std::string::npos || sp2 == sp1) return false;
  r.method = line.substr(0, sp1);
  r.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
  while (true) {
    size_t next = buf.find("\r\n", pos + 2);
    if (next == std::string::npos || next == pos + 2) break;
    auto h = buf.substr(pos + 2, next - pos - 2);
    auto colon = h.find(':');
    if (colon != std::string::npos) {
      auto v = h.substr(colon + 1);
      v.erase(0, v.find_first_not_of(" \t"));
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
      r.headers.emplace_back(h.substr(0, colon), v);
    }
    pos = next;
  }
  return true;
}

void respond(intptr_t s, const std::string& status, const std::string& extra, const std::string& body) {
  std::string out = "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n" + body;
  sendAll(s, (const uint8_t*)out.data(), out.size());
}

class WsConnection : public Connection {
public:
  explicit WsConnection(intptr_t s) : s_(s) {}
  void sendText(const std::string& t) override { frame(0x1, (const uint8_t*)t.data(), t.size()); }
  void sendBinary(const std::vector<uint8_t>& b) override { frame(0x2, b.data(), b.size()); }
  void frame(uint8_t op, const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> g(mu_);
    uint8_t h[10];
    size_t hl = 2;
    h[0] = uint8_t(0x80 | op);
    if (n < 126) h[1] = uint8_t(n);
    else if (n < 65536) { h[1] = 126; h[2] = uint8_t(n >> 8); h[3] = uint8_t(n); hl = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = uint8_t(uint64_t(n) >> (56 - 8 * i)); hl = 10; }
    if (sendAll(s_, h, hl)) sendAll(s_, p, n);
  }
  // one message; false when the connection ends
  bool read(uint8_t& op, std::vector<uint8_t>& msg) {
    msg.clear();
    op = 0;
    for (;;) {
      uint8_t h[2];
      if (!recvAll(s_, h, 2)) return false;
      bool fin = h[0] & 0x80;
      uint8_t code = h[0] & 0x0F;
      if (!(h[1] & 0x80)) return false;   // a client must mask
      uint64_t len = h[1] & 0x7F;
      if (len == 126) { uint8_t e[2]; if (!recvAll(s_, e, 2)) return false; len = uint64_t(e[0]) << 8 | e[1]; }
      else if (len == 127) { uint8_t e[8]; if (!recvAll(s_, e, 8)) return false; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | e[i]; }
      if (len > kMaxMessage || msg.size() + len > kMaxMessage) return false;
      uint8_t mask[4];
      if (!recvAll(s_, mask, 4)) return false;
      std::vector<uint8_t> part(static_cast<size_t>(len));
      if (len && !recvAll(s_, part.data(), size_t(len))) return false;
      for (size_t i = 0; i < part.size(); i++) part[i] ^= mask[i & 3];
      if (code >= 0x8) {   // control frames may come between fragments
        if (!fin || len > 125) return false;
        if (code == 0x8) { frame(0x8, part.data(), std::min<size_t>(part.size(), 2)); return false; }
        if (code == 0x9) frame(0xA, part.data(), part.size());
        continue;
      }
      if (code != 0 && op != 0) return false;   // a new message inside a fragmented one
      if (code != 0) op = code;
      if (op == 0) return false;
      msg.insert(msg.end(), part.begin(), part.end());
      if (fin) return true;
    }
  }

private:
  intptr_t s_;
  std::mutex mu_;
};
}

// ---------- server ----------
Server::Server(ServerOptions opt, Config& cfg, Pairing& pairing) : opt_(std::move(opt)), cfg_(cfg), pairing_(pairing) {}

bool Server::listen(std::string& err) {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  auto s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == decltype(s)(-1)) { err = "no socket"; return false; }
#ifdef _WIN32
  BOOL excl = TRUE;   // nobody else may share the port
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
#else
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(opt_.port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // this computer only
  if (bind(s, (sockaddr*)&a, sizeof a) != 0) {
    CLOSESOCK(s);
    err = "port " + std::to_string(opt_.port) + " is in use (is the bridge already running?)";
    return false;
  }
  if (opt_.port == 0) {
    socklen_t len = sizeof a;
    getsockname(s, (sockaddr*)&a, &len);
    opt_.port = ntohs(a.sin_port);
  }
  ::listen(s, 16);
  listener_ = intptr_t(s);
  return true;
}

void Server::stop() {
  stopping_ = true;
  if (listener_ != -1) CLOSESOCK((decltype(socket(0, 0, 0)))listener_);
}

void Server::run() {
  while (!stopping_) {
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    auto c = accept((decltype(socket(0, 0, 0)))listener_, (sockaddr*)&peer, &len);
    if (c == decltype(c)(-1)) {
      if (stopping_) break;
      continue;
    }
    if (live_ >= 8) { CLOSESOCK(c); continue; }
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    live_++;
    std::thread([this, c] {
      handle(intptr_t(c));
      CLOSESOCK(c);
      live_--;
    }).detach();
  }
}

void Server::handle(intptr_t s) {
  Request r;
  if (!readRequest(s, r)) return;
  auto origin = r.get("origin");
  bool okHost = hostAllowed(r.get("host"), opt_.port);
  bool okOrigin = originAllowed(opt_.origins, origin);
  std::string cors;
  if (okOrigin)
    cors = "Access-Control-Allow-Origin: " + origin + "\r\nVary: Origin\r\nAccess-Control-Allow-Private-Network: true\r\n";
  if (!okHost) { respond(s, "403 Forbidden", "", "host"); return; }
  if (r.method == "OPTIONS") {
    respond(s, okOrigin ? "204 No Content" : "403 Forbidden",
            cors + "Access-Control-Allow-Methods: GET\r\nAccess-Control-Max-Age: 600\r\n", "");
    return;
  }
  bool upgrade = lowerStr(r.get("upgrade")) == "websocket";
  if (r.method == "GET" && r.path == "/status" && !upgrade) {
    // a page can find the bridge without a pairing; it learns nothing else
    nlohmann::json j = {{"app", "oros-bridge"}, {"version", OROS_BRIDGE_VERSION}};
    respond(s, okOrigin ? "200 OK" : "403 Forbidden", cors + "Content-Type: application/json\r\n", okOrigin ? j.dump() : "");
    return;
  }
  if (r.method != "GET" || r.path != "/" || !upgrade) { respond(s, "404 Not Found", "", ""); return; }
  if (!okOrigin) { respond(s, "403 Forbidden", "", "origin"); return; }
  auto key = r.get("sec-websocket-key");
  std::vector<uint8_t> raw;
  if (r.get("sec-websocket-version") != "13" || !base64Decode(key, raw) || raw.size() != 16) {
    respond(s, "400 Bad Request", "", "");
    return;
  }
  std::string hs = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                   wsAccept(key) + "\r\n\r\n";
  if (!sendAll(s, (const uint8_t*)hs.data(), hs.size())) return;
  WsConnection conn(s);
  Session session(conn, cfg_, pairing_);
  uint8_t op;
  std::vector<uint8_t> msg;
  while (conn.read(op, msg)) {
    if (op == 0x1) session.onText(std::string(msg.begin(), msg.end()));
    else if (op == 0x2) session.onBinary(msg);
  }
}
}
