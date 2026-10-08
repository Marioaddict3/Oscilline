// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Primary volume descriptor and directory records.

#include "oscilline/disc/iso9660.hpp"

#include "oscilline/byte_reader.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace oscilline {
namespace {

constexpr std::uint32_t kMaxDirectoryBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxNodes = 100000;
constexpr int kMaxDepth = 32;

std::string trim_right(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) {
        text.pop_back();
    }
    return text;
}

std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

std::string normalize_path(std::string_view path) {
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    return ascii_lower(path);
}

struct ParsedRecord {
    std::uint32_t lba = 0;
    std::uint32_t size = 0;
    std::uint8_t flags = 0;
    std::string name;
    bool dot_entry = false;
    XaAttributes xa;
};

Result<ParsedRecord> parse_record(std::span<const std::uint8_t> bytes) {
    ByteReader reader(bytes);
    const std::uint8_t length = reader.u8();
    if (!reader.ok() || length != bytes.size() || length < 33) {
        return Result<ParsedRecord>::failure("directory record length is invalid");
    }
    reader.u8();
    ParsedRecord record;
    record.lba = reader.u32both();
    record.size = reader.u32both();
    reader.skip(7);
    record.flags = reader.u8();
    reader.u8();
    reader.u8();
    reader.u16both();
    const std::uint8_t name_length = reader.u8();
    const auto name_bytes = reader.bytes(name_length);
    if (!reader.ok()) {
        return Result<ParsedRecord>::failure(
            "directory record is truncated or has mismatched both-endian fields");
    }
    if (name_length == 1 && (name_bytes[0] == 0x00 || name_bytes[0] == 0x01)) {
        record.dot_entry = true;
    } else {
        record.name.assign(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
        const auto version = record.name.find(';');
        if (version != std::string::npos) {
            record.name.resize(version);
        }
        if (record.name.empty() || record.name == "." || record.name == ".." ||
            record.name.find('/') != std::string::npos ||
            record.name.find('\\') != std::string::npos ||
            record.name.find('\0') != std::string::npos) {
            return Result<ParsedRecord>::failure("directory record name is not usable");
        }
    }
    if ((name_length % 2) == 0) {
        reader.u8();
    }
    if (!reader.ok()) {
        return Result<ParsedRecord>::failure("directory record padding is missing");
    }
    if (reader.remaining() >= 14) {
        const auto system_use = reader.bytes(14);
        if (reader.ok() && system_use.size() == 14 && system_use[6] == 'X' &&
            system_use[7] == 'A') {
            record.xa.present = true;
            const auto be16 = [](std::uint8_t hi, std::uint8_t lo) {
                return static_cast<std::uint16_t>((static_cast<std::uint32_t>(hi) << 8) |
                                                  static_cast<std::uint32_t>(lo));
            };
            record.xa.owner_group = be16(system_use[0], system_use[1]);
            record.xa.owner_user = be16(system_use[2], system_use[3]);
            record.xa.attributes = be16(system_use[4], system_use[5]);
            record.xa.file_number = system_use[8];
        }
    }
    return Result<ParsedRecord>::success(std::move(record));
}

// ISO 9660 volume space counts every sector on the disc. On a mixed-mode disc
// that includes the CD-DA tracks, so it can be larger than the data track.
// A directory record whose extent sits past the data track is kept and marked;
// it is not read as a 2048-byte sector.
bool record_points_past_data_track(const DiscImage& image, std::uint32_t lba, std::uint32_t size) {
    const std::uint32_t data_sectors = image.logical_sector_count();
    if (size == 0) {
        return lba > data_sectors;
    }
    const std::uint64_t sectors_needed = (static_cast<std::uint64_t>(size) + 2047u) / 2048u;
    return static_cast<std::uint64_t>(lba) > data_sectors ||
           static_cast<std::uint64_t>(lba) + sectors_needed > data_sectors;
}

Result<std::vector<std::uint8_t>> read_extent(const DiscImage& image,
                                              std::uint32_t lba,
                                              std::uint32_t size,
                                              std::uint32_t volume_sectors) {
    if (size == 0) {
        return Result<std::vector<std::uint8_t>>::success({});
    }
    const std::uint64_t volume_bytes = static_cast<std::uint64_t>(volume_sectors) * 2048u;
    if (static_cast<std::uint64_t>(size) > volume_bytes) {
        return Result<std::vector<std::uint8_t>>::failure("extent is larger than the volume");
    }
    const std::uint64_t sectors_needed = (static_cast<std::uint64_t>(size) + 2047u) / 2048u;
    if (static_cast<std::uint64_t>(lba) + sectors_needed > image.logical_sector_count()) {
        return Result<std::vector<std::uint8_t>>::failure("extent runs past the data track");
    }
    std::vector<std::uint8_t> out;
    out.reserve(size);
    std::uint32_t left = size;
    std::uint32_t sector = lba;
    while (left > 0) {
        auto chunk = image.read_logical_sector(sector);
        if (!chunk) {
            return chunk;
        }
        const std::uint32_t take = std::min<std::uint32_t>(left, 2048);
        out.insert(out.end(),
                   chunk.value().begin(),
                   chunk.value().begin() + static_cast<std::ptrdiff_t>(take));
        left -= take;
        ++sector;
    }
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}

} // namespace

