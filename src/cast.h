#pragma once
// TVs and speakers on the home network for orOS "Play on" (0.10.0, DESIGN
// decision 18). A page cannot search the network, so the bridge does: SSDP
// (UDP multicast 239.255.255.250:1900) finds UPnP/DLNA media renderers
// (smart TVs, Kodi, network speakers and receivers), their description says
// where their AVTransport and RenderingControl services answer, and SOAP over
// plain HTTP sends a stream's address, play, pause, stop and the volume. The
// TV fetches the stream itself. The bridge serves nothing on the network:
// it only sends requests to devices that answered the search, at private
// addresses (RFC 1918, link-local, loopback), and only http(s) stream
// addresses. No Google Cast: orOS Bible rule 29, Chris 2026-11-02 option A.
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace ob {
namespace cast {

struct Renderer {
  std::string id;        // "upnp:<udn>" (or a hash of the description's address)
  std::string name, vendor, model;
  std::string host;      // IPv4, from the description's address
  int port = 0;
  std::string avtUrl, avtType;   // AVTransport control path and service type
  std::string rcUrl, rcType;     // RenderingControl (may be empty: no volume)
};

nlohmann::json toJson(const Renderer& r);

// The renderers found. full (or an empty list) searches again (about 2.5 s);
// otherwise the kept list comes back.
std::vector<Renderer> renderers(bool full);

struct Error {
  std::string code;   // notfound, unreachable, refused, badurl, other
  std::string text;
};

// kind: "audio" | "video"; mime may be empty (a default for the kind).
bool start(const std::string& id, const std::string& url, const std::string& title,
           const std::string& kind, const std::string& mime, Error& err);
// action: play | pause | stop | volume (0..100)
bool control(const std::string& id, const std::string& action, int volume, Error& err);
// state: playing | paused | stopped | loading | idle | other; volume -1 when unknown
bool status(const std::string& id, std::string& state, int& volume, Error& err);

// Opens the system's Bluetooth settings (pairing a speaker). False when none was found.
bool openBluetoothSettings();

// --test-renderer HOST:PORT: also send the search to this address (tests).
void addTestTarget(const std::string& hostPort);

// Pure helpers, exposed for tests.
std::string xmlEscape(const std::string& s);
std::string xmlText(const std::string& xml, const std::string& tag, size_t from = 0, size_t to = std::string::npos);
bool privateIPv4(const std::string& host);
std::string didl(const std::string& url, const std::string& title, const std::string& kind, const std::string& mime);

}  // namespace cast
}  // namespace ob
