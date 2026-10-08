// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// TMD model parser. Rectangle packets stay raw bytes.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

enum class PrimitiveKind {
    Polygon,
    Line,
    Rectangle,
    Unknown,
};

struct TmdVertex {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t z = 0;
};

struct TmdPrimitive {
    std::uint8_t output_words = 0;
    std::uint8_t input_words = 0;
    std::uint8_t flag = 0;
    std::uint8_t mode = 0;
    PrimitiveKind kind = PrimitiveKind::Unknown;
    bool textured = false;
    bool gouraud = false;
    bool quad = false;
    bool unlit = false;
    bool no_backface_clip = false;
    std::uint32_t color = 0;
    std::vector<std::uint32_t> texcoord_words;
    std::vector<std::uint32_t> extra_colors;
    std::vector<std::uint16_t> vertex_indices;
    std::vector<std::uint16_t> normal_indices;
    // Full packet. ilen counts body words only, so this is (ilen + 1) * 4
    // bytes. Rectangle layouts are not specified beyond the raw packet
    // (psx-spx: "Unknown").
    std::vector<std::uint8_t> raw;
};

struct TmdObject {
    std::uint32_t vertex_offset = 0;
    std::uint32_t normal_offset = 0;
    std::uint32_t primitive_offset = 0;
    std::int32_t scale = 0;
    std::vector<TmdVertex> vertices;
    std::vector<TmdVertex> normals;
    std::vector<TmdPrimitive> primitives;
};

struct TmdModel {
    std::uint32_t flags = 0;
    // Bit 0 of the header flags. When clear, vertex, normal, and primitive
    // offsets in every object entry are relative to the object table (file
    // offset 0x0C, the byte after the 12-byte header). When set, those
    // offsets are file-absolute.
    bool fixp = false;
    std::vector<TmdObject> objects;
};

// psx-spx "TMD - Modeling Data for OS Library".
[[nodiscard]] Result<TmdModel> parse_tmd(std::span<const std::uint8_t> bytes);

} // namespace oscilline
