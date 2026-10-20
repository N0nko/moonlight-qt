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

// A shallow FIFO is appropriate for steady full-rate delivery. Keep slack for
// lower-rate/bursty sources; require two seconds of stable cadence to remove it.
// Observed on the decoder thread before selection drops, using the host's
// wrapping 90 kHz RTP timestamps. Publish only the decision to the render thread.
class PresentationCadenceBudget {
public:
    int observe(uint32_t pts, uint64_t readyUs, int fps)
    {
        const uint32_t delta = pts - m_LastPts;
        if (fps < 10 || fps > 1000) {
            *this = {};
            return 2;
        }
        if (!m_Valid || fps != m_Fps || readyUs < m_LastReadyUs ||
            readyUs - m_LastReadyUs > 1000000 || delta > 90000) {
            *this = {};
            m_Valid = true;
            m_Fps = fps;
        }
        else {
            const uint32_t period = 90000 / fps;
            const bool regular = delta >= period * 3 / 4 && delta <= period * 5 / 4;
            m_StableFrames = regular ? std::min(m_StableFrames + 1, fps * 2) : 0;
            m_IrregularFrames += !regular;
            if (m_IrregularFrames >= 3) {
                m_Budget = 2;
            }
            if (m_StableFrames >= fps * 2) {
                m_Budget = 1;
            }
            if (++m_WindowFrames >= fps) {
                m_WindowFrames = m_IrregularFrames = 0;
            }
        }
        m_LastPts = pts;
        m_LastReadyUs = readyUs;
        return m_Budget;
    }

private:
    bool m_Valid = false;
    int m_Fps = 0;
    int m_Budget = 2;
    int m_StableFrames = 0;
    int m_WindowFrames = 0;
    int m_IrregularFrames = 0;
    uint32_t m_LastPts = 0;
    uint64_t m_LastReadyUs = 0;
};

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
