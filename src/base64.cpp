#include "base64.h"

namespace ob {
static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = uint32_t(data[i]) << 16;
    if (i + 1 < len) n |= uint32_t(data[i + 1]) << 8;
    if (i + 2 < len) n |= data[i + 2];
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += i + 1 < len ? kAlphabet[(n >> 6) & 63] : '=';
    out += i + 2 < len ? kAlphabet[n & 63] : '=';
  }
  return out;
}

bool base64Decode(const std::string& text, std::vector<uint8_t>& out) {
  out.clear();
  if (text.size() % 4) return false;
  uint32_t n = 0;
  int bits = 0;
  size_t pad = 0;
  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=' && i >= text.size() - 2) { pad++; v = 0; }
    else return false;
    if (pad && c != '=') return false;
    n = (n << 6) | uint32_t(v);
    bits += 6;
    if (bits == 24) {
      out.push_back(uint8_t(n >> 16));
      if (pad < 2) out.push_back(uint8_t(n >> 8));
      if (pad < 1) out.push_back(uint8_t(n));
      n = 0;
      bits = 0;
    }
  }
  return true;
}
}
