#include "../app/streaming/lifecycle/recoverypolicy.h"
#include "../app/streaming/lifecycle/recoverysettings.h"
#include "../app/streaming/lifecycle/networkcontinuitypolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
constexpr RecoveryPolicy::Config kConfig = {15000, 3000, 2500, 150};

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void setEnvironment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    }
    else {
        unsetenv(name);
    }
#endif
}

class ScopedEnvironment
{
public:
    ScopedEnvironment(const char* name, const char* value)
        : m_Name(name)
    {
        const char* previous = std::getenv(name);
        if (previous != nullptr) {
            m_HadPreviousValue = true;
            m_PreviousValue = previous;
        }
        setEnvironment(name, value);
    }

    ~ScopedEnvironment()
    {
        setEnvironment(m_Name.c_str(),
                       m_HadPreviousValue ? m_PreviousValue.c_str() : nullptr);
    }

private:
    std::string m_Name;
    std::string m_PreviousValue;
    bool m_HadPreviousValue = false;
};

void requiresPresentedFrame()
{
    RecoveryPolicy policy(kConfig);
    CHECK(policy.begin(100, -1) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.state() == RecoveryPolicy::State::Connecting);
    CHECK(policy.connectionStarted(200) == RecoveryPolicy::Action::None);
    CHECK(policy.state() == RecoveryPolicy::State::AwaitingFrame);
    CHECK(policy.framePresented() == RecoveryPolicy::Action::StreamReady);
    CHECK(policy.state() == RecoveryPolicy::State::Streaming);
}

void retriesAfterDelay()
{
    RecoveryPolicy policy(kConfig);
    CHECK(policy.begin(100, -1) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.connectionFailed(200, -2) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(349) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(350) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.attempts() == 2);
    CHECK(policy.lastError() == -2);
}

void abortsTimedOutPhasesOnce()
{
    RecoveryPolicy policy(kConfig);
    CHECK(policy.begin(100, -1) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.tick(3099) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(3100) == RecoveryPolicy::Action::AbortConnection);
    CHECK(policy.tick(3200) == RecoveryPolicy::Action::None);
    CHECK(policy.connectionFailed(3200, -3) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(3350) == RecoveryPolicy::Action::StartConnection);

    CHECK(policy.connectionStarted(3400) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(5899) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(5900) == RecoveryPolicy::Action::AbortConnection);
}

void stopsAtRecoveryDeadline()
{
    RecoveryPolicy policy({500, 300, 300, 100});
    CHECK(policy.begin(100, -1) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.connectionFailed(400, -4) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(500) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.connectionFailed(600, -5) == RecoveryPolicy::Action::GiveUp);
    CHECK(policy.state() == RecoveryPolicy::State::Failed);
    CHECK(policy.lastError() == -5);
}

void survivesTickWraparound()
{
    RecoveryPolicy policy({1000, 300, 300, 25});
    const uint32_t start = std::numeric_limits<uint32_t>::max() - 10;
    CHECK(policy.begin(start, -1) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.connectionFailed(start + 5, -2) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(13) == RecoveryPolicy::Action::None);
    CHECK(policy.tick(19) == RecoveryPolicy::Action::StartConnection);
}

void suspendsWithoutRetrying()
{
    RecoveryPolicy policy(kConfig);
    CHECK(policy.begin(100, -7) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.connectionFailed(200, -8) == RecoveryPolicy::Action::None);
    policy.suspend();
    CHECK(policy.state() == RecoveryPolicy::State::Suspended);
    CHECK(policy.tick(50000) == RecoveryPolicy::Action::None);
    CHECK(policy.begin(50000, -1) == RecoveryPolicy::Action::StartConnection);
    CHECK(policy.attempts() == 1);
    CHECK(policy.lastError() == -1);
}

void retainsTransportAcrossBriefNetworkLoss()
{
    bool unavailable = false;
    CHECK(networkContinuityAction(false, unavailable) ==
          NetworkContinuityAction::RetainTransport);
    unavailable = true;
    CHECK(networkContinuityAction(false, unavailable) ==
          NetworkContinuityAction::None);
    CHECK(networkContinuityAction(true, unavailable) ==
          NetworkContinuityAction::ReleaseRecoveryGate);
    unavailable = false;
    CHECK(networkContinuityAction(true, unavailable) ==
          NetworkContinuityAction::None);
}

void readsBoundedCompatibilitySettings()
{
    ScopedEnvironment window("MOONLIGHT_RECONNECT_WINDOW_MS", "1200");
    ScopedEnvironment video("MOONLIGHT_RECONNECT_VIDEO_STALL_MS", "12000");
    ScopedEnvironment control("MOONLIGHT_RECONNECT_CONTROL_TIMEOUT_MS", "invalid");
    ScopedEnvironment connect("MOONLIGHT_RECONNECT_CONNECT_TIMEOUT_MS", "500");

    const RecoverySettings settings = RecoverySettings::fromEnvironment();
    CHECK(settings.recoveryWindowMs == 1200);
    CHECK(settings.videoStallTimeoutMs == 12000);
    CHECK(settings.controlInactivityTimeoutMs == 3000);
    CHECK(settings.controlConnectTimeoutMs == 750);
    CHECK(settings.policyConfig().recoveryWindowMs == 1200);
}
}

int main()
{
    requiresPresentedFrame();
    retriesAfterDelay();
    abortsTimedOutPhasesOnce();
    stopsAtRecoveryDeadline();
    survivesTickWraparound();
    suspendsWithoutRetrying();
    retainsTransportAcrossBriefNetworkLoss();
    readsBoundedCompatibilitySettings();
    return EXIT_SUCCESS;
}
