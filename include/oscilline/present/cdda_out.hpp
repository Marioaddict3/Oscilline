// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// SDL playback clock for CD-DA and a decoded music file.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace oscilline {

// Plays interleaved 44.1 kHz stereo PCM. The clock is samples the device has
// consumed, not the frame clock. After the device has consumed the last
// sample, the clock keeps running from SDL_GetTicks so a course can finish.
// If the device cannot be opened, the clock follows SDL_GetTicks from the
// start and a warning is printed.
class CddaOutput {
  public:
    struct Config {
        std::vector<std::int16_t> interleaved;
        int frames = 0;
        // Silence before track zero. position_ms is negative during this interval.
        int start_delay_ms = 0;
    };

    static CddaOutput open(Config config);

    CddaOutput(CddaOutput&&) noexcept;
    CddaOutput& operator=(CddaOutput&&) noexcept;
    ~CddaOutput();

    CddaOutput(const CddaOutput&) = delete;
    CddaOutput& operator=(const CddaOutput&) = delete;

    void set_paused(bool paused);

    // Scales the music stream. 32767 is unity and is the default. This does not
    // change how many samples are queued, so position_ms stays on the same clock.
    // Sample values already in the stream are left as stored; SDL applies the
    // gain when it plays them.
    void set_music_gain(int gain_q15);

    // Track time, negative during the initial delay. Pausing freezes that delay.
    [[nodiscard]] std::int64_t position_ms();

    [[nodiscard]] bool device_open() const;

  private:
    struct Impl;
    explicit CddaOutput(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace oscilline
