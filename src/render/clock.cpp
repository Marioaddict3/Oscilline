// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Latches the first timestamp, then counts 60 Hz steps.

#include "oscilline/render/clock.hpp"

namespace oscilline {

FixedStep::FixedStep(double step_seconds) : step_(step_seconds > 0 ? step_seconds : 1.0 / 60.0) {}

void FixedStep::reset(double now_seconds) {
    previous_ = now_seconds;
    accumulator_ = 0;
    alpha_ = 0;
    started_ = true;
}

int FixedStep::advance(double now_seconds, int max_steps) {
    if (!started_) {
        previous_ = now_seconds;
        started_ = true;
        alpha_ = 0;
        return 0;
    }
    if (max_steps < 1) {
        max_steps = 1;
    }
    double delta = now_seconds - previous_;
    previous_ = now_seconds;
    if (delta < 0) {
        delta = 0;
    }
    accumulator_ += delta;
    const double cap = step_ * static_cast<double>(max_steps);
    if (accumulator_ > cap) {
        accumulator_ = cap;
    }
    int steps = 0;
    while (accumulator_ >= step_ && steps < max_steps) {
        accumulator_ -= step_;
        ++steps;
    }
    alpha_ = step_ > 0 ? accumulator_ / step_ : 0;
    return steps;
}

} // namespace oscilline
