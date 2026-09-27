#include "power_capacity/ids.hpp"

#include <cstdint>
#include <random>
#include <string>

namespace power_capacity {
namespace {

bool is_identifier_byte(unsigned char byte) noexcept {
  const bool digit =
      byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9');
  const bool upper =
      byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z');
  const bool lower =
      byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z');
  const bool punctuation = byte == static_cast<unsigned char>('.') ||
                            byte == static_cast<unsigned char>('_') ||
                            byte == static_cast<unsigned char>('-') ||
                            byte == static_cast<unsigned char>(':') ||
                            byte == static_cast<unsigned char>('@');
  return digit || upper || lower || punctuation;
}

int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  return -1;
}

char hex_digit(unsigned value) noexcept {
  return static_cast<char>(value < 10 ? ('0' + value) : ('a' + (value - 10)));
}

}  // namespace

Status validate_identifier(std::string_view text, std::size_t max_bytes) {
  if (text.empty()) {
    return Status::error(StatusCode::InvalidArgument, "identifier must not be empty");
  }
  if (text.size() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "identifier is " + std::to_string(text.size()) + " bytes, limit is " +
                             std::to_string(max_bytes));
  }
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (!is_identifier_byte(byte)) {
      return Status::error(StatusCode::InvalidArgument,
                           "identifier contains a disallowed byte; allowed: A-Z a-z 0-9 . _ - : @");
    }
  }
  return Status::success();
}

Status validate_label(std::string_view text, std::size_t max_bytes) {
  if (text.size() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "label is " + std::to_string(text.size()) + " bytes, limit is " +
                             std::to_string(max_bytes));
  }
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte == 0 || byte < 0x20 || byte == 0x7F) {
      return Status::error(StatusCode::InvalidArgument, "label contains a NUL or control byte");
    }
  }
  return Status::success();
}

template <FingerprintFamily Family>
Result<BasicFingerprint<Family>> BasicFingerprint<Family>::parse(std::string_view hex) {
  if (hex.size() != 32) {
    return Status::error(StatusCode::InvalidArgument,
                         "fingerprint must be exactly 32 lowercase hexadecimal characters");
  }
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  for (std::size_t index = 0; index < 16; ++index) {
    const int nibble = hex_value(hex[index]);
    if (nibble < 0) {
      return Status::error(StatusCode::InvalidArgument,
                           "fingerprint contains a non-lowercase-hexadecimal character");
    }
    high = (high << 4) | static_cast<std::uint64_t>(nibble);
  }
  for (std::size_t index = 16; index < 32; ++index) {
    const int nibble = hex_value(hex[index]);
    if (nibble < 0) {
      return Status::error(StatusCode::InvalidArgument,
                           "fingerprint contains a non-lowercase-hexadecimal character");
    }
    low = (low << 4) | static_cast<std::uint64_t>(nibble);
  }
  if (high == 0 && low == 0) {
    return Status::error(StatusCode::InvalidArgument, "fingerprint must not be all zero");
  }
  BasicFingerprint fingerprint;
  fingerprint.high_ = high;
  fingerprint.low_ = low;
  return fingerprint;
}

template <FingerprintFamily Family>
Result<BasicFingerprint<Family>> BasicFingerprint<Family>::from_components(std::uint64_t high,
                                                                          std::uint64_t low) {
  if (high == 0 && low == 0) {
    return Status::error(StatusCode::InvalidArgument, "fingerprint must not be all zero");
  }
  BasicFingerprint fingerprint;
  fingerprint.high_ = high;
  fingerprint.low_ = low;
  return fingerprint;
}

template <FingerprintFamily Family>
std::string BasicFingerprint<Family>::to_hex() const {
  std::string text(32, '0');
  for (std::size_t index = 0; index < 16; ++index) {
    const auto shift = static_cast<unsigned>(60 - (4 * index));
    text[index] = hex_digit(static_cast<unsigned>((high_ >> shift) & 0xFull));
    text[16 + index] = hex_digit(static_cast<unsigned>((low_ >> shift) & 0xFull));
  }
  return text;
}

template <FingerprintFamily Family>
BasicFingerprint<Family> BasicFingerprint<Family>::generate() {
  std::random_device device;
  std::mt19937_64 generator(static_cast<std::uint64_t>(device()) ^
                            (static_cast<std::uint64_t>(device()) << 17));
  std::uniform_int_distribution<std::uint64_t> distribution;
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::uint64_t high = distribution(generator);
    const std::uint64_t low = distribution(generator);
    if (high != 0 || low != 0) {
      BasicFingerprint fingerprint;
      fingerprint.high_ = high;
      fingerprint.low_ = low;
      return fingerprint;
    }
  }
  // A 128-bit generator that produced eight consecutive zero pairs is not a
  // credible entropy source; fall back to a value that is at least non-zero.
  BasicFingerprint fingerprint;
  fingerprint.high_ = 1;
  fingerprint.low_ = static_cast<std::uint64_t>(device());
  return fingerprint;
}

template class BasicFingerprint<FingerprintFamily::StoreIdentity>;
template class BasicFingerprint<FingerprintFamily::SessionId>;

}  // namespace power_capacity
