#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

inline uint64_t validatedDisplayPeriod(uint64_t reported, int displayHz,
                                       const uint64_t* intervals, size_t count)
{
    if (displayHz < 25 || displayHz > 250) return 0;
    const uint64_t nominal = 1000000000ull / displayHz;
    auto agrees = [nominal](uint64_t value) {
        return value >= nominal - nominal / 50 && value <= nominal + nominal / 50;
    };
    if (agrees(reported)) return reported;

    // Gamescope can retain its 60 Hz default. Corroborate the display mode
    // using actual presents, including sparse sources spanning several scans.
    std::array<uint64_t, 32> periods = {};
    if (count < periods.size()) return 0;
    for (size_t i = 0; i < periods.size(); ++i) {
        const uint64_t scans = std::max<uint64_t>(1, (intervals[i] + nominal / 2) / nominal);
        periods[i] = intervals[i] / scans;
    }
    std::sort(periods.begin(), periods.end());
    const uint64_t median = (periods[15] + periods[16]) / 2;
    return agrees(median) && periods[23] - periods[7] <= nominal / 50 ? median : 0;
}
