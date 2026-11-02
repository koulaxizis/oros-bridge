// TVs and speakers on the home network (0.10.0): SSDP search, UPnP device
// description, AVTransport and RenderingControl over SOAP. See cast.h.
#include "cast.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
using sock_t = SOCKET;
using socklen_t = int;
#define CLOSESOCK closesocket
#define BAD_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
using sock_t = int;
#define CLOSESOCK close
#define BAD_SOCK (-1)
extern char** environ;
#endif

namespace ob {
namespace cast {

namespace {

constexpr size_t kMaxBody = 512 * 1024;    // a description or a SOAP answer
constexpr int kIoMs = 3000;                 // per HTTP request
constexpr int kSearchMs = 2500;             // the SSDP search
constexpr size_t kMaxRenderers = 64;

std::mutex mu;
std::vector<Renderer> kept;
bool searched = false;
std::vector<std::string> testTargets;

void netInit() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); });
#endif
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string unescapeXml(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '&') { o += s[i]; continue; }
    size_t e = s.find(';', i);
    if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
    std::string n = s.substr(i + 1, e - i - 1);
    if (n == "amp") o += '&';
    else if (n == "lt") o += '<';
    else if (n == "gt") o += '>';
    else if (n == "quot") o += '"';
    else if (n == "apos") o += '\'';
    else if (!n.empty() && n[0] == '#') {
      unsigned long v = n.size() > 1 && (n[1] == 'x' || n[1] == 'X') ? std::strtoul(n.c_str() + 2, nullptr, 16)
                                                                      : std::strtoul(n.c_str() + 1, nullptr, 10);
      if (v < 0x80) { if (v >= 0x20 || v == '\t' || v == '\n') o += char(v); }
      else if (v < 0x800) { o += char(0xC0 | (v >> 6)); o += char(0x80 | (v & 0x3F)); }
      else if (v < 0x10000) { o += char(0xE0 | (v >> 12)); o += char(0x80 | ((v >> 6) & 0x3F)); o += char(0x80 | (v & 0x3F)); }
      else if (v < 0x110000) { o += char(0xF0 | (v >> 18)); o += char(0x80 | ((v >> 12) & 0x3F)); o += char(0x80 | ((v >> 6) & 0x3F)); o += char(0x80 | (v & 0x3F)); }
    } else { o += s.substr(i, e - i + 1); }
    i = e;
  }
  return o;
}

// One line of text a person may see: no control characters, at most n bytes.
std::string clean(const std::string& s, size_t n) {
  std::string o;
  for (unsigned char c : s) if (c >= 0x20 && c != 0x7f) o += char(c);
  o = trim(o);
  if (o.size() > n) {
    o.resize(n);
    while (!o.empty() && (static_cast<unsigned char>(o.back()) & 0xC0) == 0x80) o.pop_back();   // whole UTF-8 characters
    if (!o.empty() && static_cast<unsigned char>(o.back()) >= 0xC0) o.pop_back();
  }
  return o;
}

struct Url {
  std::string host;
  int port = 80;
  std::string path = "/";
};

// http://a.b.c.d[:port][/path] only; the host must be an IPv4 address.
bool parseUrl(const std::string& u, Url& out) {
  if (u.size() > 2048 || lower(u.substr(0, 7)) != "http://") return false;
  std::string rest = u.substr(7);
  size_t slash = rest.find('/');
  std::string hp = rest.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : rest.substr(slash);
  size_t colon = hp.find(':');
  out.host = hp.substr(0, colon);
  out.port = 80;
  if (colon != std::string::npos) {
    std::string p = hp.substr(colon + 1);
    if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
    out.port = std::atoi(p.c_str());
    if (out.port < 1 || out.port > 65535) return false;
  }
  in_addr a;
  if (inet_pton(AF_INET, out.host.c_str(), &a) != 1) return false;
  for (char c : out.path) if ((unsigned char)c <= 0x20 || c == 0x7f) return false;
  return true;
}

// A control URL from the description: absolute (same host only) or a path.
bool resolve(const std::string& base, const Url& loc, const std::string& ref, std::string& path) {
  std::string r = trim(ref);
  if (r.empty() || r.size() > 1024) return false;
  if (lower(r.substr(0, 7)) == "http://") {
    Url u;
    if (!parseUrl(r, u) || u.host != loc.host || u.port != loc.port) return false;
    path = u.path;
    return true;
  }
  if (r[0] != '/') {
    std::string dir = loc.path.substr(0, loc.path.rfind('/') + 1);
    Url b;
    if (!base.empty() && parseUrl(base, b) && b.host == loc.host && b.port == loc.port) dir = b.path.substr(0, b.path.rfind('/') + 1);
    r = dir + r;
  }
  for (char c : r) if ((unsigned char)c <= 0x20 || c == 0x7f) return false;
  path = r;
  return true;
}

