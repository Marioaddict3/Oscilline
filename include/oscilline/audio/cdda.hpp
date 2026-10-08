// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Reads CD-DA sectors as 44100 Hz stereo PCM.

#pragma once

#include "oscilline/disc/image.hpp"
#include "oscilline/result.hpp"

#include <cstdint>
#include <vector>

namespace oscilline {

inline constexpr int kCddaRate = 44100;
inline constexpr int kCddaFramesPerSector = 588;
inline constexpr int kMaxCddaSeconds = 15 * 60;

struct CddaTrack {
    int track_number = 0;
    int frames = 0;
    // Interleaved little-endian stereo: left, right, left, right, ...
    std::vector<std::int16_t> interleaved;
};

// How many stereo frames a read will keep. Longer tracks are truncated.
[[nodiscard]] int cdda_frames_for_sectors(std::uint32_t sectors);

[[nodiscard]] std::int32_t cdda_duration_ms(int frames);

// Reads raw 2352-byte sectors and decodes them as 44.1 kHz 16-bit stereo.
[[nodiscard]] Result<CddaTrack> read_cdda_track(const DiscImage& image, int track_number);

} // namespace oscilline
