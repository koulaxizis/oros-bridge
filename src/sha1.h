#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace ob {
// SHA-1, only for the WebSocket handshake (RFC 6455), never for security.
std::array<uint8_t, 20> sha1(const std::string& data);
}
