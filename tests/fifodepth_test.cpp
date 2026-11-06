#include "../app/streaming/video/ffmpeg-renderers/pacer/fifodepthpolicy.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint64_t kMs = 1000000ull;
constexpr uint64_t kT90 = 11111111ull;
constexpr uint64_t kT60 = 16666667ull;

// Arm a policy with a fixed lead: latch offset + render + margin.
void prime(FifoDepthPolicy& p, uint64_t period, uint64_t toVblank, uint64_t jitterNs,
           uint64_t latch = 4500000, uint64_t render = 800000)
{
    p.reset(period);
    p.setMarginNs(1000000);
    for (size_t i = 0; i < FifoDepthPolicy::kCadenceWindow; ++i) p.observeCadence(true);
    for (size_t i = 0; i < 64; ++i) {
        p.observeLatchOffset(latch, 0);
        p.observeRender(render);
    }
    for (size_t i = 0; i < FifoDepthPolicy::kArrivalWindow; ++i) {
        // Deterministic jitter: mostly on time, a few late by up to jitterNs.
        const uint64_t late = (i % 97 == 0) ? jitterNs : (i % 7) * jitterNs / 20;
        uint64_t v = toVblank > late ? toVblank - late : toVblank + period - late;
        p.observeArrival(v);
    }
}

} // namespace

