#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ridgeline {

// Standard CRC-32 (IEEE 802.3 polynomial, 0xEDB88320), self-contained so
// `ridgeline_core` — which must build without net/gRPC/zlib for the TSan
// job (see cmake/Sanitizers.cmake and CMakeLists.txt's RIDGELINE_BUILD_NET)
// — doesn't need to pull in zlib just for one checksum function.
inline std::uint32_t Crc32(const void* data, std::size_t len) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();

  auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace ridgeline
