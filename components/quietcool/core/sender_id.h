#pragma once

#include "core_types.h"

namespace quietcool {

enum class SenderIdError : std::uint8_t { InvalidPrefix };

class SenderId final {
 public:
  static Result<SenderId, SenderIdError> from_bytes(
      std::array<std::uint8_t, 4> bytes);
  static Result<SenderId, SenderIdError> from_be_u32(std::uint32_t value);
  std::array<std::uint8_t, 4> bytes() const { return bytes_; }
  std::uint32_t as_be_u32() const;
  bool operator==(const SenderId& other) const { return bytes_ == other.bytes_; }
  bool operator!=(const SenderId& other) const { return !(*this == other); }
 private:
  explicit SenderId(std::array<std::uint8_t, 4> bytes) : bytes_(bytes) {}
  std::array<std::uint8_t, 4> bytes_;
};

}  // namespace quietcool

