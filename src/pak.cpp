// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Count, offset table, NUL-terminated names, and 4-byte alignment.

#include "oscilline/pak.hpp"

#include "oscilline/byte_reader.hpp"

namespace oscilline {
namespace {

constexpr std::uint32_t kMaxEntries = 100000;
constexpr std::size_t kMaxName = 1024;

} // namespace

Result<PakArchive> parse_pak(std::span<const std::uint8_t> bytes) {
    ByteReader reader(bytes);
    const std::uint32_t count = reader.u32();
    if (!reader.ok()) {
        return Result<PakArchive>::failure("PAK header is truncated");
    }
    if (count > kMaxEntries) {
        return Result<PakArchive>::failure("PAK file count is unreasonable");
    }
    std::vector<std::uint32_t> offsets(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        offsets[i] = reader.u32();
    }
    if (!reader.ok()) {
        return Result<PakArchive>::failure("PAK offset table is truncated");
    }

    PakArchive archive;
    archive.entries.reserve(count);
    for (std::uint32_t offset : offsets) {
        if (static_cast<std::size_t>(offset) >= bytes.size()) {
            return Result<PakArchive>::failure("PAK entry offset is past the end of the file");
        }
        reader.seek(offset);
        std::string name;
        bool terminated = false;
        while (name.size() < kMaxName && reader.remaining() > 0) {
            const std::uint8_t ch = reader.u8();
            if (!reader.ok()) {
                break;
            }
            if (ch == 0) {
                terminated = true;
                break;
            }
            name.push_back(static_cast<char>(ch));
        }
        if (!terminated) {
            return Result<PakArchive>::failure("PAK entry name is missing its terminator");
        }
        // The length word is 4-byte aligned. A name whose character count is
        // already a multiple of 4 still has a terminator, so that case is four
        // nulls (the docs only spell out the 1..3 null pad).
        while ((reader.position() % 4) != 0) {
            const std::uint8_t pad = reader.u8();
            if (!reader.ok() || pad != 0) {
                return Result<PakArchive>::failure("PAK entry name padding is not zero");
            }
        }
        const std::uint32_t size = reader.u32();
        if (!reader.ok()) {
            return Result<PakArchive>::failure("PAK entry length is truncated");
        }
        if (static_cast<std::uint64_t>(size) > reader.remaining()) {
            return Result<PakArchive>::failure("PAK entry data runs past the end of the file");
        }
        const auto data = reader.bytes(size);
        if (!reader.ok()) {
            return Result<PakArchive>::failure("PAK entry data is truncated");
        }
        PakEntry entry;
        entry.name = std::move(name);
        entry.offset = static_cast<std::uint32_t>(reader.position() - size);
        entry.size = size;
        entry.data.assign(data.begin(), data.end());
        archive.entries.push_back(std::move(entry));
    }
    return Result<PakArchive>::success(std::move(archive));
}

} // namespace oscilline
