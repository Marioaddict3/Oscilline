// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Opens an ISO or a CUE and reads sectors inside the data track.

#include "oscilline/disc/image.hpp"

#include "oscilline/disc/cue.hpp"

#include <cctype>
#include <fstream>
#include <mutex>
#include <utility>

namespace oscilline {
namespace {

constexpr std::uint32_t kLogicalSector = 2048;
constexpr std::uint64_t kMaxCueBytes = 1u << 20;

class ByteSource {
  public:
    virtual ~ByteSource() = default;
    [[nodiscard]] virtual std::uint64_t size() const = 0;
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> read(std::uint64_t offset,
                                                                 std::size_t count) const = 0;
};

class MemorySource final : public ByteSource {
  public:
    explicit MemorySource(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    [[nodiscard]] std::uint64_t size() const override { return data_.size(); }

    [[nodiscard]] Result<std::vector<std::uint8_t>> read(std::uint64_t offset,
                                                         std::size_t count) const override {
        if (offset > data_.size() || count > data_.size() - offset) {
            return Result<std::vector<std::uint8_t>>::failure("read past end of image");
        }
        const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(offset);
        return Result<std::vector<std::uint8_t>>::success(
            std::vector<std::uint8_t>(begin, begin + static_cast<std::ptrdiff_t>(count)));
    }

  private:
    std::vector<std::uint8_t> data_;
};

class FileSource final : public ByteSource {
  public:
    FileSource(std::ifstream in, std::uint64_t size) : in_(std::move(in)), size_(size) {}

    [[nodiscard]] std::uint64_t size() const override { return size_; }

    [[nodiscard]] Result<std::vector<std::uint8_t>> read(std::uint64_t offset,
                                                         std::size_t count) const override {
        if (offset > size_ || count > size_ - offset) {
            return Result<std::vector<std::uint8_t>>::failure("read past end of image");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(offset));
        if (!in_) {
            return Result<std::vector<std::uint8_t>>::failure("failed to seek in image");
        }
        std::vector<std::uint8_t> buffer(count);
        in_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
        if (static_cast<std::size_t>(in_.gcount()) != count) {
            return Result<std::vector<std::uint8_t>>::failure("short read from image");
        }
        return Result<std::vector<std::uint8_t>>::success(std::move(buffer));
    }

  private:
    mutable std::ifstream in_;
    std::uint64_t size_ = 0;
    mutable std::mutex mutex_;
};

Result<std::unique_ptr<ByteSource>> open_file_source(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<std::unique_ptr<ByteSource>>::failure("cannot open '" + path.string() + "'");
    }
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) {
        return Result<std::unique_ptr<ByteSource>>::failure("cannot size '" + path.string() + "'");
    }
    in.seekg(0, std::ios::beg);
    return Result<std::unique_ptr<ByteSource>>::success(
        std::make_unique<FileSource>(std::move(in), static_cast<std::uint64_t>(end)));
}

bool looks_like_pvd(std::span<const std::uint8_t> sector) {
    return sector.size() >= 6 && sector[0] == 0x01 && sector[1] == 'C' && sector[2] == 'D' &&
           sector[3] == '0' && sector[4] == '0' && sector[5] == '1';
}

std::string ascii_lower_ext(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& ch : ext) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ext;
}

const CueIndex* find_index(const CueTrack& track, int number) {
    for (const CueIndex& index : track.indices) {
        if (index.number == number) {
            return &index;
        }
    }
    return nullptr;
}

const CueIndex* earliest_index(const CueTrack& track) {
    const CueIndex* best = nullptr;
    for (const CueIndex& index : track.indices) {
        if (best == nullptr || index.as_sectors() < best->as_sectors()) {
            best = &index;
        }
    }
    return best;
}

struct ResolvedTrack {
    int number = 0;
    RawSectorKind kind = RawSectorKind::Audio2352;
    std::size_t source_index = 0;
    std::uint64_t index1_offset = 0;
    std::uint32_t sector_count = 0;
};

} // namespace

struct DiscImage::Impl {
    std::vector<std::shared_ptr<ByteSource>> sources;
    std::vector<ResolvedTrack> tracks;
    std::size_t data_track = 0;
    std::vector<AudioTrackInfo> audio;
};

