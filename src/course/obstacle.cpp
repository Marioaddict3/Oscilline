// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Names and button masks for obstacle ids 0 through 9.

#include "oscilline/course/obstacle.hpp"

namespace oscilline {
namespace {

struct Kind {
    std::uint8_t actions;
    const char* name;
};

// Order matches the public obstacle enum: block, pit, loop, wave, then the pairs.
constexpr Kind kKinds[kObstacleKindCount] = {
    {kActionBlock, "block"},
    {kActionPit, "pit"},
    {kActionLoop, "loop"},
    {kActionWave, "wave"},
    {static_cast<std::uint8_t>(kActionBlock | kActionPit), "block+pit"},
    {static_cast<std::uint8_t>(kActionBlock | kActionLoop), "block+loop"},
    {static_cast<std::uint8_t>(kActionBlock | kActionWave), "block+wave"},
    {static_cast<std::uint8_t>(kActionPit | kActionLoop), "pit+loop"},
    {static_cast<std::uint8_t>(kActionPit | kActionWave), "pit+wave"},
    {static_cast<std::uint8_t>(kActionLoop | kActionWave), "loop+wave"},
};

} // namespace

bool obstacle_known(std::uint32_t id) {
    return id < static_cast<std::uint32_t>(kObstacleKindCount);
}

std::uint8_t obstacle_actions(std::uint32_t id) {
    if (!obstacle_known(id)) {
        return 0;
    }
    return kKinds[id].actions;
}

std::string_view obstacle_name(std::uint32_t id) {
    if (!obstacle_known(id)) {
        return "unknown";
    }
    return kKinds[id].name;
}

} // namespace oscilline
