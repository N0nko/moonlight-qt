#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>

// Bound presentations still owned by the compositor, not GPU submissions.
// Zero disables the limit. IDs start at one independently on each swapchain.
inline uint64_t presentationToWaitFor(uint64_t submittedId, int maxPending)
{
    return maxPending > 0 && submittedId >= static_cast<uint64_t>(maxPending) ?
        submittedId - static_cast<uint64_t>(maxPending) + 1 : 0;
}

inline int latestFramesToDrop(int depth, bool enabled)
{
    return enabled && depth > 1 ? depth - 1 : 0;
}

// Only discard presentation of a fully decoded frame. Encoded reference frames
// must still be decoded, and the caller must retain the last available picture.
inline bool staleDecodedFrame(uint64_t queuedUs, uint64_t readyUs, int fps)
{
    if (queuedUs == 0 || readyUs < queuedUs || fps <= 0) {
        return false;
    }
    const uint64_t maxAgeUs = std::max<uint64_t>(50000, 3000000ull / fps);
    return readyUs - queuedUs > maxAgeUs;
}

// Bridge the decode clock to presentation MONOTONIC using a bracketed sample.
inline uint64_t decodeReadyMonotonicNs(int64_t readyUs, uint64_t beforeUs,
                                      uint64_t monoNs, uint64_t afterUs)
{
    if (readyUs <= 0 || afterUs < beforeUs || afterUs - beforeUs > 250) {
        return 0;
    }
    const uint64_t localUs = beforeUs + (afterUs - beforeUs) / 2;
    if (static_cast<uint64_t>(readyUs) > localUs || localUs - readyUs > 5000000) {
        return 0;
    }
    const uint64_t ageNs = (localUs - readyUs) * 1000;
    return monoNs > ageNs ? monoNs - ageNs : 0;
}

class PresentationHistory {
public:
    struct Sample {
        uint32_t id = 0;
        uint64_t readyNs = 0;
        uint64_t submitNs = 0;
    };

    void clear() { m_Samples = {}; }

    void record(uint32_t id, uint64_t readyNs, uint64_t submitNs)
    {
        m_Samples[id % m_Samples.size()] =
            readyNs != 0 && submitNs >= readyNs ? Sample{id, readyNs, submitNs} : Sample{};
    }

    Sample take(uint32_t id, uint64_t actualNs)
    {
        auto& stored = m_Samples[id % m_Samples.size()];
        if (id == 0 || stored.id != id) {
            return {};
        }
        const Sample sample = stored;
        stored = {};
        if (actualNs < sample.submitNs || actualNs - sample.readyNs > 5000000000ull) {
            return {};
        }
        return sample;
    }

private:
    std::array<Sample, 256> m_Samples{};
};

class AdaptivePresentLead {
public:
    void reset() { *this = {}; }

    void observe(uint64_t submitNs, uint64_t earliestNs, uint64_t actualNs,
                 uint64_t marginNs, uint64_t periodNs, uint64_t baselineNs)
    {
        if (periodNs < 4000000 || periodNs > 40000000 || earliestNs < submitNs ||
            earliestNs > actualNs || marginNs == 0 || marginNs > earliestNs - submitNs) {
            return;
        }
        const uint64_t workNs = earliestNs - submitNs - marginNs;
        if (workNs > 5000000) {
            return;
        }
        if (periodNs != m_PeriodNs || actualNs < m_LastFeedbackNs ||
            actualNs - m_LastFeedbackNs > 500000000) {
            reset();
            m_PeriodNs = periodNs;
            m_LeadNs = baselineNs;
        }
        m_LastFeedbackNs = actualNs;
        m_WorkNs[m_Count++] = workNs;
        if (m_Count == m_WorkNs.size()) {
            auto sorted = m_WorkNs;
            std::sort(sorted.begin(), sorted.end());
            const uint64_t target = std::clamp<uint64_t>(sorted[30] + 250000, 500000,
                                                        std::min<uint64_t>(periodNs / 4, 2000000));
            // Change slowly; a late compositor queue is not a reason to add a frame.
            m_LeadNs = target > m_LeadNs ? std::min(target, m_LeadNs + 125000) :
                                         std::max(target, m_LeadNs > 125000 ? m_LeadNs - 125000 : 0);
            m_Count = 0;
        }
    }

