#pragma once

#include "recoverypolicy.h"

#include <cstdint>

struct RecoverySettings
{
    uint32_t recoveryWindowMs = 20000;
    uint32_t connectionAttemptTimeoutMs = 8000;
    uint32_t firstFrameTimeoutMs = 3500;
    uint32_t retryDelayMs = 100;
    uint32_t videoStallTimeoutMs = 10000;
    uint32_t controlInactivityTimeoutMs = 3000;
    uint32_t controlConnectTimeoutMs = 1500;

    static RecoverySettings fromEnvironment();

    RecoveryPolicy::Config policyConfig() const;
};
