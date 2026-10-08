// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Mount, identity check, and language-PAK lookup.

#include "oscilline/disc/archive.hpp"

#include <algorithm>
#include <cctype>

namespace oscilline {
namespace {

std::string canonical_path(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch == '\\') {
            ch = '/';
        }
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    return out;
}

bool ends_with_type(std::string_view name, std::string_view type) {
    return name.size() >= type.size() &&
           name.compare(name.size() - type.size(), type.size(), type) == 0;
}

} // namespace

Result<MountedDisc> mount_disc(const std::filesystem::path& path) {
    auto image = DiscImage::open(path);
    if (!image) {
        return Result<MountedDisc>::failure(image.error());
    }
    auto volume = IsoVolume::read(image.value());
    if (!volume) {
        return Result<MountedDisc>::failure(volume.error());
    }
    auto identity = identify_disc(volume.value());
    if (!identity) {
        return Result<MountedDisc>::failure(identity.error());
    }
    MountedDisc mounted;
    mounted.image = std::move(image.value());
    mounted.volume = std::move(volume.value());
    mounted.identity = std::move(identity.value());
    return Result<MountedDisc>::success(std::move(mounted));
}

const PakEntry* find_entry(const PakArchive& archive, std::string_view name) {
    const std::string wanted = canonical_path(name);
    for (const PakEntry& entry : archive.entries) {
        if (canonical_path(entry.name) == wanted) {
            return &entry;
        }
    }
    return nullptr;
}

ModelList list_models(const PakArchive& archive) {
    ModelList list;
    for (const PakEntry& entry : archive.entries) {
        const std::string name = canonical_path(entry.name);
        if (ends_with_type(name, ".tmd")) {
            list.tmd.push_back(entry.name);
        } else if (ends_with_type(name, ".anm")) {
            list.anm.push_back(entry.name);
        } else if (ends_with_type(name, ".anc")) {
            list.anc.push_back(entry.name);
        }
    }
    const auto by_name = [](const std::string& a, const std::string& b) {
        return canonical_path(a) < canonical_path(b);
    };
    std::sort(list.tmd.begin(), list.tmd.end(), by_name);
    std::sort(list.anm.begin(), list.anm.end(), by_name);
    std::sort(list.anc.begin(), list.anc.end(), by_name);
    return list;
}

Result<PakArchive> load_language_pak(const IsoVolume& volume, std::string_view override_path) {
    std::vector<std::string> tried;
    const auto attempt = [&](std::string_view path) -> Result<PakArchive> {
        tried.emplace_back(path);
        const FsNode* node = volume.find(path);
        if (node == nullptr || node->directory || node->outside_data_track) {
            return Result<PakArchive>::failure("missing");
        }
        auto bytes = volume.read_file(*node);
        if (!bytes) {
            return Result<PakArchive>::failure(bytes.error());
        }
        return parse_pak(bytes.value());
    };

    if (!override_path.empty()) {
        auto parsed = attempt(override_path);
        if (!parsed) {
            if (parsed.error() == "missing") {
                return Result<PakArchive>::failure("archive '" + std::string(override_path) +
                                                   "' is not on this disc");
            }
            return parsed;
        }
        return parsed;
    }

    for (const std::string_view candidate : kLanguagePakCandidates) {
        auto parsed = attempt(candidate);
        if (parsed) {
            return parsed;
        }
        if (parsed.error() != "missing") {
            return Result<PakArchive>::failure(std::string(candidate) + ": " + parsed.error());
        }
    }
    std::string message = "no language archive on this disc (tried";
    for (const std::string& path : tried) {
        message += " ";
        message += path;
    }
    message += ")";
    return Result<PakArchive>::failure(std::move(message));
}

std::optional<std::string> locate_language_pak(const IsoVolume& volume,
                                               std::string_view override_path) {
    const auto present = [&](std::string_view path) {
        const FsNode* node = volume.find(path);
        return node != nullptr && !node->directory && !node->outside_data_track;
    };
    if (!override_path.empty()) {
        if (present(override_path)) {
            return std::string(override_path);
        }
        return std::nullopt;
    }
    for (const std::string_view candidate : kLanguagePakCandidates) {
        if (present(candidate)) {
            return std::string(candidate);
        }
    }
    return std::nullopt;
}

} // namespace oscilline
