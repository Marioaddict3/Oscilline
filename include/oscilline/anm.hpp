// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// ANM animation parser and pose sampling.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

struct AnmKeyframe {
    std::uint8_t object_index = 0;
    std::uint8_t flags = 0;
    bool has_rotation = false;
    bool has_scale = false;
    bool has_position = false;
    std::int16_t rotation_x = 0;
    std::int16_t rotation_y = 0;
    std::int16_t rotation_z = 0;
    std::int16_t scale_x = 0;
    std::int16_t scale_y = 0;
    std::int16_t scale_z = 0;
    std::int16_t position_x = 0;
    std::int16_t position_y = 0;
    std::int16_t position_z = 0;
};

struct AnmFrame {
    std::vector<AnmKeyframe> keys;
};

struct AnmFile {
    // open-ribbon ANM header: both int16 fields are unnamed. The first is
    // documented as always 0x8000. The second is unused, with observed values
    // 1, 10, 15, 20, 30, or 60.
    std::int16_t unk0 = 0;
    std::int16_t unk1 = 0;
    std::uint16_t frame_count = 0;
    std::vector<std::uint16_t> frame_offset_table;
    std::vector<AnmFrame> frames;
};

// open-ribbon documentation, "ANM - Model Animation".
// An object with no key in a frame is not drawn for that frame; this parser
// reports only the keys that are present.
[[nodiscard]] Result<AnmFile> parse_anm(std::span<const std::uint8_t> bytes);

} // namespace oscilline
