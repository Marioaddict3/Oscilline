// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Code point to object index for the disc line font.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace oscilline {

// Glyph object index for one code point. Numbers only: no outlines.
// PAL font ordering checked on the PAL disc through our TMD
// parser: digits, uppercase, then lowercase. Punctuation is not ASCII-ordered.
// Only visually identified punctuation is mapped. Spaces are skipped by layout.
// Extended Latin and uncertain symbols stay unmapped and draw the box.
// '%' is intentionally absent: VectorFont draws an original blocky percent.
struct GlyphIndex {
    char32_t codepoint = 0;
    std::uint16_t object = 0;
};

inline constexpr GlyphIndex kFontGlyphs[] = {
    {U' ', 0},  {U'!', 63}, {U'&', 64},  {U'\'', 67}, {U'(', 69}, {U')', 70}, {U',', 66},
    {U'-', 68}, {U'.', 65}, {U'0', 0},   {U'1', 1},   {U'2', 2},  {U'3', 3},  {U'4', 4},
    {U'5', 5},  {U'6', 6},  {U'7', 7},   {U'8', 8},   {U'9', 9},  {U'?', 62}, {U'A', 10},
    {U'B', 11}, {U'C', 12}, {U'D', 13},  {U'E', 14},  {U'F', 15}, {U'G', 16}, {U'H', 17},
    {U'I', 18}, {U'J', 19}, {U'K', 20},  {U'L', 21},  {U'M', 22}, {U'N', 23}, {U'O', 24},
    {U'P', 25}, {U'Q', 26}, {U'R', 27},  {U'S', 28},  {U'T', 29}, {U'U', 30}, {U'V', 31},
    {U'W', 32}, {U'X', 33}, {U'Y', 34},  {U'Z', 35},  {U'a', 36}, {U'b', 37}, {U'c', 38},
    {U'd', 39}, {U'e', 40}, {U'f', 41},  {U'g', 42},  {U'h', 43}, {U'i', 44}, {U'j', 45},
    {U'k', 46}, {U'l', 47}, {U'm', 48},  {U'n', 49},  {U'o', 50}, {U'p', 51}, {U'q', 52},
    {U'r', 53}, {U's', 54}, {U't', 55},  {U'u', 56},  {U'v', 57}, {U'w', 58}, {U'x', 59},
    {U'y', 60}, {U'z', 61}, {U'~', 108},
};

[[nodiscard]] inline std::span<const GlyphIndex> font_glyphs() {
    return kFontGlyphs;
}

[[nodiscard]] inline std::optional<std::uint16_t> glyph_object(char32_t codepoint) {
    for (const GlyphIndex& row : kFontGlyphs) {
        if (row.codepoint == codepoint) {
            return row.object;
        }
    }
    return std::nullopt;
}

} // namespace oscilline
