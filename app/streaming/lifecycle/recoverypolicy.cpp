#include "recoverypolicy.h"

#include <cassert>

RecoveryPolicy::RecoveryPolicy(Config config)
    : m_Config(config),
      m_State(State::Streaming),
      m_RecoveryDeadline(0),
      m_PhaseDeadline(0),
      m_RetryAt(0),
      m_LastError(0),
      m_Attempts(0)
{
    assert(m_Config.recoveryWindowMs > 0);
    assert(m_Config.connectTimeoutMs > 0);
    assert(m_Config.frameTimeoutMs > 0);
}

RecoveryPolicy::Action RecoveryPolicy::begin(uint32_t now, int errorCode)
{
    if (m_State == State::Stopped) {
        return Action::GiveUp;
    }

    m_RecoveryDeadline = now + m_Config.recoveryWindowMs;
    m_LastError = errorCode;
    m_Attempts = 0;
    return startAttempt(now);
}

RecoveryPolicy::Action RecoveryPolicy::connectionStarted(uint32_t now)
{
    if (m_State != State::Connecting) {
        return Action::None;
    }

    m_State = State::AwaitingFrame;
    m_PhaseDeadline = now + m_Config.frameTimeoutMs;
    return Action::None;
}

RecoveryPolicy::Action RecoveryPolicy::connectionFailed(uint32_t now, int errorCode)
{
    if (m_State != State::Connecting &&
            m_State != State::AwaitingFrame &&
            m_State != State::Aborting) {
        return Action::None;
    }

    return scheduleRetry(now, errorCode);
}

RecoveryPolicy::Action RecoveryPolicy::framePresented()
{
    if (m_State != State::AwaitingFrame) {
        return Action::None;
    }

    markStreaming();
    return Action::StreamReady;
}

RecoveryPolicy::Action RecoveryPolicy::tick(uint32_t now)
{
    switch (m_State) {
    case State::WaitingToRetry:
        if (deadlineReached(now, m_RecoveryDeadline)) {
            m_State = State::Failed;
            return Action::GiveUp;
        }
        if (deadlineReached(now, m_RetryAt)) {
            return startAttempt(now);
        }
        break;

    case State::Connecting:
    case State::AwaitingFrame:
        if (deadlineReached(now, m_PhaseDeadline) ||
                deadlineReached(now, m_RecoveryDeadline)) {
            m_State = State::Aborting;
            return Action::AbortConnection;
        }
        break;

    default:
        break;
    }

    return Action::None;
}

void RecoveryPolicy::markStreaming()
{
    m_State = State::Streaming;
    m_RecoveryDeadline = 0;
    m_PhaseDeadline = 0;
    m_RetryAt = 0;
    m_LastError = 0;
    m_Attempts = 0;
}

void RecoveryPolicy::suspend()
{
    if (m_State != State::Stopped) {
        m_State = State::Suspended;
    }
}

void RecoveryPolicy::stop()
{
    m_State = State::Stopped;
}

RecoveryPolicy::State RecoveryPolicy::state() const
{
    return m_State;
}

int RecoveryPolicy::lastError() const
{
    return m_LastError;
}

unsigned int RecoveryPolicy::attempts() const
{
    return m_Attempts;
}

bool RecoveryPolicy::deadlineReached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

RecoveryPolicy::Action RecoveryPolicy::startAttempt(uint32_t now)
{
    if (deadlineReached(now, m_RecoveryDeadline)) {
        m_State = State::Failed;
        return Action::GiveUp;
    }

    m_State = State::Connecting;
    m_PhaseDeadline = now + m_Config.connectTimeoutMs;
    m_Attempts++;
    return Action::StartConnection;
}

RecoveryPolicy::Action RecoveryPolicy::scheduleRetry(uint32_t now, int errorCode)
{
    m_LastError = errorCode;
    if (deadlineReached(now, m_RecoveryDeadline)) {
        m_State = State::Failed;
        return Action::GiveUp;
    }

    m_State = State::WaitingToRetry;
    m_RetryAt = now + m_Config.retryDelayMs;
    return Action::None;
}
