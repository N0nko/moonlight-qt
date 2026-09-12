#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

inline int latestFramesToDrop(int depth, bool enabled)
{
    return enabled && depth > 1 ? depth - 1 : 0;
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
