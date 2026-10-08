// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// VH program and tone tables, then the VB body sized by the VAG table.

#include "oscilline/vab.hpp"

#include "oscilline/adpcm.hpp"
#include "oscilline/byte_reader.hpp"

namespace oscilline {
namespace {

constexpr std::uint32_t kProgramSlots = 128;
constexpr std::uint32_t kTonesPerProgram = 16;
constexpr std::uint32_t kVagSlots = 256;
constexpr std::size_t kMaxBody = 2u * 1024u * 1024u;

std::string read_programs(ByteReader& reader, std::vector<VabProgram>& programs) {
    programs.resize(kProgramSlots);
    for (VabProgram& program : programs) {
        program.tone_count = reader.u8();
        program.volume = reader.u8();
        program.priority = reader.u8();
        program.mode = reader.u8();
        program.pan = reader.u8();
        reader.u8();
        program.attribute = reader.u16();
        reader.skip(8);
    }
    if (!reader.ok()) {
        return "VH program table is truncated";
    }
    return {};
}

std::string
read_tones(ByteReader& reader, std::uint32_t program_count, std::vector<VabTone>& tones) {
    const std::uint64_t count = static_cast<std::uint64_t>(program_count) * kTonesPerProgram;
    if (count * 32u > reader.remaining()) {
        return "VH tone attribute table is truncated";
    }
    tones.resize(static_cast<std::size_t>(count));
    for (VabTone& tone : tones) {
        tone.priority = reader.u8();
        tone.mode = reader.u8();
        tone.volume = reader.u8();
        tone.pan = reader.u8();
        tone.center = reader.u8();
        tone.shift = reader.u8();
        tone.note_min = reader.u8();
        tone.note_max = reader.u8();
        tone.vib_w = reader.u8();
        tone.vib_t = reader.u8();
        tone.por_w = reader.u8();
        tone.por_t = reader.u8();
        tone.pitch_bend_min = reader.u8();
        tone.pitch_bend_max = reader.u8();
        reader.u8();
        reader.u8();
        tone.adsr1 = reader.u16();
        tone.adsr2 = reader.u16();
        tone.program = reader.u16();
        tone.vag = reader.u16();
        reader.skip(8);
    }
    if (!reader.ok()) {
        return "VH tone attribute table is truncated";
    }
    return {};
}

} // namespace

Result<VabHeader> parse_vh(std::span<const std::uint8_t> bytes) {
    ByteReader reader(bytes);
    const auto magic = reader.bytes(4);
    const std::uint32_t version = reader.u32();
    const std::uint32_t bank_id = reader.u32();
    const std::uint32_t total_size = reader.u32();
    reader.u16();
    const std::uint16_t program_field = reader.u16();
    const std::uint16_t tone_field = reader.u16();
    const std::uint16_t vag_field = reader.u16();
    const std::uint8_t master_volume = reader.u8();
    const std::uint8_t master_pan = reader.u8();
    const std::uint8_t attribute1 = reader.u8();
    const std::uint8_t attribute2 = reader.u8();
    reader.u32();
    if (!reader.ok() || magic.size() != 4) {
        return Result<VabHeader>::failure("VH header is truncated");
    }
    if (magic[0] != 'p' || magic[1] != 'B' || magic[2] != 'A' || magic[3] != 'V') {
        return Result<VabHeader>::failure("VH magic is not \"pBAV\"");
    }
    if (program_field == 0 || program_field > kProgramSlots) {
        return Result<VabHeader>::failure("VH program count must be from 1 to 128");
    }
    // The field is the count, not count minus one. A 128-slot program table
    // still follows the 0x20-byte header.
    const std::uint32_t program_count = program_field;

    VabHeader header;
    header.version = version;
    header.bank_id = bank_id;
    header.total_size = total_size;
    header.program_count_field = program_field;
    header.program_count = static_cast<std::uint16_t>(program_count);
    header.tone_count_field = tone_field;
    header.vag_count_field = vag_field;
    header.master_volume = master_volume;
    header.master_pan = master_pan;
    header.attribute1 = attribute1;
    header.attribute2 = attribute2;

    const std::string programs_error = read_programs(reader, header.programs);
    if (!programs_error.empty()) {
        return Result<VabHeader>::failure(programs_error);
    }
    const std::string tones_error = read_tones(reader, program_count, header.tones);
    if (!tones_error.empty()) {
        return Result<VabHeader>::failure(tones_error);
    }
    if (reader.remaining() < kVagSlots * 2u) {
        return Result<VabHeader>::failure("VH VAG size table is truncated");
    }
    header.vag_sizes.resize(kVagSlots);
    for (std::uint16_t& size : header.vag_sizes) {
        size = reader.u16();
    }
    if (!reader.ok()) {
        return Result<VabHeader>::failure("VH VAG size table is truncated");
    }
    if (reader.remaining() != 0) {
        return Result<VabHeader>::failure("VH VAG size table does not end at the end of the file");
    }
    return Result<VabHeader>::success(std::move(header));
}

Result<VabBank> parse_vab(std::span<const std::uint8_t> vh, std::span<const std::uint8_t> vb) {
    auto header = parse_vh(vh);
    if (!header) {
        return Result<VabBank>::failure(header.error());
    }
    if (vb.size() > kMaxBody) {
        return Result<VabBank>::failure("VB body is unreasonably large");
    }
    VabBank bank;
    bank.header = std::move(header.value());
    bank.has_body = true;
    bank.vags.resize(kVagSlots);
    std::size_t cursor = 0;
    for (std::uint32_t index = 0; index < kVagSlots; ++index) {
        const std::uint32_t units = bank.header.vag_sizes[index];
        const std::uint32_t size = units * 8u;
        if ((size % 16u) != 0) {
            return Result<VabBank>::failure("VAG size is not a whole number of ADPCM blocks");
        }
        if (cursor + size > vb.size()) {
            return Result<VabBank>::failure("VB body is shorter than the VAG size table");
        }
        DecodedVag& vag = bank.vags[index];
        vag.size_units = static_cast<std::uint16_t>(units);
        if (size != 0) {
            auto pcm = decode_spu_adpcm(vb.subspan(cursor, size));
            if (!pcm) {
                return Result<VabBank>::failure(pcm.error());
            }
            vag.pcm = std::move(pcm.value().samples);
            vag.last_flags = pcm.value().last_flags;
            vag.loop = pcm.value().loop;
        }
        cursor += size;
    }
    if (cursor != vb.size()) {
        return Result<VabBank>::failure("VB length does not match the VAG size table");
    }
    return Result<VabBank>::success(std::move(bank));
}

} // namespace oscilline
