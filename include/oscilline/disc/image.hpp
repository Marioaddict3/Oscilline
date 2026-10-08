// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Opens a 2048-byte ISO or a BIN/CUE image.

#pragma once

#include "oscilline/disc/sector.hpp"
#include "oscilline/result.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oscilline {

struct AudioTrackInfo {
    int number = 0;
    std::uint32_t sector_count = 0;
};

// A disc image that can serve 2048-byte logical sectors (the ISO filesystem)
// and raw 2352-byte CD-DA sectors from audio tracks.
class DiscImage {
  public:
    DiscImage();
    DiscImage(DiscImage&&) noexcept;
    DiscImage& operator=(DiscImage&&) noexcept;
    DiscImage(const DiscImage&);
    DiscImage& operator=(const DiscImage&);
    ~DiscImage();

    // .iso (2048-byte sectors), .cue (BIN/CUE, including multi-file cues),
    // or a .bin that has a sibling cue sheet.
    [[nodiscard]] static Result<DiscImage> open(const std::filesystem::path& path);

    // In-memory images used by tests and by anything that already holds the bytes.
    [[nodiscard]] static Result<DiscImage> open_iso_bytes(std::vector<std::uint8_t> bytes);
    [[nodiscard]] static Result<DiscImage>
    open_cue_bytes(std::string cue_text, std::map<std::string, std::vector<std::uint8_t>> files);

    [[nodiscard]] std::uint32_t logical_sector_count() const;
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_logical_sector(std::uint32_t lba) const;

    [[nodiscard]] const std::vector<AudioTrackInfo>& audio_tracks() const;
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_audio_sector(int track_number,
                                                                      std::uint32_t index) const;

  private:
    struct Impl;
    friend struct ImageBuilder;
    explicit DiscImage(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace oscilline
