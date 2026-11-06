#pragma once

// FIFO deadline wake (Linux FIFO pacing profile only). Pure policy code, no
// Vulkan/Qt/SDL dependencies, so it is unit tested in tests/fifodepth_test.cpp.
//
// Gamescope signals present-wait at its latch, presentMargin before the
// reported vblank, so a frame queued behind the previous present waits one
// whole refresh. This policy learns the arrival phase relative to the vblank
// grid and decides when a standing one-frame backlog (depth 1, which is the
// existing behaviour) may be spent with a single drop to reach depth 0 (each
// frame on the first vblank it can safely make). It never reorders frames.
//
// All times are CLOCK_MONOTONIC nanoseconds. "toVblank" is the distance from a
// frame's decode-ready time to the first grid vblank after it, in (0, T].

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <climits>

// Source-cadence check: one new source frame per refresh (within 10%).
// False for 45 fps on 90 Hz, 90 fps on 60 Hz, etc.
inline bool fifoOneToOneDelta(uint32_t ptsDelta90k, uint64_t periodNs)
{
    if (periodNs == 0 || ptsDelta90k == 0) {
        return false;
    }
    const uint64_t deltaNs = static_cast<uint64_t>(ptsDelta90k) * 1000000000ull / 90000ull;
    return deltaNs * 10 >= periodNs * 9 && deltaNs * 10 <= periodNs * 11;
}

// Refreshes the source cadence expects between two frames (rounded, >= 1).
inline uint32_t fifoSourceRefreshes(uint32_t ptsDelta90k, uint64_t periodNs)
{
    if (periodNs == 0) {
        return 1;
    }
    const uint64_t deltaNs = static_cast<uint64_t>(ptsDelta90k) * 1000000000ull / 90000ull;
    const uint64_t refreshes = (deltaNs + periodNs / 2) / periodNs;
    return refreshes == 0 ? 1 : static_cast<uint32_t>(std::min<uint64_t>(refreshes, 1000));
}

// Refreshes between two reported vblanks (rounded).
inline uint32_t fifoDisplayRefreshes(uint64_t previousNs, uint64_t currentNs, uint64_t periodNs)
{
    if (periodNs == 0 || currentNs <= previousNs) {
        return 0;
    }
    return static_cast<uint32_t>(std::min<uint64_t>((currentNs - previousNs + periodNs / 2) / periodNs, 1000));
}

// Distance from t to the first vblank strictly after it on the grid
// anchor + n * period. Result is in (0, period].
inline uint64_t fifoToNextVblank(uint64_t t, uint64_t anchor, uint64_t period)
{
    if (period == 0) {
        return 0;
    }
    if (t >= anchor) {
        return period - (t - anchor) % period;
    }
    const uint64_t before = (anchor - t) % period;
    return before == 0 ? period : before;
}

class FifoDepthPolicy {
public:
    static constexpr size_t kArrivalWindow = 2048; // ~23 s at 90 Hz
    static constexpr size_t kRenderWindow = 2048;
    static constexpr size_t kLatchWindow = 4096;   // ~45 s at 90 Hz
    static constexpr size_t kCadenceWindow = 64;
    static constexpr size_t kCadenceRequired = 58;
    static constexpr size_t kMinArrivals = kArrivalWindow; // >= 2000 frames before arming
    static constexpr uint64_t kSecond = 1000000000ull;
    static constexpr uint64_t kBaseHoldNs = 10 * kSecond;  // >= 10 s between drains
    static constexpr uint64_t kMaxHoldNs = 160 * kSecond;
    static constexpr uint64_t kQuietNs = 5 * kSecond;      // >= 5 s without misses
    static constexpr uint64_t kDisturbedNs = 10 * kSecond; // after host gaps / latch jumps
    static constexpr uint64_t kLatchJumpNs = 500000;
    static constexpr uint64_t kEvalNs = kSecond;

    void reset(uint64_t periodNs)
    {
        const uint64_t margin = m_MarginNs;
        *this = FifoDepthPolicy();
        m_MarginNs = margin;
        m_PeriodNs = periodNs;
    }

    void setMarginNs(uint64_t marginNs) { m_MarginNs = marginNs; }
    uint64_t periodNs() const { return m_PeriodNs; }

    void observeArrival(uint64_t toVblankNs)
    {
        if (m_PeriodNs == 0 || toVblankNs == 0 || toVblankNs > m_PeriodNs) {
            return;
        }
        m_Arrivals[m_ArrivalNext] = static_cast<uint32_t>(toVblankNs);
        m_ArrivalNext = (m_ArrivalNext + 1) % kArrivalWindow;
        m_ArrivalCount = std::min(m_ArrivalCount + 1, kArrivalWindow);
    }

    // Wake-to-submit CPU time plus GPU execution time of one frame.
    void observeRender(uint64_t renderNs)
    {
        m_Render[m_RenderNext] = static_cast<uint32_t>(std::min<uint64_t>(renderNs, 0xffffffffu));
        m_RenderNext = (m_RenderNext + 1) % kRenderWindow;
        m_RenderCount = std::min(m_RenderCount + 1, kRenderWindow);
    }

