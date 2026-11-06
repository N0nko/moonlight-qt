// FIFO queue-depth controller: policy unit tests plus a one-present-gate
// simulation at 90 Hz with the slot-aware trim in the loop.
#include "../app/streaming/video/ffmpeg-renderers/pacer/lowlatencypolicy.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

namespace {

constexpr uint32_t kSlotTicks = 1000;          // 90 kHz RTP ticks per 90 Hz refresh
constexpr double kT = 1e6 / 90.0;              // refresh period, us
constexpr uint64_t kTns = 11111111;
constexpr uint64_t kSec = 1000000000ull;
constexpr uint64_t kWaitBudgetUs = 4500;

// Deterministic jitter in [-amp, amp].
double jitter(uint32_t i, double amp)
{
    uint32_t x = (i + 1) * 2654435761u;
    x ^= x >> 15;
    return amp * ((x % 2001) / 1000.0 - 1.0);
}

// ---------------------------------------------------------------- unit tests

void feed(FifoDepthController& c, int n, uint64_t phaseNs, uint64_t workNs, bool onArrival)
{
    for (int i = 0; i < n; ++i) {
        c.observeFrame(phaseNs, onArrival, workNs);
    }
}

FifoDepthGate gateAt(uint64_t nowNs, bool backlog = true, int queued = 1, bool extra = false)
{
    FifoDepthGate g;
    g.backlog = backlog;
    g.queued = queued;
    g.hostExtra = extra;
    g.nowNs = nowNs;
    return g;
}

void unitOff()
{
    FifoDepthController c;
    assert(c.state() == FifoDepthState::Off);
    assert(!c.gate(gateAt(100 * kSec, true, 1, true)));
    c.configure(kTns, false); // MOONLIGHT_FIFO_DEPTH=0
    assert(c.state() == FifoDepthState::Off);
    feed(c, 3000, 9000000, 4000000, true);
    assert(!c.gate(gateAt(100 * kSec, true, 1, true)));
    assert(!c.gate(gateAt(200 * kSec, false, 1, false)));
    assert(c.state() == FifoDepthState::Off && !c.onArrival());
    c.configure(0, true); // period unknown
    assert(c.state() == FifoDepthState::Off);
}

void unitSlackWindow()
{
    FifoDepthController c;
    c.configure(kTns, true);
    // Backlog frames with no on-arrival work yet use the conservative default (6.5 ms).
    assert(c.observeFrame(9000000, false, 1000000) == 9000000 - 6500000);
    // On-arrival frames are real samples.
    assert(c.observeFrame(9000000, true, 4000000) == 5000000);
    assert(c.observeFrame(1000000, true, 4000000) == -3000000);
    // Phases outside [0, T) are ignored.
    assert(c.observeFrame(kTns, true, 1000000) == 0);
    assert(c.slackSamples() == 3);

    c.configure(kTns, true);
    feed(c, 1999, 9000000, 4000000, true);
    assert(!c.slackOk()); // < 2000 samples
    feed(c, 1, 9000000, 4000000, true);
    assert(c.slackOk() && c.slackLoNs() == 5000000);
    // Two late frames in 2048 are inside p99.9; three are not.
    c.observeFrame(1000000, true, 4000000);
    c.observeFrame(1000000, true, 4000000);
    assert(c.slackOk());
    c.observeFrame(1000000, true, 4000000);
    assert(!c.slackOk() && c.slackLoNs() == -3000000);
    // A full window of good frames forgets them.
    feed(c, 2048, 9000000, 4000000, true);
    assert(c.slackOk());
    // Slack just at the limit is not enough.
    c.configure(kTns, true);
    feed(c, 2048, 5500000, 4000000, true);
    assert(!c.slackOk());
}

void unitBacklogPairsWithWork()
{
    FifoDepthController c;
    c.configure(kTns, true);
    feed(c, 100, 9000000, 7000000, true); // on-arrival work 7 ms
    // A backlog frame with its own small work is paired with on-arrival work.
    assert(c.observeFrame(9000000, false, 1000000) == 2000000);
    assert(c.workSamples() == 100);
}

FifoDepthController ready(uint64_t& t)
{
    FifoDepthController c;
    c.configure(kTns, true);
    feed(c, 2048, 9000000, 4500000, true);
    t = 30 * kSec;
    return c;
}

void unitDrainAtExtra()
{
    uint64_t t = 0;
    FifoDepthController c = ready(t);
    // Depth 1, no extra yet: waits for one (up to 20 s).
    assert(!c.gate(gateAt(t)));
    assert(!c.gate(gateAt(t + 5 * kSec)));
    // Only one frame queued: a two-deep backlog is not drained.
    assert(!c.gate(gateAt(t + 6 * kSec, true, 2, true)));
    assert(c.gate(gateAt(t + 7 * kSec, true, 1, true)));
    assert(c.onArrival() && c.attempts() == 1 && c.freeEntries() == 0);
    // At depth 0 no further drains.
    assert(!c.gate(gateAt(t + 8 * kSec, false)));
    assert(c.attempts() == 1);
}

void unitDrainWithoutExtraAfter20s()
{
    uint64_t t = 0;
    FifoDepthController c = ready(t);
    assert(!c.gate(gateAt(t)));
    assert(!c.gate(gateAt(t + 19 * kSec)));
    assert(c.gate(gateAt(t + 20 * kSec)));
    assert(c.onArrival());
}

void unitReadyRestartsAfterBlock()
{
    uint64_t t = 0;
    FifoDepthController c = ready(t);
    assert(!c.gate(gateAt(t)));
    // A gate without a single-frame backlog restarts the 20 s wait.
    assert(!c.gate(gateAt(t + 15 * kSec, true, 2)));
    assert(!c.gate(gateAt(t + 25 * kSec)));
    assert(c.gate(gateAt(t + 45 * kSec)));
}

void unitDisturbancesBlock()
{
    uint64_t t = 0;
    FifoDepthController c = ready(t);
    c.observeDisturbance(t);
    assert(!c.gate(gateAt(t + 9 * kSec, true, 1, true)));
    assert(c.gate(gateAt(t + 10 * kSec, true, 1, true)));

    c = ready(t);
    c.observeRepeat(t);
    assert(!c.gate(gateAt(t + 9 * kSec, true, 1, true)));
    assert(c.gate(gateAt(t + 11 * kSec, true, 1, true)));

    // Latch offset: a jump of more than 0.5 ms toward an earlier latch blocks.
    c = ready(t);
    for (int i = 0; i < 300; ++i) {
        c.observeLatchOffset(5400000 + (i % 3) * 100000, t);
    }
    assert(c.gate(gateAt(t + 1, true, 1, true)) == true);
    c = ready(t);
    for (int i = 0; i < 300; ++i) {
        c.observeLatchOffset(5400000, t - kSec);
    }
    c.observeLatchOffset(5800000, t); // +0.4 ms: fine
    assert(c.drainReady(t + 1));
    c.observeLatchOffset(6000000, t); // +0.6 ms: disturbance
    assert(!c.drainReady(t + 9 * kSec));
    assert(c.drainReady(t + 10 * kSec));
    c.observeLatchOffset(4000000, t + 10 * kSec); // a later latch is not a risk
    assert(c.drainReady(t + 10 * kSec));

    // Wi-Fi stall: a late arrival without a source gap, applied at the next gate.
    c = ready(t);
    c.observeArrival(18000000, 1000, kSlotTicks);
    assert(!c.gate(gateAt(t, true, 1, true)));
    assert(!c.gate(gateAt(t + 9 * kSec, true, 1, true)));
    assert(c.gate(gateAt(t + 10 * kSec, true, 1, true)));
    // A late arrival that the source skipped too (host gap) is not a stall.
    c = ready(t);
    c.observeArrival(22000000, 2000, kSlotTicks);
    assert(c.gate(gateAt(t, true, 1, true)));
}

void unitLateFrameAndHold()
{
    uint64_t t = 0;
    FifoDepthController c = ready(t);
    assert(c.gate(gateAt(t, true, 1, true)));
    // First late frame: back to depth 1.
    c.observeRepeat(t + 20 * kSec);
    assert(c.state() == FifoDepthState::Backlog && c.lateExits() == 1);
    // The attempt lasted 20 s (< 60 s): the hold doubles to 120 s.
    assert(c.holdNs() == 120 * kSec);
    assert(!c.gate(gateAt(t + 60 * kSec, true, 1, true)));
    assert(!c.gate(gateAt(t + 119 * kSec, true, 1, true)));
    assert(c.gate(gateAt(t + 120 * kSec, true, 1, true)));
    assert(c.attempts() == 2);
    // A long depth-0 run resets the backoff.
    c.observeRepeat(t + 500 * kSec);
    assert(c.holdNs() == 60 * kSec);
    // Backoff is capped at 8 min.
    for (int i = 0; i < 10; ++i) {
        const uint64_t at = t + (1000 + 1000 * i) * kSec;
        assert(c.gate(gateAt(at, true, 1, true)));
        c.observeRepeat(at + kSec);
    }
    assert(c.holdNs() == FifoDepthController::kMaxAttemptHoldNs);
}

void unitAttemptHold60s()
{
    uint64_t t = 0;
    FifoDepthController c = ready(t);
    assert(c.gate(gateAt(t, true, 1, true)));
    // Back to depth 1 by a standing backlog after 70 s (no backoff: lasted > 60 s).
    for (int i = 0; i < 3; ++i) {
        assert(!c.gate(gateAt(t + 70 * kSec + i, true, 2)));
        assert(c.onArrival());
    }
    assert(!c.gate(gateAt(t + 70 * kSec + 3, true, 2)));
    assert(c.state() == FifoDepthState::Backlog && c.backlogExits() == 1 && c.holdNs() == 60 * kSec);
    // One attempt per 60 s: the last one was at t.
    assert(c.gate(gateAt(t + 71 * kSec, true, 1, true)));
}

void unitBacklogRunResets()
{
    uint64_t t = 0;
    FifoDepthController c = ready(t);
    assert(c.gate(gateAt(t, true, 1, true)));
    for (int i = 0; i < 20; ++i) {
        // Early frames now and then (queued before the gate) do not leave depth 0.
        c.gate(gateAt(t + i, (i % 3) != 2));
        assert(c.onArrival());
    }
}

void unitFreeEntry()
{
    uint64_t t = 0;
    FifoDepthController c = ready(t);
    c.observeDisturbance(t); // the host gap itself
    assert(!c.gate(gateAt(t + 1, false)));
    assert(c.onArrival() && c.freeEntries() == 1 && c.attempts() == 0);
    // Without good slack an empty queue is just a host gap: stay at depth 1.
    FifoDepthController bad;
    bad.configure(kTns, true);
    feed(bad, 2048, 3000000, 4500000, true);
    assert(!bad.gate(gateAt(t, false)));
    assert(bad.state() == FifoDepthState::Backlog && bad.freeEntries() == 0);
}

void unitTrimLoneSlotless()
{
    FifoBacklogTrim t;
    t.configure(kSlotTicks);
    t.presented(1, true, 1000, 0, false);
    FifoTrimInput in;
    in.depth = 1;
    in.headPtsValid = true;
    in.headPts = 1200; // host extra
    in.nowUs = 10;
    in.waitDeadlineUs = 4510;
    assert(t.decide(in).action == FifoTrimAction::WaitForFollower);
    in.dropLoneSlotless = true;
    assert(t.decide(in).action == FifoTrimAction::DropSlotless);
    // A regular head is never dropped alone.
    in.headPts = 2000;
    assert(t.decide(in).action == FifoTrimAction::Present);
}


void unitNeedsRealWork()
{
    FifoDepthController c;
    c.configure(kTns, true);
    // Backlog frames only (no on-arrival work measured): never ready, however
    // good the phase looks.
    feed(c, 4000, 11000000, 500000, false);
    assert(c.slackLoNs() > 4000000 && !c.slackOk());
    assert(!c.gate(gateAt(100 * kSec, true, 1, true)));
    feed(c, 63, 9000000, 4000000, true);
    assert(!c.slackOk());
    feed(c, 1, 9000000, 4000000, true);
    assert(c.slackOk());
}

// ----------------------------------------------------------------- simulation

struct Arrival {
    double atUs;
    uint32_t pts;
    double workUs;   // on-arrival work (decode wait + render + GPU)
};

struct StreamSpec {
    double seconds = 120;
    double phaseUs = 2200;        // decode-ready after the latch (phase c = T - phaseUs)
    double workUs = 4500;         // on-arrival work
    double workJitUs = 800;
    int extraEvery = 450;         // a host capture extra every ~5 s
    int gapEvery = 0;             // periodic host gaps
    std::vector<int> gapsAt;      // host gaps at these frame indices
    std::vector<double> lateAtUs; // a 12 ms work spike (late frame) at these times
};

struct SimResult {
    int refreshes = 0;
    int events = 0;
    int repeats = 0;
    int skips = 0;
    int nearRepeats = 0;          // a host extra shown for a refresh
    int nearRepeatsAfter = 0;
    int hostGaps = 0;
    int drains = 0;
    int frames = 0;
    int onArrivalFrames = 0;
    double latSumUs = 0;
    double latAfterSumUs = 0;
    int latAfterN = 0;
    std::vector<double> drainTimesUs;
    FifoDepthController ctl;
    double latAfter() const { return latAfterN ? latAfterSumUs / latAfterN : 0; }
};

// Host at 90 fps; extras like 2026-11-06 (pts +200 ticks at the regular time,
// the regular frame 1.5 ms later).
std::vector<Arrival> stream(const StreamSpec& sp)
{
    std::vector<Arrival> out;
    const int frames = static_cast<int>(sp.seconds * 1e6 / kT);
    uint32_t pts = 50000;
    // Start with a two-frame burst so the session begins at depth 1.
    out.push_back({sp.phaseUs - 1000, pts - kSlotTicks, sp.workUs});
    for (int i = 0; i < frames; ++i) {
        const double at = i * kT + sp.phaseUs + jitter(static_cast<uint32_t>(i), 300);
        double w = sp.workUs + jitter(static_cast<uint32_t>(i) * 7 + 3, sp.workJitUs);
        for (double late : sp.lateAtUs) {
            if (at >= late && at < late + kT) {
                w = 12000;
            }
        }
        bool gap = sp.gapEvery && i % sp.gapEvery == sp.gapEvery - 1;
        for (int g : sp.gapsAt) {
            gap = gap || g == i;
        }
        if (gap) {
            pts += kSlotTicks;
            continue;
        }
        if (sp.extraEvery && i % sp.extraEvery == sp.extraEvery - 1) {
            out.push_back({at, pts - kSlotTicks + 200, w});
            out.push_back({at + 1500, pts, w});
        }
        else {
            out.push_back({at, pts, w});
        }
        pts += kSlotTicks;
    }
    return out;
}

// One-present FIFO gate on a latch grid k*T: the gate opens at the latch that
// took the previous frame; a frame rendered at t with work w is latched at the
// first latch >= t + w. Backlog frames cost backlogWorkUs (decode finished).
SimResult simulate(const std::vector<Arrival>& arrivals, double endUs, bool controller,
                   double afterUs = 1e18, double backlogWorkUs = 1200)
{
    FifoBacklogTrim trim;
    trim.configure(kSlotTicks);
    SimResult res;
    res.ctl.configure(kTns, controller);
    std::deque<Arrival> queue;
    size_t next = 0;
    double gate = 0;
    long lastSlot = -1;
    uint32_t lastPts = 0;
    int clientDrops = 0;
    auto admit = [&](double now) {
        while (next < arrivals.size() && arrivals[next].atUs <= now) {
            queue.push_back(arrivals[next++]);
        }
    };
    auto waitArrival = [&](double& now) -> bool {
        if (queue.empty()) {
            if (next >= arrivals.size()) {
                return false;
            }
            now = std::max(now, arrivals[next].atUs);
            admit(now);
        }
        return true;
    };
    while (gate < endUs) {
        admit(gate);
        double now = gate;
        if (!waitArrival(now)) {
            break;
        }
        const bool onArrivalState = res.ctl.onArrival();
        int action = 0;
        bool waited = false;
        for (;;) {
            FifoTrimInput in;
            in.depth = static_cast<int>(queue.size());
            in.headPtsValid = true;
            in.headPts = queue.front().pts;
            in.nowUs = static_cast<uint64_t>(now);
            in.waitDeadlineUs = static_cast<uint64_t>(gate) + kWaitBudgetUs;
            in.dropLoneSlotless = onArrivalState;
            const FifoTrimDecision d = trim.decide(in);
            if (d.action == FifoTrimAction::WaitForFollower) {
                waited = true;
                double until = static_cast<double>(d.untilUs);
                if (next < arrivals.size() && arrivals[next].atUs < until) {
                    until = arrivals[next].atUs;
                }
                now = std::max(now, until);
                admit(now);
                continue;
            }
            const bool lone = queue.size() < 2;
            if (d.action != FifoTrimAction::Present &&
                (!lone || (d.action == FifoTrimAction::DropSlotless && onArrivalState))) {
                queue.pop_front();
                ++clientDrops;
                trim.dropped(d.action, static_cast<uint64_t>(now));
                action = d.action == FifoTrimAction::DropSlotless ? (lone ? 9 : (waited ? 7 : 3)) : 5;
            }
            break;
        }
        if (!queue.empty()) {
            FifoDepthGate g;
            g.backlog = queue.front().atUs < gate;
            g.queued = static_cast<int>(queue.size());
            g.hostExtra = action == 3 || action == 7;
            g.nowNs = static_cast<uint64_t>(now * 1000);
            if (res.ctl.gate(g)) {
                clientDrops += static_cast<int>(queue.size());
                queue.clear();
                ++res.drains;
                res.drainTimesUs.push_back(now);
            }
        }
        if (!waitArrival(now)) {
            break;
        }
        const Arrival frame = queue.front();
        queue.pop_front();
        trim.presented(static_cast<int>(queue.size()) + 1, true, frame.pts,
                       static_cast<uint64_t>(now), false);
        const bool onArrival = frame.atUs > gate;
        const double work = onArrival ? frame.workUs : backlogWorkUs;
        const long slot = static_cast<long>(std::ceil((now + work) / kT - 1e-9));
        const double latch = slot * kT;
        ++res.frames;
        res.onArrivalFrames += onArrival ? 1 : 0;
        res.latSumUs += latch - frame.atUs;
        if (frame.atUs >= afterUs) {
            res.latAfterSumUs += latch - frame.atUs;
            ++res.latAfterN;
        }
        // Feedback: phase from decode-ready to the first latch after it.
        double phase = std::fmod(std::ceil(frame.atUs / kT - 1e-9) * kT - frame.atUs, kT);
        if (phase < 0) {
            phase += kT;
        }
        res.ctl.observeFrame(static_cast<uint64_t>(phase * 1000), onArrival,
                             static_cast<uint64_t>(frame.workUs * 1000));
        if (lastSlot >= 0) {
            const int R = static_cast<int>(slot - lastSlot);
            const int32_t step = static_cast<int32_t>(frame.pts - lastPts);
            const int P = std::max(1, static_cast<int>(std::lround(step / static_cast<double>(kSlotTicks))));
            res.refreshes += R;
            if (step * 2 < static_cast<int32_t>(kSlotTicks)) {
                ++res.nearRepeats;
                if (frame.atUs >= afterUs) {
                    ++res.nearRepeatsAfter;
                }
            }
            if (R > P) {
                res.repeats += R - P;
                res.ctl.observeRepeat(static_cast<uint64_t>(latch * 1000));
            }
            if (R < P) {
                res.skips += P - R;
            }
            res.events += std::abs(R - P);
            const int gaps = std::max(0, P - 1 - clientDrops);
            res.hostGaps += gaps;
            if (gaps > 0) {
                res.ctl.observeDisturbance(static_cast<uint64_t>(latch * 1000));
            }
        }
        clientDrops = 0;
        lastSlot = slot;
        lastPts = frame.pts;
        gate = latch;
    }
    return res;
}

void report(const char* name, const SimResult& r)
{
    std::printf("%-34s refreshes=%d events=%d (rep %d, skip %d) near_rep=%d host_gaps=%d drains=%d "
                "free=%u late_exits=%u on_arrival=%.1f%% lat=%.2f ms lat_after=%.2f ms\n",
                name, r.refreshes, r.events, r.repeats, r.skips, r.nearRepeats, r.hostGaps, r.drains,
                r.ctl.freeEntries(), r.ctl.lateExits(), 100.0 * r.onArrivalFrames / std::max(1, r.frames),
                r.latSumUs / std::max(1, r.frames) / 1000.0, r.latAfter() / 1000.0);
}

void simGoodPhase()
{
    // Phase c = 8.9 ms, on-arrival work 4.5 +/- 0.8 ms. One host gap at 3 s
    // empties the queue (depth 0 for free); a late frame at 40 s refills it.
    StreamSpec sp;
    sp.gapsAt = {270};
    sp.lateAtUs = {40e6};
    const auto s = stream(sp);
    const SimResult off = simulate(s, 120e6, false, 70e6);
    const SimResult on = simulate(s, 120e6, true, 70e6);
    report("good phase, trim only", off);
    report("good phase, trim + depth", on);
    // Both: one host gap, one late frame (repeat), no near-repeats.
    assert(off.repeats == 1 && off.hostGaps == 1 && off.nearRepeats == 0 && off.drains == 0);
    assert(on.repeats == 1 && on.hostGaps == 1 && on.nearRepeats == 0);
    // Trim only stays at depth 1 after the late frame.
    assert(off.latAfter() > 19000);
    // Controller: a free entry, a late exit, then exactly one drain (one skip)
    // at a host extra after 10 quiet seconds, and one refresh less latency.
    assert(on.ctl.freeEntries() == 1 && on.ctl.lateExits() == 1);
    assert(on.drains == 1 && on.skips == off.skips + 1 && on.events == off.events + 1);
    assert(on.drainTimesUs[0] >= 50e6 && on.drainTimesUs[0] < 56e6);
    assert(on.ctl.onArrival());
    assert(off.latAfter() - on.latAfter() > kT - 100);
}

void simBadPhase()
{
    // Frames ready 1 ms before a latch: depth 0 is impossible. Never drain.
    StreamSpec sp;
    sp.phaseUs = kT - 1000;
    sp.gapsAt = {270};
    const auto s = stream(sp);
    const SimResult off = simulate(s, 120e6, false);
    const SimResult on = simulate(s, 120e6, true);
    report("bad phase, trim only", off);
    report("bad phase, trim + depth", on);
    assert(on.drains == 0 && on.ctl.attempts() == 0 && on.ctl.freeEntries() == 0);
    assert(on.events == off.events && on.refreshes == off.refreshes);
}

void simMarginalPhase()
{
    // Phase c = 6.1 ms against 4.5 +/- 0.8 ms work: p99.9 slack below 1.5 ms
    // (the followers of extras arrive 1.5 ms later still). Never drain.
    StreamSpec sp;
    sp.phaseUs = kT - 6100;
    sp.gapsAt = {270};
    sp.lateAtUs = {30e6};
    const auto s = stream(sp);
    const SimResult on = simulate(s, 120e6, true);
    report("marginal phase, trim + depth", on);
    assert(on.drains == 0 && on.ctl.freeEntries() == 0);
}

void simLateFramesBackoff()
{
    // Late frames at 40 s and 70 s: drain at ~50 s, late at 70 s (the attempt
    // lasted 20 s) doubles the hold, so the next drain comes >= 120 s later.
    StreamSpec sp;
    sp.seconds = 220;
    sp.gapsAt = {270};
    sp.lateAtUs = {40e6, 70e6};
    const auto s = stream(sp);
    const SimResult on = simulate(s, 220e6, true);
    report("late frames, trim + depth", on);
    assert(on.ctl.lateExits() == 2 && on.repeats == 2);
    assert(on.drains == 2);
    assert(on.drainTimesUs[1] - on.drainTimesUs[0] >= 120e6);
}

void simHostGaps()
{
    // A host gap every ~11 s keeps depth 0 for free: never spend a drop.
    StreamSpec sp;
    sp.gapEvery = 1000;
    sp.lateAtUs = {40e6};
    const auto s = stream(sp);
    const SimResult off = simulate(s, 120e6, false);
    const SimResult on = simulate(s, 120e6, true);
    report("host gaps, trim only", off);
    report("host gaps, trim + depth", on);
    assert(on.drains == 0 && on.ctl.attempts() == 0);
    assert(on.ctl.freeEntries() >= 2);
    assert(on.events == off.events);
}

void simExtraAtDepth0()
{
    // Phase c = 6.9 ms, work 3.0 ms: the follower of a lone extra at depth 0
    // comes 5.7 ms after the gate, past the trim wait of 4.5 ms. Trim only
    // shows the extra (a near-repeat) and falls back to depth 1; at depth 0
    // the controller drops the lone extra and the follower makes the latch.
    StreamSpec sp;
    sp.seconds = 150;
    sp.phaseUs = kT - 6900;
    sp.workUs = 3000;
    sp.workJitUs = 300;
    sp.gapsAt = {100};
    const auto s = stream(sp);
    const SimResult off = simulate(s, 150e6, false, 60e6);
    const SimResult on = simulate(s, 150e6, true, 60e6);
    report("phase 6.9 ms, trim only", off);
    report("phase 6.9 ms, trim + depth", on);
    assert(off.nearRepeats == 1 && off.latAfter() > 17000);
    assert(on.drains <= 1 && on.repeats == 0 && on.nearRepeatsAfter == 0);
    assert(on.ctl.onArrival() && on.latAfter() < 7500);
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unitOff();
    unitSlackWindow();
    unitBacklogPairsWithWork();
    unitDrainAtExtra();
    unitDrainWithoutExtraAfter20s();
    unitReadyRestartsAfterBlock();
    unitDisturbancesBlock();
    unitLateFrameAndHold();
    unitAttemptHold60s();
    unitBacklogRunResets();
    unitFreeEntry();
    unitTrimLoneSlotless();
    unitNeedsRealWork();
    simGoodPhase();
    simBadPhase();
    simMarginalPhase();
    simLateFramesBackoff();
    simHostGaps();
    simExtraAtDepth0();
    std::printf("fifodepthctl_test: all passed\n");
    return 0;
}
