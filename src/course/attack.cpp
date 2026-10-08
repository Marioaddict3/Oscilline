// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Short-over-long emphasis and the evaluation time stamp.

#include "oscilline/course/attack.hpp"

#include "oscilline/audio/cdda.hpp"

#include <algorithm>
#include <cstddef>

namespace oscilline {

std::vector<float> attack_emphasis(std::span<const std::int16_t> interleaved, int frames) {
    std::vector<float> ratio;
    if (frames <= 0) {
        return ratio;
    }
    const std::size_t usable = std::min(static_cast<std::size_t>(frames), interleaved.size() / 2);
    const auto hop = static_cast<std::size_t>(kAttackHopFrames);
    const std::size_t count = usable / hop;
    if (count == 0) {
        return ratio;
    }
    // Prefix sums of the squared mono mix. A full 15-minute track stays far
    // inside the 2^53 range where double holds integers exactly.
    std::vector<double> prefix(count * hop + 1, 0.0);
    for (std::size_t i = 0; i < count * hop; ++i) {
        const double mono = (static_cast<double>(interleaved[i * 2]) +
                             static_cast<double>(interleaved[i * 2 + 1])) *
                            0.5;
        prefix[i + 1] = prefix[i] + mono * mono;
    }
    const auto window = [&](std::size_t end, std::size_t length) {
        const std::size_t begin = end >= length ? end - length : 0;
        return prefix[end] - prefix[begin];
    };
    ratio.resize(count, 0.0f);
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t end = (k + 1) * hop;
        if (end < static_cast<std::size_t>(kAttackLongFrames)) {
            continue; // The long window is not full yet.
        }
        const double ps = window(end, static_cast<std::size_t>(kAttackShortFrames));
        const double pl = window(end, static_cast<std::size_t>(kAttackLongFrames));
        const double mean = pl / static_cast<double>(kAttackLongFrames);
        // A noise floor has a ratio. It is still silence.
        if (!(mean > kAttackSilenceMeanSquare)) {
            continue;
        }
        ratio[k] = pl > 0.0 ? static_cast<float>(ps / pl) : 0.0f;
    }
    return ratio;
}

std::int64_t attack_stamp_frame(std::size_t k) {
    return static_cast<std::int64_t>(k + 1) * kAttackHopFrames + kAttackStampFrames;
}

double attack_stamp_ms(std::size_t k) {
    return static_cast<double>(attack_stamp_frame(k)) * 1000.0 / 44100.0;
}

std::int32_t attack_audible_end_ms(std::span<const std::int16_t> interleaved, int frames) {
    if (frames <= 0) {
        return 0;
    }
    const std::size_t usable = std::min(static_cast<std::size_t>(frames), interleaved.size() / 2);
    const auto hop = static_cast<std::size_t>(kAttackHopFrames);
    const std::size_t count = usable / hop;
    if (count == 0) {
        return 0;
    }
    std::vector<double> prefix(count * hop + 1, 0.0);
    for (std::size_t i = 0; i < count * hop; ++i) {
        const double mono = (static_cast<double>(interleaved[i * 2]) +
                             static_cast<double>(interleaved[i * 2 + 1])) *
                            0.5;
        prefix[i + 1] = prefix[i] + mono * mono;
    }
    const auto window = [&](std::size_t end, std::size_t length) {
        const std::size_t begin = end >= length ? end - length : 0;
        return prefix[end] - prefix[begin];
    };
    std::int64_t last_loud_frame = -1;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t end = (k + 1) * hop;
        if (end < static_cast<std::size_t>(kAttackLongFrames)) {
            continue;
        }
        const double pl = window(end, static_cast<std::size_t>(kAttackLongFrames));
        const double mean = pl / static_cast<double>(kAttackLongFrames);
        if (mean > kAttackSilenceMeanSquare) {
            last_loud_frame = static_cast<std::int64_t>(end);
        }
    }
    if (last_loud_frame < 0) {
        return 0;
    }
    const std::int32_t file_ms = cdda_duration_ms(frames);
    if (static_cast<std::int64_t>(frames) - last_loud_frame <= static_cast<std::int64_t>(hop)) {
        return file_ms;
    }
    const std::int64_t loud_ms = last_loud_frame * 1000 / static_cast<std::int64_t>(kCddaRate);
    if (loud_ms <= 0) {
        return 0;
    }
    if (loud_ms >= file_ms) {
        return file_ms;
    }
    return static_cast<std::int32_t>(loud_ms);
}

} // namespace oscilline
