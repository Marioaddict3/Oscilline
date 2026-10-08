// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// ANC camera-path parser, sampling, and the projection scale.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// One camera sample. Field order is confirmed: eye, target, roll, fov.
// Roll uses the ANM turn: 4096 is a full rotation. Positive roll is a
// counter-clockwise image roll around the look axis.
struct AncKeyframe {
    std::int16_t eye_x = 0;
    std::int16_t eye_y = 0;
    std::int16_t eye_z = 0;
    std::int16_t target_x = 0;
    std::int16_t target_y = 0;
    std::int16_t target_z = 0;
    std::int16_t roll = 0;
    std::int16_t fov = 0;
};

struct AncFile {
    std::uint16_t magic = 0;
    // Second header word. Probably a rate in Hz (medium confidence, by analogy
    // with ANM). Stored and reported. Playback does not require it.
    std::uint16_t reserved = 0;
    std::uint16_t key_count = 0;
    std::vector<AncKeyframe> keys;
};

inline constexpr int kAncHeaderBytes = 6;
inline constexpr int kAncRecordBytes = 16;
inline constexpr int kAncMaxKeys = 4096;
inline constexpr std::uint16_t kAncMagic = 0x8000;

// 6-byte header (magic 0x8000, reserved, count) plus count records of 16 bytes.
// The file length must be 6 + count * 16. A different record size is rejected
// with the size the file implies. Record order is eye xyz, target xyz, roll,
// fov. That order is confirmed; do not permute it.
[[nodiscard]] Result<AncFile> parse_anc(std::span<const std::uint8_t> bytes);

struct AncSample {
    float eye_x = 0.f;
    float eye_y = 0.f;
    float eye_z = 0.f;
    float target_x = 0.f;
    float target_y = 0.f;
    float target_z = 0.f;
    float roll = 0.f;
    float fov = 0.f;
};

// The fov channel narrows the view as it grows: the projection distance (GTE H,
// in PS pixels) is kAncProjectionScale / fov, so the S01 play key (fov 499) keeps
// H ~= 501. Fitted against private emulator frames of figure height across the
// intro spin. Reading the channel as H directly made the figure two to three
// times too tall at keys 85-150.
inline constexpr float kAncProjectionScale = 250000.f;

[[nodiscard]] inline float anc_projection_h(float fov) {
    return fov > 0.f ? kAncProjectionScale / fov : 0.f;
}

// `t` is 0 at the first key and 1 at the last. One key ignores `t`.
// Roll takes the short way around a 4096-unit turn. `loop` wraps.
// A non-finite `t` is read as 0.
[[nodiscard]] AncSample sample_anc(const AncFile& file, float t, bool loop = false);

// Sample at a key index. 0 is the first key. The fraction between keys lerps,
// and roll takes the short way around a 4096-unit turn. An index past the last
// key holds that key. A negative or non-finite index holds the first key.
// One key ignores the index. This does not loop.
[[nodiscard]] AncSample sample_anc_at(const AncFile& file, float key_index);

// Look from eye to target. Pitch uses the same Y flip as to_view_space
// (view Y = -PS Y): atan2(-dy, hypot(dx, dz)). Positive pitch means the
// look points up in view space. Yaw is atan2(dx, dz).
struct AncLook {
    float dx = 0.f;
    float dy = 0.f;
    float dz = 0.f;
    float distance = 0.f;
    float pitch_rad = 0.f;
    float yaw_rad = 0.f;
};

[[nodiscard]] AncLook anc_look(const AncSample& sample);

// Viewer overlay only. Sampling and disc-camera playback do not use this.
// Stride between drawn eye-to-target segments:
// N = max(1, round((key_count - 1) / kAncLookSegmentGoal)).
// The overlay also draws the last key when it is off that grid, so a long
// path lands near the goal (about 40–60 segments). A short path keeps N = 1.
inline constexpr int kAncLookSegmentGoal = 50;

[[nodiscard]] constexpr int anc_look_step(std::size_t key_count) noexcept {
    if (key_count <= 1) {
        return 1;
    }
    const std::size_t span = key_count - 1;
    const std::size_t goal = static_cast<std::size_t>(kAncLookSegmentGoal);
    // round(span / goal), half up, for a non-negative span.
    const std::size_t step = (span + goal / 2) / goal;
    return step == 0 ? 1 : static_cast<int>(step);
}

// True when the overlay draws the eye-to-target segment at `index`.
// `step` below 1 draws every key. The first and last keys are included.
[[nodiscard]] constexpr bool
anc_overlay_draws_look(std::size_t index, std::size_t key_count, int step) noexcept {
    if (key_count == 0 || index >= key_count) {
        return false;
    }
    const std::size_t stride = step < 1 ? std::size_t{1} : static_cast<std::size_t>(step);
    return index % stride == 0 || index + 1 == key_count;
}

} // namespace oscilline
