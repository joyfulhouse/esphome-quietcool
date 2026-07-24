#include "sender_id.h"

namespace quietcool {

Result<SenderId, SenderIdError> SenderId::from_bytes(
    std::array<std::uint8_t, 4> bytes) {
  if (bytes[0] != 0xCB) return Result<SenderId, SenderIdError>::err(SenderIdError::InvalidPrefix);
  return Result<SenderId, SenderIdError>::ok(SenderId(bytes));
}

Result<SenderId, SenderIdError> SenderId::from_be_u32(std::uint32_t value) {
  return from_bytes({static_cast<std::uint8_t>(value >> 24U),
                     static_cast<std::uint8_t>(value >> 16U),
                     static_cast<std::uint8_t>(value >> 8U),
                     static_cast<std::uint8_t>(value)});
}

std::uint32_t SenderId::as_be_u32() const {
  return (static_cast<std::uint32_t>(bytes_[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes_[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes_[2]) << 8U) |
         static_cast<std::uint32_t>(bytes_[3]);
}

}  // namespace quietcool

