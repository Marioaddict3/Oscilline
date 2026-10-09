// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Course drawing shared by the app and the course-draw tests.

#pragma once

#include "oscilline/anc.hpp"
#include "oscilline/asset/character.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/present/host.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace oscilline {

class AssetRegistry;
class TextPainter;

struct CourseFrame {
    TriangleList triangles;
    std::vector<TextGlyph> text;
};

// In-level HUD choices. The default draws coupons and no control line.
struct CourseHud {
    // True shows the score as digits where the coupons would be.
    bool score_number = false;
    // True shows PERFECT, GOOD, or MISS below the coupons after each hit.
    bool timing_hints = false;
    // Drawn at the top left while playing. Empty hides it.
    std::string_view control_hint;
};

// Disc road cameras. Used only when CourseView::disc_camera is set. Section
// spans on the course choose among S01 (`play`), S02, B01, and the looping
// pans TV_SS (`intro`), TV_BB, and TV_BS.
struct CourseCameras {
    const AncFile* intro = nullptr;
    const AncFile* play = nullptr;
    const AncFile* s02 = nullptr;
    const AncFile* b01 = nullptr;
    const AncFile* tv_bb = nullptr;
    const AncFile* tv_bs = nullptr;
};

// The first round of a pair, carried into the second round's results.
struct RoundTotals {
    // Negative means this screen is a single course.
    int score = -1;
    int perfects = 0;
    int goods = 0;
    int misses = 0;
};

// Everything draw_course needs besides the course, play state, and time. The
// defaults draw a plain single course with debug text and the built-in camera.
struct CourseView {
    // Highlighted row: pause is resume/quit; the end screen is retry plus quit
    // (out) or back (results).
    int menu = 0;
    // Replace the gameplay view with the centered results or game-over card.
    bool show_end = false;
    // Above 0 draws Round N during the first half of the eight-second prelude.
    int round_number = 0;
    // When set, the end screen shows the pair total and combined counts.
    RoundTotals prior;
    // Replaces the course number (used for a music file).
    std::string_view heading;
    // Null keeps SDL debug text.
    const TextPainter* text = nullptr;
    AssetRegistry* assets = nullptr;
    // Disc model pose. Null draws the original placeholder figure.
    const DiscFigurePose* figure = nullptr;
    bool disc_camera = false;
    CourseCameras cameras;
    // Tick marks and the hit marker. The ribbon is one line without them.
    bool ribbon_guides = false;
    CourseHud hud;
};

// The ribbon, the figure, and the obstacles on screen at `time_ms`, plus the
// screen-space HUD. Negative time is the prelude. Course strokes use a 1 px core
// and a 0.5 px feather.
[[nodiscard]] CourseFrame draw_course(const CourseTimeline& course,
                                      const PlayState& play,
                                      std::int64_t time_ms,
                                      const CourseView& view = {});

// Bottom progress arc. Yellow grows from the right as `fill` goes from 0 to 1.
// The color boundary is that fraction, and the curve still runs from x = 104 to 536.
[[nodiscard]] std::vector<Segment> progress_arc_segments(float fill);

} // namespace oscilline
