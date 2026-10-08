// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// SPU-ADPCM decode of a VAG body to 16-bit PCM.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// Loop marks from the ADPCM flag byte. psx-spx "SPU ADPCM Samples":
// bit 0 ends the loop, bit 1 repeats (only with bit 0), bit 2 sets the start.
struct AdpcmLoop {
    bool has_start = false;
    bool has_end = false;
    bool repeat = false;
    // Sample index of the last block flagged as the loop start.
    int start_sample = 0;
    // One past the last sample of the last block flagged as the loop end.
    int end_sample = 0;
};

struct AdpcmPcm {
    std::vector<std::int16_t> samples;
    std::uint8_t last_flags = 0;
    AdpcmLoop loop;
};

// SPU-ADPCM, 16-byte blocks, 28 samples each.
// psx-spx "SPU ADPCM Samples" and "CDROM XA Audio ADPCM Compression"
// (the same decode, with the five filter pairs in pos/neg_xa_adpcm_table).
[[nodiscard]] Result<AdpcmPcm> decode_spu_adpcm(std::span<const std::uint8_t> bytes);

} // namespace oscilline
