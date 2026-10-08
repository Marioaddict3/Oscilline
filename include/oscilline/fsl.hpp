// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Parser for the FSL course script. It does not place obstacles.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// open-ribbon documentation, "FSL - Obstacle Course Data".
// The absolute offsets on that page (0x40, 0xEC, ...) describe one real file
// after its header. This parser walks the sections in order from the counts.
// Public notes do not state ObstacleEnum's width. Values are read as uint32.
// docs/disc-map.md records that width as matching the PAL file.

struct FslHeader {
    std::uint32_t control_segment_table_count = 0;
    std::uint32_t pattern_segment_table_count = 0;
    std::uint32_t event_segment_count = 0;
    std::uint32_t track_index_table_count = 0;
    std::uint32_t fixed_pattern_table_count = 0;
    std::uint32_t distribution_pattern_table_count = 0;
    std::uint32_t unk1 = 0; // TODO: possibly the number of standard tracks
    std::uint32_t unk2 = 0; // TODO: undocumented
    std::uint32_t unk3 = 0; // TODO: possibly the custom-CD track count
    std::vector<std::uint32_t> unk_array;
};

struct FslFixedPattern {
    std::vector<std::uint32_t> obstacles;
};

struct FslDistributionPattern {
    // Stored in the record. Not a spawn count; the mapper does not use it.
    std::uint32_t count = 0;
    std::int32_t random_seed = 0;
    std::int32_t max_prob = 0;
    std::int32_t obstacle_prob[10] = {};
};

struct FslControlSegment {
    std::int32_t start_time = 0;
    std::int32_t base_speed = 0;
    std::int32_t base_shadow_period = 0;
    std::int32_t delta_speed = 0;
    std::int32_t delta_shadow_period = 0;
};

struct FslControlTrack {
    std::vector<FslControlSegment> segments;
};

struct FslPatternSegment {
    std::int32_t start_time = 0;
    std::int32_t unk = 0; // TODO: documented as unused in-game
    std::int32_t pattern_type = 0;
    std::int32_t index = 0;
};

struct FslPatternTrack {
    std::vector<FslPatternSegment> segments;
};

struct FslEventSegment {
    std::int32_t start_time = 0;
    std::int32_t random_speed_duration = 0;
    std::int32_t random_speed_param = 0;
    std::int32_t unused_duration = 0;
    std::int32_t flip_duration = 0;
    std::int32_t camera_event = 0;
};

struct FslEventTrack {
    std::vector<FslEventSegment> segments;
};

struct FslTrackIndex {
    std::uint32_t control_segment_index = 0;
    std::uint32_t pattern_segment_index = 0;
    std::uint32_t event_segment_index = 0;
};

struct FslFile {
    FslHeader header;
    std::uint32_t fixed_pattern_offset = 0;
    std::uint32_t distribution_pattern_offset = 0;
    std::uint32_t control_segment_offset = 0;
    std::uint32_t pattern_segment_offset = 0;
    std::uint32_t event_segment_offset = 0;
    std::uint32_t track_index_offset = 0;
    std::vector<FslFixedPattern> fixed_patterns;
    std::vector<FslDistributionPattern> distribution_patterns;
    std::vector<FslControlTrack> control_tracks;
    std::vector<FslPatternTrack> pattern_tracks;
    std::vector<FslEventTrack> event_tracks;
    std::vector<FslTrackIndex> track_index;
    std::vector<std::uint8_t> trailing;
};

[[nodiscard]] Result<FslFile> parse_fsl(std::span<const std::uint8_t> bytes);

} // namespace oscilline
