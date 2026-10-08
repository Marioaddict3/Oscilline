// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Progress, evolution, and speed meter curves from a TMD.

#pragma once

#include <optional>

namespace oscilline {

// Maps a 0..1 fill onto an animation frame. 0 is the first frame, 1 is the
// last, and the middle rounds to the nearest frame.
[[nodiscard]] int meter_frame_index(float fill, int frame_count);

// Digit objects are 0..9 in order. This is a guess and stays unused until the
// results-digit slot is allowed on.
[[nodiscard]] std::optional<int> digit_object(int digit, int object_count);

} // namespace oscilline
