// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Samples a meter TMD into a screen curve.

#include "oscilline/asset/meter.hpp"

#include <algorithm>
#include <cmath>

namespace oscilline {

int meter_frame_index(float fill, int frame_count) {
    if (frame_count <= 1) {
        return 0;
    }
    if (!std::isfinite(fill)) {
        fill = 0.f;
    }
    fill = std::clamp(fill, 0.f, 1.f);
    const int last = frame_count - 1;
    const int frame = static_cast<int>(std::lround(fill * static_cast<float>(last)));
    return std::clamp(frame, 0, last);
}

std::optional<int> digit_object(int digit, int object_count) {
    if (digit < 0 || digit > 9 || object_count <= digit) {
        return std::nullopt;
    }
    return digit;
}

} // namespace oscilline
