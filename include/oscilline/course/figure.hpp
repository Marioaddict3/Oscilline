// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Placeholder stick figure, damage strokes, and form bursts.

#pragma once

#include "oscilline/course/play.hpp"
#include "oscilline/render/stroke.hpp"

#include <vector>

namespace oscilline {

// Course stroke is a 1 px core plus 0.5 px of feather each side, so a foot
// planted on the ribbon center draws through the line. This lifts the
// placeholder and the disc model by that much.
inline constexpr float kFigureRibbonClearancePx = 2.f;

// Original placeholder figures. The ribbon is `y`; feet sit
// `kFigureRibbonClearancePx` above it. Not traced from a disc.
void placeholder_figure(std::vector<Segment>& out, Form form, float x, float y);

} // namespace oscilline
