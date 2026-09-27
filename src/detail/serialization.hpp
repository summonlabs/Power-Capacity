#pragma once

// Internal artifact serialization. Not installed.
//
// Artifact layout:
//
//   header  72 bytes: magic, format version, header size, endian marker, flags,
//                     payload length, payload CRC-32C, header CRC-32C, and a
//                     redundant copy of the generation, epoch, and incarnation
//   payload  n bytes: six length-prefixed sections in fixed order
//   footer  16 bytes: end magic, repeated payload CRC-32C, footer CRC-32C

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "power_capacity/limits.hpp"
#include "power_capacity/state.hpp"
#include "power_capacity/status.hpp"

namespace power_capacity::detail {

inline constexpr std::size_t kArtifactHeaderBytes = 72;
inline constexpr std::size_t kArtifactFooterBytes = 16;
inline constexpr std::uint32_t kArtifactFormatVersion = 1;

struct DecodedArtifact {
  CapacityContent content;
  std::uint32_t format_version = 0;
  std::uint32_t payload_crc32c = 0;
  std::uint64_t file_bytes = 0;
};

struct EncodedArtifact {
  std::vector<std::byte> bytes;
  std::uint32_t payload_crc32c = 0;
};

/// Encodes a full artifact. Fails rather than silently truncating if any
/// declared field exceeds a bound.
Result<EncodedArtifact> encode_artifact(const CapacityContent& content,
                                        const ResourceLimits& limits);

/// Decodes and validates a full artifact. The artifact must be exactly the
/// declared length; trailing bytes are rejected.
Result<DecodedArtifact> decode_artifact(std::span<const std::byte> artifact,
                                        const ResourceLimits& limits);

}  // namespace power_capacity::detail
