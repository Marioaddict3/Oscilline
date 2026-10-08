// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Returns the version string from the generated header.

#include "oscilline/version.hpp"

#include "oscilline/version_config.hpp"

namespace oscilline {

const char* version_string() {
    return OSCILLINE_VERSION;
}

} // namespace oscilline
