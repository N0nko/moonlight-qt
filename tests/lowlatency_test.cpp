#include "../app/streaming/video/ffmpeg-renderers/pacer/lowlatencypolicy.h"

#include <cassert>
#include <deque>
#include <iostream>
#include <limits>

int main()
{
    for (bool enabled : {false, true}) {
        for (int depth = 0; depth <= 3; ++depth) {
            std::deque<int> queue;
            for (int i = 0; i < depth; ++i) queue.push_back(i);
            const int drops = latestFramesToDrop(depth, enabled);
            for (int i = 0; i < drops; ++i) queue.pop_front();
            assert(queue.size() == static_cast<size_t>(depth - drops));
            if (depth > 0) assert(queue.front() == (enabled ? depth - 1 : 0));
        }
    }
    assert(latestFramesToDrop(-1, true) == 0);
    assert(decodeReadyMonotonicNs(900, 1000, 9000000, 1020) == 8890000);
    assert(decodeReadyMonotonicNs(900, 1000, 9000000, 1300) == 0);
    assert(decodeReadyMonotonicNs(900, 1000, 9000000, 999) == 0);
    assert(decodeReadyMonotonicNs(-1, 1000, 9000000, 1000) == 0);
    assert(decodeReadyMonotonicNs(1100, 1000, 9000000, 1000) == 0);
    assert(decodeReadyMonotonicNs(1, 6000000, 9000000000, 6000000) == 0);
    assert(decodeReadyMonotonicNs(1, 1000, 100, 1000) == 0);

    PresentationHistory history;
    history.record(1, 1000000, 2000000);
    auto sample = history.take(1, 3000000);
    assert(sample.id == 1 && sample.readyNs == 1000000 && sample.submitNs == 2000000);
    assert(history.take(1, 3000000).id == 0); // Never count feedback twice.
    history.record(1, 1000000, 2000000);
    history.record(257, 2000000, 3000000);
    assert(history.take(1, 4000000).id == 0);
    assert(history.take(257, 4000000).id == 257);
    history.record(2, 0, 100);
    assert(history.take(2, 200).id == 0);
    history.record(2, 200, 100);
    assert(history.take(2, 300).id == 0);
    history.record(2, 100, 200);
    assert(history.take(2, 199).id == 0);
    history.record(2, 100, 200);
    assert(history.take(2, 6000000000).id == 0);
    const uint32_t lastId = std::numeric_limits<uint32_t>::max();
    history.record(lastId, 100, 200);
    assert(history.take(lastId, 300).id == lastId);
    history.record(3, 100, 200);
    history.clear();
    assert(history.take(3, 300).id == 0);

    AdaptivePresentLead lead;
    constexpr uint64_t period = 11111111, baseline = period / 8;
    uint64_t t = 1000000000;
    assert(lead.lead(t, period, baseline) == baseline);
    for (int i = 0; i < 32; ++i) {
        lead.observe(t, t + 4000000, t + 4000000, 3750000, period, baseline);
        t += period;
    }
    assert(lead.lead(t, period, baseline) == baseline - 125000);
    for (int i = 0; i < 32 * 20; ++i) {
        lead.observe(t, t + 4000000, t + 4000000, 3750000, period, baseline);
        t += period;
    }
    assert(lead.lead(t, period, baseline) == 500000);
    assert(lead.lead(t + 1000000000, period, baseline) == baseline);
    assert(lead.lead(t, period * 2, baseline) == baseline);
    lead.reset();
    for (int i = 0; i < 32 * 20; ++i) {
        lead.observe(t, t + 6000000, t + 6000000, 2000000, period, baseline);
        t += period;
    }
    assert(lead.lead(t, period, baseline) == 2000000);
    lead.reset();
    for (int i = 0; i < 64; ++i) {
        lead.observe(t, t + 20000000, t + 20000000, 1000000, period, baseline);
        lead.observe(t, t + 4000000, t + 4000000, 0, period, baseline);
        lead.observe(t, t + 4000000, t + 4000000, 5000000, period, baseline);
        lead.observe(t, t - 1, t + 4000000, 1, period, baseline);
        lead.observe(t, t + 4000000, t + 3000000, 1, period, baseline);
        t += period;
    }
    assert(lead.lead(t, period, baseline) == baseline);
    std::cout << "Low-latency selection, clock bridge, feedback history and bounded lead: PASS\n";
}
