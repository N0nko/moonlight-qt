#include "recoverysettings.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>

namespace {
uint32_t readBoundedValue(const char* name,
                          uint32_t fallback,
                          uint32_t minimum,
                          uint32_t maximum)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0' || *value == '-') {
        return fallback;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
            parsed > std::numeric_limits<uint32_t>::max()) {
        return fallback;
    }

    return std::clamp(static_cast<uint32_t>(parsed), minimum, maximum);
}
}

RecoverySettings RecoverySettings::fromEnvironment()
{
    RecoverySettings settings;
    settings.recoveryWindowMs = readBoundedValue(
                "MOONLIGHT_RECONNECT_WINDOW_MS",
                settings.recoveryWindowMs, 1000, 60000);
    settings.videoStallTimeoutMs = readBoundedValue(
                "MOONLIGHT_RECONNECT_VIDEO_STALL_MS",
                settings.videoStallTimeoutMs, 750, 60000);
    settings.controlInactivityTimeoutMs = readBoundedValue(
                "MOONLIGHT_RECONNECT_CONTROL_TIMEOUT_MS",
                settings.controlInactivityTimeoutMs, 1500, 30000);
    settings.controlConnectTimeoutMs = readBoundedValue(
                "MOONLIGHT_RECONNECT_CONNECT_TIMEOUT_MS",
                settings.controlConnectTimeoutMs, 750, 10000);
    return settings;
}

RecoveryPolicy::Config RecoverySettings::policyConfig() const
{
    return {
        recoveryWindowMs,
        connectionAttemptTimeoutMs,
        firstFrameTimeoutMs,
        retryDelayMs,
    };
}
