#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Maps the host's wrapping 90 kHz RTP clock to local decode-ready time.
// No host/client clock synchronization is assumed. Only relative age is known.
class SourceTimeline {
public:
    enum class Observation { Accepted, Duplicate, Reset };

    Observation observe(uint32_t pts, uint64_t readyUs) {
        const uint32_t delta = pts - m_LastPts;
        if (!m_Valid || readyUs < m_LastReadyUs ||
            readyUs - m_LastReadyUs > 1000000 || delta > 90000) {
            m_Valid = true;
            m_LastPts = pts;
            m_SourceTicks = 0;
            m_OffsetUs = static_cast<int64_t>(readyUs);
            m_LastReadyUs = m_WindowStartUs = m_EpochStartUs = readyUs;
            m_MinOffsetUs = m_OffsetUs;
            return Observation::Reset;
        }
        if (delta == 0) {
            return Observation::Duplicate;
        }
        m_SourceTicks += delta;
        m_LastPts = pts;
        m_LastReadyUs = readyUs;
        const int64_t offset = static_cast<int64_t>(readyUs) - ticksToUs(m_SourceTicks);
        if (offset - m_OffsetUs > 250000 || offset - m_OffsetUs < -250000) {
            m_Valid = false;
            return observe(pts, readyUs);
        }
        m_MinOffsetUs = std::min(m_MinOffsetUs, offset);
        if (readyUs - m_EpochStartUs < 250000) {
            // Remove startup decoder warm-up delay before settling the clock.
            m_OffsetUs = std::min(m_OffsetUs, offset);
        }
        if (readyUs - m_WindowStartUs >= 1000000) {
            // Follow oscillator drift, not individual packet/decode spikes.
            const int64_t limit = static_cast<int64_t>((readyUs - m_WindowStartUs) / 2000);
            m_OffsetUs += std::clamp(m_MinOffsetUs - m_OffsetUs, -limit, limit);
            m_MinOffsetUs = offset;
            m_WindowStartUs = readyUs;
        }
        return Observation::Accepted;
    }

    int64_t dueUs(uint32_t pts, uint64_t reserveUs) const {
        const int64_t ticks = m_SourceTicks - static_cast<uint32_t>(m_LastPts - pts);
        return m_OffsetUs + ticksToUs(ticks) + static_cast<int64_t>(reserveUs);
    }

    int newestDue(const uint32_t* pts, size_t count, uint64_t presentUs,
                  uint64_t reserveUs) const {
        int candidate = -1;
        for (size_t i = 0; i < count; ++i) {
            if (dueUs(pts[i], reserveUs) <= static_cast<int64_t>(presentUs)) {
                candidate = static_cast<int>(i);
            }
        }
        return candidate;
    }

private:
    static int64_t ticksToUs(int64_t ticks) { return ticks * 100 / 9; }
    bool m_Valid = false;
    uint32_t m_LastPts = 0;
    int64_t m_SourceTicks = 0;
    int64_t m_OffsetUs = 0;
    int64_t m_MinOffsetUs = 0;
    uint64_t m_LastReadyUs = 0;
    uint64_t m_WindowStartUs = 0;
    uint64_t m_EpochStartUs = 0;
};
