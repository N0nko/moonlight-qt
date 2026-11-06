// FIFO backlog trim: policy unit tests plus a one-present-gate simulation at 90 Hz.
#include "../app/streaming/video/ffmpeg-renderers/pacer/lowlatencypolicy.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <vector>

namespace {

constexpr uint32_t kSlotTicks = 1000;   // 90 kHz RTP ticks per 90 Hz refresh
constexpr double kT = 1e6 / 90.0;       // refresh period, us
constexpr double kRenderUs = 3000;      // dequeue -> submit
constexpr uint64_t kWaitBudgetUs = 4500;

struct Arrival {
    double atUs;
    uint32_t pts;
};

struct SimResult {
    int refreshes = 0;
    int events = 0;         // analyze.py definition: sum |R - P|
    int repeats = 0;
    int skips = 0;
    int hostGaps = 0;       // sum max(0, P - 1 - client drops)
    int drops[5] = {};
    int enqueueDrops = 0;
    int maxDepth = 0;
    double maxLatencyUs = 0;
    double maxLatencyAfterUs = 0;   // after latencyFromUs
    std::vector<double> dropTimesUs;
};

// One-present FIFO gate: the gate opens at a latch; the submitted frame is
// latched at the first latch after submit and stays on glass until the next.
SimResult simulate(const std::vector<Arrival>& arrivals, double endUs, bool trimEnabled = true,
                   double latencyFromUs = 0)
{
    FifoBacklogTrim trim;
    trim.configure(kSlotTicks);
    SimResult res;
    std::deque<Arrival> queue;
    size_t next = 0;
    double gate = 0;
    long lastSlot = -1;
    uint32_t lastPts = 0;
    int clientDrops = 0;
    auto admit = [&](double now) {
        while (next < arrivals.size() && arrivals[next].atUs <= now) {
            if (queue.size() == 3) {   // MAX_QUEUED_FRAMES
                queue.pop_front();
                ++res.enqueueDrops;
                ++clientDrops;
            }
            queue.push_back(arrivals[next++]);
        }
    };
    while (gate < endUs) {
        admit(gate);
        double now = gate;
        if (queue.empty()) {
            if (next >= arrivals.size()) {
                break;
            }
            now = arrivals[next].atUs;
            admit(now);
        }
        const bool starved = FifoBacklogTrim::starvedSlot(static_cast<uint64_t>(gate),
                                                          static_cast<uint64_t>(now),
                                                          static_cast<uint64_t>(kT));
        while (trimEnabled) {
            FifoTrimInput in;
            in.depth = static_cast<int>(queue.size());
            in.headPtsValid = true;
            in.headPts = queue.front().pts;
            in.nowUs = static_cast<uint64_t>(now);
            in.waitDeadlineUs = static_cast<uint64_t>(gate) + kWaitBudgetUs;
            const FifoTrimDecision d = trim.decide(in);
            if (d.action == FifoTrimAction::Present) {
                break;
            }
            if (d.action == FifoTrimAction::WaitForFollower) {
                double until = static_cast<double>(d.untilUs);
                if (next < arrivals.size() && arrivals[next].atUs < until) {
                    until = arrivals[next].atUs;
                }
                now = std::max(now, until);
                admit(now);
                continue;
            }
            assert(queue.size() >= 2);
            queue.pop_front();
            ++res.drops[static_cast<int>(d.action)];
            ++clientDrops;
            res.dropTimesUs.push_back(now);
            trim.dropped(d.action, static_cast<uint64_t>(now));
            break;
        }
        const Arrival frame = queue.front();
        res.maxDepth = std::max(res.maxDepth, static_cast<int>(queue.size()));
        trim.presented(static_cast<int>(queue.size()), true, frame.pts,
                       static_cast<uint64_t>(now), starved);
        queue.pop_front();
        const long slot = static_cast<long>(std::ceil((now + kRenderUs) / kT - 1e-9));
        const double latch = slot * kT;
        res.maxLatencyUs = std::max(res.maxLatencyUs, latch - frame.atUs);
        if (frame.atUs >= latencyFromUs) {
            res.maxLatencyAfterUs = std::max(res.maxLatencyAfterUs, latch - frame.atUs);
        }
        if (lastSlot >= 0) {
            const int R = static_cast<int>(slot - lastSlot);
            const int P = static_cast<int>(std::lround(static_cast<int32_t>(frame.pts - lastPts) /
                                                       static_cast<double>(kSlotTicks)));
            res.refreshes += R;
            if (R > P) {
                res.repeats += R - P;
            }
            if (R < P) {
                res.skips += P - R;
            }
            res.events += std::abs(R - P);
            res.hostGaps += std::max(0, P - 1 - clientDrops);
        }
        clientDrops = 0;
        lastSlot = slot;
        lastPts = frame.pts;
        gate = latch;
    }
    return res;
}

// Deterministic jitter in [-amp, amp].
double jitter(uint32_t i, double amp)
{
    uint32_t x = (i + 1) * 2654435761u;
    x ^= x >> 15;
    return amp * ((x % 2001) / 1000.0 - 1.0);
}

// Host at nominal 90 fps. phaseUs: arrival offset from the latch grid.
// extraEvery: every N frames the host captures an extra frame 200 ticks after
// the previous one (as seen on 2026-11-06); it arrives at the regular time and
// the regular frame follows followerLagUs later.
// gapEvery: every N frames the host skips a capture.
std::vector<Arrival> stream(double seconds, double phaseUs, double jitterUs, int extraEvery = 0,
                            double followerLagUs = 1500, double ppm = 0, int gapEvery = 0)
{
    std::vector<Arrival> out;
    const double period = kT * (1.0 - ppm * 1e-6);
    const int frames = static_cast<int>(seconds * 1e6 / period);
    uint32_t pts = 123456;
    for (int i = 0; i < frames; ++i) {
        const double at = i * period + phaseUs + jitter(static_cast<uint32_t>(i), jitterUs);
        if (gapEvery && i % gapEvery == gapEvery - 1) {
            pts += kSlotTicks;
            continue;
        }
        if (extraEvery && i % extraEvery == extraEvery - 1) {
            out.push_back({at, pts - kSlotTicks + 200});
            out.push_back({at + followerLagUs, pts});
        }
        else {
            out.push_back({at, pts});
        }
        pts += kSlotTicks;
    }
    return out;
}

int count(const SimResult& r, FifoTrimAction a)
{
    return r.drops[static_cast<int>(a)];
}

void report(const char* name, const SimResult& r)
{
    std::printf("%-22s refreshes=%d events=%d (rep %d, skip %d) host_gaps=%d drops slotless=%d "
                "recovery=%d backlog=%d enqueue=%d max_depth=%d max_lat=%.2f ms\n",
                name, r.refreshes, r.events, r.repeats, r.skips, r.hostGaps,
                count(r, FifoTrimAction::DropSlotless), count(r, FifoTrimAction::DropRecovery),
                count(r, FifoTrimAction::DropBacklog), r.enqueueDrops, r.maxDepth,
                r.maxLatencyUs / 1000.0);
}

void unitDecide()
{
    FifoBacklogTrim t;
    t.configure(kSlotTicks);
    FifoTrimInput in;
    in.depth = 1;
    in.headPtsValid = true;
    in.headPts = 1000;
    in.nowUs = 100;
    // No history: never slotless.
    assert(t.decide(in).action == FifoTrimAction::Present);
    in.depth = 2;
    assert(t.decide(in).action == FifoTrimAction::Present);
    t.presented(1, true, 1000, 100, false);

    // Duplicate and near pts are slotless; half a slot or more is not.
    in.headPts = 1000;
    assert(t.decide(in).action == FifoTrimAction::DropSlotless);
    in.headPts = 1499;
    assert(t.decide(in).action == FifoTrimAction::DropSlotless);
    in.headPts = 1500;
    assert(t.decide(in).action == FifoTrimAction::Present);
    // Backwards pts (reorder or restart) is never slotless.
    in.headPts = 900;
    assert(t.decide(in).action == FifoTrimAction::Present);
    // Invalid pts is never slotless.
    in.headPts = 1200;
    in.headPtsValid = false;
    assert(t.decide(in).action == FifoTrimAction::Present);
    in.headPtsValid = true;

    // Alone: wait for a follower until the deadline, then keep it.
    in.depth = 1;
    in.waitDeadlineUs = 5000;
    FifoTrimDecision d = t.decide(in);
    assert(d.action == FifoTrimAction::WaitForFollower && d.untilUs == 5000);
    in.nowUs = 5000;
    assert(t.decide(in).action == FifoTrimAction::Present);
    in.waitDeadlineUs = 0;
    in.nowUs = 100;
    assert(t.decide(in).action == FifoTrimAction::Present);

    // RTP wrap: a forward step across 2^32 is still a step.
    t.configure(kSlotTicks);
    t.presented(1, true, 0xFFFFFF00u, 0, false);
    in.headPts = 0x00000010u;  // +272 ticks
    in.depth = 2;
    assert(t.decide(in).action == FifoTrimAction::DropSlotless);

    // Depth 1 is never trimmed, however long it lasts.
    t.configure(kSlotTicks);
    uint32_t pts = 0;
    for (uint64_t now = 0; now < 60000000; now += 11111) {
        t.presented(1, true, pts += kSlotTicks, now, false);
        in = {};
        in.depth = 1;
        in.headPtsValid = true;
        in.headPts = pts + kSlotTicks;
        in.nowUs = now + 11111;
        assert(t.decide(in).action == FifoTrimAction::Present);
    }

    // Depth 2 at every dequeue: one backstop drop per full hold, no sooner.
    t.configure(kSlotTicks);
    pts = 0;
    int backlogDrops = 0;
    uint64_t firstDrop = 0;
    for (uint64_t now = 0; now < 25000000; now += 11111) {
        in = {};
        in.depth = 2;
        in.headPtsValid = true;
        in.headPts = pts + kSlotTicks;
        in.nowUs = now;
        const FifoTrimAction a = t.decide(in).action;
        if (a == FifoTrimAction::DropBacklog) {
            if (!backlogDrops) {
                firstDrop = now;
            }
            ++backlogDrops;
            t.dropped(a, now);
            in.headPts += kSlotTicks;
        }
        else {
            assert(a == FifoTrimAction::Present);
        }
        t.presented(2, true, in.headPts, now, false);
        pts = in.headPts;
    }
    assert(backlogDrops == 2);
    assert(firstDrop >= FifoBacklogTrim::kBacklogHoldUs);

    // One dequeue at depth 1 restarts the hold.
    t.configure(kSlotTicks);
    t.presented(2, true, 1000, 0, false);
    t.presented(2, true, 2000, 9000000, false);
    t.presented(1, true, 3000, 9011111, false);
    t.presented(2, true, 4000, 9022222, false);
    in = {};
    in.depth = 2;
    in.headPtsValid = true;
    in.headPts = 5000;
    in.nowUs = 15000000;
    assert(t.decide(in).action == FifoTrimAction::Present);
    in.nowUs = 9022222 + FifoBacklogTrim::kBacklogHoldUs;
    assert(t.decide(in).action == FifoTrimAction::DropBacklog);

    // Depth 3 has the shorter hold.
    t.configure(kSlotTicks);
    t.presented(3, true, 1000, 0, false);
    in.depth = 3;
    in.headPts = 2000;
    in.nowUs = FifoBacklogTrim::kBurstHoldUs - 1;
    assert(t.decide(in).action == FifoTrimAction::Present);
    in.nowUs = FifoBacklogTrim::kBurstHoldUs;
    assert(t.decide(in).action == FifoTrimAction::DropBacklog);

    // Recovery: after a starved slot, drop while backed up, for a few dequeues.
    t.configure(kSlotTicks);
    t.presented(1, true, 1000, 0, true);
    in = {};
    in.headPtsValid = true;
    in.headPts = 2000;
    in.nowUs = 11111;
    in.depth = 1;
    assert(t.decide(in).action == FifoTrimAction::Present);
    in.depth = 3;
    assert(t.decide(in).action == FifoTrimAction::DropRecovery);
    for (int i = 0; i < FifoBacklogTrim::kRecoveryDequeues; ++i) {
        t.presented(2, true, 2000 + i * 1000, 11111 * (i + 1), false);
    }
    in.headPts = 9000;
    assert(t.decide(in).action == FifoTrimAction::Present);  // window used up
    // Recovery ends as soon as a dequeue is caught up.
    t.configure(kSlotTicks);
    t.presented(1, true, 1000, 0, true);
    t.presented(2, true, 2000, 11111, false);
    in.headPts = 3000;
    assert(t.decide(in).action == FifoTrimAction::DropRecovery);
    t.presented(1, true, 3000, 22222, false);
    in.headPts = 4000;
    assert(t.decide(in).action == FifoTrimAction::Present);

    // Starved means more than 1.5 refreshes after the gate; a lone host gap is not.
    assert(!FifoBacklogTrim::starvedSlot(1000000, 1000000 + 11111 + 1500, 11111));
    assert(FifoBacklogTrim::starvedSlot(1000000, 1000000 + 16667, 11111));
    assert(!FifoBacklogTrim::starvedSlot(0, 5000000, 11111));

    // A pts restart (reconnect) forgets history: no slotless and no holds.
    t.configure(kSlotTicks);
    for (int i = 0; i < 2000; ++i) {
        t.presented(2, true, 1000u * i, 11111u * i, false);
    }
    t.presented(2, true, 50, 22222000, false);  // restarted RTP clock
    in = {};
    in.depth = 2;
    in.headPtsValid = true;
    in.headPts = 1050;
    in.nowUs = 22233111;
    assert(t.decide(in).action == FifoTrimAction::Present);
    // ...and does not slot-match against the old clock.
    in.headPts = 60;
    assert(t.decide(in).action == FifoTrimAction::DropSlotless);  // +10 ticks on the new clock

    // Unconfigured: inert.
    FifoBacklogTrim off;
    off.presented(1, true, 1000, 0, false);
    in = {};
    in.depth = 2;
    in.headPtsValid = true;
    in.headPts = 1000;
    assert(off.decide(in).action == FifoTrimAction::Present);

    // Empty queue: nothing to do.
    in.depth = 0;
    assert(t.decide(in).action == FifoTrimAction::Present);
}

void simulations()
{
    // Steady, frame arrives just after the latch: perfect, no drops.
    {
        SimResult r = simulate(stream(60, 1500, 400), 60e6);
        report("steady-after-latch", r);
        assert(r.events == 0 && r.hostGaps == 0 && r.enqueueDrops == 0);
        for (int d : r.drops) {
            assert(d == 0);
        }
        assert(r.maxDepth == 1);
    }
    // Steady, frame arrives just before the latch: perfect, no drops.
    {
        SimResult r = simulate(stream(60, -2000, 400), 60e6);
        report("steady-before-latch", r);
        assert(r.events == 0);
        for (int d : r.drops) {
            assert(d == 0);
        }
    }
    // Host extras every ~5 s, head alone at the gate, follower 1.5 ms later
    // (the 2026-11-06 a2 phase): drop each extra, zero events, no latency rise.
    {
        const auto s = stream(180, 300, 300, 450, 1500);
        SimResult r = simulate(s, 180e6);
        report("extras-alone", r);
        assert(r.events == 0);
        assert(count(r, FifoTrimAction::DropSlotless) >= 34);
        assert(count(r, FifoTrimAction::DropBacklog) == 0 && r.enqueueDrops == 0);
        assert(r.maxLatencyUs < kT + kRenderUs);
        // Without the trim the extras pile up until frames are thrown away.
        SimResult old = simulate(s, 180e6, false);
        report("extras-old", old);
        assert(old.enqueueDrops > 0 && old.events > 0);
    }
    // Same extras with the follower already queued at the gate (a1 phase).
    {
        SimResult r = simulate(stream(180, -2500, 300, 450, 2400), 180e6);
        report("extras-queued", r);
        assert(r.events == 0);
        assert(count(r, FifoTrimAction::DropSlotless) >= 34);
        assert(count(r, FifoTrimAction::DropBacklog) == 0 && r.enqueueDrops == 0);
    }
    // Follower too late for the wait budget: the extra is kept and shows as
    // a near-repeat (one analyzer repeat per kept extra), never as a skip.
    {
        SimResult r = simulate(stream(30, 300, 0, 450, 6000), 30e6);
        report("extras-late-follower", r);
        assert(r.skips == 0 && r.repeats <= 6);
        assert(count(r, FifoTrimAction::DropBacklog) == 0 && r.enqueueDrops == 0);
    }
    // Host gaps: never a client drop; with no backlog the display holds for
    // exactly the missing capture, which is clean against the source cadence.
    {
        SimResult r = simulate(stream(60, 1500, 300, 0, 0, 0, 700), 60e6);
        report("host-gaps", r);
        assert(r.events == 0);
        assert(r.hostGaps == 7);
        for (int d : r.drops) {
            assert(d == 0);
        }
    }
    // Drift: host 100 ppm fast, a slot of latency builds about every 111 s.
    // Only slow backstop drops, never in the first minute, depth bounded.
    {
        SimResult r = simulate(stream(600, 1500, 300, 0, 0, 100), 600e6);
        report("drift-100ppm", r);
        assert(count(r, FifoTrimAction::DropBacklog) >= 3 && count(r, FifoTrimAction::DropBacklog) <= 7);
        assert(count(r, FifoTrimAction::DropSlotless) == 0 && count(r, FifoTrimAction::DropRecovery) == 0);
        for (double t : r.dropTimesUs) {
            assert(t > 60e6);
        }
        assert(r.maxDepth <= 3 && r.enqueueDrops == 0);
        assert(r.events <= count(r, FifoTrimAction::DropBacklog) + 2);
    }
    // Wi-Fi stall: 80 ms without frames, then the backlog lands in a burst.
    // Recovery drops land in the stall; latency is back to normal right after.
    {
        std::vector<Arrival> s = stream(20, 1500, 300);
        for (auto& a : s) {
            if (a.atUs > 10e6 && a.atUs < 10.08e6) {
                a.atUs = 10.08e6 + (a.atUs - 10e6) * 0.01;
            }
        }
        SimResult r = simulate(s, 20e6, true, 10.3e6);
        report("wifi-stall", r);
        const int recovery = count(r, FifoTrimAction::DropRecovery);
        assert(recovery >= 1 && recovery <= FifoBacklogTrim::kRecoveryDequeues);
        for (double t : r.dropTimesUs) {
            assert(t >= 10.08e6 && t < 10.2e6);
        }
        assert(count(r, FifoTrimAction::DropBacklog) == 0);
        // At most the one-slot FIFO lag remains after the burst...
        assert(r.maxLatencyAfterUs < 2 * kT + kRenderUs);
        // ...and the next host gap returns the stream to its early phase.
        std::vector<Arrival> g;
        for (const Arrival& a : s) {
            if (a.atUs < 12e6 || a.atUs > 12e6 + kT) {
                g.push_back(a);
            }
        }
        SimResult rg = simulate(g, 20e6, true, 12.1e6);
        report("wifi-stall+host-gap", rg);
        assert(rg.maxLatencyAfterUs < kT + kRenderUs);
        assert(count(rg, FifoTrimAction::DropBacklog) == 0);
    }
    // Reconnect: the RTP clock restarts mid-stream; nothing is dropped for it.
    {
        std::vector<Arrival> s = stream(20, 1500, 300);
        for (auto& a : s) {
            if (a.atUs > 10e6) {
                a.pts -= 777777;
            }
        }
        SimResult r = simulate(s, 20e6);
        report("reconnect", r);
        for (int d : r.drops) {
            assert(d == 0);
        }
    }
}

}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unitDecide();
    simulations();
    std::puts("fifotrim tests passed");
    return 0;
}