    uint64_t lead(uint64_t nowNs, uint64_t periodNs, uint64_t baselineNs) const
    {
        return m_LastFeedbackNs != 0 && periodNs == m_PeriodNs &&
                       nowNs >= m_LastFeedbackNs && nowNs - m_LastFeedbackNs <= 500000000 ?
                   m_LeadNs : baselineNs;
    }

private:
    std::array<uint64_t, 32> m_WorkNs{};
    size_t m_Count = 0;
    uint64_t m_PeriodNs = 0;
    uint64_t m_LeadNs = 0;
    uint64_t m_LastFeedbackNs = 0;
};

// FIFO profile backlog trim, evaluated once the present gate opens and before
// the head frame is submitted. It replaces the 500 ms queue-history trim, which
// dropped a frame that still owned a refresh slot about every 5 s at 90 Hz.
//
// Rules, in order:
//  1. Slotless head: its RTP pts is less than half a refresh after the frame on
//     glass (a host capture extra). With a follower queued, drop the head; the
//     follower takes this slot with an exact one-slot content step. Alone, wait
//     for the follower until the caller's submit deadline, then keep the head.
//  2. Recovery: after a starved slot (the display already repeated), drop one
//     stale frame per dequeue until caught up (depth 1), for a few dequeues at
//     most. The skips land in the stall's own hitch.
//  3. Drift backstop: drop one frame only after every dequeue for a long hold
//     had a backlog (depth >= 2), or depth >= 3 for a shorter hold. Host gaps
//     already drain a backlog for free, so this rarely fires.
// Never more than one drop per dequeue, and never the last queued frame (except
// a lone slotless head at depth 0, see FifoTrimInput::dropLoneSlotless).
enum class FifoTrimAction : uint8_t {
    Present,          // submit the head
    DropSlotless,     // drop the head; the follower owns the slot
    WaitForFollower,  // hold the slotless head until untilUs for a follower
    DropRecovery,     // drop the head after a starved slot
    DropBacklog,      // drift backstop
};

struct FifoTrimInput {
    int depth = 0;              // queued decoded frames, head included
    bool headPtsValid = false;
    uint32_t headPts = 0;       // RTP 90 kHz
    uint64_t nowUs = 0;
    uint64_t waitDeadlineUs = 0;// latest time to hold a slotless head; 0 = never wait
    // Depth 0 (FifoDepthController): frames are rendered on arrival and the
    // follower of a lone slotless head will make the next latch. Drop the head
    // (the caller then waits for the follower) instead of showing a near-repeat.
    bool dropLoneSlotless = false;
};

struct FifoTrimDecision {
    FifoTrimAction action = FifoTrimAction::Present;
    uint64_t untilUs = 0;
};

class FifoBacklogTrim {
public:
    static constexpr uint64_t kBacklogHoldUs = 10000000;  // depth >= 2 at every dequeue
    static constexpr uint64_t kBurstHoldUs = 1000000;     // depth >= 3 at every dequeue
    static constexpr int kRecoveryDequeues = 4;

    // The head became available more than 1.5 refreshes after the gate opened:
    // at least one slot repeated beyond what a single host gap explains.
    static bool starvedSlot(uint64_t gateUs, uint64_t availableUs, uint64_t periodUs)
    {
        return gateUs != 0 && availableUs > gateUs + periodUs + periodUs / 2;
    }

    // slotTicks: one display refresh in RTP 90 kHz ticks.
    void configure(uint32_t slotTicks)
    {
        *this = {};
        m_SlotTicks = slotTicks;
    }

    void reset()
    {
        const uint32_t slot = m_SlotTicks;
        configure(slot);
    }

    // The head shares the slot of the frame on glass: a forward pts step under half a refresh.
    bool slotless(const FifoTrimInput& in) const
    {
        if (m_SlotTicks == 0 || !in.headPtsValid || !m_LastPtsValid) {
            return false;
        }
        const int32_t step = static_cast<int32_t>(in.headPts - m_LastPts);
        return step >= 0 && static_cast<uint32_t>(step) * 2 < m_SlotTicks;
    }

