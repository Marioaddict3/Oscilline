// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Loads SCRIPT/SYSTEM3.FSL from the language archive.

#pragma once

#include "oscilline/fsl.hpp"
#include "oscilline/pak.hpp"
#include "oscilline/result.hpp"

#include <string_view>

namespace oscilline {

// Language-archive path of the built-in course script.
inline constexpr std::string_view kCourseScriptPath = "SCRIPT/SYSTEM3.FSL";

[[nodiscard]] Result<FslFile> load_course_script(const PakArchive& archive);

} // namespace oscilline
