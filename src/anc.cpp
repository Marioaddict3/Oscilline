// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Reads an ANC file and samples keys, including roll across a turn.

#include "oscilline/anc.hpp"

#include "oscilline/byte_reader.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace oscilline {
namespace {

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Shortest step in the ANM 4096-unit turn.
float lerp_roll(float a, float b, float t) {
    float delta = b - a;
    constexpr float kTurn = 4096.f;
    constexpr float kHalf = 2048.f;
    if (delta > kHalf) {
        delta -= kTurn;
    } else if (delta < -kHalf) {
        delta += kTurn;
    }
    return a + delta * t;
}

} // namespace

Result<AncFile> parse_anc(std::span<const std::uint8_t> bytes) {
    ByteReader reader(bytes);
    const std::uint16_t magic = reader.u16();
    const std::uint16_t reserved = reader.u16();
    const std::uint16_t count = reader.u16();
    if (!reader.ok()) {
        return Result<AncFile>::failure("ANC header is truncated");
    }
    if (magic != kAncMagic) {
        return Result<AncFile>::failure("ANC magic is not 0x8000");
    }
    if (count == 0) {
        return Result<AncFile>::failure("ANC keyframe count is zero");
    }
    if (count > kAncMaxKeys) {
        return Result<AncFile>::failure("ANC keyframe count is too large");
    }
    const std::size_t need = static_cast<std::size_t>(kAncHeaderBytes) +
                             static_cast<std::size_t>(count) * kAncRecordBytes;
    if (bytes.size() != need) {
        if ((bytes.size() - static_cast<std::size_t>(kAncHeaderBytes)) % count == 0) {
            const std::size_t stride =
                (bytes.size() - static_cast<std::size_t>(kAncHeaderBytes)) / count;
            if (stride != static_cast<std::size_t>(kAncRecordBytes)) {
                return Result<AncFile>::failure("ANC record is " + std::to_string(stride) +
                                                " bytes; this parser accepts 16");
            }
        }
        return Result<AncFile>::failure("ANC length is not a 6-byte header plus 16-byte records");
    }

    AncFile file;
    file.magic = magic;
    file.reserved = reserved;
    file.key_count = count;
    file.keys.resize(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        AncKeyframe& key = file.keys[i];
        // Confirmed order: eye xyz, target xyz, roll, fov.
        key.eye_x = reader.i16();
        key.eye_y = reader.i16();
        key.eye_z = reader.i16();
        key.target_x = reader.i16();
        key.target_y = reader.i16();
        key.target_z = reader.i16();
        key.roll = reader.i16();
        key.fov = reader.i16();
    }
    if (!reader.ok() || reader.remaining() != 0) {
        return Result<AncFile>::failure("ANC record is truncated");
    }
    return Result<AncFile>::success(std::move(file));
}

namespace {

AncSample sample_key(const AncKeyframe& key) {
    AncSample sample;
    sample.eye_x = key.eye_x;
    sample.eye_y = key.eye_y;
    sample.eye_z = key.eye_z;
    sample.target_x = key.target_x;
    sample.target_y = key.target_y;
    sample.target_z = key.target_z;
    sample.roll = key.roll;
    sample.fov = key.fov;
    return sample;
}

AncSample blend_keys(const AncKeyframe& a, const AncKeyframe& b, float fraction) {
    if (!(fraction > 0.f)) {
        return sample_key(a);
    }
    if (!(fraction < 1.f)) {
        return sample_key(b);
    }
    AncSample sample;
    sample.eye_x = lerp(a.eye_x, b.eye_x, fraction);
    sample.eye_y = lerp(a.eye_y, b.eye_y, fraction);
    sample.eye_z = lerp(a.eye_z, b.eye_z, fraction);
    sample.target_x = lerp(a.target_x, b.target_x, fraction);
    sample.target_y = lerp(a.target_y, b.target_y, fraction);
    sample.target_z = lerp(a.target_z, b.target_z, fraction);
    sample.roll = lerp_roll(a.roll, b.roll, fraction);
    sample.fov = lerp(a.fov, b.fov, fraction);
    return sample;
}

} // namespace

AncSample sample_anc(const AncFile& file, float t, bool loop) {
    if (file.keys.empty()) {
        return {};
    }
    if (!std::isfinite(t)) {
        t = 0.f;
    }
    if (file.keys.size() == 1) {
        return sample_key(file.keys.front());
    }

    const float last = static_cast<float>(file.keys.size() - 1);
    float pos = t * last;
    if (loop) {
        pos = std::fmod(pos, last);
        if (pos < 0.f) {
            pos += last;
        }
    } else {
        pos = std::clamp(pos, 0.f, last);
    }
    const int index = std::clamp(static_cast<int>(pos), 0, static_cast<int>(file.keys.size()) - 2);
    const float fraction = std::clamp(pos - static_cast<float>(index), 0.f, 1.f);
    const AncKeyframe& a = file.keys[static_cast<std::size_t>(index)];
    const AncKeyframe& b = file.keys[static_cast<std::size_t>(index) + 1];
    return blend_keys(a, b, fraction);
}

AncSample sample_anc_at(const AncFile& file, float key_index) {
    if (file.keys.empty()) {
        return {};
    }
    if (!std::isfinite(key_index) || file.keys.size() == 1 || !(key_index > 0.f)) {
        return sample_key(file.keys.front());
    }
    const float last = static_cast<float>(file.keys.size() - 1);
    if (!(key_index < last)) {
        return sample_key(file.keys.back());
    }
    const int index = static_cast<int>(key_index);
    const float fraction = key_index - static_cast<float>(index);
    const AncKeyframe& a = file.keys[static_cast<std::size_t>(index)];
    const AncKeyframe& b = file.keys[static_cast<std::size_t>(index) + 1];
    return blend_keys(a, b, fraction);
}

AncLook anc_look(const AncSample& sample) {
    AncLook look;
    look.dx = sample.target_x - sample.eye_x;
    look.dy = sample.target_y - sample.eye_y;
    look.dz = sample.target_z - sample.eye_z;
    const float dist2 = look.dx * look.dx + look.dy * look.dy + look.dz * look.dz;
    look.distance = std::sqrt(std::max(dist2, 0.f));
    const float horiz = std::sqrt(look.dx * look.dx + look.dz * look.dz);
    // Same Y flip as to_view_space: view Y = -PS Y.
    look.pitch_rad = std::atan2(-look.dy, std::max(horiz, 1.e-3f));
    look.yaw_rad = std::atan2(look.dx, look.dz);
    if (!std::isfinite(look.distance)) {
        look.distance = 0.f;
    }
    if (!std::isfinite(look.pitch_rad)) {
        look.pitch_rad = 0.f;
    }
    if (!std::isfinite(look.yaw_rad)) {
        look.yaw_rad = 0.f;
    }
    return look;
}

} // namespace oscilline
