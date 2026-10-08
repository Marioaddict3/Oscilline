// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Finds SYSTEM3.FSL inside the mounted language archive.

#include "oscilline/course/load.hpp"

#include "oscilline/disc/archive.hpp"

namespace oscilline {

Result<FslFile> load_course_script(const PakArchive& archive) {
    const PakEntry* entry = find_entry(archive, kCourseScriptPath);
    if (entry == nullptr) {
        return Result<FslFile>::failure("course script SCRIPT/SYSTEM3.FSL is not in the archive");
    }
    return parse_fsl(entry->data);
}

} // namespace oscilline
