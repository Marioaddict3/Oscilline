// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Bronze, silver, and gold pairs, and session high scores.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace oscilline {

enum class Difficulty : std::uint8_t { Bronze, Silver, Gold };

inline constexpr int kDifficultyCount = 3;

// Wheel rows: the three difficulties, then scores, then back.
inline constexpr int kWheelSlots = 5;
inline constexpr int kWheelScores = 3;
inline constexpr int kWheelBack = 4;

// Shown once before a course or a pair. A pair loads both rounds behind it.
inline constexpr int kLoadingMs = 1200;

struct CoursePair {
    // Course indices, counting from 0. Bronze is 0–1, silver 2–3, gold 4–5.
    int first = 0;
    int second = 1;
};

struct ScoreBoard {
    int best[kDifficultyCount] = {};
};

[[nodiscard]] Result<ScoreBoard> load_high_scores(const std::filesystem::path& file);
[[nodiscard]] Result<int> save_high_scores(const std::filesystem::path& file,
                                           const ScoreBoard& scores);
[[nodiscard]] std::filesystem::path high_score_path();

[[nodiscard]] CoursePair courses_for(Difficulty difficulty);

[[nodiscard]] std::string_view difficulty_name(Difficulty difficulty);

// Moves by `delta` rows (+1 is down, -1 is up) and wraps.
[[nodiscard]] int wheel_move(int index, int delta);

// The difficulty on this row, or empty for scores and back.
[[nodiscard]] std::optional<Difficulty> wheel_difficulty(int index);

// Keeps the best total seen for that difficulty.
void note_score(ScoreBoard& board, Difficulty difficulty, int score);

} // namespace oscilline
