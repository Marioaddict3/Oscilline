// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// VH header and VB body parser for an SPU sound bank.

#pragma once

#include "oscilline/adpcm.hpp"
#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

struct VabProgram {
    std::uint8_t tone_count = 0;
    std::uint8_t volume = 0;
    std::uint8_t priority = 0;
    std::uint8_t mode = 0;
    std::uint8_t pan = 0;
    std::uint16_t attribute = 0;
};

struct VabTone {
    std::uint8_t priority = 0;
    std::uint8_t mode = 0;
    std::uint8_t volume = 0;
    std::uint8_t pan = 0;
    std::uint8_t center = 0;
    std::uint8_t shift = 0;
    std::uint8_t note_min = 0;
    std::uint8_t note_max = 0;
    std::uint8_t vib_w = 0;
    std::uint8_t vib_t = 0;
    std::uint8_t por_w = 0;
    std::uint8_t por_t = 0;
    std::uint8_t pitch_bend_min = 0;
    std::uint8_t pitch_bend_max = 0;
    std::uint16_t adsr1 = 0;
    std::uint16_t adsr2 = 0;
    std::uint16_t program = 0;
    std::uint16_t vag = 0;
};

struct VabHeader {
    std::uint32_t version = 0;
    std::uint32_t bank_id = 0;
    std::uint32_t total_size = 0;
    // The program field is the actual program count (1..128). After the
    // 0x20-byte header there is always a 128-entry program table (16 bytes
    // each), then one 512-byte tone table per program. Tone and VAG counts
    // are documented as "minus?" so those raw fields are kept as stored.
    std::uint16_t program_count_field = 0;
    std::uint16_t program_count = 0;
    std::uint16_t tone_count_field = 0;
    std::uint16_t vag_count_field = 0;
    std::uint8_t master_volume = 0;
    std::uint8_t master_pan = 0;
    std::uint8_t attribute1 = 0;
    std::uint8_t attribute2 = 0;
    std::vector<VabProgram> programs;
    std::vector<VabTone> tones;
    // 256 entries. Each value is the VAG body size divided by 8.
    std::vector<std::uint16_t> vag_sizes;
};

struct DecodedVag {
    std::uint16_t size_units = 0;
    std::vector<std::int16_t> pcm;
    std::uint8_t last_flags = 0;
    AdpcmLoop loop;
};

struct VabBank {
    VabHeader header;
    bool has_body = false;
    std::vector<DecodedVag> vags;
};

// VAB header and body. Layout: 0x20-byte header, a fixed 128-entry program
// table (0x800 bytes), program_count tone tables of 0x200 bytes, then 256
// uint16 VAG sizes in 8-byte units. The size table ends at the end of the VH.
// The sizes times 8 sum to the VB length.
[[nodiscard]] Result<VabHeader> parse_vh(std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<VabBank> parse_vab(std::span<const std::uint8_t> vh,
                                        std::span<const std::uint8_t> vb);

} // namespace oscilline
