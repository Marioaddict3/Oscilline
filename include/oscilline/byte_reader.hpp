// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Little-endian reader that latches the first out-of-range read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

// Bounds-checked little-endian reader. Every failed read latches ok() to false
// and returns zero, so callers can parse straight through and check once.
class ByteReader {
  public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

    bool ok() const { return ok_; }

    std::size_t position() const { return pos_; }

    std::size_t size() const { return data_.size(); }

    std::size_t remaining() const { return pos_ <= data_.size() ? data_.size() - pos_ : 0; }

    void seek(std::size_t position) {
        if (position > data_.size()) {
            ok_ = false;
            return;
        }
        pos_ = position;
    }

    void skip(std::size_t count) {
        if (count > remaining()) {
            ok_ = false;
            pos_ = data_.size();
            return;
        }
        pos_ += count;
    }

    std::uint8_t u8() {
        if (remaining() < 1) {
            ok_ = false;
            return 0;
        }
        return data_[pos_++];
    }

    std::uint16_t u16() {
        if (remaining() < 2) {
            ok_ = false;
            return 0;
        }
        const auto lo = static_cast<std::uint32_t>(data_[pos_]);
        const auto hi = static_cast<std::uint32_t>(data_[pos_ + 1]);
        pos_ += 2;
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }

    std::uint32_t u32() {
        if (remaining() < 4) {
            ok_ = false;
            return 0;
        }
        const auto b0 = static_cast<std::uint32_t>(data_[pos_]);
        const auto b1 = static_cast<std::uint32_t>(data_[pos_ + 1]);
        const auto b2 = static_cast<std::uint32_t>(data_[pos_ + 2]);
        const auto b3 = static_cast<std::uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }

    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    std::uint16_t u16be() {
        if (remaining() < 2) {
            ok_ = false;
            return 0;
        }
        const auto hi = static_cast<std::uint32_t>(data_[pos_]);
        const auto lo = static_cast<std::uint32_t>(data_[pos_ + 1]);
        pos_ += 2;
        return static_cast<std::uint16_t>((hi << 8) | lo);
    }

    std::uint32_t u32be() {
        if (remaining() < 4) {
            ok_ = false;
            return 0;
        }
        const auto b0 = static_cast<std::uint32_t>(data_[pos_]);
        const auto b1 = static_cast<std::uint32_t>(data_[pos_ + 1]);
        const auto b2 = static_cast<std::uint32_t>(data_[pos_ + 2]);
        const auto b3 = static_cast<std::uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }

    // ISO 9660 both-endian 16-bit: little-endian value, then big-endian copy.
    std::uint16_t u16both() {
        const std::uint16_t le = u16();
        const std::uint16_t be = u16be();
        if (ok_ && le != be) {
            ok_ = false;
        }
        return le;
    }

    std::uint32_t u32both() {
        const std::uint32_t le = u32();
        const std::uint32_t be = u32be();
        if (ok_ && le != be) {
            ok_ = false;
        }
        return le;
    }

    std::span<const std::uint8_t> bytes(std::size_t count) {
        if (count > remaining()) {
            ok_ = false;
            return {};
        }
        const auto slice = data_.subspan(pos_, count);
        pos_ += count;
        return slice;
    }

    std::string read_string(std::size_t count) {
        const auto slice = bytes(count);
        if (!ok_) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(slice.data()), slice.size());
    }

  private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace oscilline