struct IsoVolume::Impl {
    DiscImage image;
    std::string volume_id;
    std::uint32_t volume_space = 0;
    std::uint32_t block_size = 0;
    std::uint32_t path_table_lba = 0;
    std::vector<FsNode> nodes;
};

IsoVolume::IsoVolume() = default;
IsoVolume::IsoVolume(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
IsoVolume::IsoVolume(IsoVolume&&) noexcept = default;
IsoVolume& IsoVolume::operator=(IsoVolume&&) noexcept = default;
IsoVolume::IsoVolume(const IsoVolume&) = default;
IsoVolume& IsoVolume::operator=(const IsoVolume&) = default;
IsoVolume::~IsoVolume() = default;

Result<IsoVolume> IsoVolume::read(const DiscImage& image) {
    auto sector = image.read_logical_sector(16);
    if (!sector) {
        return Result<IsoVolume>::failure(sector.error());
    }
    ByteReader reader(sector.value());
    const std::uint8_t type = reader.u8();
    const std::string magic = reader.read_string(5);
    const std::uint8_t version = reader.u8();
    if (!reader.ok() || type != 0x01 || magic != "CD001" || version != 0x01) {
        return Result<IsoVolume>::failure("sector 16 is not an ISO 9660 primary volume descriptor");
    }
    reader.u8();
    reader.read_string(32);
    std::string volume_id = trim_right(reader.read_string(32));
    reader.skip(8);
    const std::uint32_t volume_space = reader.u32both();
    reader.skip(32);
    reader.u16both();
    reader.u16both();
    const std::uint16_t block_size = reader.u16both();
    reader.u32both();
    const std::uint32_t path_table_lba = reader.u32();
    if (!reader.ok()) {
        return Result<IsoVolume>::failure("primary volume descriptor is truncated or inconsistent");
    }
    if (block_size != 2048) {
        return Result<IsoVolume>::failure("only 2048-byte logical blocks are supported");
    }
    if (volume_space < 17) {
        return Result<IsoVolume>::failure(
            "volume space size does not cover the volume descriptors");
    }

    reader.seek(0x9C);
    const auto root_bytes = reader.bytes(34);
    if (!reader.ok()) {
        return Result<IsoVolume>::failure("primary volume descriptor has no root directory record");
    }
    auto root_record = parse_record(root_bytes);
    if (!root_record) {
        return Result<IsoVolume>::failure(root_record.error());
    }
    if ((root_record.value().flags & 0x02) == 0) {
        return Result<IsoVolume>::failure("root directory record is not a directory");
    }
    if (record_points_past_data_track(image, root_record.value().lba, root_record.value().size)) {
        return Result<IsoVolume>::failure("directory record '/' points past the data track");
    }

    auto impl = std::make_shared<Impl>();
    impl->image = image;
    impl->volume_id = std::move(volume_id);
    impl->volume_space = volume_space;
    impl->block_size = block_size;
    impl->path_table_lba = path_table_lba;

    FsNode root_node;
    root_node.path = "/";
    root_node.directory = true;
    root_node.lba = root_record.value().lba;
    root_node.size = root_record.value().size;
    root_node.flags = root_record.value().flags;
    impl->nodes.push_back(root_node);

    std::set<std::uint32_t> seen;
    const auto walk = [&](auto&& self,
                          std::uint32_t lba,
                          std::uint32_t size,
                          const std::string& parent,
                          int depth) -> Result<bool> {
        if (depth > kMaxDepth) {
            return Result<bool>::failure("directory tree is too deep");
        }
        if (!seen.insert(lba).second) {
            return Result<bool>::failure("directory extent repeats");
        }
        if (size > kMaxDirectoryBytes) {
            return Result<bool>::failure("directory is larger than the supported limit");
        }
        auto bytes = read_extent(image, lba, size, volume_space);
        if (!bytes) {
            return Result<bool>::failure(bytes.error());
        }
        std::size_t offset = 0;
        while (offset < bytes.value().size()) {
            if (bytes.value()[offset] == 0) {
                const std::size_t next = (offset + 2048u) & ~std::size_t{2047};
                if (next <= offset || next > bytes.value().size()) {
                    break;
                }
                offset = next;
                continue;
            }
            const std::uint8_t length = bytes.value()[offset];
            if (length < 33 || offset + length > bytes.value().size()) {
                return Result<bool>::failure("directory record runs past the directory extent");
            }
            auto record =
                parse_record(std::span<const std::uint8_t>(bytes.value()).subspan(offset, length));
            if (!record) {
                return Result<bool>::failure(record.error());
            }
            offset += length;
            if (record.value().dot_entry) {
                continue;
            }
            if (impl->nodes.size() >= kMaxNodes) {
                return Result<bool>::failure("directory tree has too many entries");
            }
            const bool is_dir = (record.value().flags & 0x02) != 0;
            const std::string path =
                parent.empty() ? "/" + record.value().name : parent + "/" + record.value().name;
            FsNode node;
            node.path = path;
            node.directory = is_dir;
            node.outside_data_track =
                record_points_past_data_track(image, record.value().lba, record.value().size);
            node.lba = record.value().lba;
            node.size = record.value().size;
            node.flags = record.value().flags;
            node.xa = record.value().xa;
            impl->nodes.push_back(node);
            if (is_dir && !node.outside_data_track) {
                auto nested = self(self, node.lba, node.size, node.path, depth + 1);
                if (!nested) {
                    return nested;
                }
            }
        }
        return Result<bool>::success(true);
    };

    auto walked = walk(walk, root_node.lba, root_node.size, "", 0);
    if (!walked) {
        return Result<IsoVolume>::failure(walked.error());
    }
    return Result<IsoVolume>::success(IsoVolume{std::move(impl)});
}

const std::string& IsoVolume::volume_id() const {
    return impl_->volume_id;
}

std::uint32_t IsoVolume::volume_space_sectors() const {
    return impl_->volume_space;
}

std::uint32_t IsoVolume::logical_block_size() const {
    return impl_->block_size;
}

std::uint32_t IsoVolume::path_table_lba() const {
    return impl_->path_table_lba;
}

const std::vector<FsNode>& IsoVolume::nodes() const {
    return impl_->nodes;
}

const FsNode* IsoVolume::find(std::string_view path) const {
    if (!impl_) {
        return nullptr;
    }
    const std::string wanted = normalize_path(path);
    for (const FsNode& node : impl_->nodes) {
        if (normalize_path(node.path) == wanted) {
            return &node;
        }
    }
    return nullptr;
}

Result<std::vector<std::uint8_t>> IsoVolume::read_file(const FsNode& node) const {
    if (!impl_) {
        return Result<std::vector<std::uint8_t>>::failure("volume is empty");
    }
    if (node.directory) {
        return Result<std::vector<std::uint8_t>>::failure("'" + node.path + "' is a directory");
    }
    if (node.outside_data_track) {
        return Result<std::vector<std::uint8_t>>::failure("'" + node.path +
                                                          "' points past the data track");
    }
    return read_extent(impl_->image, node.lba, node.size, impl_->volume_space);
}

} // namespace oscilline
