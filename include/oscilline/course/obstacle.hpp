// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Obstacle ids 0-9 and the four action buttons.

#pragma once

#include <cstdint>
#include <string_view>

namespace oscilline {

// Public obstacle ids. 0..3 are the four shapes; 4..9 are the pairs.
// The button mask is separate so the mapping can change without touching the parser.
inline constexpr std::uint8_t kActionBlock = 1u;
inline constexpr std::uint8_t kActionLoop = 2u;
inline constexpr std::uint8_t kActionWave = 4u;
inline constexpr std::uint8_t kActionPit = 8u;

inline constexpr int kObstacleKindCount = 10;

[[nodiscard]] bool obstacle_known(std::uint32_t id);

// Zero when `id` is not one of the ten kinds.
[[nodiscard]] std::uint8_t obstacle_actions(std::uint32_t id);

[[nodiscard]] std::string_view obstacle_name(std::uint32_t id);

} // namespace oscilline
