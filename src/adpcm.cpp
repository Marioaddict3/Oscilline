// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// SPU-ADPCM block decode. Filter tables follow the public SPU notes.

#include "oscilline/adpcm.hpp"

namespace oscilline {
namespace {

// psx-spx pos_xa_adpcm_table / neg_xa_adpcm_table. SPU-ADPCM uses all five.
constexpr int kPositive[5] = {0, 60, 115, 98, 122};
constexpr int kNegative[5] = {0, 0, -52, -55, -60};

std::int32_t clamp16(std::int32_t sample) {
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }
    return sample;
}

} // namespace

Result<AdpcmPcm> decode_spu_adpcm(std::span<const std::uint8_t> bytes) {
    if (bytes.size() % 16 != 0) {
        return Result<AdpcmPcm>::failure("SPU-ADPCM length is not a multiple of 16");
    }
    AdpcmPcm decoded;
    decoded.samples.reserve(bytes.size() / 16 * 28);
    std::int32_t older = 0;
    std::int32_t old = 0;
    for (std::size_t block = 0; block < bytes.size(); block += 16) {
        const std::uint8_t header = bytes[block];
        const std::uint8_t flags = bytes[block + 1];
        int shift_field = header & 0x0F;
        const int filter = (header >> 4) & 0x0F;
        if (filter > 4) {
            return Result<AdpcmPcm>::failure("SPU-ADPCM filter index is outside 0..4");
        }
        // psx-spx: reserved shift values 13..15 behave as shift 9.
        if (shift_field > 12) {
            shift_field = 9;
        }
        const int shift = 12 - shift_field;
        const int positive = kPositive[filter];
        const int negative = kNegative[filter];
        decoded.last_flags = flags;
        // psx-spx flag bits: 0 loop end, 1 loop repeat, 2 loop start.
        const int block_start = static_cast<int>(decoded.samples.size());
        if ((flags & 0x04) != 0) {
            decoded.loop.has_start = true;
            decoded.loop.start_sample = block_start;
        }
        for (int i = 0; i < 14; ++i) {
            const std::uint8_t packed = bytes[block + 2 + static_cast<std::size_t>(i)];
            for (int nibble = 0; nibble < 2; ++nibble) {
                int value = (packed >> (nibble * 4)) & 0x0F;
                if (value >= 8) {
                    value -= 16;
                }
                const std::int32_t expanded = static_cast<std::int32_t>(value) * (1 << shift);
                // Public SPU formula. The +32 rounds the /64 filter multiply.
                const std::int32_t filtered =
                    expanded + ((old * positive + older * negative + 32) >> 6);
                const std::int32_t sample = clamp16(filtered);
                decoded.samples.push_back(static_cast<std::int16_t>(sample));
                older = old;
                old = sample;
            }
        }
        if ((flags & 0x01) != 0) {
            decoded.loop.has_end = true;
            decoded.loop.repeat = (flags & 0x02) != 0;
            decoded.loop.end_sample = static_cast<int>(decoded.samples.size());
        }
    }
    return Result<AdpcmPcm>::success(std::move(decoded));
}

} // namespace oscilline
