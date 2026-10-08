// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Builds the silent stand-in tone.

#include "oscilline/asset/fallback.hpp"

namespace oscilline {

void SilentSound::play(int /*program*/, int /*note*/) {
    ++plays;
}

} // namespace oscilline