bool waitSock(sock_t s, bool write, int ms) {
  fd_set f;
  FD_ZERO(&f);
  FD_SET(s, &f);
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  int n = select(int(s) + 1, write ? nullptr : &f, write ? &f : nullptr, nullptr, &tv);
  return n > 0;
}

void setBlocking(sock_t s, bool on) {
#ifdef _WIN32
  u_long nb = on ? 0 : 1;
  ioctlsocket(s, FIONBIO, &nb);
#else
  int fl = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Answer {
  int status = 0;
  std::string headers;   // lower case
  std::string body;
};

std::string dechunk(const std::string& b) {
  std::string o;
  size_t i = 0;
  while (i < b.size()) {
    size_t e = b.find("\r\n", i);
    if (e == std::string::npos) break;
    unsigned long n = std::strtoul(b.substr(i, e - i).c_str(), nullptr, 16);
    if (n == 0) break;
    i = e + 2;
    if (i + n > b.size()) { o += b.substr(i); break; }
    o += b.substr(i, n);
    i += n + 2;
  }
  return o;
}

// One HTTP/1.1 request, Connection: close, to a private IPv4 address.
bool http(const Url& u, const std::string& method, const std::string& extraHeaders, const std::string& body,
          Answer& out, Error& err) {
  netInit();
  if (!privateIPv4(u.host)) { err = {"refused", "not a home network address"}; return false; }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(u.port));
  inet_pton(AF_INET, u.host.c_str(), &a.sin_addr);
  sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == BAD_SOCK) { err = {"other", "no socket"}; return false; }
  setBlocking(s, false);
  long long end = nowMs() + kIoMs;
  int c = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
  if (c != 0) {
    if (!waitSock(s, true, kIoMs)) { CLOSESOCK(s); err = {"unreachable", "the device does not answer"}; return false; }
    int e = 0;
    socklen_t el = sizeof e;
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &el);
    if (e != 0) { CLOSESOCK(s); err = {"unreachable", "the device does not answer"}; return false; }
  }
  std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                    "\r\nUser-Agent: orOS-Bridge/" OROS_BRIDGE_VERSION " UPnP/1.0\r\nConnection: close\r\n" + extraHeaders;
  if (method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n" + body;
  size_t sent = 0;
  while (sent < req.size()) {
    long long left = end - nowMs();
    if (left <= 0 || !waitSock(s, true, int(left))) { CLOSESOCK(s); err = {"unreachable", "the device does not answer"}; return false; }
    int n = send(s, req.data() + sent, int(req.size() - sent), 0);
    if (n <= 0) { CLOSESOCK(s); err = {"unreachable", "the device closed the connection"}; return false; }
    sent += size_t(n);
  }
  std::string data;
  char buf[8192];
  for (;;) {
    long long left = end - nowMs();
    if (left <= 0 || !waitSock(s, false, int(left))) break;
    int n = recv(s, buf, sizeof buf, 0);
    if (n <= 0) break;
    data.append(buf, size_t(n));
    if (data.size() > kMaxBody + 16384) { CLOSESOCK(s); err = {"other", "the answer is too long"}; return false; }
    size_t he = data.find("\r\n\r\n");
    if (he != std::string::npos) {   // done once Content-Length bytes are in
      std::string h = lower(data.substr(0, he));
      size_t cl = h.find("\r\ncontent-length:");
      if (cl != std::string::npos && data.size() - he - 4 >= std::strtoull(h.c_str() + cl + 17, nullptr, 10)) break;
    }
  }
  CLOSESOCK(s);
  size_t he = data.find("\r\n\r\n");
  if (data.compare(0, 5, "HTTP/") != 0 || he == std::string::npos) { err = {"unreachable", "no answer from the device"}; return false; }
  out.status = std::atoi(data.c_str() + data.find(' ') + 1);
  out.headers = lower(data.substr(0, he + 2));
  out.body = data.substr(he + 4);
  if (out.headers.find("transfer-encoding: chunked") != std::string::npos) out.body = dechunk(out.body);
  size_t cl = out.headers.find("\r\ncontent-length:");
  if (cl != std::string::npos) {
    size_t n = std::strtoull(out.headers.c_str() + cl + 17, nullptr, 10);
    if (out.body.size() > n) out.body.resize(n);
  }
  if (out.body.size() > kMaxBody) { err = {"other", "the answer is too long"}; return false; }
  return true;
}

