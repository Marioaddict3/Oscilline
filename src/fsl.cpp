// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Walks FSL sections from the header counts. Offsets are not hard-coded.

#include "oscilline/fsl.hpp"

#include "oscilline/byte_reader.hpp"

namespace oscilline {
namespace {

// Counts are capped so a corrupt length cannot force a multi-gigabyte allocation.
constexpr std::uint32_t kMaxCount = 100000;
constexpr std::size_t kMaxFile = 8u * 1024u * 1024u;

std::string
take_count(ByteReader& reader, std::uint32_t& count, std::size_t stride, const char* what) {
    count = reader.u32();
    if (!reader.ok()) {
        return std::string("FSL ") + what + " count is truncated";
    }
    if (count > kMaxCount ||
        (stride != 0 && static_cast<std::uint64_t>(count) > reader.remaining() / stride)) {
        return std::string("FSL ") + what + " count is unreasonable";
    }
    return {};
}

} // namespace

Result<FslFile> parse_fsl(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kMaxFile) {
        return Result<FslFile>::failure("FSL file is unreasonably large");
    }
    ByteReader reader(bytes);
    FslFile file;
    file.header.control_segment_table_count = reader.u32();
    file.header.pattern_segment_table_count = reader.u32();
    file.header.event_segment_count = reader.u32();
    file.header.track_index_table_count = reader.u32();
    file.header.fixed_pattern_table_count = reader.u32();
    file.header.distribution_pattern_table_count = reader.u32();
    file.header.unk1 = reader.u32();
    file.header.unk2 = reader.u32();
    file.header.unk3 = reader.u32();
    if (!reader.ok()) {
        return Result<FslFile>::failure("FSL header is truncated");
    }
    const std::uint64_t unk_sum =
        static_cast<std::uint64_t>(file.header.unk1) + file.header.unk2 + file.header.unk3;
    if (unk_sum > kMaxCount || unk_sum > reader.remaining() / 4u) {
        return Result<FslFile>::failure("FSL unknown array is unreasonable");
    }
    file.header.unk_array.resize(static_cast<std::size_t>(unk_sum));
    for (std::uint32_t& value : file.header.unk_array) {
        value = reader.u32();
    }
    if (!reader.ok()) {
        return Result<FslFile>::failure("FSL unknown array is truncated");
    }

    const auto too_many = [](std::uint32_t count, const char* what) -> std::string {
        if (count > kMaxCount) {
            return std::string("FSL ") + what + " count is unreasonable";
        }
        return {};
    };
    if (const std::string error = too_many(file.header.fixed_pattern_table_count, "fixed pattern");
        !error.empty()) {
        return Result<FslFile>::failure(error);
    }
    if (const std::string error =
            too_many(file.header.distribution_pattern_table_count, "distribution pattern");
        !error.empty()) {
        return Result<FslFile>::failure(error);
    }
    if (const std::string error =
            too_many(file.header.control_segment_table_count, "control track");
        !error.empty()) {
        return Result<FslFile>::failure(error);
    }
    if (const std::string error =
            too_many(file.header.pattern_segment_table_count, "pattern track");
        !error.empty()) {
        return Result<FslFile>::failure(error);
    }
    if (const std::string error = too_many(file.header.event_segment_count, "event track");
        !error.empty()) {
        return Result<FslFile>::failure(error);
    }
    if (const std::string error = too_many(file.header.track_index_table_count, "track index");
        !error.empty()) {
        return Result<FslFile>::failure(error);
    }

    file.fixed_pattern_offset = static_cast<std::uint32_t>(reader.position());
    file.fixed_patterns.resize(file.header.fixed_pattern_table_count);
    for (FslFixedPattern& pattern : file.fixed_patterns) {
        std::uint32_t count = 0;
        // TODO: ObstacleEnum width is not stated. Each value is read as uint32.
        const std::string error = take_count(reader, count, 4, "obstacle");
        if (!error.empty()) {
            return Result<FslFile>::failure(error);
        }
        pattern.obstacles.resize(count);
        for (std::uint32_t& obstacle : pattern.obstacles) {
            obstacle = reader.u32();
        }
        if (!reader.ok()) {
            return Result<FslFile>::failure("FSL fixed pattern is truncated");
        }
    }

