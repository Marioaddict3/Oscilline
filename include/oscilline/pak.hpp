// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// PAK archive parser. Entry offsets are file-absolute.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace oscilline {

struct PakEntry {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::vector<std::uint8_t> data;
};

struct PakArchive {
    std::vector<PakEntry> entries;
};

// open-ribbon documentation, file type PAK:
// u32 count, u32 offset table, then at each offset a name, 4-byte aligned
// length, and uncompressed bytes.
[[nodiscard]] Result<PakArchive> parse_pak(std::span<const std::uint8_t> bytes);

} // namespace oscilline