// The value of one header in an SSDP answer, any case.
std::string header(const std::string& msg, const std::string& name) {
  std::string l = lower(msg);
  size_t p = l.find("\r\n" + name + ":");
  if (p == std::string::npos) return "";
  p += name.size() + 3;
  size_t e = msg.find("\r\n", p);
  return trim(msg.substr(p, e == std::string::npos ? std::string::npos : e - p));
}

std::vector<std::string> ssdpSearch() {
  netInit();
  std::vector<std::string> locations;
  sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == BAD_SOCK) return locations;
  sockaddr_in any{};
  any.sin_family = AF_INET;
  any.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(s, reinterpret_cast<sockaddr*>(&any), sizeof any);
  unsigned char ttl = 2;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
  const std::string msg =
      "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
      "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\nUSER-AGENT: orOS-Bridge/" OROS_BRIDGE_VERSION " UPnP/1.0\r\n\r\n";
  std::vector<sockaddr_in> to;
  sockaddr_in mc{};
  mc.sin_family = AF_INET;
  mc.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &mc.sin_addr);
  to.push_back(mc);
  {
    std::lock_guard<std::mutex> g(mu);
    for (auto& t : testTargets) {
      size_t c = t.rfind(':');
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(uint16_t(std::atoi(t.c_str() + c + 1)));
      if (c != std::string::npos && inet_pton(AF_INET, t.substr(0, c).c_str(), &a.sin_addr) == 1) to.push_back(a);
    }
  }
  long long end = nowMs() + kSearchMs;
  for (int round = 0; round < 2; round++)   // UDP may drop one
    for (auto& a : to) sendto(s, msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
  char buf[2048];
  for (;;) {
    long long left = end - nowMs();
    if (left <= 0 || !waitSock(s, false, int(left))) break;
    int n = recv(s, buf, sizeof buf - 1, 0);
    if (n <= 0) continue;
    std::string m(buf, size_t(n));
    if (lower(m.substr(0, 15)) != "http/1.1 200 ok") continue;
    std::string loc = header(m, "location");
    if (!loc.empty() && loc.size() <= 1024 && std::find(locations.begin(), locations.end(), loc) == locations.end() &&
        locations.size() < kMaxRenderers)
      locations.push_back(loc);
  }
  CLOSESOCK(s);
  return locations;
}

std::string safeId(const std::string& s) {
  std::string o;
  for (char c : s)
    if (std::isalnum((unsigned char)c) || c == ':' || c == '.' || c == '_' || c == '-') o += c;
  return o.substr(0, 200);
}

// Reads one description; true when it is a renderer with AVTransport.
bool describe(const std::string& location, Renderer& r) {
  Url loc;
  if (!parseUrl(location, loc) || !privateIPv4(loc.host)) return false;
  Answer a;
  Error e;
  if (!http(loc, "GET", "", "", a, e) || a.status != 200) return false;
  const std::string& x = a.body;
  r.host = loc.host;
  r.port = loc.port;
  r.name = clean(unescapeXml(xmlText(x, "friendlyName")), 120);
  r.vendor = clean(unescapeXml(xmlText(x, "manufacturer")), 120);
  r.model = clean(unescapeXml(xmlText(x, "modelName")), 120);
  std::string udn = safeId(unescapeXml(xmlText(x, "UDN")));
  if (udn.size() > 5 && lower(udn.substr(0, 5)) == "uuid:") udn = udn.substr(5);
  r.id = "upnp:" + (udn.empty() ? safeId(loc.host + "_" + std::to_string(loc.port)) : udn);
  std::string base = trim(unescapeXml(xmlText(x, "URLBase")));
  size_t p = 0;
  for (int guard = 0; guard < 64; guard++) {
    size_t s = x.find("<service>", p);
    if (s == std::string::npos) { s = x.find("<service ", p); if (s == std::string::npos) break; }
    size_t e = x.find("</service>", s);
    if (e == std::string::npos) break;
    std::string type = trim(unescapeXml(xmlText(x, "serviceType", s, e)));
    std::string ctl = unescapeXml(xmlText(x, "controlURL", s, e));
    std::string path;
    if (type.find(":service:AVTransport:") != std::string::npos && r.avtUrl.empty() && resolve(base, loc, ctl, path)) {
      r.avtUrl = path;
      r.avtType = type;
    } else if (type.find(":service:RenderingControl:") != std::string::npos && r.rcUrl.empty() && resolve(base, loc, ctl, path)) {
      r.rcUrl = path;
      r.rcType = type;
    }
    p = e + 10;
  }
  if (r.name.empty()) r.name = r.model.empty() ? r.host : r.model;
  return !r.avtUrl.empty() && r.avtType.size() <= 200 && r.rcType.size() <= 200;
}

