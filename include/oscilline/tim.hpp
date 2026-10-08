// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// TIM image parser.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

struct TimImage {
    int bpp = 0;
    bool has_clut = false;
    std::uint32_t flags = 0;
    int clut_x = 0;
    int clut_y = 0;
    int clut_width = 0;
    int clut_height = 0;
    std::vector<Rgba8> clut;
    int origin_x = 0;
    int origin_y = 0;
    int vram_width = 0;
    int width = 0;
    int height = 0;
    std::vector<Rgba8> pixels;
};

// psx-spx "CDROM File Video Texture Image TIM/PXL/CLT" and the open-ribbon TIM page.
// Indexed images are expanded with CLUT row 0. 16-bit colors use the STP rules
// on that TIM page (0x0000 transparent, 0x8000 opaque black, other STP colors
// semi-transparent with alpha 128).
[[nodiscard]] Result<TimImage> parse_tim(std::span<const std::uint8_t> bytes);

} // namespace oscilline
