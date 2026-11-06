#pragma once

#include <algorithm>
#include <array>
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
// Never more than one drop per dequeue, and never the last queued frame.
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
            if (in.depth >= 2) {
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