bool find(const std::string& id, Renderer& out) {
  std::lock_guard<std::mutex> g(mu);
  for (auto& r : kept)
    if (r.id == id) { out = r; return true; }
  return false;
}

// One SOAP action; the answer's body on success.
bool soap(const Renderer& r, bool rc, const std::string& action, const std::string& args, std::string& body, Error& err) {
  const std::string& type = rc ? r.rcType : r.avtType;
  Url u;
  u.host = r.host;
  u.port = r.port;
  u.path = rc ? r.rcUrl : r.avtUrl;
  if (u.path.empty()) { err = {"other", "the device has no volume control"}; return false; }
  std::string env =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body><u:" + action + " xmlns:u=\"" + xmlEscape(type) + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
  Answer a;
  std::string h = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"" + type + "#" + action + "\"\r\n";
  if (!http(u, "POST", h, env, a, err)) return false;
  if (a.status != 200) {
    std::string d = clean(unescapeXml(xmlText(a.body, "errorDescription")), 200);
    std::string code = clean(xmlText(a.body, "errorCode"), 16);
    err = {"other", "the device refused " + action + (code.empty() ? "" : " (" + code + (d.empty() ? "" : ": " + d) + ")")};
    return false;
  }
  body = a.body;
  return true;
}

}  // namespace

std::string xmlEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&apos;"; break;
      default:
        if ((unsigned char)c >= 0x20 || c == '\t' || c == '\n') o += c;
    }
  }
  return o;
}

// The text of the first <tag> (any namespace prefix) between from and to.
std::string xmlText(const std::string& xml, const std::string& tag, size_t from, size_t to) {
  if (to > xml.size()) to = xml.size();
  size_t p = from;
  while (p < to) {
    size_t lt = xml.find('<', p);
    if (lt == std::string::npos || lt >= to) return "";
    size_t gt = xml.find('>', lt);
    if (gt == std::string::npos || gt >= to) return "";
    std::string name = xml.substr(lt + 1, gt - lt - 1);
    size_t sp = name.find_first_of(" \t\r\n/");
    std::string bare = name.substr(0, sp);
    size_t colon = bare.find(':');
    std::string local = colon == std::string::npos ? bare : bare.substr(colon + 1);
    if (local == tag && !name.empty() && name.back() != '/') {
      size_t close = xml.find("</" + bare + ">", gt);
      if (close == std::string::npos || close > to) return "";
      return xml.substr(gt + 1, close - gt - 1);
    }
    p = gt + 1;
  }
  return "";
}

bool privateIPv4(const std::string& host) {
  in_addr a;
  if (inet_pton(AF_INET, host.c_str(), &a) != 1) return false;
  uint32_t v = ntohl(a.s_addr);
  return (v >> 24) == 10 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 16) == 0xA9FE || (v >> 24) == 127;
}

std::string didl(const std::string& url, const std::string& title, const std::string& kind, const std::string& mime) {
  std::string cls = kind == "video" ? "object.item.videoItem.videoBroadcast" : "object.item.audioItem.audioBroadcast";
  return "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
         "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\"><item id=\"0\" parentID=\"-1\" restricted=\"1\"><dc:title>" +
         xmlEscape(title) + "</dc:title><upnp:class>" + cls + "</upnp:class><res protocolInfo=\"http-get:*:" + xmlEscape(mime) +
         ":*\">" + xmlEscape(url) + "</res></item></DIDL-Lite>";
}

nlohmann::json toJson(const Renderer& r) {
  return {{"id", r.id}, {"name", r.name}, {"vendor", r.vendor}, {"model", r.model}, {"volume", !r.rcUrl.empty()}};
}