    // Gamescope's presentMargin: target vblank minus the latch that took the commit.
    // A jump above the recent maximum (overlay, draw-time sawtooth) blocks drains.
    void observeLatchOffset(uint64_t marginNs, uint64_t nowNs)
    {
        if (marginNs == 0 || m_PeriodNs == 0 || marginNs >= m_PeriodNs) {
            return;
        }
        if (m_LatchHiNs != 0 && marginNs > m_LatchHiNs + kLatchJumpNs) {
            observeDisturbance(nowNs);
        }
        m_Latch[m_LatchNext] = static_cast<uint32_t>(marginNs);
        m_LatchNext = (m_LatchNext + 1) % kLatchWindow;
        m_LatchCount = std::min(m_LatchCount + 1, kLatchWindow);
    }

    void observeCadence(bool oneToOne)
    {
        if (m_CadenceCount == kCadenceWindow && m_Cadence[m_CadenceNext]) {
            --m_CadenceOk;
        }
        if (oneToOne) {
            ++m_CadenceOk;
        }
        m_Cadence[m_CadenceNext] = oneToOne;
        m_CadenceNext = (m_CadenceNext + 1) % kCadenceWindow;
        m_CadenceCount = std::min(m_CadenceCount + 1, kCadenceWindow);
    }

    bool cadenceOneToOne() const
    {
        return m_CadenceCount == kCadenceWindow && m_CadenceOk >= kCadenceRequired;
    }

    // A client-visible repeat or an aborted drain: stay up, back off drains.
    void observeMiss(uint64_t nowNs)
    {
        if (m_LastDrainNs != 0 && nowNs >= m_LastDrainNs && nowNs - m_LastDrainNs < 30 * kSecond) {
            m_HoldNs = std::min(m_HoldNs * 2, kMaxHoldNs);
        }
        m_LastMissNs = nowNs;
    }

    void noteDrain(uint64_t nowNs) { m_LastDrainNs = nowNs; }

    // Host-side source gap or compositor disturbance: no drain for 10 s.
    void observeDisturbance(uint64_t nowNs) { m_LastDisturbanceNs = nowNs; }

    // Recompute statistics at most once per second.
    void evaluate(uint64_t nowNs)
    {
        if (m_LastEvalNs != 0 && nowNs >= m_LastEvalNs && nowNs - m_LastEvalNs < kEvalNs) {
            return;
        }
        m_LastEvalNs = nowNs;
        if (m_LastMissNs != 0 && nowNs >= m_LastMissNs && nowNs - m_LastMissNs > 120 * kSecond) {
            m_HoldNs = kBaseHoldNs;
        }

        m_Armed = false;
        m_Depth = 1;
        if (m_PeriodNs == 0 || m_ArrivalCount < kMinArrivals || m_LatchCount == 0 ||
            m_RenderCount == 0 || !cadenceOneToOne()) {
            m_ArmedSinceNs = 0;
            return;
        }

        m_LatchHiNs = *std::max_element(m_Latch.begin(), m_Latch.begin() + m_LatchCount);
        m_RenderHiNs = *std::max_element(m_Render.begin(), m_Render.begin() + m_RenderCount);
        m_LeadNs = m_LatchHiNs + m_RenderHiNs + m_MarginNs;
        if (m_LeadNs >= m_PeriodNs - m_PeriodNs / 10) {
            m_ArmedSinceNs = 0;
            return;
        }

        // Slack to the first feasible latch, s in [0, T), for every arrival.
        std::array<uint32_t, kArrivalWindow> s{};
        for (size_t i = 0; i < m_ArrivalCount; ++i) {
            s[i] = static_cast<uint32_t>(rawSlack(m_Arrivals[i]));
        }
        std::sort(s.begin(), s.begin() + m_ArrivalCount);

        // Find the arrival cluster's median: cut the circle at its largest gap.
        uint64_t bestGap = s[0] + m_PeriodNs - s[m_ArrivalCount - 1];
        size_t start = 0;
        for (size_t i = 0; i + 1 < m_ArrivalCount; ++i) {
            const uint64_t gap = s[i + 1] - s[i];
            if (gap > bestGap) {
                bestGap = gap;
                start = i + 1;
            }
        }
        const size_t medianIndex = (start + m_ArrivalCount / 2) % m_ArrivalCount;
        m_MedNs = s[medianIndex];

        // Each frame belongs to the slot nearest the median (within T/2): only
        // late frames are at risk, early ones simply wait behind the gate.
        int64_t lo = INT64_MAX;
        int64_t hi = INT64_MIN;
        for (size_t i = 0; i < m_ArrivalCount; ++i) {
            const int64_t u = unwrap(s[i]);
            lo = std::min(lo, u);
            hi = std::max(hi, u);
        }
        m_SlackLoNs = lo;
        m_SlackMedNs = static_cast<int64_t>(m_MedNs);
        m_SpreadNs = static_cast<uint64_t>(hi - lo);
        if (m_SpreadNs * 2 >= m_PeriodNs) {
            m_ArmedSinceNs = 0;
            return; // Too jittery to assign frames to slots.
        }

        m_Armed = true;
        if (m_ArmedSinceNs == 0) {
            m_ArmedSinceNs = nowNs;
        }
        // The worst arrival in the window (stricter than p99.9) must still make it.
        m_Depth = m_SlackLoNs >= 0 ? 0 : 1;
    }

