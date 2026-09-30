// Private, same-build protocol between the desktop preview and its native host.
#pragma once

#include <cstdint>
#include <type_traits>

#include "media/media_preview.h"

namespace easnap::media::preview_host {

inline constexpr uint32_t kProtocolMagic = 0x50534145;  // EASP
inline constexpr uint32_t kProtocolVersion = 4;
inline constexpr uint32_t kMaximumPathCharacters = 32'767;

enum class PacketType : uint32_t {
    Open = 1,
    Intent = 2,
    Resize = 3,
    Close = 4,
    Snapshot = 5,
    SnapshotAck = 6,
};

struct PacketHeader final {
    uint32_t magic{kProtocolMagic};
    uint32_t version{kProtocolVersion};
    PacketType type{};
    uint32_t size{0};
};

// Open is followed by exactly path_length UTF-16 code units, without a NUL.
// The source is sent over inherited private pipes, never the command line.
struct OpenPacket final {
    PacketHeader header{kProtocolMagic, kProtocolVersion, PacketType::Open,
                        sizeof(OpenPacket)};
    uint64_t parent_window{0};
    uint32_t path_length{0};
    int32_t width{1};
    int32_t height{1};
};

// Every intent carries the complete desired state. position_sequence == 0
// means there has been no explicit position request in this host generation.
// A newer playback-only intent preserves the last position_sequence/position.
struct IntentPacket final {
    PacketHeader header{kProtocolMagic, kProtocolVersion, PacketType::Intent,
                        sizeof(IntentPacket)};
    uint64_t intent_sequence{0};
    uint64_t position_sequence{0};
    int64_t position{0};
    uint32_t play_requested{0};
    uint32_t exclusive_end{0};
};

struct ResizePacket final {
    PacketHeader header{kProtocolMagic, kProtocolVersion, PacketType::Resize,
                        sizeof(ResizePacket)};
    uint64_t sequence{0};
    int32_t width{1};
    int32_t height{1};
};

struct ClosePacket final {
    PacketHeader header{kProtocolMagic, kProtocolVersion, PacketType::Close,
                        sizeof(ClosePacket)};
};

struct SnapshotPacket final {
    PacketHeader header{kProtocolMagic, kProtocolVersion, PacketType::Snapshot,
                        sizeof(SnapshotPacket)};
    uint64_t sequence{0};
    uint64_t acknowledged_intent{0};
    uint64_t acknowledged_resize{0};
    uint64_t video_window{0};
    uint64_t sent_ms{0};
    MediaPreviewSnapshot snapshot{};
    uint32_t diagnostics_length{0};
    wchar_t diagnostics[2'048]{};
};

// The host publishes at most one ordinary snapshot until it is consumed.
// This bounds both directions even when a native modal loop delays UI polling.
struct SnapshotAckPacket final {
    PacketHeader header{kProtocolMagic, kProtocolVersion, PacketType::SnapshotAck,
                        sizeof(SnapshotAckPacket)};
    uint64_t sequence{0};
};

inline constexpr uint32_t kMaximumPacketSize =
    sizeof(OpenPacket) + kMaximumPathCharacters * sizeof(wchar_t);

static_assert(sizeof(wchar_t) == 2);
static_assert(std::is_trivially_copyable_v<MediaPreviewSnapshot>);
static_assert(std::is_trivially_copyable_v<OpenPacket>);
static_assert(std::is_trivially_copyable_v<IntentPacket>);
static_assert(std::is_trivially_copyable_v<SnapshotPacket>);
static_assert(std::is_standard_layout_v<SnapshotPacket>);
static_assert(2 * sizeof(SnapshotPacket) < kMaximumPacketSize);

}  // namespace easnap::media::preview_host
