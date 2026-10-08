// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// User payload of Mode 1, Mode 2 Form 1/2, and CD-DA sectors.

#pragma once

#include "oscilline/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// User-data windows inside a raw CD sector.
// Layout: psx-spx "CDROM Sector Encoding" (Audio, Mode 1, Mode 2 Form 1, Mode 2 Form 2).
enum class RawSectorKind {
    Audio2352,
    Mode1_2048,
    Mode1_2352,
    Mode2_2336,
    Mode2_2352,
};

struct UserPayload {
    std::vector<std::uint8_t> bytes;
    bool form2 = false;
};

// One stereo CD-DA frame. psx-spx stores audio as LeftLsb, LeftMsb, RightLsb, RightMsb.
struct StereoSample {
    std::int16_t left = 0;
    std::int16_t right = 0;
};

[[nodiscard]] Result<UserPayload> extract_user_payload(std::span<const std::uint8_t> sector,
                                                       RawSectorKind kind);

[[nodiscard]] Result<std::vector<StereoSample>>
decode_cdda_sector(std::span<const std::uint8_t> sector);

[[nodiscard]] constexpr std::uint32_t sector_stride(RawSectorKind kind) {
    switch (kind) {
    case RawSectorKind::Audio2352:
    case RawSectorKind::Mode1_2352:
    case RawSectorKind::Mode2_2352:
        return 2352;
    case RawSectorKind::Mode2_2336:
        return 2336;
    case RawSectorKind::Mode1_2048:
        return 2048;
    }
    return 0;
}

} // namespace oscilline
