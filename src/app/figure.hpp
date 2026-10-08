// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Chooses the placeholder or the disc model for one play form.

#pragma once

#include "oscilline/course/play.hpp"
#include "oscilline/render/stroke.hpp"

#include <vector>

namespace oscilline {

// The on-ribbon figure. PlaceholderFigure is the original line art. A disc
// model replaces it only while that form's rig is in use.
class FigurePainter {
  public:
    virtual ~FigurePainter() = default;
    virtual void paint(std::vector<Segment>& out, Form form, float x, float y) const = 0;
};

class PlaceholderFigure final : public FigurePainter {
  public:
    void paint(std::vector<Segment>& out, Form form, float x, float y) const override;
};

} // namespace oscilline
