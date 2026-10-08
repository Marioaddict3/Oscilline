// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Maps bronze, silver, and gold onto course pairs and best totals.

#include "oscilline/course/difficulty.hpp"

#include "oscilline/course/music.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace oscilline {

CoursePair courses_for(Difficulty difficulty) {
    CoursePair pair;
    switch (difficulty) {
    case Difficulty::Bronze:
        pair.first = 0;
        pair.second = 1;
        break;
    case Difficulty::Silver:
        pair.first = 2;
        pair.second = 3;
        break;
    case Difficulty::Gold:
        pair.first = 4;
        pair.second = 5;
        break;
    }
    return pair;
}

std::string_view difficulty_name(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Bronze:
        return "bronze";
    case Difficulty::Silver:
        return "silver";
    case Difficulty::Gold:
        return "gold";
    }
    return "bronze";
}

int wheel_move(int index, int delta) {
    if (kWheelSlots <= 0) {
        return 0;
    }
    int next = index + delta;
    next %= kWheelSlots;
    if (next < 0) {
        next += kWheelSlots;
    }
    return next;
}

std::optional<Difficulty> wheel_difficulty(int index) {
    if (index < 0 || index >= kDifficultyCount) {
        return std::nullopt;
    }
    return static_cast<Difficulty>(index);
}

void note_score(ScoreBoard& board, Difficulty difficulty, int score) {
    const int slot = static_cast<int>(difficulty);
    if (slot < 0 || slot >= kDifficultyCount) {
        return;
    }
    if (score > board.best[slot]) {
        board.best[slot] = score;
    }
}

Result<ScoreBoard> load_high_scores(const std::filesystem::path& file) {
    ScoreBoard scores;
    if (file.empty()) {
        return Result<ScoreBoard>::success(scores);
    }
    std::ifstream in(file);
    if (!in) {
        return Result<ScoreBoard>::success(scores);
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        std::string name;
        int score = 0;
        if (!(row >> name >> score) || score < 0) {
            continue;
        }
        Difficulty difficulty = Difficulty::Bronze;
        if (name == "silver") {
            difficulty = Difficulty::Silver;
        } else if (name == "gold") {
            difficulty = Difficulty::Gold;
        } else if (name != "bronze") {
            continue;
        }
        note_score(scores, difficulty, score);
    }
    if (in.bad()) {
        return Result<ScoreBoard>::failure("could not read high scores");
    }
    return Result<ScoreBoard>::success(scores);
}

Result<int> save_high_scores(const std::filesystem::path& file, const ScoreBoard& scores) {
    if (file.empty()) {
        return Result<int>::success(0);
    }
    std::error_code error;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            return Result<int>::failure("could not save high scores");
        }
    }
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        return Result<int>::failure("could not save high scores");
    }
    out << "# oscilline course high scores\n";
    for (int i = 0; i < kDifficultyCount; ++i) {
        out << difficulty_name(static_cast<Difficulty>(i)) << ' ' << scores.best[i] << '\n';
    }
    if (!out) {
        return Result<int>::failure("could not save high scores");
    }
    return Result<int>::success(0);
}

std::filesystem::path high_score_path() {
    const std::filesystem::path dir = user_config_directory();
    if (dir.empty()) {
        return {};
    }
    return dir / "course-scores.txt";
}

} // namespace oscilline