    file.distribution_pattern_offset = static_cast<std::uint32_t>(reader.position());
    if (static_cast<std::uint64_t>(file.header.distribution_pattern_table_count) * 52u >
        reader.remaining()) {
        return Result<FslFile>::failure("FSL distribution pattern table is truncated");
    }
    file.distribution_patterns.resize(file.header.distribution_pattern_table_count);
    for (FslDistributionPattern& pattern : file.distribution_patterns) {
        // The inner count is stored as documented ("always 1 except last?"). It does
        // not repeat this 52-byte record, and the course mapper does not use it.
        pattern.count = reader.u32();
        pattern.random_seed = reader.i32();
        pattern.max_prob = reader.i32();
        for (std::int32_t& probability : pattern.obstacle_prob) {
            probability = reader.i32();
        }
    }
    if (!reader.ok()) {
        return Result<FslFile>::failure("FSL distribution pattern table is truncated");
    }

    file.control_segment_offset = static_cast<std::uint32_t>(reader.position());
    file.control_tracks.resize(file.header.control_segment_table_count);
    for (FslControlTrack& track : file.control_tracks) {
        std::uint32_t count = 0;
        const std::string error = take_count(reader, count, 0x14, "control segment");
        if (!error.empty()) {
            return Result<FslFile>::failure(error);
        }
        track.segments.resize(count);
        for (FslControlSegment& segment : track.segments) {
            segment.start_time = reader.i32();
            segment.base_speed = reader.i32();
            segment.base_shadow_period = reader.i32();
            segment.delta_speed = reader.i32();
            segment.delta_shadow_period = reader.i32();
        }
        if (!reader.ok()) {
            return Result<FslFile>::failure("FSL control track is truncated");
        }
    }

    file.pattern_segment_offset = static_cast<std::uint32_t>(reader.position());
    file.pattern_tracks.resize(file.header.pattern_segment_table_count);
    for (FslPatternTrack& track : file.pattern_tracks) {
        std::uint32_t count = 0;
        const std::string error = take_count(reader, count, 0x10, "pattern segment");
        if (!error.empty()) {
            return Result<FslFile>::failure(error);
        }
        track.segments.resize(count);
        for (FslPatternSegment& segment : track.segments) {
            segment.start_time = reader.i32();
            segment.unk = reader.i32();
            segment.pattern_type = reader.i32();
            segment.index = reader.i32();
        }
        if (!reader.ok()) {
            return Result<FslFile>::failure("FSL pattern track is truncated");
        }
    }

    file.event_segment_offset = static_cast<std::uint32_t>(reader.position());
    file.event_tracks.resize(file.header.event_segment_count);
    for (FslEventTrack& track : file.event_tracks) {
        std::uint32_t count = 0;
        const std::string error = take_count(reader, count, 0x18, "event segment");
        if (!error.empty()) {
            return Result<FslFile>::failure(error);
        }
        track.segments.resize(count);
        for (FslEventSegment& segment : track.segments) {
            segment.start_time = reader.i32();
            segment.random_speed_duration = reader.i32();
            segment.random_speed_param = reader.i32();
            segment.unused_duration = reader.i32();
            segment.flip_duration = reader.i32();
            segment.camera_event = reader.i32();
        }
        if (!reader.ok()) {
            return Result<FslFile>::failure("FSL event track is truncated");
        }
    }

    file.track_index_offset = static_cast<std::uint32_t>(reader.position());
    if (static_cast<std::uint64_t>(file.header.track_index_table_count) * 12u >
        reader.remaining()) {
        return Result<FslFile>::failure("FSL track index table is truncated");
    }
    file.track_index.resize(file.header.track_index_table_count);
    for (FslTrackIndex& index : file.track_index) {
        index.control_segment_index = reader.u32();
        index.pattern_segment_index = reader.u32();
        index.event_segment_index = reader.u32();
    }
    if (!reader.ok()) {
        return Result<FslFile>::failure("FSL track index table is truncated");
    }

    if (reader.remaining() > 0) {
        const auto tail = reader.bytes(reader.remaining());
        file.trailing.assign(tail.begin(), tail.end());
    }
    return Result<FslFile>::success(std::move(file));
}

} // namespace oscilline
