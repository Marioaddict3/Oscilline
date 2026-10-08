// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Strips sync, header, and subheader to the user data.

#include "oscilline/disc/sector.hpp"

#include <cstring>

namespace oscilline {
namespace {

constexpr std::size_t kMode1UserOffset = 0x10;
constexpr std::size_t kMode2UserOffset = 0x18;
constexpr std::size_t kUser2048 = 2048;
constexpr std::size_t kForm2User = 2324;
constexpr std::uint8_t kForm2Bit = 0x20;

Result<UserPayload> payload_from(std::span<const std::uint8_t> sector,
                                 std::size_t offset,
                                 std::size_t length,
                                 bool form2) {
    if (offset > sector.size() || length > sector.size() - offset) {
        return Result<UserPayload>::failure("sector is shorter than its user-data window");
    }
    UserPayload payload;
    payload.form2 = form2;
    payload.bytes.assign(sector.begin() + static_cast<std::ptrdiff_t>(offset),
                         sector.begin() + static_cast<std::ptrdiff_t>(offset + length));
    return Result<UserPayload>::success(std::move(payload));
}

} // namespace

Result<UserPayload> extract_user_payload(std::span<const std::uint8_t> sector, RawSectorKind kind) {
    switch (kind) {
    case RawSectorKind::Audio2352:
        if (sector.size() != 2352) {
            return Result<UserPayload>::failure("CD-DA sector must be 2352 bytes");
        }
        return payload_from(sector, 0, 2352, false);
    case RawSectorKind::Mode1_2048:
        if (sector.size() != kUser2048) {
            return Result<UserPayload>::failure("MODE1/2048 sector must be 2048 bytes");
        }
        return payload_from(sector, 0, kUser2048, false);
    case RawSectorKind::Mode1_2352:
        if (sector.size() != 2352) {
            return Result<UserPayload>::failure("MODE1/2352 sector must be 2352 bytes");
        }
        // Header mode nibble lives at offset 15. psx-spx Mode 1 layout.
        if (sector[15] != 0x01) {
            return Result<UserPayload>::failure("MODE1/2352 sector header mode is not 1");
        }
        return payload_from(sector, kMode1UserOffset, kUser2048, false);
    case RawSectorKind::Mode2_2352: {
        if (sector.size() != 2352) {
            return Result<UserPayload>::failure("MODE2/2352 sector must be 2352 bytes");
        }
        if (sector[15] != 0x02) {
            return Result<UserPayload>::failure("MODE2/2352 sector header mode is not 2");
        }
        // Subheader submode is at offset 0x12. Bit 5 set means Form 2 (psx-spx).
        const bool form2 = (sector[0x12] & kForm2Bit) != 0;
        if (form2) {
            return payload_from(sector, kMode2UserOffset, kForm2User, true);
        }
        return payload_from(sector, kMode2UserOffset, kUser2048, false);
    }
    case RawSectorKind::Mode2_2336: {
        if (sector.size() != 2336) {
            return Result<UserPayload>::failure("MODE2/2336 sector must be 2336 bytes");
        }
        // 2336-byte sectors omit sync and the 4-byte header, so they start at the subheader.
        // psx-spx: MODE2/2336 is bytes 010h..92Fh of a raw sector.
        const bool form2 = (sector[2] & kForm2Bit) != 0;
        if (form2) {
            return payload_from(sector, 8, kForm2User, true);
        }
        return payload_from(sector, 8, kUser2048, false);
    }
    }
    return Result<UserPayload>::failure("unknown sector kind");
}

Result<std::vector<StereoSample>> decode_cdda_sector(std::span<const std::uint8_t> sector) {
    if (sector.size() != 2352) {
        return Result<std::vector<StereoSample>>::failure("CD-DA sector must be 2352 bytes");
    }
    std::vector<StereoSample> frames;
    frames.reserve(sector.size() / 4);
    for (std::size_t i = 0; i < sector.size(); i += 4) {
        StereoSample sample;
        const auto left = static_cast<std::uint32_t>(sector[i]) |
                          (static_cast<std::uint32_t>(sector[i + 1]) << 8);
        const auto right = static_cast<std::uint32_t>(sector[i + 2]) |
                           (static_cast<std::uint32_t>(sector[i + 3]) << 8);
        sample.left = static_cast<std::int16_t>(left);
        sample.right = static_cast<std::int16_t>(right);
        frames.push_back(sample);
    }
    return Result<std::vector<StereoSample>>::success(std::move(frames));
}

} // namespace oscilline