    FifoTrimDecision decide(const FifoTrimInput& in) const
    {
        FifoTrimDecision d;
        if (in.depth <= 0) {
            return d;
        }
        if (slotless(in)) {
            if (in.depth >= 2 || in.dropLoneSlotless) {
                d.action = FifoTrimAction::DropSlotless;
            }
            else if (in.waitDeadlineUs > in.nowUs) {
                d.action = FifoTrimAction::WaitForFollower;
                d.untilUs = in.waitDeadlineUs;
            }
            return d;
        }
        if (in.depth < 2) {
            return d;
        }
        if (m_RecoveryLeft > 0) {
            d.action = FifoTrimAction::DropRecovery;
        }
        else if ((in.depth >= 3 && m_Depth3 && in.nowUs - m_Depth3SinceUs >= kBurstHoldUs) ||
                 (m_Depth2 && in.nowUs - m_Depth2SinceUs >= kBacklogHoldUs)) {
            d.action = FifoTrimAction::DropBacklog;
        }
        return d;
    }

    // A drop was applied; a backstop drop restarts the hold.
    void dropped(FifoTrimAction action, uint64_t nowUs)
    {
        if (action == FifoTrimAction::DropBacklog) {
            m_Depth2SinceUs = nowUs;
            m_Depth3SinceUs = nowUs;
        }
    }

    // The head is being submitted. depth counts it; starved means the head
    // reached the gate after its slot had already been repeated.
    void presented(int depth, bool ptsValid, uint32_t pts, uint64_t nowUs, bool starved)
    {
        if (ptsValid && m_LastPtsValid) {
            const int32_t step = static_cast<int32_t>(pts - m_LastPts);
            // A stream restart or wrap far outside one second: forget history.
            if (step < 0 || step > 90000) {
                const uint32_t slot = m_SlotTicks;
                configure(slot);
            }
        }
        m_LastPtsValid = ptsValid;
        m_LastPts = pts;
        if (!m_Depth2 || depth < 2) {
            m_Depth2SinceUs = nowUs;
        }
        if (!m_Depth3 || depth < 3) {
            m_Depth3SinceUs = nowUs;
        }
        m_Depth2 = depth >= 2;
        m_Depth3 = depth >= 3;
        if (starved) {
            m_RecoveryLeft = kRecoveryDequeues;
        }
        else if (depth <= 1) {
            m_RecoveryLeft = 0;  // caught up
        }
        else if (m_RecoveryLeft > 0) {
            --m_RecoveryLeft;
        }
    }

private:
    uint32_t m_SlotTicks = 0;
    bool m_LastPtsValid = false;
    uint32_t m_LastPts = 0;
    bool m_Depth2 = false;          // every dequeue since m_Depth2SinceUs had depth >= 2
    bool m_Depth3 = false;
    uint64_t m_Depth2SinceUs = 0;
    uint64_t m_Depth3SinceUs = 0;
    int m_RecoveryLeft = 0;
};

// FIFO queue-depth controller (FIFO profile, stream fps == refresh rate).
//
// With the one-present gate, decode-ready to scanout is c + O + k*T: c is the
// arrival phase (decode-ready to the next Gamescope latch), O the latch offset
// (presentMargin) and k the standing backlog at the gate. Both k = 1 (every
// frame waits for the gate behind its predecessor) and k = 0 (every frame is
// rendered on arrival and makes the very next latch) are stable; which one a
// session sits in depends on its history. k = 0 saves one refresh but needs
// every frame done (submit + GPU) before the latch: slack = c - work > 0.
//
// The controller never sleeps toward a deadline. It
//  - measures per-frame slack: real for frames rendered on arrival; for
//    backlog frames, the frame's phase minus a recent on-arrival work sample;
//  - spends ONE drop to reach depth 0 when the backlog is one frame, the
//    p99.9 slack over >= 2000 frames is above 1.5 ms, and no host gap, stall,
//    repeat or latch-offset jump happened for 10 s; preferably at a gate where
//    the trim drops a host capture extra, so the skip lands next to the host's
//    own hiccup;
//  - returns to depth 1 at the first late frame (a repeat; the queue refills
//    by itself) and allows at most one attempt per 60 s, backing off when an
//    attempt ends within a minute.
// A host gap drains the backlog for free; with good slack that counts as an
// entry to depth 0 without an attempt.
enum class FifoDepthState : uint8_t {
    Off = 0,       // not configured or disabled
    Backlog = 1,   // depth 1: frames wait for the gate
    OnArrival = 2, // depth 0: frames are rendered on arrival
};

struct FifoDepthGate {
    bool backlog = false;   // the head was queued before the gate opened
    int queued = 0;         // frames queued after the trim, head included
    bool hostExtra = false; // the trim dropped a host capture extra at this gate
    uint64_t nowNs = 0;
};

