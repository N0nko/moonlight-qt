#pragma once

#include <cstdint>

class RecoveryPolicy
{
public:
    struct Config {
        uint32_t recoveryWindowMs;
        uint32_t connectTimeoutMs;
        uint32_t frameTimeoutMs;
        uint32_t retryDelayMs;
    };

    enum class State {
        Streaming,
        Suspended,
        WaitingToRetry,
        Connecting,
        AwaitingFrame,
        Aborting,
        Failed,
        Stopped,
    };

    enum class Action {
        None,
        StartConnection,
        AbortConnection,
        StreamReady,
        GiveUp,
    };

    explicit RecoveryPolicy(Config config);

    Action begin(uint32_t now, int errorCode);
    Action connectionStarted(uint32_t now);
    Action connectionFailed(uint32_t now, int errorCode);
    Action framePresented();
    Action tick(uint32_t now);

    void markStreaming();
    void suspend();
    void stop();

    State state() const;
    int lastError() const;
    unsigned int attempts() const;

private:
    static bool deadlineReached(uint32_t now, uint32_t deadline);

    Action startAttempt(uint32_t now);
    Action scheduleRetry(uint32_t now, int errorCode);

    Config m_Config;
    State m_State;
    uint32_t m_RecoveryDeadline;
    uint32_t m_PhaseDeadline;
    uint32_t m_RetryAt;
    int m_LastError;
    unsigned int m_Attempts;
};
