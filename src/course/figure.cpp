// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Placeholder figure strokes, the streak ring, and the promotion dashes.

#include "oscilline/course/figure.hpp"

namespace oscilline {
namespace {

void add(std::vector<Segment>& out, float x0, float y0, float x1, float y1) {
    Segment segment;
    segment.x0 = x0;
    segment.y0 = y0;
    segment.x1 = x1;
    segment.y1 = y1;
    out.push_back(segment);
}

void rabbit(std::vector<Segment>& out, float x, float y) {
    const float head = y - 64.f;
    add(out, x - 7.f, head - 6.f, x + 7.f, head - 6.f);
    add(out, x + 7.f, head - 6.f, x + 7.f, head + 8.f);
    add(out, x + 7.f, head + 8.f, x - 7.f, head + 8.f);
    add(out, x - 7.f, head + 8.f, x - 7.f, head - 6.f);
    add(out, x - 5.f, head - 6.f, x - 10.f, head - 22.f);
    add(out, x - 10.f, head - 22.f, x - 1.f, head - 8.f);
    add(out, x + 5.f, head - 6.f, x + 10.f, head - 22.f);
    add(out, x + 10.f, head - 22.f, x + 1.f, head - 8.f);
    add(out, x, head + 8.f, x, y - 8.f);
    add(out, x, y - 40.f, x - 16.f, y - 22.f);
    add(out, x, y - 40.f, x + 16.f, y - 22.f);
    add(out, x, y - 8.f, x - 12.f, y);
    add(out, x, y - 8.f, x + 12.f, y);
}

void frog(std::vector<Segment>& out, float x, float y) {
    const float body = y - 22.f;
    add(out, x - 16.f, body, x + 16.f, body);
    add(out, x + 16.f, body, x + 12.f, body + 14.f);
    add(out, x + 12.f, body + 14.f, x - 12.f, body + 14.f);
    add(out, x - 12.f, body + 14.f, x - 16.f, body);
    add(out, x - 8.f, body - 2.f, x - 4.f, body - 8.f);
    add(out, x - 4.f, body - 8.f, x, body - 2.f);
    add(out, x + 8.f, body - 2.f, x + 4.f, body - 8.f);
    add(out, x + 4.f, body - 8.f, x, body - 2.f);
    add(out, x - 14.f, body + 6.f, x - 26.f, y - 2.f);
    add(out, x - 26.f, y - 2.f, x - 14.f, y);
    add(out, x + 14.f, body + 6.f, x + 26.f, y - 2.f);
    add(out, x + 26.f, y - 2.f, x + 14.f, y);
}

void worm(std::vector<Segment>& out, float x, float y) {
    add(out, x - 14.f, y - 2.f, x - 6.f, y - 8.f);
    add(out, x - 6.f, y - 8.f, x + 4.f, y - 2.f);
    add(out, x + 4.f, y - 2.f, x + 14.f, y - 7.f);
}

// Original stick figure. The crown and wings are not traced from a disc model.
void super(std::vector<Segment>& out, float x, float y) {
    rabbit(out, x, y);
    const float head = y - 64.f;
    add(out, x - 8.f, head - 6.f, x - 4.f, head - 16.f);
    add(out, x - 4.f, head - 16.f, x, head - 8.f);
    add(out, x, head - 8.f, x + 4.f, head - 16.f);
    add(out, x + 4.f, head - 16.f, x + 8.f, head - 6.f);
    add(out, x - 2.f, y - 48.f, x - 22.f, y - 62.f);
    add(out, x + 2.f, y - 48.f, x + 22.f, y - 62.f);
}

} // namespace

void placeholder_figure(std::vector<Segment>& out, Form form, float x, float y) {
    // Existing feet-on-ribbon art, plus clearance so the stroke clears the line.
    y -= kFigureRibbonClearancePx;
    if (form == Form::Super) {
        super(out, x, y);
    } else if (form == Form::Rabbit) {
        rabbit(out, x, y);
    } else if (form == Form::Frog) {
        frog(out, x, y);
    } else if (form == Form::Worm) {
        worm(out, x, y);
    }
}

} // namespace oscilline