class FifoDepthController {
public:
    static constexpr size_t kSlackWindow = 2048;
    static constexpr size_t kMinSlackSamples = 2000;
    static constexpr size_t kWorkWindow = 2048;
    static constexpr size_t kMinWorkSamples = 64;
    static constexpr uint64_t kSecond = 1000000000ull;
    static constexpr int64_t kRequiredSlackNs = 1500000;
    static constexpr uint64_t kDefaultWorkNs = 6500000;      // until kMinWorkSamples (never drains on it)
    static constexpr uint64_t kQuietNs = 10 * kSecond;
    static constexpr uint64_t kAttemptHoldNs = 60 * kSecond;
    static constexpr uint64_t kMaxAttemptHoldNs = 480 * kSecond;
    static constexpr uint64_t kStableNs = 300 * kSecond;     // a depth-0 run this long resets the backoff
    static constexpr uint64_t kPreferExtraNs = 20 * kSecond; // then drain at any eligible gate
    static constexpr uint64_t kLatchJumpNs = 500000;
    static constexpr size_t kLatchWindow = 256;
    static constexpr int kExitBacklogGates = 4;
    static constexpr uint64_t kPhaseWindow = 256;
    static constexpr uint64_t kEarlyToleranceNs = 1500000;

    void configure(uint64_t periodNs, bool enabled)
    {
        *this = FifoDepthController();
        m_PeriodNs = periodNs;
        m_Enabled = enabled && periodNs != 0;
        m_State = m_Enabled ? FifoDepthState::Backlog : FifoDepthState::Off;
    }

    FifoDepthState state() const { return m_State; }
    bool onArrival() const { return m_State == FifoDepthState::OnArrival; }
    uint64_t periodNs() const { return m_PeriodNs; }

    // One presented frame at feedback. phaseNs: decode-ready to the first latch
    // after it, in [0, T). onArrival: rendered without waiting in the queue,
    // so workNs (decode-ready to submit + GPU) is its real cost. Returns the
    // slack sample (negative: that frame would miss the latch at depth 0).
    int64_t observeFrame(uint64_t phaseNs, bool onArrival, uint64_t workNs)
    {
        if (m_PeriodNs == 0 || phaseNs >= m_PeriodNs) {
            return 0;
        }
        // Arrival delay is one-sided: frames cluster just below an early
        // edge and trail off late (smaller phase). A frame later than its
        // phase wraps past 0 to a large phase and would look safe; unwrap any
        // phase more than kEarlyToleranceNs above the session's circular mean
        // into a negative one (late for its own latch).
        int64_t phase = static_cast<int64_t>(phaseNs);
        {
            constexpr double kTwoPi = 6.283185307179586;
            const double angle = kTwoPi * static_cast<double>(phaseNs) / static_cast<double>(m_PeriodNs);
            const double alpha = m_PhaseCount < kPhaseWindow ? 1.0 / static_cast<double>(m_PhaseCount + 1) :
                                                               1.0 / static_cast<double>(kPhaseWindow);
            m_PhaseCos += alpha * (std::cos(angle) - m_PhaseCos);
            m_PhaseSin += alpha * (std::sin(angle) - m_PhaseSin);
            ++m_PhaseCount;
            if (m_PhaseCount >= kPhaseWindow &&
                m_PhaseCos * m_PhaseCos + m_PhaseSin * m_PhaseSin > 0.25) {
                double centre = std::atan2(m_PhaseSin, m_PhaseCos) / kTwoPi * static_cast<double>(m_PeriodNs);
                if (centre < 0) {
                    centre += static_cast<double>(m_PeriodNs);
                }
                const double top = centre + static_cast<double>(kEarlyToleranceNs);
                if (static_cast<double>(phase) >= top) {
                    phase -= static_cast<int64_t>(m_PeriodNs);
                }
                else if (static_cast<double>(phase) < top - static_cast<double>(m_PeriodNs)) {
                    phase += static_cast<int64_t>(m_PeriodNs);
                }
            }
        }
        uint64_t work = workNs;
        if (onArrival && workNs != 0 && workNs < 2 * m_PeriodNs) {
            m_Work[m_WorkNext] = static_cast<uint32_t>(workNs);
            m_WorkNext = (m_WorkNext + 1) % kWorkWindow;
            m_WorkCount = std::min(m_WorkCount + 1, kWorkWindow);
        }
        else {
            // A backlog frame's own work excludes the decode it finished while
            // queued: pair its phase with a recent on-arrival work sample,
            // cycling so each sample is used about once per slack window.
            if (m_WorkCount >= kMinWorkSamples) {
                m_Pick = (m_Pick + 1) % m_WorkCount;
                work = m_Work[m_Pick];
            }
            else {
                work = kDefaultWorkNs;
            }
        }
        const int64_t slack = phase - static_cast<int64_t>(work);
        m_Slack[m_SlackNext] = static_cast<int32_t>(std::max<int64_t>(std::min<int64_t>(slack, INT32_MAX), INT32_MIN));
        m_SlackNext = (m_SlackNext + 1) % kSlackWindow;
        m_SlackCount = std::min(m_SlackCount + 1, kSlackWindow);
        m_SlackDirty = true;
        return slack;
    }

