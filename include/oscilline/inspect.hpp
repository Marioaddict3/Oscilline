// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// JSON dump of one parsed PAK, TIM, TMD, ANM, ANC, VH/VB, or FSL.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

struct InspectRequest {
    std::string type;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> body;
    bool full_pcm = false;
};

// JSON dump of a parsed file. `type` is pak, tim, tmd, anm, anc, vh, vb, or fsl.
[[nodiscard]] Result<std::string> inspect_to_json(const InspectRequest& request);

} // namespace oscilline
