// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Builds a tiny ISO 9660 image for tests. No disc bytes.

#pragma once

// Builds a tiny ISO 9660 image (and raw Mode 1 / Mode 2 wrappers) for tests.
// The bytes follow psx-spx "CDROM ISO Volume Descriptors", "CDROM ISO File and
// Directory Descriptors", and "CDROM Sector Encoding". Nothing here is copied
// from a disc.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline::testutil {

struct IsoNode {
    std::string name;
    bool directory = false;
    std::vector<std::uint8_t> data;
    std::uint16_t xa_attributes = 0x0D55;
    std::vector<IsoNode> children;
};

inline void write_u16_both(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFF);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>(value & 0xFF);
}

inline void write_u32_both(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFF);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    out[4] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    out[5] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[6] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[7] = static_cast<std::uint8_t>(value & 0xFF);
}

inline std::uint8_t to_bcd(int value) {
    return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

inline std::size_t record_length(std::size_t name_length) {
    const std::size_t pad = (name_length % 2) == 0 ? 1 : 0;
    return 33 + name_length + pad + 14;
}

inline std::size_t directory_bytes(const IsoNode& directory) {
    std::size_t offset = 0;
    const auto place = [&](std::size_t length) {
        if ((offset % 2048) != 0 && (offset % 2048) + length > 2048) {
            offset = (offset + 2047) & ~std::size_t{2047};
        }
        offset += length;
    };
    place(record_length(1));
    place(record_length(1));
    for (const IsoNode& child : directory.children) {
        const std::string stored = child.directory ? child.name : child.name + ";1";
        place(record_length(stored.size()));
    }
    return (offset + 2047) & ~std::size_t{2047};
}

struct PlacedNode {
    const IsoNode* source = nullptr;
    bool directory = false;
    std::uint32_t lba = 0;
    std::uint32_t size = 0;
    std::vector<PlacedNode> children;
};

inline void
assign_extents(const IsoNode& node, PlacedNode& placed, std::uint32_t& next, bool as_directory) {
    placed.source = &node;
    placed.directory = as_directory;
    if (as_directory) {
        placed.size = static_cast<std::uint32_t>(directory_bytes(node));
        placed.lba = next;
        next += placed.size / 2048u;
        placed.children.resize(node.children.size());
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            const bool child_dir = node.children[i].directory;
            assign_extents(node.children[i], placed.children[i], next, child_dir);
        }
        return;
    }
    placed.size = static_cast<std::uint32_t>(node.data.size());
    if (placed.size == 0) {
        placed.lba = 0;
        return;
    }
    placed.lba = next;
    next += (placed.size + 2047u) / 2048u;
}

inline void append_record(std::vector<std::uint8_t>& out,
                          std::uint32_t lba,
                          std::uint32_t size,
                          std::uint8_t flags,
                          const std::vector<std::uint8_t>& name,
                          std::uint16_t xa_attributes) {
    const std::size_t length = record_length(name.size());
    if ((out.size() % 2048) != 0 && (out.size() % 2048) + length > 2048) {
        out.resize((out.size() + 2047) & ~std::size_t{2047}, 0);
    }
    const std::size_t start = out.size();
    out.resize(start + length, 0);
    std::uint8_t* record = out.data() + start;
    record[0] = static_cast<std::uint8_t>(length);
    write_u32_both(record + 2, lba);
    write_u32_both(record + 10, size);
    record[25] = flags;
    write_u16_both(record + 28, 1);
    record[32] = static_cast<std::uint8_t>(name.size());
    if (!name.empty()) {
        std::memcpy(record + 33, name.data(), name.size());
    }
    const std::size_t pad = (name.size() % 2) == 0 ? 1 : 0;
    std::uint8_t* system_use = record + 33 + name.size() + pad;
    system_use[4] = static_cast<std::uint8_t>((xa_attributes >> 8) & 0xFF);
    system_use[5] = static_cast<std::uint8_t>(xa_attributes & 0xFF);
    system_use[6] = 'X';
    system_use[7] = 'A';
}

inline std::vector<std::uint8_t> build_directory(const PlacedNode& placed,
                                                 std::uint32_t parent_lba) {
    std::vector<std::uint8_t> bytes;
    append_record(bytes, placed.lba, placed.size, 0x02, {0x00}, 0x8D55);
    append_record(bytes, parent_lba, placed.size, 0x02, {0x01}, 0x8D55);
    for (const PlacedNode& child : placed.children) {
        std::string stored = child.source->name;
        if (!child.directory) {
            stored += ";1";
        }
        const std::uint8_t flags = child.directory ? 0x02 : 0x00;
        const std::uint16_t xa = child.directory ? 0x8D55 : child.source->xa_attributes;
        append_record(bytes,
                      child.lba,
                      child.size,
                      flags,
                      std::vector<std::uint8_t>(stored.begin(), stored.end()),
                      xa);
    }
    bytes.resize(placed.size, 0);
    return bytes;
}

