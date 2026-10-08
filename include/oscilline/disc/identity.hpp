// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Accepts the PAL and Japanese boot files and refuses the rest.

#pragma once

#include "oscilline/disc/iso9660.hpp"
#include "oscilline/result.hpp"

#include <string>
#include <string_view>

namespace oscilline {

enum class Region {
    Pal,
    Japan,
};

struct DiscIdentity {
    Region region = Region::Pal;
    std::string serial;
    std::string boot_file;
};

[[nodiscard]] const char* region_name(Region region);

// Reads SYSTEM.CNF and accepts only the two supported boot files.
// psx-spx "CDROM File Playstation EXE and SYSTEM.CNF" (BOOT = cdrom:\...\ ;1).
[[nodiscard]] Result<DiscIdentity> identify_disc(const IsoVolume& volume);

[[nodiscard]] std::string unrecognized_disc_message(std::string_view boot_file);

} // namespace oscilline
