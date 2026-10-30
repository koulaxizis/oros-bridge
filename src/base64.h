#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace ob {
std::string base64Encode(const uint8_t* data, size_t len);
inline std::string base64Encode(const std::vector<uint8_t>& v) { return base64Encode(v.data(), v.size()); }
// false when the text is not base64
bool base64Decode(const std::string& text, std::vector<uint8_t>& out);
}
