// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Reads ISO 9660 directories and file extents.

#pragma once

#include "oscilline/disc/image.hpp"
#include "oscilline/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

// CD-XA system-use field on a directory record (14 bytes, signature "XA").
// psx-spx "CDROM ISO File and Directory Descriptors".
struct XaAttributes {
    bool present = false;
    std::uint16_t owner_group = 0;
    std::uint16_t owner_user = 0;
    std::uint16_t attributes = 0;
    std::uint8_t file_number = 0;

    [[nodiscard]] bool is_mode2() const { return (attributes & (1u << 11)) != 0; }
    [[nodiscard]] bool is_form2() const { return (attributes & (1u << 12)) != 0; }
    [[nodiscard]] bool is_interleaved() const { return (attributes & (1u << 13)) != 0; }
    [[nodiscard]] bool is_cdda() const { return (attributes & (1u << 14)) != 0; }
    [[nodiscard]] bool is_directory() const { return (attributes & (1u << 15)) != 0; }
};

struct FsNode {
    std::string path;
    bool directory = false;
    // Mixed-mode discs list CD-DA files in ISO 9660 with extents past the data
    // track. Those entries stay in the tree and are not read as 2048-byte sectors.
    bool outside_data_track = false;
    std::uint32_t lba = 0;
    std::uint32_t size = 0;
    std::uint8_t flags = 0;
    XaAttributes xa;
};

class IsoVolume {
  public:
    IsoVolume();
    IsoVolume(IsoVolume&&) noexcept;
    IsoVolume& operator=(IsoVolume&&) noexcept;
    IsoVolume(const IsoVolume&);
    IsoVolume& operator=(const IsoVolume&);
    ~IsoVolume();

    [[nodiscard]] static Result<IsoVolume> read(const DiscImage& image);

    [[nodiscard]] const std::string& volume_id() const;
    [[nodiscard]] std::uint32_t volume_space_sectors() const;
    [[nodiscard]] std::uint32_t logical_block_size() const;
    [[nodiscard]] std::uint32_t path_table_lba() const;
    [[nodiscard]] const std::vector<FsNode>& nodes() const;

    // Case-insensitive. Accepts "SYSTEM.CNF" and "/SYSTEM.CNF".
    [[nodiscard]] const FsNode* find(std::string_view path) const;
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const FsNode& node) const;

  private:
    struct Impl;
    explicit IsoVolume(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace oscilline