std::vector<Renderer> renderers(bool full) {
  {
    std::lock_guard<std::mutex> g(mu);
    if (searched && !full) return kept;
  }
  std::vector<Renderer> found;
  for (auto& loc : ssdpSearch()) {
    Renderer r;
    if (!describe(loc, r)) continue;
    bool dup = false;
    for (auto& f : found) dup = dup || f.id == r.id;
    if (!dup) found.push_back(r);
  }
  std::sort(found.begin(), found.end(), [](const Renderer& a, const Renderer& b) { return lower(a.name) < lower(b.name); });
  std::lock_guard<std::mutex> g(mu);
  kept = found;
  searched = true;
  return kept;
}

bool start(const std::string& id, const std::string& url, const std::string& title, const std::string& kind,
           const std::string& mime, Error& err) {
  Renderer r;
  if (!find(id, r)) { err = {"notfound", "that device is not in the list"}; return false; }
  std::string lu = lower(url.substr(0, 8));
  if (url.size() > 4096 || !(lu.rfind("http://", 0) == 0 || lu.rfind("https://", 0) == 0) ||
      url.find_first_of(" \t\r\n") != std::string::npos) {
    err = {"badurl", "only an http or https address can be sent"};
    return false;
  }
  std::string m = mime.empty() ? (kind == "video" ? "application/vnd.apple.mpegurl" : "audio/mpeg") : mime;
  std::string args = "<InstanceID>0</InstanceID><CurrentURI>" + xmlEscape(url) + "</CurrentURI><CurrentURIMetaData>" +
                     xmlEscape(didl(url, clean(title, 200), kind, m)) + "</CurrentURIMetaData>";
  std::string body;
  if (!soap(r, false, "SetAVTransportURI", args, body, err)) {
    // some renderers take a new address only when stopped
    Error ignore;
    soap(r, false, "Stop", "<InstanceID>0</InstanceID>", body, ignore);
    if (!soap(r, false, "SetAVTransportURI", args, body, err)) return false;
  }
  return soap(r, false, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", body, err);
}

bool control(const std::string& id, const std::string& action, int volume, Error& err) {
  Renderer r;
  if (!find(id, r)) { err = {"notfound", "that device is not in the list"}; return false; }
  std::string body;
  if (action == "play") return soap(r, false, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", body, err);
  if (action == "pause") return soap(r, false, "Pause", "<InstanceID>0</InstanceID>", body, err);
  if (action == "stop") return soap(r, false, "Stop", "<InstanceID>0</InstanceID>", body, err);
  if (action == "volume") {
    int v = std::max(0, std::min(100, volume));
    return soap(r, true, "SetVolume",
                "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>" + std::to_string(v) + "</DesiredVolume>",
                body, err);
  }
  err = {"other", "bad action"};
  return false;
}

bool status(const std::string& id, std::string& state, int& volume, Error& err) {
  Renderer r;
  if (!find(id, r)) { err = {"notfound", "that device is not in the list"}; return false; }
  std::string body;
  if (!soap(r, false, "GetTransportInfo", "<InstanceID>0</InstanceID>", body, err)) return false;
  std::string s = trim(xmlText(body, "CurrentTransportState"));
  state = s == "PLAYING" ? "playing" : s == "PAUSED_PLAYBACK" || s == "PAUSED_RECORDING" ? "paused"
        : s == "STOPPED" ? "stopped" : s == "TRANSITIONING" ? "loading" : s == "NO_MEDIA_PRESENT" ? "idle" : "other";
  volume = -1;
  Error ignore;
  if (!r.rcUrl.empty() && soap(r, true, "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>", body, ignore)) {
    std::string v = trim(xmlText(body, "CurrentVolume"));
    if (!v.empty() && v.size() <= 3 && v.find_first_not_of("0123456789") == std::string::npos) volume = std::atoi(v.c_str());
  }
  return true;
}

bool openBluetoothSettings() {
#ifdef _WIN32
  auto h = ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(h) > 32;
#else
  const char* const tries[][3] = {{"gnome-control-center", "bluetooth", nullptr},
                                  {"systemsettings", "kcm_bluetooth", nullptr},
                                  {"blueman-manager", nullptr, nullptr},
                                  {"blueberry", nullptr, nullptr}};
  for (auto& t : tries) {
    pid_t pid;
    if (posix_spawnp(&pid, t[0], nullptr, nullptr, const_cast<char* const*>(t), environ) == 0) {
      // reap it in the background so it never stays a zombie
      std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
      return true;
    }
  }
  return false;
#endif
}

void addTestTarget(const std::string& hostPort) {
  std::lock_guard<std::mutex> g(mu);
  testTargets.push_back(hostPort);
}

}  // namespace cast
}  // namespace ob