    // A client-visible repeat: at depth 0 that frame was late and the queue
    // refills by itself, so the stream is back at depth 1.
    void observeRepeat(uint64_t nowNs)
    {
        m_LastRepeatNs = nowNs;
        if (m_State == FifoDepthState::OnArrival) {
            leaveOnArrival(nowNs, true);
        }
    }

    // Host gap, Wi-Fi stall or compositor disturbance.
    void observeDisturbance(uint64_t nowNs) { m_LastDisturbanceNs = nowNs; }

    // Gamescope's presentMargin; a jump to an earlier latch is a disturbance.
    void observeLatchOffset(uint64_t marginNs, uint64_t nowNs)
    {
        if (m_PeriodNs == 0 || marginNs == 0 || marginNs >= m_PeriodNs) {
            return;
        }
        if (m_LatchCount >= 64) {
            if (m_LatchDirty) {
                std::array<uint32_t, kLatchWindow> sorted = m_Latch;
                std::nth_element(sorted.begin(), sorted.begin() + m_LatchCount / 2, sorted.begin() + m_LatchCount);
                m_LatchMedNs = sorted[m_LatchCount / 2];
                m_LatchDirty = false;
            }
            if (marginNs > m_LatchMedNs + kLatchJumpNs) {
                observeDisturbance(nowNs);
            }
        }
        m_Latch[m_LatchNext] = static_cast<uint32_t>(marginNs);
        m_LatchNext = (m_LatchNext + 1) % kLatchWindow;
        m_LatchCount = std::min(m_LatchCount + 1, kLatchWindow);
        if (m_LatchNext % 32 == 0) {
            m_LatchDirty = true;
        }
    }

    // A decoded frame became ready gapNs after its predecessor, ptsTicks later
    // in source time. A late arrival without a source gap is a network stall;
    // it is applied at the next gate.
    void observeArrival(uint64_t gapNs, uint32_t ptsTicks, uint32_t slotTicks)
    {
        if (m_PeriodNs != 0 && slotTicks != 0 && gapNs > m_PeriodNs + m_PeriodNs / 2 &&
            static_cast<uint64_t>(ptsTicks) * 2 < static_cast<uint64_t>(slotTicks) * 3) {
            m_StallPending = true;
        }
    }

    // p99.9 of the slack window (its 0.1th percentile), recomputed lazily.
    int64_t slackLoNs()
    {
        if (m_SlackDirty && m_SlackCount != 0) {
            std::array<int32_t, kSlackWindow> sorted = m_Slack;
            const size_t k = m_SlackCount / 1000;
            std::nth_element(sorted.begin(), sorted.begin() + k, sorted.begin() + m_SlackCount);
            m_SlackLoNs = sorted[k];
            m_SlackDirty = false;
        }
        return m_SlackCount != 0 ? m_SlackLoNs : 0;
    }
    size_t slackSamples() const { return m_SlackCount; }
    size_t workSamples() const { return m_WorkCount; }

    // Needs real on-arrival work samples: backlog frames alone cannot show
    // what rendering on arrival costs (the decode they finished while queued).
    bool slackOk()
    {
        return m_SlackCount >= kMinSlackSamples && m_WorkCount >= kMinWorkSamples &&
               slackLoNs() > kRequiredSlackNs;
    }

    // Every condition for a deliberate drain except the backlog shape.
    bool drainReady(uint64_t nowNs)
    {
        return m_Enabled && slackOk() && quietSince(m_LastDisturbanceNs, nowNs, kQuietNs) &&
               quietSince(m_LastRepeatNs, nowNs, kQuietNs) &&
               quietSince(m_LastAttemptNs, nowNs, m_HoldNs);
    }

