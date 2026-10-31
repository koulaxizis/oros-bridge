#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace ob {
// SHA-256 (FIPS 180-4), for checking downloaded updates against SHA256SUMS.txt.
class Sha256 {
public:
  Sha256();
  void update(const void* data, size_t len);
  std::array<uint8_t, 32> finish();

private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t used_ = 0;
  uint64_t total_ = 0;
};
// The lowercase hex SHA-256 of a file, "" when it cannot be read.
std::string sha256File(const std::string& path);
std::string sha256Hex(const std::string& data);
}
