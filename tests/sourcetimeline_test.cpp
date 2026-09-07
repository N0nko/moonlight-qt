#include "../app/streaming/video/ffmpeg-renderers/pacer/sourcetimeline.h"
#include "../app/streaming/video/ffmpeg-renderers/pacer/displayperiod.h"
#include <cassert>
#include <cmath>
#include <deque>
#include <iostream>
#include <vector>

using Observation = SourceTimeline::Observation;

void edgeCases()
{
    SourceTimeline clock;
    assert(clock.observe(0xfffffff0u, 1000000) == Observation::Reset);
    assert(clock.observe(0xfffffff0u, 1000001) == Observation::Duplicate);
    assert(clock.observe(984, 1011111) == Observation::Accepted);
    assert(clock.dueUs(984, 11111) == 1022222);
    assert(clock.dueUs(0xfffffff0u, 11111) == 1011111);
    assert(clock.observe(984, 3000000) == Observation::Reset); // wake/long gap
    assert(clock.dueUs(984, 11111) == 3011111);
    assert(clock.observe(10, 3011111) == Observation::Reset); // new source epoch
    assert(clock.observe(1010, 100) == Observation::Reset); // local clock discontinuity

    SourceTimeline lowRate;
    lowRate.observe(0, 1000000);
    uint32_t queued[] = {0};
    assert(lowRate.newestDue(queued, 1, 1011110, 11111) == -1);
    assert(lowRate.newestDue(queued, 1, 1011111, 11111) == 0);
    // The first 30 FPS frame does not wait for a second frame at 33.3 ms.
    lowRate.observe(3000, 1033333);
    uint32_t pair[] = {0, 3000};
    assert(lowRate.newestDue(pair, 2, 1033333, 11111) == 0);
    assert(lowRate.newestDue(pair, 2, 1044444, 11111) == 1);
}

// Exercise the production selector with a three-frame decoded queue and a
// fixed 90 Hz panel. This is a policy simulation, not a GPU/latency benchmark.
unsigned simulate(double fps, int delayUs)
{
    struct Frame { uint32_t pts; uint64_t readyUs; unsigned index; };
    std::vector<Frame> input;
    constexpr uint64_t start = 1000000;
    for (unsigned i = 0; i < 600; ++i) {
        uint32_t pts = static_cast<uint32_t>(std::llround(i * 90000.0 / fps));
        uint64_t ready = start + static_cast<uint64_t>(pts) * 100 / 9;
        if (i == 100) ready += delayUs;
        if (!input.empty()) ready = std::max(ready, input.back().readyUs);
        input.push_back({pts, ready, i});
    }
    SourceTimeline clock;
    std::deque<Frame> queue;
    size_t next = 0;
    unsigned last = 0;
    unsigned selectedCount = 0;
    unsigned lateSelections = 0;
    bool shown = false;
    const uint64_t stop = input.back().readyUs + 100000;
    for (uint64_t slot = 1; start + slot * 1000000 / 90 < stop; ++slot) {
        uint64_t present = start + slot * 1000000 / 90;
        uint64_t deadline = present - 2000;
        while (next < input.size() && input[next].readyUs <= deadline) {
            const auto frame = input[next++];
            auto result = clock.observe(frame.pts, frame.readyUs);
            if (result == Observation::Reset) queue.clear();
            if (result != Observation::Duplicate) {
                if (queue.size() == 3) queue.pop_front();
                queue.push_back(frame);
            }
        }
        uint32_t pts[3] = {};
        for (size_t i = 0; i < queue.size(); ++i) pts[i] = queue[i].pts;
        int chosen = clock.newestDue(pts, queue.size(), present, 11111);
        if (chosen < 0) continue;
        auto selected = queue[chosen];
        assert(!shown || selected.index > last);
        assert(clock.dueUs(selected.pts, 11111) <= static_cast<int64_t>(present));
        assert(present - selected.readyUs < 35000);
        if (present > start + static_cast<uint64_t>(selected.pts) * 100 / 9 + 23000) ++lateSelections;
        queue.erase(queue.begin(), queue.begin() + chosen + 1);
        last = selected.index;
        shown = true;
        ++selectedCount;
    }
    assert(selectedCount > 500);
    assert(last == 599);
    std::cout << "fps=" << fps << " injected_delay_us=" << delayUs
              << " selected=" << selectedCount << " late=" << lateSelections << '\n';
    return selectedCount;
}