    // Called at every present gate after the trim. Returns true when the
    // caller must drop every queued frame (one) and render the next arrival:
    // the one-time drain to depth 0.
    bool gate(const FifoDepthGate& in)
    {
        if (!m_Enabled) {
            return false;
        }
        if (m_StallPending) {
            m_StallPending = false;
            observeDisturbance(in.nowNs);
        }
        if (!in.backlog) {
            m_BacklogRun = 0;
            if (m_State == FifoDepthState::Backlog && slackOk()) {
                // A host gap emptied the queue: depth 0 for free.
                enterOnArrival(in.nowNs, false);
            }
        }
        else if (m_State == FifoDepthState::OnArrival && ++m_BacklogRun >= kExitBacklogGates) {
            leaveOnArrival(in.nowNs, false);
        }
        if (m_State != FifoDepthState::Backlog || !in.backlog || in.queued != 1 ||
            !drainReady(in.nowNs)) {
            m_ReadySinceNs = 0;
            return false;
        }
        if (m_ReadySinceNs == 0) {
            m_ReadySinceNs = in.nowNs;
        }
        if (!in.hostExtra && in.nowNs - m_ReadySinceNs < kPreferExtraNs) {
            return false;
        }
        m_LastAttemptNs = in.nowNs;
        ++m_Attempts;
        enterOnArrival(in.nowNs, true);
        return true;
    }

    uint32_t attempts() const { return m_Attempts; }
    uint32_t freeEntries() const { return m_FreeEntries; }
    uint32_t lateExits() const { return m_LateExits; }
    uint32_t backlogExits() const { return m_BacklogExits; }
    uint64_t holdNs() const { return m_HoldNs; }

private:
    static bool quietSince(uint64_t eventNs, uint64_t nowNs, uint64_t quietNs)
    {
        return eventNs == 0 || (nowNs >= eventNs && nowNs - eventNs >= quietNs);
    }

    void enterOnArrival(uint64_t nowNs, bool attempt)
    {
        m_State = FifoDepthState::OnArrival;
        m_OnArrivalSinceNs = nowNs;
        m_FromAttempt = attempt;
        m_BacklogRun = 0;
        m_ReadySinceNs = 0;
        if (!attempt) {
            ++m_FreeEntries;
        }
    }

    void leaveOnArrival(uint64_t nowNs, bool late)
    {
        const uint64_t lasted = nowNs >= m_OnArrivalSinceNs ? nowNs - m_OnArrivalSinceNs : 0;
        if (m_FromAttempt && lasted < kAttemptHoldNs) {
            m_HoldNs = std::min(m_HoldNs * 2, kMaxAttemptHoldNs);
        }
        else if (lasted >= kStableNs) {
            m_HoldNs = kAttemptHoldNs;
        }
        m_State = FifoDepthState::Backlog;
        m_BacklogRun = 0;
        if (late) {
            ++m_LateExits;
        }
        else {
            ++m_BacklogExits;
        }
    }

    bool m_Enabled = false;
    FifoDepthState m_State = FifoDepthState::Off;
    uint64_t m_PeriodNs = 0;
    std::array<int32_t, kSlackWindow> m_Slack{};
    size_t m_SlackNext = 0;
    size_t m_SlackCount = 0;
    bool m_SlackDirty = false;
    int64_t m_SlackLoNs = 0;
    double m_PhaseCos = 0;
    double m_PhaseSin = 0;
    uint64_t m_PhaseCount = 0;
    std::array<uint32_t, kWorkWindow> m_Work{};
    size_t m_WorkNext = 0;
    size_t m_WorkCount = 0;
    size_t m_Pick = 0;
    std::array<uint32_t, kLatchWindow> m_Latch{};
    size_t m_LatchNext = 0;
    size_t m_LatchCount = 0;
    bool m_LatchDirty = true;
    uint64_t m_LatchMedNs = 0;
    bool m_StallPending = false;
    uint64_t m_LastDisturbanceNs = 0;
    uint64_t m_LastRepeatNs = 0;
    uint64_t m_LastAttemptNs = 0;
    uint64_t m_HoldNs = kAttemptHoldNs;
    uint64_t m_ReadySinceNs = 0;
    uint64_t m_OnArrivalSinceNs = 0;
    bool m_FromAttempt = false;
    int m_BacklogRun = 0;
    uint32_t m_Attempts = 0;
    uint32_t m_FreeEntries = 0;
    uint32_t m_LateExits = 0;
    uint32_t m_BacklogExits = 0;
};
