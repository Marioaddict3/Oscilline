// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Converts 2352-byte CD-DA sectors to interleaved stereo frames.

#include "oscilline/audio/cdda.hpp"

#include "oscilline/disc/sector.hpp"

#include <string>

namespace oscilline {

int cdda_frames_for_sectors(std::uint32_t sectors) {
    constexpr std::int64_t kCap =
        static_cast<std::int64_t>(kMaxCddaSeconds) * static_cast<std::int64_t>(kCddaRate);
    std::int64_t frames =
        static_cast<std::int64_t>(sectors) * static_cast<std::int64_t>(kCddaFramesPerSector);
    if (frames > kCap) {
        frames = kCap;
    }
    if (frames > 0x7fffffff) {
        frames = 0x7fffffff;
    }
    return static_cast<int>(frames);
}

std::int32_t cdda_duration_ms(int frames) {
    if (frames <= 0) {
        return 0;
    }
    const std::int64_t ms =
        static_cast<std::int64_t>(frames) * 1000 / static_cast<std::int64_t>(kCddaRate);
    if (ms > 0x7fffffff) {
        return 0x7fffffff;
    }
    return static_cast<std::int32_t>(ms);
}

Result<CddaTrack> read_cdda_track(const DiscImage& image, int track_number) {
    const AudioTrackInfo* info = nullptr;
    for (const AudioTrackInfo& track : image.audio_tracks()) {
        if (track.number == track_number) {
            info = &track;
            break;
        }
    }
    if (info == nullptr) {
        return Result<CddaTrack>::failure("audio track " + std::to_string(track_number) +
                                          " is not in this image");
    }

    const int frames_wanted = cdda_frames_for_sectors(info->sector_count);
    CddaTrack out;
    out.track_number = track_number;
    out.interleaved.reserve(static_cast<std::size_t>(frames_wanted) * 2u);
    int frames_got = 0;
    for (std::uint32_t sector = 0; frames_got < frames_wanted; ++sector) {
        auto raw = image.read_audio_sector(track_number, sector);
        if (!raw) {
            return Result<CddaTrack>::failure(raw.error());
        }
        auto decoded = decode_cdda_sector(raw.value());
        if (!decoded) {
            return Result<CddaTrack>::failure(decoded.error());
        }
        for (const StereoSample& sample : decoded.value()) {
            if (frames_got >= frames_wanted) {
                break;
            }
            out.interleaved.push_back(sample.left);
            out.interleaved.push_back(sample.right);
            ++frames_got;
        }
    }
    out.frames = frames_got;
    return Result<CddaTrack>::success(std::move(out));
}

} // namespace oscilline
