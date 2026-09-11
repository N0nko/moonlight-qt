#include "../app/streaming/audio/adaptivebuffer.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <thread>

int main()
{
    {
        AdaptiveAudioBuffer b;
        assert(!b.configure(0, 2, 240, 480));
        assert(!b.configure(48000, 9, 240, 480));
        assert(!b.configure(48000, 2, 0, 480));
        assert(!b.configure(48000, 2, 240, 48000));
    }
    for (uint32_t channels : {1, 2, 6, 8}) {
        AdaptiveAudioBuffer b;
        assert(b.configure(48000, channels, 240, 480));
        std::vector<float> packet(240 * channels, 0.5f), output(480 * channels, -1);
        b.render(output.data(), 480);
        for (float v : output) assert(v == 0);
        assert(b.stats().underruns == 0); // startup silence is not a fault
        for (int n = 0; n < 3; ++n) assert(b.push(packet.data(), 240));
        b.render(output.data(), 480);
        assert(output.back() == 0.5f);
        // 10 minutes of steady playback: no depth growth, trims, or underruns.
        for (int n = 0; n < 60000; ++n) {
            assert(b.push(packet.data(), 240));
            assert(b.push(packet.data(), 240));
            b.render(output.data(), 480);
        }
        assert(b.stats().underruns == 0 && b.stats().trims == 0 && b.stats().targetMs == 15);
        b.reset();
        b.render(output.data(), 480);
        for (float v : output) assert(v == 0);
        assert(b.stats().resets == 1);
    }
    {
        AdaptiveAudioBuffer b;
        assert(b.configure(48000, 2, 240, 480));
        std::vector<float> packet(480, 1), output(960);
        for (int n = 0; n < 40; ++n) assert(b.push(packet.data(), 240));
        assert(!b.push(packet.data(), 240));
        b.render(output.data(), 480);
        assert(b.stats().trims == 1 && b.stats().overflows == 1);
        for (float v : output) assert(std::isfinite(v) && v >= 0 && v <= 1);
        // Repeated starvation increases the target; silence alone cannot grow it.
        for (int cycle = 0; cycle < 10; ++cycle) {
            for (int n = 0; n < 12; ++n) b.push(packet.data(), 240);
            for (int n = 0; n < 8; ++n) b.render(output.data(), 480);
        }
        assert(b.stats().underruns >= 3 && b.stats().targetMs > 15 && b.stats().targetMs <= 60);
        b.reset(); b.render(output.data(), 480);
        assert(b.stats().targetMs == 15);
    }
    {
        // Independent clocks: +500 ppm producer drift, one hour, bounded depth.
        AdaptiveAudioBuffer b;
        assert(b.configure(48000, 2, 240, 480));
        std::vector<float> packet(962, 0.25f), output(960);
        assert(b.push(packet.data(), 480));
        double carry = 0;
        for (int n = 0; n < 360000; ++n) {
            carry += 0.24;
            const auto extra = static_cast<uint32_t>(carry);
            carry -= extra;
            assert(b.push(packet.data(), 480 + extra));
            b.render(output.data(), 480);
            assert(b.stats().depthMs < 60);
        }
        assert(b.stats().underruns == 0 && b.stats().trims > 0);
    }
    {
        // Real concurrent producer/callback, including ring wrap and resets.
        AdaptiveAudioBuffer b;
        assert(b.configure(48000, 2, 240, 480));
        std::atomic<bool> done {false};
        std::thread producer([&] {
            std::vector<float> packet(480, 0.75f);
            for (int n = 0; n < 100000; ++n) {
                b.push(packet.data(), 240);
                if (n % 4096 == 0) b.reset();
            }
            done.store(true);
        });
        std::vector<float> output(960);
        do {
            b.render(output.data(), 480);
            for (float v : output) assert(std::isfinite(v) && v >= 0 && v <= 0.75f);
        } while (!done.load());
        producer.join();
    }
    std::cout << "adaptive audio: format, priming, steady state, overflow, recovery, drift and SPSC tests passed\n";
}
