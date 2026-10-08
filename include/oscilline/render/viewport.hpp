// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// 640 by 480 logical view, presented at the selected aspect ratio.

#pragma once

namespace oscilline {

// The 4:3 layout remains the baseline. Wider layouts increase the horizontal
// coordinate range while preserving the 480-pixel gameplay height.
inline constexpr int kLogicalWidth = 640;
inline constexpr int kLogicalHeight = 480;

enum class ViewportAspect {
    FourThree,
    SixteenNine,
    Unrestricted,
};

[[nodiscard]] constexpr int logical_width_for_aspect(ViewportAspect aspect,
                                                     int output_width = 0,
                                                     int output_height = 0) noexcept {
    if (aspect == ViewportAspect::SixteenNine) {
        return (kLogicalHeight * 16 + 4) / 9;
    }
    if (aspect == ViewportAspect::Unrestricted && output_width > 0 && output_height > 0) {
        return (output_width * kLogicalHeight + output_height / 2) / output_height;
    }
    return kLogicalWidth;
}

// Host updates this when opening, changing aspect, or resizing an unrestricted
// window. Game drawing reads it so gameplay fills the wider logical viewport.
inline int gLogicalViewportWidth = kLogicalWidth;

[[nodiscard]] inline int logical_width() noexcept {
    return gLogicalViewportWidth;
}

inline void set_logical_width(int width) noexcept {
    gLogicalViewportWidth = width > 0 ? width : kLogicalWidth;
}

} // namespace oscilline
