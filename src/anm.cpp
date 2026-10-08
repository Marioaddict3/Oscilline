// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Reads an ANM file. Frame-table entries are half-word offsets.

#include "oscilline/anm.hpp"

#include "oscilline/byte_reader.hpp"

namespace oscilline {
namespace {

// Rotation is value * 2 * pi / 4096 radians. Scale is value / 4096.
// Those conversions are left to the renderer; the file stores the raw int16s.
bool read_triple(ByteReader& reader, std::int16_t& x, std::int16_t& y, std::int16_t& z) {
    x = reader.i16();
    y = reader.i16();
    z = reader.i16();
    return reader.ok();
}

} // namespace

Result<AnmFile> parse_anm(std::span<const std::uint8_t> bytes) {
    ByteReader reader(bytes);
    const std::uint16_t unk0_bits = reader.u16();
    const std::int16_t unk1 = reader.i16();
    const std::uint16_t frame_count = reader.u16();
    if (!reader.ok()) {
        return Result<AnmFile>::failure("ANM header is truncated");
    }
    if (unk0_bits != 0x8000) {
        return Result<AnmFile>::failure("ANM header word is not 0x8000");
    }
    const std::uint32_t table_entries = static_cast<std::uint32_t>(frame_count) + 1u;
    if (reader.remaining() < static_cast<std::size_t>(table_entries) * 2u) {
        return Result<AnmFile>::failure("ANM frame offset table is truncated");
    }

    AnmFile file;
    file.unk0 = static_cast<std::int16_t>(unk0_bits);
    file.unk1 = unk1;
    file.frame_count = frame_count;
    file.frame_offset_table.resize(table_entries);
    for (std::uint32_t i = 0; i < table_entries; ++i) {
        file.frame_offset_table[i] = reader.u16();
    }
    if (!reader.ok()) {
        return Result<AnmFile>::failure("ANM frame offset table is truncated");
    }

    file.frames.resize(frame_count);
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        const std::uint32_t start_units = file.frame_offset_table[frame];
        const std::uint32_t end_units = file.frame_offset_table[frame + 1];
        if (end_units < start_units) {
            return Result<AnmFile>::failure("ANM frame offset table goes backwards");
        }
        const std::uint64_t start = static_cast<std::uint64_t>(start_units) * 2u;
        const std::uint64_t end = static_cast<std::uint64_t>(end_units) * 2u;
        if (end > bytes.size()) {
            return Result<AnmFile>::failure("ANM frame extends past the end of the file");
        }
        reader.seek(static_cast<std::size_t>(start));
        while (reader.position() < end) {
            if (end - reader.position() < 2) {
                return Result<AnmFile>::failure("ANM keyframe is truncated");
            }
            AnmKeyframe key;
            key.object_index = reader.u8();
            key.flags = reader.u8();
            key.has_rotation = (key.flags & 0x01) != 0;
            key.has_scale = (key.flags & 0x02) != 0;
            key.has_position = (key.flags & 0x04) != 0;
            if (key.has_rotation &&
                !read_triple(reader, key.rotation_x, key.rotation_y, key.rotation_z)) {
                return Result<AnmFile>::failure("ANM rotation triple is truncated");
            }
            if (key.has_scale && !read_triple(reader, key.scale_x, key.scale_y, key.scale_z)) {
                return Result<AnmFile>::failure("ANM scale triple is truncated");
            }
            if (key.has_position &&
                !read_triple(reader, key.position_x, key.position_y, key.position_z)) {
                return Result<AnmFile>::failure("ANM position triple is truncated");
            }
            if (reader.position() > end) {
                return Result<AnmFile>::failure("ANM keyframe crosses the frame end offset");
            }
            file.frames[frame].keys.push_back(key);
        }
    }
    return Result<AnmFile>::success(std::move(file));
}

} // namespace oscilline
