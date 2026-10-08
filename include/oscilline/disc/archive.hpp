// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Mounts a disc image and loads one language PAK.

#pragma once

#include "oscilline/disc/identity.hpp"
#include "oscilline/disc/image.hpp"
#include "oscilline/disc/iso9660.hpp"
#include "oscilline/pak.hpp"
#include "oscilline/result.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

struct MountedDisc {
    DiscImage image;
    IsoVolume volume;
    DiscIdentity identity;
};

struct ModelList {
    std::vector<std::string> tmd;
    std::vector<std::string> anm;
    std::vector<std::string> anc;
};

// Opens a disc image and refuses a boot file other than the two supported ones.
[[nodiscard]] Result<MountedDisc> mount_disc(const std::filesystem::path& path);

// Case-insensitive, and treats '\\' and '/' as the same separator.
[[nodiscard]] const PakEntry* find_entry(const PakArchive& archive, std::string_view name);

[[nodiscard]] ModelList list_models(const PakArchive& archive);

// PAL English is the primary archive. Japanese discs use FILES.PAK, and a
// language-prefixed 01 archive is accepted when that is what the image has.
inline constexpr std::string_view kLanguagePakCandidates[] = {
    "GAME/02_FILES.PAK",
    "GAME/01_FILES.PAK",
    "GAME/FILES.PAK",
};

// The title archive keeps this name on both supported discs.
inline constexpr std::string_view kTitlePakPath = "TITLE/FILES.PAK";

[[nodiscard]] Result<PakArchive> load_language_pak(const IsoVolume& volume,
                                                   std::string_view override_path = {});

// The candidate load_language_pak would open, or the override when that file
// is on the volume. Empty when none of them are present.
[[nodiscard]] std::optional<std::string> locate_language_pak(const IsoVolume& volume,
                                                             std::string_view override_path = {});

} // namespace oscilline