    bool armed() const { return m_Armed; }
    int depth() const { return m_Depth; }
    uint64_t leadNs() const { return m_LeadNs; }
    uint64_t latchHiNs() const { return m_LatchHiNs; }
    uint64_t renderHiNs() const { return m_RenderHiNs; }
    uint64_t marginNs() const { return m_MarginNs; }
    int64_t slackLoNs() const { return m_SlackLoNs; }
    int64_t slackMedNs() const { return m_SlackMedNs; }
    uint64_t spreadNs() const { return m_SpreadNs; }
    uint64_t holdNs() const { return m_HoldNs; }
    size_t arrivals() const { return m_ArrivalCount; }

    // Vblank index, relative to the first vblank after arrival (index 0), that
    // the arrival cluster assigns to this frame at depth 0. A late frame maps
    // to a vblank it can no longer make; frameSlackNs() is then negative.
    int nominalVblank(uint64_t toVblankNs) const
    {
        if (!m_Armed) {
            return 0;
        }
        const int64_t x = static_cast<int64_t>(toVblankNs) - static_cast<int64_t>(m_LeadNs);
        const int first = x >= 0 ? 0 : 1;
        const uint64_t raw = rawSlack(toVblankNs);
        const int64_t u = unwrap(raw);
        const int shift = u < 0 ? -1 : (u >= static_cast<int64_t>(m_PeriodNs) ? 1 : 0);
        return first + shift;
    }

    int64_t frameSlackNs(uint64_t toVblankNs) const { return unwrap(rawSlack(toVblankNs)); }

    bool drainAllowed(uint64_t nowNs) const
    {
        return m_Armed && m_Depth == 0 &&
               m_ArmedSinceNs != 0 && nowNs >= m_ArmedSinceNs && nowNs - m_ArmedSinceNs >= kQuietNs &&
               (m_LastMissNs == 0 || (nowNs >= m_LastMissNs && nowNs - m_LastMissNs >= kQuietNs)) &&
               (m_LastDisturbanceNs == 0 ||
                (nowNs >= m_LastDisturbanceNs && nowNs - m_LastDisturbanceNs >= kDisturbedNs)) &&
               (m_LastDrainNs == 0 || (nowNs >= m_LastDrainNs && nowNs - m_LastDrainNs >= m_HoldNs));
    }

private:
    // Slack in [0, T) moved by +/-T to lie within T/2 of the cluster median.
    int64_t unwrap(uint64_t raw) const
    {
        const int64_t t = static_cast<int64_t>(m_PeriodNs);
        const int64_t med = static_cast<int64_t>(m_MedNs);
        int64_t u = static_cast<int64_t>(raw);
        if (u < med - t / 2) {
            u += t;
        }
        else if (u >= med + t / 2) {
            u -= t;
        }
        return u;
    }

    uint64_t rawSlack(uint64_t toVblankNs) const
    {
        const int64_t x = static_cast<int64_t>(toVblankNs) - static_cast<int64_t>(m_LeadNs);
        return static_cast<uint64_t>(x >= 0 ? x : x + static_cast<int64_t>(m_PeriodNs));
    }

    uint64_t m_PeriodNs = 0;
    uint64_t m_MarginNs = 2000000;
    std::array<uint32_t, kArrivalWindow> m_Arrivals{};
    size_t m_ArrivalNext = 0;
    size_t m_ArrivalCount = 0;
    std::array<uint32_t, kRenderWindow> m_Render{};
    size_t m_RenderNext = 0;
    size_t m_RenderCount = 0;
    std::array<uint32_t, kLatchWindow> m_Latch{};
    size_t m_LatchNext = 0;
    size_t m_LatchCount = 0;
    std::array<bool, kCadenceWindow> m_Cadence{};
    size_t m_CadenceNext = 0;
    size_t m_CadenceCount = 0;
    size_t m_CadenceOk = 0;

    bool m_Armed = false;
    int m_Depth = 1;
    uint64_t m_LeadNs = 0;
    uint64_t m_LatchHiNs = 0;
    uint64_t m_RenderHiNs = 0;
    uint64_t m_MedNs = 0;
    int64_t m_SlackLoNs = 0;
    int64_t m_SlackMedNs = 0;
    uint64_t m_SpreadNs = 0;
    uint64_t m_LastEvalNs = 0;
    uint64_t m_ArmedSinceNs = 0;
    uint64_t m_LastMissNs = 0;
    uint64_t m_LastDrainNs = 0;
    uint64_t m_LastDisturbanceNs = 0;
    uint64_t m_HoldNs = kBaseHoldNs;
};
