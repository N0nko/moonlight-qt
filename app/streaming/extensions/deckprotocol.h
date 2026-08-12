#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace DeckProtocol {
constexpr std::uint8_t LiveBitrateFeature = 1;
constexpr std::uint8_t LiveBitrateSet = 1;
constexpr std::uint8_t LiveBitrateResult = 2;
constexpr std::uint32_t MinimumBitrateKbps = 500;
constexpr std::uint32_t MaximumBitrateKbps = 500000;
constexpr std::uint8_t RemoteDisplayFeature = 3;
constexpr std::uint8_t RemoteDisplayApply = 1;
constexpr std::uint8_t RemoteDisplayResult = 2;
constexpr std::uint8_t RemoteDisplayPolicy = 3;
constexpr std::uint8_t RemoteDisplayIntentionalDisconnect = 4;

enum class BitrateStatus : std::uint8_t {
    Applied = 2,
    Clamped = 3,
    Failed = 4,
    Unsupported = 5,
};

enum class DisplayProfile : std::uint8_t {
    Desk = 1,
    Remote = 2,
    Tv = 3,
};

enum class DisplayStatus : std::uint8_t {
    Applied = 2,
    Unavailable = 3,
    Failed = 4,
};

struct DisplayResult {
    DisplayStatus status;
    DisplayProfile profile;
};

struct BitrateResult {
    BitrateStatus status;
    std::uint32_t appliedBitrateKbps;
};

constexpr std::uint32_t readLe32(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8 |
           static_cast<std::uint32_t>(data[2]) << 16 |
           static_cast<std::uint32_t>(data[3]) << 24;
}

constexpr void writeLe32(std::uint8_t* data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8);
    data[2] = static_cast<std::uint8_t>(value >> 16);
    data[3] = static_cast<std::uint8_t>(value >> 24);
}

inline std::array<std::uint8_t, 4> makeBitrateRequest(std::uint32_t bitrateKbps)
{
    std::array<std::uint8_t, 4> payload {};
    writeLe32(payload.data(), bitrateKbps);
    return payload;
}

inline bool parseBitrateResult(const std::uint8_t* payload,
                               std::size_t payloadLength,
                               BitrateResult* result)
{
    if (payload == nullptr || result == nullptr || payloadLength != 8 ||
            payload[1] != 0 || payload[2] != 0 || payload[3] != 0) {
        return false;
    }

    const auto status = static_cast<BitrateStatus>(payload[0]);
    const std::uint32_t appliedBitrate = readLe32(payload + 4);
    switch (status) {
    case BitrateStatus::Applied:
    case BitrateStatus::Clamped:
        if (appliedBitrate < MinimumBitrateKbps ||
                appliedBitrate > MaximumBitrateKbps) {
            return false;
        }
        break;
    case BitrateStatus::Failed:
    case BitrateStatus::Unsupported:
        if (appliedBitrate != 0) {
            return false;
        }
        break;
    default:
        return false;
    }

    result->status = status;
    result->appliedBitrateKbps = appliedBitrate;
    return true;
}

inline std::array<std::uint8_t, 4> makeDisplayApply(DisplayProfile profile)
{
    std::array<std::uint8_t, 4> payload {};
    payload[0] = static_cast<std::uint8_t>(profile);
    return payload;
}

inline std::array<std::uint8_t, 4> makeDisplayPolicy(bool applyRemoteOnConnect,
                                                     bool restoreDeskOnDisconnect)
{
    std::array<std::uint8_t, 4> payload {};
    payload[0] = static_cast<std::uint8_t>(
                (applyRemoteOnConnect ? 0x01 : 0) |
                (restoreDeskOnDisconnect ? 0x02 : 0));
    return payload;
}

inline bool parseDisplayResult(const std::uint8_t* payload,
                               std::size_t payloadLength,
                               DisplayResult* result)
{
    if (payload == nullptr || result == nullptr || payloadLength != 4 ||
            payload[2] != 0 || payload[3] != 0) {
        return false;
    }

    const auto status = static_cast<DisplayStatus>(payload[0]);
    const auto profile = static_cast<DisplayProfile>(payload[1]);
    if ((status != DisplayStatus::Applied &&
         status != DisplayStatus::Unavailable &&
         status != DisplayStatus::Failed) ||
            profile < DisplayProfile::Desk || profile > DisplayProfile::Tv) {
        return false;
    }

    result->status = status;
    result->profile = profile;
    return true;
}
} // namespace DeckProtocol
