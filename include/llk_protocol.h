#pragma once

#include <cstdint>

namespace llk::proto {

constexpr std::uint32_t kMagic = 0x4C4C4B32;  // LLK2
constexpr std::uint16_t kVersion = 1;

enum class ControlKind : std::uint16_t {
  Pointer = 1,
  Key = 2,
  RequestSync = 3,
  SyncState = 4,
};

constexpr std::uint32_t kRequestSyncFlagWakeSender = 1u << 0;
constexpr std::uint32_t kRequestSyncFlagStopSender = 1u << 1;
constexpr std::uint32_t kRequestSyncFlagUpdateViewerHost = 1u << 2;
constexpr std::uint32_t kRequestSyncFlagUpdateVideoPort = 1u << 3;
constexpr std::uint32_t kRequestSyncFlagUpdateControlPort = 1u << 4;
constexpr std::uint32_t kRequestSyncFlagUpdateTransferPort = 1u << 5;
constexpr std::uint32_t kRequestSyncFlagUpdateFps = 1u << 6;
constexpr std::uint32_t kRequestSyncFlagUpdateBitrate = 1u << 7;
constexpr std::uint32_t kRequestSyncFlagUpdateOutputIndex = 1u << 8;
constexpr std::uint32_t kRequestSyncFlagUseSourceHost = 1u << 9;

constexpr std::uint32_t kSyncStateFlagSenderRequested = 1u << 0;
constexpr std::uint32_t kSyncStateFlagSenderRunning = 1u << 1;
constexpr std::uint32_t kSyncStateFlagLeaseActive = 1u << 2;
constexpr std::uint32_t kSyncStateFlagViewerHostReady = 1u << 3;

constexpr std::uint32_t kRequestSyncDefaultKeepaliveMs = 3000;
constexpr std::uint32_t kRequestSyncMaxKeepaliveMs = 60000;
constexpr std::uint32_t kRequestSyncMaxFps = 240;
constexpr std::uint32_t kRequestSyncMaxBitrateMbps = 200;
constexpr std::uint32_t kViewerHostBytes = 128;

struct ControlHeader final {
  std::uint32_t magic = kMagic;
  std::uint16_t version = kVersion;
  std::uint16_t kind = 0;
};

struct PointerPayload final {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t wheel_delta = 0;
  std::uint32_t button_mask = 0;
};

struct KeyPayload final {
  std::uint32_t virtual_key = 0;
  std::uint32_t down = 0;
};

struct RequestSyncPayload final {
  std::uint32_t flags = 0;
  std::uint32_t keepalive_ms = 0;
  std::uint16_t video_port = 0;
  std::uint16_t control_port = 0;
  std::uint16_t transfer_port = 0;
  std::uint16_t reserved = 0;
  std::uint32_t fps = 0;
  std::uint32_t bitrate_mbps = 0;
  std::uint32_t output_index = 0;
  char viewer_host[kViewerHostBytes]{};
};

struct SyncStatePayload final {
  std::uint32_t status_flags = 0;
  std::uint32_t keepalive_remaining_ms = 0;
  std::uint16_t video_port = 0;
  std::uint16_t control_port = 0;
  std::uint16_t transfer_port = 0;
  std::uint16_t reserved = 0;
  std::uint32_t desktop_width = 0;
  std::uint32_t desktop_height = 0;
  std::uint32_t fps = 0;
  std::uint32_t bitrate_mbps = 0;
  std::uint32_t output_index = 0;
  char viewer_host[kViewerHostBytes]{};
};

}  // namespace llk::proto