inline void
write_tree(std::vector<std::uint8_t>& image, const PlacedNode& placed, std::uint32_t parent_lba) {
    if (placed.directory) {
        const auto bytes = build_directory(placed, parent_lba);
        std::memcpy(
            image.data() + static_cast<std::size_t>(placed.lba) * 2048, bytes.data(), bytes.size());
        for (const PlacedNode& child : placed.children) {
            write_tree(image, child, placed.lba);
        }
        return;
    }
    if (placed.size == 0 || placed.source == nullptr) {
        return;
    }
    std::memcpy(image.data() + static_cast<std::size_t>(placed.lba) * 2048,
                placed.source->data.data(),
                placed.source->data.size());
}

inline std::vector<std::uint8_t> build_iso(const IsoNode& root, const std::string& volume_id) {
    PlacedNode placed;
    std::uint32_t next = 18;
    assign_extents(root, placed, next, true);
    std::vector<std::uint8_t> image(static_cast<std::size_t>(next) * 2048, 0);

    std::uint8_t* pvd = image.data() + 16 * 2048;
    pvd[0] = 0x01;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 0x01;
    const char system_id[] = "PLAYSTATION";
    std::memcpy(pvd + 8, system_id, sizeof(system_id) - 1);
    std::memset(pvd + 8 + sizeof(system_id) - 1, ' ', 32 - (sizeof(system_id) - 1));
    std::memset(pvd + 0x28, ' ', 32);
    std::memcpy(pvd + 0x28, volume_id.data(), std::min(volume_id.size(), std::size_t{32}));
    write_u32_both(pvd + 0x50, next);
    write_u16_both(pvd + 0x78, 1);
    write_u16_both(pvd + 0x7C, 1);
    write_u16_both(pvd + 0x80, 2048);
    pvd[0x9C] = 34;
    write_u32_both(pvd + 0x9C + 2, placed.lba);
    write_u32_both(pvd + 0x9C + 10, placed.size);
    pvd[0x9C + 25] = 0x02;
    write_u16_both(pvd + 0x9C + 28, 1);
    pvd[0x9C + 32] = 1;
    pvd[0x9C + 33] = 0x00;

    std::uint8_t* terminator = image.data() + 17 * 2048;
    terminator[0] = 0xFF;
    std::memcpy(terminator + 1, "CD001", 5);
    terminator[6] = 0x01;

    write_tree(image, placed, placed.lba);
    return image;
}

inline std::vector<std::uint8_t> wrap_raw_sectors(const std::vector<std::uint8_t>& iso,
                                                  std::uint8_t mode) {
    const std::size_t sectors = iso.size() / 2048;
    std::vector<std::uint8_t> raw(sectors * 2352, 0);
    for (std::size_t index = 0; index < sectors; ++index) {
        std::uint8_t* sector = raw.data() + index * 2352;
        sector[0] = 0x00;
        std::memset(sector + 1, 0xFF, 10);
        sector[11] = 0x00;
        const int minute = static_cast<int>(index / (75 * 60));
        const int second = static_cast<int>((index / 75) % 60);
        const int frame = static_cast<int>(index % 75);
        sector[12] = to_bcd(minute);
        sector[13] = to_bcd(second);
        sector[14] = to_bcd(frame);
        sector[15] = mode;
        if (mode == 1) {
            std::memcpy(sector + 16, iso.data() + index * 2048, 2048);
        } else {
            std::memcpy(sector + 20, sector + 16, 4);
            std::memcpy(sector + 24, iso.data() + index * 2048, 2048);
        }
    }
    return raw;
}

inline void
append_cdda(std::vector<std::uint8_t>& image, std::uint32_t sectors, std::int16_t left) {
    const std::size_t start = image.size();
    image.resize(start + static_cast<std::size_t>(sectors) * 2352, 0);
    for (std::uint32_t sector = 0; sector < sectors; ++sector) {
        std::uint8_t* cursor = image.data() + start + static_cast<std::size_t>(sector) * 2352;
        for (int frame = 0; frame < 588; ++frame) {
            const std::int16_t right = static_cast<std::int16_t>(frame);
            cursor[0] = static_cast<std::uint8_t>(left & 0xFF);
            cursor[1] = static_cast<std::uint8_t>((left >> 8) & 0xFF);
            cursor[2] = static_cast<std::uint8_t>(right & 0xFF);
            cursor[3] = static_cast<std::uint8_t>((right >> 8) & 0xFF);
            cursor += 4;
        }
    }
}

inline std::string msf(std::uint32_t sectors) {
    const std::uint32_t minutes = sectors / (75u * 60u);
    const std::uint32_t seconds = (sectors / 75u) % 60u;
    const std::uint32_t frames = sectors % 75u;
    auto two = [](std::uint32_t value) {
        std::string text = std::to_string(value);
        if (text.size() < 2) {
            text.insert(text.begin(), '0');
        }
        return text;
    };
    return two(minutes) + ":" + two(seconds) + ":" + two(frames);
}

inline std::vector<std::uint8_t> bytes_from(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

} // namespace oscilline::testutil
