#include "../app/streaming/lifecycle/recoverypolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

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
    policy.suspend();
    CHECK(policy.state() == RecoveryPolicy::State::Suspended);
    CHECK(policy.tick(50000) == RecoveryPolicy::Action::None);
    CHECK(policy.begin(50000, -1) == RecoveryPolicy::Action::StartConnection);
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
    return EXIT_SUCCESS;
}
