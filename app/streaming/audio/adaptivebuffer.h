#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// One decoder producer, one device callback consumer. Only configure() allocates.
// Depths/counts are channel frames, not interleaved samples. No video clock is delayed.
class AdaptiveAudioBuffer
{
public:
    struct Stats {
        uint32_t depthMs, targetMs, underruns, trims, overflows, resets;
    };

    bool configure(uint32_t rate, uint32_t channels, uint32_t packet, uint32_t quantum)
    {
        if (rate < 8000 || rate > 192000 || channels == 0 || channels > 8 ||
                packet == 0 || packet > rate / 20 || quantum == 0 || quantum > rate / 20) {
            return false;
        }
        m_Rate = rate;
        m_Channels = channels;
        m_Packet = packet;
        m_Capacity = rate / 5;
        m_Base = std::max(rate * 15 / 1000, quantum + packet);
        m_Target = m_Base;
        m_Ring.assign(static_cast<size_t>(m_Capacity) * channels, 0.0f);
        m_TargetMs.store(m_Target * 1000 / rate, std::memory_order_relaxed);
        return true;
    }

    // Producer: drop the new packet only if the bounded ring is full. The consumer
    // sheds old data on its next callback; neither thread waits for the other.
    bool push(const float* data, uint32_t frames)
    {
        if (frames == 0) return true;
        const auto write = m_Write.load(std::memory_order_relaxed);
        const auto read = m_Read.load(std::memory_order_acquire);
        if (frames > m_Capacity || write - read > m_Capacity - frames) {
            m_Overflows.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto first = std::min<uint32_t>(frames, m_Capacity - write % m_Capacity);
        std::memcpy(&m_Ring[(write % m_Capacity) * m_Channels], data,
                    static_cast<size_t>(first) * m_Channels * sizeof(float));
        std::memcpy(m_Ring.data(), data + first * m_Channels,
                    static_cast<size_t>(frames - first) * m_Channels * sizeof(float));
        m_Write.store(write + frames, std::memory_order_release);
        return true;
    }

    // Producer: a discontinuity (resume/network gap) invalidates queued old audio.
    void reset() { m_Reset.store(true, std::memory_order_release); }

    // Consumer: SDL calls this with exactly the device's requested quantum.
    // No allocation, mutex, logging, sleeping, or device calls in this method.
    void render(float* output, uint32_t frames)
    {
        auto read = m_Read.load(std::memory_order_relaxed);
        const auto write = m_Write.load(std::memory_order_acquire);
        auto depth = static_cast<uint32_t>(write - read);
        if (m_Reset.exchange(false, std::memory_order_acq_rel)) {
            read = write;
            depth = 0;
            m_Primed = false;
            m_Target = m_Base;
            m_Average = 0;
            m_ExcessFrames = m_QuietFrames = m_WindowFrames = m_WindowUnderruns = 0;
            m_Last.fill(0);
            m_Resets.fetch_add(1, std::memory_order_relaxed);
        }
        const auto floor = std::min(m_Capacity, std::max(m_Base, frames + m_Packet));
        m_Target = std::max(m_Target, floor);
        m_TargetMs.store(m_Target * 1000 / m_Rate, std::memory_order_relaxed);
        m_DepthMs.store(depth * 1000 / m_Rate, std::memory_order_relaxed);

        if (!m_Primed && depth >= m_Target) {
            m_Primed = true;
            m_Average = depth;
            startFade();
        }
        if (m_Primed) {
            const auto alpha = std::min(1.0, static_cast<double>(frames) / m_Rate);
            m_Average += (depth - m_Average) * alpha;
            m_ExcessFrames = m_Average > m_Target + m_Rate / 100 ?
                        m_ExcessFrames + frames : 0;
            uint32_t drop = 0;
            if (depth > m_Target + m_Rate / 25) {
                drop = depth - m_Target;
            } else if (m_ExcessFrames >= m_Rate && depth >= frames + m_Packet) {
                drop = m_Packet;
            }
            if (drop != 0) {
                read += drop;
                depth -= drop;
                m_Average = depth;
                m_ExcessFrames = 0;
                startFade();
                m_Trims.fetch_add(1, std::memory_order_relaxed);
            }

            m_WindowFrames += frames;
            if (m_WindowFrames >= m_Rate * 5) {
                m_WindowFrames = m_WindowUnderruns = 0;
            }
            if (depth < frames) {
                m_Underruns.fetch_add(1, std::memory_order_relaxed);
                m_QuietFrames = 0;
                if (++m_WindowUnderruns >= 3) {
                    m_Target = std::min(std::max(floor, m_Rate * 60 / 1000),
                                        m_Target + m_Rate / 100);
                    m_WindowUnderruns = 0;
                }
            } else {
                m_QuietFrames += frames;
                if (m_QuietFrames >= m_Rate * 30) {
                    m_Target = std::max(floor, m_Target - std::min(m_Target, m_Rate / 200));
                    m_QuietFrames = 0;
                }
            }
        }

        const auto take = m_Primed ? std::min(frames, depth) : 0;
        for (uint32_t i = 0; i < take; ++i) {
            const float weight = m_Fade ? 1.0f - static_cast<float>(m_Fade) / m_FadeLength : 1.0f;
            for (uint32_t ch = 0; ch < m_Channels; ++ch) {
                const float value = m_Ring[((read + i) % m_Capacity) * m_Channels + ch];
                output[i * m_Channels + ch] = m_Last[ch] =
                        value * weight + m_FadeFrom[ch] * (1.0f - weight);
            }
            if (m_Fade != 0) --m_Fade;
        }
        if (take < frames) {
            // Fade to silence without consuming samples while re-priming.
            const auto fade = std::min(frames - take, std::max(1u, m_Rate / 500));
            for (uint32_t i = take; i < frames; ++i) {
                const float weight = 1.0f - static_cast<float>(std::min(i - take + 1, fade)) / fade;
                for (uint32_t ch = 0; ch < m_Channels; ++ch)
                    output[i * m_Channels + ch] = m_Last[ch] * weight;
            }
            m_Last.fill(0);
            m_Primed = false;
        }
        m_Read.store(read + take, std::memory_order_release);
    }

    Stats stats() const
    {
        return {m_DepthMs.load(std::memory_order_relaxed), m_TargetMs.load(std::memory_order_relaxed),
                m_Underruns.load(std::memory_order_relaxed), m_Trims.load(std::memory_order_relaxed),
                m_Overflows.load(std::memory_order_relaxed), m_Resets.load(std::memory_order_relaxed)};
    }

private:
    void startFade()
    {
        m_FadeFrom = m_Last;
        m_Fade = m_FadeLength = std::max(1u, m_Rate / 500);
    }

    std::vector<float> m_Ring;
    std::atomic<uint64_t> m_Read {0}, m_Write {0};
    std::atomic<bool> m_Reset {false};
    std::atomic<uint32_t> m_DepthMs {0}, m_TargetMs {0}, m_Underruns {0},
                          m_Trims {0}, m_Overflows {0}, m_Resets {0};
    uint32_t m_Rate = 0, m_Channels = 0, m_Packet = 0, m_Capacity = 0;
    uint32_t m_Base = 0, m_Target = 0, m_Fade = 0, m_FadeLength = 1;
    uint64_t m_ExcessFrames = 0, m_QuietFrames = 0, m_WindowFrames = 0;
    uint32_t m_WindowUnderruns = 0;
    double m_Average = 0;
    bool m_Primed = false;
    std::array<float, 8> m_Last {}, m_FadeFrom {};
};