void driftAndLongSession()
{
    for (int driftPpm : {-200, 200}) {
        SourceTimeline clock;
        uint64_t ready = 0;
        for (uint64_t i = 0; i < 90 * 3600ull; ++i) {
            const uint64_t ticks = i * 1000;
            ready = 1000000 + ticks * 100 / 9 +
                static_cast<int64_t>(ticks * 100 / 9) * driftPpm / 1000000;
            clock.observe(static_cast<uint32_t>(ticks), ready);
        }
        const int64_t error = clock.dueUs(static_cast<uint32_t>((90 * 3600ull - 1) * 1000), 0) - ready;
        assert(std::abs(error) < 1500);
    }
    // Twenty hours crosses RTP wrap while preserving a continuous timeline.
    SourceTimeline longSession;
    for (uint64_t seconds = 0; seconds < 72000; ++seconds) {
        auto result = longSession.observe(static_cast<uint32_t>(seconds * 90000),
                                          1000000 + seconds * 1000000);
        assert(result == (seconds == 0 ? Observation::Reset : Observation::Accepted));
    }
}

void burstAfterResume()
{
    SourceTimeline clock;
    clock.observe(0, 1000000);
    assert(clock.observe(1000, 4000000) == Observation::Reset);
    for (uint32_t pts = 2000; pts <= 180000; pts += 1000) {
        clock.observe(pts, 4000000);
    }
    // A backlog arriving all at once must not create seconds of new delay.
    assert(clock.dueUs(180000, 11111) == 4011111);
}

void variableRate()
{
    SourceTimeline clock;
    uint32_t pts = 0;
    for (unsigned i = 0; i < 300; ++i) {
        pts += i < 100 ? 3000 : (i < 200 ? 2000 : 1000);
        uint64_t ready = 1000000 + static_cast<uint64_t>(pts) * 100 / 9;
        clock.observe(pts, ready);
        assert(std::abs(clock.dueUs(pts, 11111) - static_cast<int64_t>(ready + 11111)) <= 1);
    }
}

int main()
{
    std::array<uint64_t, 32> scans;
    scans.fill(11111111);
    assert(validatedDisplayPeriod(16666666, 90, scans.data(), 0) == 0);
    assert(validatedDisplayPeriod(16666666, 90, scans.data(), 32) == 11111111);
    assert(validatedDisplayPeriod(11111111, 90, scans.data(), 0) == 11111111);
    scans.fill(33333333); // 30 FPS on a 90 Hz panel is three scans, not 30 Hz.
    assert(validatedDisplayPeriod(16666666, 90, scans.data(), 32) == 11111111);
    scans.fill(22222222);
    assert(validatedDisplayPeriod(16666666, 90, scans.data(), 32) == 11111111);
    scans.fill(16666666); // Conflicting real timing must not validate as 90 Hz.
    assert(validatedDisplayPeriod(16666666, 90, scans.data(), 32) == 0);
    assert(validatedDisplayPeriod(16666666, 60, scans.data(), 32) == 16666666);
    assert(validatedDisplayPeriod(16666666, 0, scans.data(), 32) == 0);
    edgeCases();
    driftAndLongSession();
    burstAfterResume();
    variableRate();
    for (double fps : {24.0, 30.0, 40.0, 45.0, 50.0, 59.94, 60.0, 72.0, 90.0}) {
        for (int delay : {0, 2000, 8000, 16000, 80000}) simulate(fps, delay);
    }
    std::cout << "Source timeline tests passed\n";
}
