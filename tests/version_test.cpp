// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// The version string matches the CMake project version.

#include "oscilline/version.hpp"

#include <doctest/doctest.h>
#include <string>

TEST_CASE("version string is published") {
    const std::string version = oscilline::version_string();
    CHECK_FALSE(version.empty());
    CHECK(version == "0.1.1");
}