DiscImage::DiscImage() = default;
DiscImage::DiscImage(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
DiscImage::DiscImage(DiscImage&&) noexcept = default;
DiscImage& DiscImage::operator=(DiscImage&&) noexcept = default;
DiscImage::DiscImage(const DiscImage&) = default;
DiscImage& DiscImage::operator=(const DiscImage&) = default;
DiscImage::~DiscImage() = default;

struct ImageBuilder {
    static Result<DiscImage> image_from_sources(std::vector<std::shared_ptr<ByteSource>> sources,
                                                const CueSheet& sheet,
                                                const std::vector<std::string>& file_order);
};

Result<DiscImage> ImageBuilder::image_from_sources(std::vector<std::shared_ptr<ByteSource>> sources,
                                                   const CueSheet& sheet,
                                                   const std::vector<std::string>& file_order) {
    if (sources.size() != file_order.size()) {
        return Result<DiscImage>::failure("internal cue file mismatch");
    }
    auto impl = std::make_shared<DiscImage::Impl>();
    impl->sources = std::move(sources);

    std::vector<std::size_t> track_source(sheet.tracks.size());
    for (std::size_t i = 0; i < sheet.tracks.size(); ++i) {
        bool found = false;
        for (std::size_t file = 0; file < file_order.size(); ++file) {
            if (file_order[file] == sheet.tracks[i].file_name) {
                track_source[i] = file;
                found = true;
                break;
            }
        }
        if (!found) {
            return Result<DiscImage>::failure("cue track references unknown file '" +
                                              sheet.tracks[i].file_name + "'");
        }
    }

    for (std::size_t file = 0; file < file_order.size(); ++file) {
        RawSectorKind kind = RawSectorKind::Audio2352;
        bool have_kind = false;
        for (std::size_t i = 0; i < sheet.tracks.size(); ++i) {
            if (track_source[i] != file) {
                continue;
            }
            if (!have_kind) {
                kind = sheet.tracks[i].kind;
                have_kind = true;
            } else if (sector_stride(kind) != sector_stride(sheet.tracks[i].kind)) {
                return Result<DiscImage>::failure("cue file '" + file_order[file] +
                                                  "' mixes sector sizes");
            }
        }
        const std::uint32_t stride = sector_stride(kind);
        if (stride == 0 || impl->sources[file]->size() % stride != 0) {
            return Result<DiscImage>::failure("cue file '" + file_order[file] +
                                              "' size is not a multiple of its sector size");
        }
    }

    bool saw_data = false;
    for (std::size_t i = 0; i < sheet.tracks.size(); ++i) {
        const CueTrack& track = sheet.tracks[i];
        const CueIndex* index1 = find_index(track, 1);
        if (index1 == nullptr) {
            return Result<DiscImage>::failure("cue track " + std::to_string(track.number) +
                                              " has no INDEX 01");
        }
        const std::uint32_t stride = sector_stride(track.kind);
        const std::uint64_t start =
            static_cast<std::uint64_t>(index1->as_sectors()) * static_cast<std::uint64_t>(stride);
        const std::uint64_t file_size = impl->sources[track_source[i]]->size();
        if (start > file_size) {
            return Result<DiscImage>::failure("cue track " + std::to_string(track.number) +
                                              " INDEX 01 is past the end of the file");
        }

        std::uint64_t end = file_size;
        for (std::size_t next = i + 1; next < sheet.tracks.size(); ++next) {
            if (track_source[next] != track_source[i]) {
                continue;
            }
            const CueIndex* boundary = earliest_index(sheet.tracks[next]);
            if (boundary == nullptr) {
                return Result<DiscImage>::failure("cue track is missing indexes");
            }
            end = static_cast<std::uint64_t>(boundary->as_sectors()) * stride;
            break;
        }
        if (end < start || (end - start) % stride != 0) {
            return Result<DiscImage>::failure("cue track " + std::to_string(track.number) +
                                              " has a misaligned range");
        }
        ResolvedTrack resolved;
        resolved.number = track.number;
        resolved.kind = track.kind;
        resolved.source_index = track_source[i];
        resolved.index1_offset = start;
        resolved.sector_count = static_cast<std::uint32_t>((end - start) / stride);
        if (track.kind == RawSectorKind::Audio2352) {
            impl->audio.push_back(AudioTrackInfo{track.number, resolved.sector_count});
        } else if (!saw_data) {
            impl->data_track = impl->tracks.size();
            saw_data = true;
        }
        impl->tracks.push_back(resolved);
    }
    if (!saw_data) {
        return Result<DiscImage>::failure("cue sheet has no data track");
    }
    return Result<DiscImage>::success(DiscImage{std::move(impl)});
}

namespace {

Result<std::string> read_text_file(const std::filesystem::path& path) {
    auto source = open_file_source(path);
    if (!source) {
        return Result<std::string>::failure(source.error());
    }
    if (source.value()->size() > kMaxCueBytes) {
        return Result<std::string>::failure("cue sheet '" + path.string() +
                                            "' is unexpectedly large");
    }
    auto bytes = source.value()->read(0, static_cast<std::size_t>(source.value()->size()));
    if (!bytes) {
        return Result<std::string>::failure(bytes.error());
    }
    return Result<std::string>::success(
        std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
}

std::filesystem::path resolve_cue_file(const std::filesystem::path& cue_path,
                                       const std::string& file_name) {
    std::string normalized = file_name;
    for (char& ch : normalized) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    const std::filesystem::path relative{normalized};
    if (relative.is_absolute()) {
        return relative;
    }
    return cue_path.parent_path() / relative;
}

Result<DiscImage> open_cue_path(const std::filesystem::path& cue_path) {
    auto text = read_text_file(cue_path);
    if (!text) {
        return Result<DiscImage>::failure(text.error());
    }
    auto sheet = parse_cue(text.value());
    if (!sheet) {
        return Result<DiscImage>::failure(sheet.error());
    }
    std::vector<std::string> file_order;
    std::vector<std::shared_ptr<ByteSource>> sources;
    for (const CueTrack& track : sheet.value().tracks) {
        bool seen = false;
        for (const std::string& name : file_order) {
            if (name == track.file_name) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        const auto path = resolve_cue_file(cue_path, track.file_name);
        auto source = open_file_source(path);
        if (!source) {
            return Result<DiscImage>::failure(source.error());
        }
        file_order.push_back(track.file_name);
        sources.push_back(std::shared_ptr<ByteSource>(std::move(source.value())));
    }
    return ImageBuilder::image_from_sources(std::move(sources), sheet.value(), file_order);
}

Result<DiscImage> open_iso_source(std::shared_ptr<ByteSource> source) {
    if (source->size() % kLogicalSector != 0) {
        return Result<DiscImage>::failure("ISO image size is not a multiple of 2048");
    }
    if (source->size() / kLogicalSector < 17) {
        return Result<DiscImage>::failure("ISO image is too small to contain a volume descriptor");
    }
    auto sector = source->read(16ull * kLogicalSector, kLogicalSector);
    if (!sector) {
        return Result<DiscImage>::failure(sector.error());
    }
    if (!looks_like_pvd(sector.value())) {
        return Result<DiscImage>::failure(
            "ISO image has no primary volume descriptor at sector 16");
    }
    CueSheet sheet;
    CueTrack track;
    track.number = 1;
    track.kind = RawSectorKind::Mode1_2048;
    track.file_name = "image";
    track.file_type = "BINARY";
    track.indices.push_back(CueIndex{1, 0, 0, 0});
    sheet.tracks.push_back(std::move(track));
    std::vector<std::shared_ptr<ByteSource>> sources;
    sources.push_back(std::move(source));
    return ImageBuilder::image_from_sources(std::move(sources), sheet, {"image"});
}

bool sector_has_pvd(const std::vector<std::uint8_t>& raw, RawSectorKind kind) {
    auto payload = extract_user_payload(raw, kind);
    return payload && !payload.value().form2 && looks_like_pvd(payload.value().bytes);
}

Result<DiscImage> open_raw_bin(const std::filesystem::path& path) {
    auto source_result = open_file_source(path);
    if (!source_result) {
        return Result<DiscImage>::failure(source_result.error());
    }
    std::shared_ptr<ByteSource> source(std::move(source_result.value()));
    if (source->size() % 2352 != 0) {
        return Result<DiscImage>::failure(
            "BIN image size is not a multiple of 2352 and no cue sheet was found");
    }
    const std::uint64_t sectors = source->size() / 2352;
    const RawSectorKind candidates[] = {RawSectorKind::Mode2_2352, RawSectorKind::Mode1_2352};
    // Standard dumps start at 00:00:00 in the file (physical 00:02:00). Some dumps also
    // store the two-second pregap, so logical sector 16 may sit at file sector 166.
    const std::uint32_t origins[] = {0u, 150u};
    for (RawSectorKind kind : candidates) {
        for (std::uint32_t origin : origins) {
            const std::uint64_t lba = static_cast<std::uint64_t>(origin) + 16u;
            if (lba >= sectors) {
                continue;
            }
            auto raw = source->read(lba * 2352, 2352);
            if (!raw || !sector_has_pvd(raw.value(), kind)) {
                continue;
            }
            CueSheet sheet;
            CueTrack track;
            track.number = 1;
            track.kind = kind;
            track.file_name = "image";
            track.file_type = "BINARY";
            const int frames = static_cast<int>(origin % 75);
            const int seconds = static_cast<int>((origin / 75) % 60);
            const int minutes = static_cast<int>(origin / (75 * 60));
            if (origin != 0) {
                track.indices.push_back(CueIndex{0, 0, 0, 0});
            }
            track.indices.push_back(CueIndex{1, minutes, seconds, frames});
            sheet.tracks.push_back(std::move(track));
            std::vector<std::shared_ptr<ByteSource>> sources;
            sources.push_back(std::move(source));
            return ImageBuilder::image_from_sources(std::move(sources), sheet, {"image"});
        }
    }
    return Result<DiscImage>::failure(
        "BIN image has no cue sheet and no ISO volume descriptor in Mode 1 or Mode 2");
}

} // namespace

Result<DiscImage> DiscImage::open(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return Result<DiscImage>::failure("'" + path.string() + "' is not a file");
    }
    const std::string ext = ascii_lower_ext(path);
    if (ext == ".cue") {
        return open_cue_path(path);
    }
    if (ext == ".iso") {
        auto source = open_file_source(path);
        if (!source) {
            return Result<DiscImage>::failure(source.error());
        }
        return open_iso_source(std::shared_ptr<ByteSource>(std::move(source.value())));
    }
    if (ext == ".bin") {
        const auto cue = path.parent_path() / (path.stem().string() + ".cue");
        if (std::filesystem::is_regular_file(cue)) {
            return open_cue_path(cue);
        }
        return open_raw_bin(path);
    }
    return Result<DiscImage>::failure("unsupported image '" + path.string() +
                                      "'; expected .iso, .cue, or .bin");
}

Result<DiscImage> DiscImage::open_iso_bytes(std::vector<std::uint8_t> bytes) {
    return open_iso_source(std::make_shared<MemorySource>(std::move(bytes)));
}

Result<DiscImage>
DiscImage::open_cue_bytes(std::string cue_text,
                          std::map<std::string, std::vector<std::uint8_t>> files) {
    auto sheet = parse_cue(cue_text);
    if (!sheet) {
        return Result<DiscImage>::failure(sheet.error());
    }
    std::vector<std::string> file_order;
    std::vector<std::shared_ptr<ByteSource>> sources;
    for (const CueTrack& track : sheet.value().tracks) {
        bool seen = false;
        for (const std::string& name : file_order) {
            if (name == track.file_name) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        const auto found = files.find(track.file_name);
        if (found == files.end()) {
            return Result<DiscImage>::failure("cue references '" + track.file_name +
                                              "' which was not provided");
        }
        file_order.push_back(track.file_name);
        sources.push_back(std::make_shared<MemorySource>(std::move(found->second)));
    }
    return ImageBuilder::image_from_sources(std::move(sources), sheet.value(), file_order);
}

std::uint32_t DiscImage::logical_sector_count() const {
    if (!impl_ || impl_->tracks.empty()) {
        return 0;
    }
    return impl_->tracks[impl_->data_track].sector_count;
}

Result<std::vector<std::uint8_t>> DiscImage::read_logical_sector(std::uint32_t lba) const {
    if (!impl_ || impl_->tracks.empty()) {
        return Result<std::vector<std::uint8_t>>::failure("disc image is empty");
    }
    const ResolvedTrack& track = impl_->tracks[impl_->data_track];
    if (lba >= track.sector_count) {
        return Result<std::vector<std::uint8_t>>::failure("logical sector " + std::to_string(lba) +
                                                          " is past the data track");
    }
    const std::uint32_t stride = sector_stride(track.kind);
    const std::uint64_t offset =
        track.index1_offset + static_cast<std::uint64_t>(lba) * static_cast<std::uint64_t>(stride);
    auto raw = impl_->sources[track.source_index]->read(offset, stride);
    if (!raw) {
        return raw;
    }
    auto payload = extract_user_payload(raw.value(), track.kind);
    if (!payload) {
        return Result<std::vector<std::uint8_t>>::failure(payload.error());
    }
    if (payload.value().form2 || payload.value().bytes.size() != kLogicalSector) {
        return Result<std::vector<std::uint8_t>>::failure("logical sector " + std::to_string(lba) +
                                                          " is not a 2048-byte data sector");
    }
    return Result<std::vector<std::uint8_t>>::success(std::move(payload.value().bytes));
}

const std::vector<AudioTrackInfo>& DiscImage::audio_tracks() const {
    if (!impl_) {
        static const std::vector<AudioTrackInfo> kEmpty;
        return kEmpty;
    }
    return impl_->audio;
}

Result<std::vector<std::uint8_t>> DiscImage::read_audio_sector(int track_number,
                                                               std::uint32_t index) const {
    if (!impl_) {
        return Result<std::vector<std::uint8_t>>::failure("disc image is empty");
    }
    const ResolvedTrack* track = nullptr;
    for (const ResolvedTrack& candidate : impl_->tracks) {
        if (candidate.number == track_number && candidate.kind == RawSectorKind::Audio2352) {
            track = &candidate;
            break;
        }
    }
    if (track == nullptr) {
        return Result<std::vector<std::uint8_t>>::failure(
            "audio track " + std::to_string(track_number) + " is not in this image");
    }
    if (index >= track->sector_count) {
        return Result<std::vector<std::uint8_t>>::failure("audio sector is past the end of track " +
                                                          std::to_string(track_number));
    }
    const std::uint64_t offset = track->index1_offset + static_cast<std::uint64_t>(index) * 2352u;
    return impl_->sources[track->source_index]->read(offset, 2352);
}

} // namespace oscilline
