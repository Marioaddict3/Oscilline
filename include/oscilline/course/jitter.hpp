// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Segmented ribbon and obstacle vibration.

#pragma once

#include "oscilline/course/play.hpp"
#include "oscilline/render/stroke.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// PS pixels of the 512x286 frame, stretched onto the 640x480 logical view.
// x_logical = 1.25 * PS px, y_logical = 1.678 * PS px.
inline constexpr float kPlayStationPixelToLogicalX = 1.25f;
inline constexpr float kPlayStationPixelToLogicalY = 1.678f;

// Segmented ribbon jitter. Geometry is high confidence. The worm peak, the
// raised floor, and the two times are medium: the emulator clock ran about
// 3.2x slow and those times are already converted to course time.
// Rest amplitude after a form change and at course start. 1 PS px, about 1.7 logical.
inline constexpr float kRibbonJitterRestPs = 1.f;
// Peak for rabbit and frog when 1 to 3 misses have landed since the form change.
// 6 PS px, about 10 logical.
inline constexpr float kRibbonJitterPeakLowPs = 6.f;
// Peak for rabbit and frog at 4 or more misses. 9.5 PS px, about 16 logical.
inline constexpr float kRibbonJitterPeakHighPs = 9.5f;
// Worm peak, any hit count. 3.5 PS px, about 6 logical. Medium confidence.
inline constexpr float kRibbonJitterPeakWormPs = 3.5f;
// Floor after the fade when 4 or more misses have landed. 2.5 PS px, about 4 logical.
// Medium confidence. Fewer misses fade back to the rest amplitude.
inline constexpr float kRibbonJitterFloorHighPs = 2.5f;
// Hold near the peak, then a linear fade, in course-time milliseconds. Medium.
inline constexpr int kRibbonJitterHoldMs = 1600;
inline constexpr int kRibbonJitterFadeMs = 800;
// Original picture is 50 Hz. Offsets and the amplitude sample hold for this long.
inline constexpr int kRibbonJitterFrameMs = 20;

// Course time floored to the 50 Hz sample. Negative times floor too.
[[nodiscard]] std::int64_t ribbon_jitter_time(std::int64_t time_ms);

// Amplitude in PS pixels. `hits_since_form` is misses since the last form
// change. A negative `last_hit_ms`, or a hit count of 0, is the rest amplitude.
// A clear does not change the inputs. Evaluated on the 50 Hz sample of `now_ms`.
[[nodiscard]] float ribbon_jitter_amplitude_ps(Form form,
                                               int hits_since_form,
                                               std::int64_t last_hit_ms,
                                               std::int64_t now_ms);

// Uniform in [-amplitude, +amplitude], constant across one 50 Hz sample.
// `channel` selects an independent draw. A non-positive amplitude is 0.
[[nodiscard]] float
ribbon_jitter_offset(std::int64_t time_ms, std::uint32_t channel, float amplitude);

// Vertical position of one ribbon break. The offset is uniform in ±amplitude_y.
[[nodiscard]] float
ribbon_break_y(float baseline_y, float x, std::int64_t time_ms, float amplitude_y);

// One straight segment per gap between the screen edges and the attachment
// x positions that fall strictly inside (left, right). No interior vertices.
[[nodiscard]] std::vector<Segment> ribbon_jitter_spine(float left,
                                                       float right,
                                                       float baseline_y,
                                                       std::span<const float> attachment_x,
                                                       std::int64_t time_ms,
                                                       float amplitude_ps,
                                                       float depth);

// Where an obstacle outline leaves and rejoins the ribbon, in local x.
// Invalid when the outline does not meet y = 0 at two distinct places.
struct RibbonAttachment {
    float left = 0.f;
    float right = 0.f;
    bool valid = false;
};

[[nodiscard]] RibbonAttachment obstacle_ribbon_attachment(std::uint8_t id, float loop_px);

// Outline in screen space. Ends that meet the ribbon stay on that break.
// Every other vertex jitters by up to the amplitude in x and in y.
void append_jittered_obstacle(std::vector<Segment>& out,
                              std::uint8_t id,
                              float origin_x,
                              float baseline_y,
                              float loop_px,
                              float scale,
                              std::int64_t time_ms,
                              float amplitude_ps,
                              std::uint32_t salt);

// Share of (amplitude - rest) that the figure's own vertices take. The
// ribbon and obstacles keep the full amplitude. Medium: fitted to emulator
// frames at 4+ misses, where she stays recognizable (about 3 PS px at the peak).
inline constexpr float kFigureJitterScale = 0.35f;

// Jitter every segment endpoint independently at the given stage amplitude.
void jitter_segments(std::span<Segment> segments,
                     std::int64_t time_ms,
                     float amplitude_ps,
                     std::uint32_t salt);

// Line vertices of the figure. At the rest amplitude this is a no-op.
// Above it, each vertex jitters by up to jitter_scale * (amplitude - rest)
// in x and in y. The figure is not translated as a whole.
void jitter_figure_vertices(std::span<Segment> segments,
                            std::int64_t time_ms,
                            float amplitude_ps,
                            std::uint32_t salt,
                            float jitter_scale = kFigureJitterScale);

} // namespace oscilline
