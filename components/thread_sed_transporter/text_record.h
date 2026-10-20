#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace esphome::thread_sed_transporter {

inline constexpr uint32_t TEXT_VERSION = 0x52545431;
inline constexpr uint32_t TEXT_HEADER_SIZE = 8;
inline constexpr uint32_t TEXT_MAX_LENGTH = 128;
using TextRecord = std::array<uint8_t, TEXT_HEADER_SIZE + TEXT_MAX_LENGTH>;

constexpr void put_u32(TextRecord &record, unsigned offset, uint32_t value) {
  for (unsigned i = 0; i < 4; i++)
    record[offset + i] = (value >> (i * 8)) & 0xFF;
}

constexpr uint32_t get_u32(const TextRecord &record, unsigned offset) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; i++)
    value |= static_cast<uint32_t>(record[offset + i]) << (i * 8);
  return value;
}

constexpr bool encode_text(TextRecord &record, std::string_view value, uint32_t max_length) {
  if (max_length > TEXT_MAX_LENGTH || value.size() > max_length)
    return false;
  record = {};
  put_u32(record, 0, TEXT_VERSION);
  put_u32(record, 4, value.size());
  for (unsigned i = 0; i < value.size(); i++)
    record[TEXT_HEADER_SIZE + i] = static_cast<uint8_t>(value[i]);
  return true;
}

constexpr bool valid_text_record(const TextRecord &record, uint32_t max_length) {
  return max_length <= TEXT_MAX_LENGTH && get_u32(record, 0) == TEXT_VERSION &&
         get_u32(record, 4) <= max_length;
}

}  // namespace esphome::thread_sed_transporter
