// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Parses a CUE sheet and the BIN files it names.

#pragma once

#include "oscilline/disc/sector.hpp"
#include "oscilline/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

// CDRWIN cue sheet. INDEX times are offsets into the FILE, not physical CD addresses.
// psx-spx "CDROM Disk Images CUE/BIN/CDT (Cdrwin)".
struct CueIndex {
    int number = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;

    [[nodiscard]] std::uint32_t as_sectors() const {
        return static_cast<std::uint32_t>(minutes) * 60u * 75u +
               static_cast<std::uint32_t>(seconds) * 75u + static_cast<std::uint32_t>(frames);
    }
};

struct CueTrack {
    int number = 0;
    RawSectorKind kind = RawSectorKind::Audio2352;
    std::string file_name;
    std::string file_type;
    std::vector<CueIndex> indices;
    // PREGAP is silence that is not stored in the BIN. Recorded so callers can see it.
    bool has_pregap = false;
    std::uint32_t pregap_sectors = 0;
};

struct CueSheet {
    std::vector<CueTrack> tracks;
};

[[nodiscard]] Result<CueSheet> parse_cue(std::string_view text);

} // namespace oscilline
