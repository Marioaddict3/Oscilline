// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Fixed 60 Hz step clock. A stall does not fast-forward.

#pragma once

namespace oscilline {

// Fixed-step accumulator. The first sample only latches the clock so a late
// start does not replay a pile of steps. alpha() is the leftover fraction of
// a step, in [0, 1), for rendering between the last two updates.
class FixedStep {
  public:
    explicit FixedStep(double step_seconds = 1.0 / 60.0);

    void reset(double now_seconds);

    // Drops time beyond max_steps so a stall does not fast-forward the sim.
    [[nodiscard]] int advance(double now_seconds, int max_steps = 8);

    [[nodiscard]] double alpha() const { return alpha_; }

    [[nodiscard]] double step_seconds() const { return step_; }

  private:
    double step_ = 1.0 / 60.0;
    double previous_ = 0;
    double accumulator_ = 0;
    double alpha_ = 0;
    bool started_ = false;
};

} // namespace oscilline