int main()
{
    // Cadence helpers.
    assert(fifoOneToOneDelta(1000, kT90));        // 90 fps on 90 Hz
    assert(!fifoOneToOneDelta(2000, kT90));       // 45 fps on 90 Hz
    assert(!fifoOneToOneDelta(1000, kT60));       // 90 fps on 60 Hz
    assert(fifoOneToOneDelta(1500, kT60));        // 60 fps on 60 Hz
    assert(!fifoOneToOneDelta(0, kT90));
    assert(!fifoOneToOneDelta(1000, 0));
    assert(fifoSourceRefreshes(1000, kT90) == 1);
    assert(fifoSourceRefreshes(2000, kT90) == 2);  // host gap or 45 fps
    assert(fifoSourceRefreshes(3000, kT90) == 3);
    assert(fifoSourceRefreshes(0, kT90) == 1);
    assert(fifoDisplayRefreshes(0, kT90, kT90) == 1);
    assert(fifoDisplayRefreshes(0, 2 * kT90 + 100000, kT90) == 2);
    assert(fifoDisplayRefreshes(5, 5, kT90) == 0);

    // Grid distance is in (0, T].
    assert(fifoToNextVblank(0, 0, kT90) == kT90);
    assert(fifoToNextVblank(1, 0, kT90) == kT90 - 1);
    assert(fifoToNextVblank(kT90 - 1, 0, kT90) == 1);
    assert(fifoToNextVblank(100 * kT90 + 5 * kMs, 3 * kT90, kT90) == kT90 - 5 * kMs);
    assert(fifoToNextVblank(2 * kT90 - 3 * kMs, 7 * kT90, kT90) == 3 * kMs); // before anchor
    assert(fifoToNextVblank(2 * kT90, 7 * kT90, kT90) == kT90);

    // Not armed before enough arrivals, without cadence, or without feedback.
    {
        FifoDepthPolicy p;
        p.reset(kT90);
        p.evaluate(1);
        assert(!p.armed() && p.depth() == 1);
        assert(!p.drainAllowed(100 * FifoDepthPolicy::kSecond));
        assert(p.nominalVblank(5 * kMs) == 0);
    }
    {
        FifoDepthPolicy p;
        prime(p, kT90, 9 * kMs, 300000);
        for (size_t i = 0; i < 10; ++i) p.observeCadence(false); // 45 fps arrives
        p.evaluate(1);
        assert(!p.cadenceOneToOne() && !p.armed() && p.depth() == 1);
    }

    // Plenty of slack: lead 6.3 ms, frames arrive 9 ms before vblank -> depth 0.
    {
        FifoDepthPolicy p;
        prime(p, kT90, 9 * kMs, 1 * kMs);
        p.evaluate(10 * FifoDepthPolicy::kSecond);
        assert(p.armed());
        assert(p.leadNs() == 4500000 + 800000 + 1000000);
        assert(p.depth() == 0);
        assert(p.slackMedNs() > 0 && p.slackLoNs() >= 0);
        // An on-time frame belongs to the first vblank after arrival.
        assert(p.nominalVblank(9 * kMs) == 0);
        assert(p.frameSlackNs(9 * kMs) == static_cast<int64_t>(9 * kMs - p.leadNs()));
        // A frame later than ever seen misses that vblank: negative slack.
        assert(p.frameSlackNs(6 * kMs) < 0);
        assert(p.nominalVblank(6 * kMs) == 0);
        // Hysteresis: armed for >= 5 s before the first drain.
        assert(!p.drainAllowed(10 * FifoDepthPolicy::kSecond));
        assert(p.drainAllowed(15 * FifoDepthPolicy::kSecond));
        p.noteDrain(15 * FifoDepthPolicy::kSecond);
        assert(!p.drainAllowed(24 * FifoDepthPolicy::kSecond));  // < 10 s
        assert(p.drainAllowed(25 * FifoDepthPolicy::kSecond));
        // A miss shortly after a drain doubles the hold and needs 5 quiet seconds.
        p.observeMiss(27 * FifoDepthPolicy::kSecond);
        assert(p.holdNs() == 20 * FifoDepthPolicy::kSecond);
        assert(!p.drainAllowed(31 * FifoDepthPolicy::kSecond));
        assert(!p.drainAllowed(34 * FifoDepthPolicy::kSecond)); // hold 20 s from drain
        assert(p.drainAllowed(35 * FifoDepthPolicy::kSecond));
        for (int i = 0; i < 6; ++i) {
            p.noteDrain((40 + i) * FifoDepthPolicy::kSecond);
            p.observeMiss((40 + i) * FifoDepthPolicy::kSecond + 1);
        }
        assert(p.holdNs() == FifoDepthPolicy::kMaxHoldNs);
        // Long quiet period relaxes the hold again.
        p.evaluate(200 * FifoDepthPolicy::kSecond);
        assert(p.holdNs() == FifoDepthPolicy::kBaseHoldNs);
    }

    // Host gaps and latch-offset jumps block drains for 10 s.
    {
        FifoDepthPolicy p;
        prime(p, kT90, 9 * kMs, 1 * kMs);
        p.evaluate(10 * FifoDepthPolicy::kSecond);
        assert(p.drainAllowed(15 * FifoDepthPolicy::kSecond));
        p.observeDisturbance(15 * FifoDepthPolicy::kSecond);
        assert(!p.drainAllowed(24 * FifoDepthPolicy::kSecond));
        assert(p.drainAllowed(25 * FifoDepthPolicy::kSecond));
        p.observeLatchOffset(4500000 + 400000, 26 * FifoDepthPolicy::kSecond); // small: fine
        assert(p.drainAllowed(27 * FifoDepthPolicy::kSecond));
        p.observeLatchOffset(4500000 + 600000, 28 * FifoDepthPolicy::kSecond); // jump
        assert(!p.drainAllowed(37 * FifoDepthPolicy::kSecond));
        assert(p.drainAllowed(38 * FifoDepthPolicy::kSecond));
    }

    // Not enough slack: arrivals 7 ms before vblank, 1.5 ms worst jitter vs lead 6.3.
    {
        FifoDepthPolicy p;
        prime(p, kT90, 7 * kMs, 1500000);
        p.evaluate(10 * FifoDepthPolicy::kSecond);
        assert(p.armed());
        assert(p.depth() == 1);   // existing behaviour retained
        assert(!p.drainAllowed(100 * FifoDepthPolicy::kSecond));
    }

    // Cluster right at the latch boundary: on-time frames barely make it,
    // late ones wrap to "next vblank with plenty of slack" and must be
    // recognised as late (nominal vblank one earlier than their first feasible).
    {
        FifoDepthPolicy p;
        // lead 6.3 ms; arrivals 6.8 ms before vblank -> s = 0.5 ms; late by 1 ms -> wraps.
        prime(p, kT90, 6800000, 1 * kMs);
        p.evaluate(10 * FifoDepthPolicy::kSecond);
        assert(p.armed());
        assert(p.depth() == 1);
        assert(p.slackLoNs() < 0);
        assert(p.nominalVblank(6800000) == 0);
        assert(p.frameSlackNs(6800000) == 500000);
        // 5.8 ms before vblank: cannot make the first vblank (first feasible = 1)
        // but belongs to vblank 0 by the cluster -> late.
        assert(p.nominalVblank(5800000) == 0);
        assert(p.frameSlackNs(5800000) == -500000);
    }

    // Early frames with nearly a full period of slack: arrivals just after a
    // latch-lead point; early jitter wraps the raw slack to ~0 and must map to
    // the later vblank (not be mistaken for an overdue frame).
    {
        FifoDepthPolicy p;
        p.reset(kT90);
        p.setMarginNs(1000000);
        for (size_t i = 0; i < FifoDepthPolicy::kCadenceWindow; ++i) p.observeCadence(true);
        for (int i = 0; i < 64; ++i) { p.observeLatchOffset(4500000, 0); p.observeRender(800000); }
        const uint64_t lead = 6300000;
        for (size_t i = 0; i < FifoDepthPolicy::kArrivalWindow; ++i) {
            // s around T - 0.5 ms, early by up to 1 ms (crossing to s ~ 0.5 ms).
            const uint64_t early = (i % 7 == 0) ? 1000000 : (i % 7 == 1 ? 500000 : 0);
            uint64_t s = kT90 - 500000 + early;     // may exceed T
            if (s >= kT90) s -= kT90;
            uint64_t toVblank = (s + lead) % kT90;
            if (toVblank == 0) toVblank = kT90;
            p.observeArrival(toVblank);
        }
        p.evaluate(10 * FifoDepthPolicy::kSecond);
        assert(p.armed());
        assert(p.depth() == 0);
        const uint64_t onTime = (kT90 - 500000 + lead) % kT90;   // 5.8 ms
        const uint64_t earlyOne = (500000 + lead) % kT90;        // 6.8 ms
        assert(p.frameSlackNs(onTime) == static_cast<int64_t>(kT90 - 500000));
        // The early frame could make vblank 0, but its slot is vblank 1.
        assert(p.nominalVblank(earlyOne) == 1);
        assert(p.frameSlackNs(earlyOne) == static_cast<int64_t>(kT90 + 500000));
        // An on-time frame: raw first feasible is vblank 1 (toVblank < lead).
        assert(p.nominalVblank(onTime) == 1);
    }

    // Too jittery (spread >= T/2): disarmed.
    {
        FifoDepthPolicy p;
        p.reset(kT90);
        for (size_t i = 0; i < FifoDepthPolicy::kCadenceWindow; ++i) p.observeCadence(true);
        p.observeLatchOffset(4500000, 0);
        p.observeRender(800000);
        for (size_t i = 0; i < FifoDepthPolicy::kArrivalWindow; ++i) {
            p.observeArrival(1 + (i * 7919) % kT90);
        }
        p.evaluate(1);
        assert(!p.armed() && p.depth() == 1);
    }

    // Lead too large for the period: disarmed.
    {
        FifoDepthPolicy p;
        prime(p, kT90, 9 * kMs, 0, 9 * kMs, 1 * kMs);
        p.evaluate(1);
        assert(!p.armed());
    }

    // 60 Hz: more slack per period.
    {
        FifoDepthPolicy p;
        prime(p, kT60, 12 * kMs, 2 * kMs);
        p.evaluate(1);
        assert(p.armed() && p.depth() == 0);
    }

    // reset keeps the margin and clears the window.
    {
        FifoDepthPolicy p;
        prime(p, kT90, 9 * kMs, 0);
        p.setMarginNs(1500000);
        p.reset(kT60);
        assert(p.periodNs() == kT60 && p.marginNs() == 1500000 && p.arrivals() == 0);
    }

    // Default margin is 2 ms; fewer than a full window of arrivals never arms.
    {
        FifoDepthPolicy p;
        assert(p.marginNs() == 2000000);
        p.reset(kT90);
        for (size_t i = 0; i < FifoDepthPolicy::kCadenceWindow; ++i) p.observeCadence(true);
        p.observeLatchOffset(4500000, 0);
        p.observeRender(800000);
        for (size_t i = 0; i + 1 < FifoDepthPolicy::kArrivalWindow; ++i) p.observeArrival(9 * kMs);
        p.evaluate(1);
        assert(!p.armed());
        p.observeArrival(9 * kMs);
        p.evaluate(2 * FifoDepthPolicy::kSecond);
        assert(p.armed() && p.depth() == 0);
    }

    std::puts("fifodepth policy tests passed");
    return 0;
}
