#pragma once

#include <cstdint>

namespace llk::transfer {

constexpr std::uint32_t kMagic = 0x544B4C4C;  // "LLKT"
constexpr std::uint16_t kVersion = 1;
constexpr std::uint64_t kMaxClipboardBytes = 1024 * 1024;
constexpr std::uint32_t kMaxNameBytes = 4096;
constexpr std::uint32_t kStatusOk = 0;
constexpr std::uint32_t kStatusError = 1;

enum class Kind : std::uint16_t {
  ClipboardText = 1,
  ClipboardFile = 2,
  ClipboardDirectory = 3,
  ClipboardCommit = 4,
  ClipboardReset = 5,
};

#pragma pack(push, 1)
struct Header final {
  std::uint32_t magic = kMagic;
  std::uint16_t version = kVersion;
  std::uint16_t kind = 0;
  std::uint32_t name_bytes = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t transfer_id = 0;
  std::uint64_t offset_bytes = 0;
};

struct Response final {
  std::uint32_t status = kStatusError;
};
#pragma pack(pop)

}  // namespace llk::transfer
