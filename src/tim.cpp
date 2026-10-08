// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// TIM header, CLUT, and pixel indexes.

#include "oscilline/tim.hpp"

#include "oscilline/byte_reader.hpp"

namespace oscilline {
namespace {

// 15-bit color. STP clear on black is transparent. STP set on any other
// color is the semi-transparent case.
Rgba8 from_1555(std::uint16_t color) {
    const int red = color & 31;
    const int green = (color >> 5) & 31;
    const int blue = (color >> 10) & 31;
    const bool stp = (color & 0x8000) != 0;
    const bool zero = (color & 0x7FFF) == 0;
    Rgba8 pixel;
    pixel.r = static_cast<std::uint8_t>(red * 255 / 31);
    pixel.g = static_cast<std::uint8_t>(green * 255 / 31);
    pixel.b = static_cast<std::uint8_t>(blue * 255 / 31);
    if (!stp && zero) {
        pixel.a = 0;
    } else if (stp && !zero) {
        pixel.a = 128;
    } else {
        pixel.a = 255;
    }
    return pixel;
}

Result<std::vector<Rgba8>> read_clut(ByteReader& reader, int& x, int& y, int& width, int& height) {
    const std::uint32_t block = reader.u32();
    x = reader.i16();
    y = reader.i16();
    width = reader.u16();
    height = reader.u16();
    if (!reader.ok() || width < 0 || height < 0) {
        return Result<std::vector<Rgba8>>::failure("TIM CLUT header is truncated");
    }
    const std::uint64_t colors =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (colors > 65536) {
        return Result<std::vector<Rgba8>>::failure("TIM CLUT is unreasonably large");
    }
    const std::uint64_t bytes = colors * 2;
    if (block < 12 || static_cast<std::uint64_t>(block) - 12 < bytes) {
        return Result<std::vector<Rgba8>>::failure("TIM CLUT block is shorter than its colors");
    }
    if (bytes > reader.remaining()) {
        return Result<std::vector<Rgba8>>::failure("TIM CLUT runs past the end of the file");
    }
    std::vector<Rgba8> clut;
    clut.reserve(static_cast<std::size_t>(colors));
    for (std::uint64_t i = 0; i < colors; ++i) {
        clut.push_back(from_1555(reader.u16()));
    }
    const std::uint64_t padding = static_cast<std::uint64_t>(block) - 12 - bytes;
    if (padding > reader.remaining()) {
        return Result<std::vector<Rgba8>>::failure(
            "TIM CLUT padding runs past the end of the file");
    }
    reader.skip(static_cast<std::size_t>(padding));
    if (!reader.ok()) {
        return Result<std::vector<Rgba8>>::failure("TIM CLUT is truncated");
    }
    return Result<std::vector<Rgba8>>::success(std::move(clut));
}

Rgba8 clut_at(const std::vector<Rgba8>& clut, int width, int index) {
    if (width <= 0 || index < 0 || index >= width ||
        static_cast<std::size_t>(index) >= clut.size()) {
        return {};
    }
    return clut[static_cast<std::size_t>(index)];
}

} // namespace

Result<TimImage> parse_tim(std::span<const std::uint8_t> bytes) {
    ByteReader reader(bytes);
    const std::uint32_t id = reader.u32();
    const std::uint32_t flags = reader.u32();
    if (!reader.ok() || id != 0x10) {
        return Result<TimImage>::failure("TIM id is not 0x10");
    }
    const int type = static_cast<int>(flags & 7u);
    const bool has_clut = (flags & 8u) != 0;
    int bpp = 0;
    switch (type) {
    case 0:
        bpp = 4;
        break;
    case 1:
    case 5:
        bpp = 8;
        break;
    case 2:
        bpp = 16;
        break;
    case 3:
        bpp = 24;
        break;
    default:
        return Result<TimImage>::failure("TIM color type is not 4, 8, 16, or 24 bpp");
    }

    TimImage image;
    image.bpp = bpp;
    image.has_clut = has_clut;
    image.flags = flags;
    if (has_clut) {
        auto clut =
            read_clut(reader, image.clut_x, image.clut_y, image.clut_width, image.clut_height);
        if (!clut) {
            return Result<TimImage>::failure(clut.error());
        }
        image.clut = std::move(clut.value());
    }

    const std::uint32_t block = reader.u32();
    image.origin_x = reader.i16();
    image.origin_y = reader.i16();
    image.vram_width = reader.u16();
    image.height = reader.u16();
    if (!reader.ok()) {
        return Result<TimImage>::failure("TIM pixel header is truncated");
    }
    if (image.vram_width < 0 || image.height < 0) {
        return Result<TimImage>::failure("TIM dimensions are negative");
    }
    const std::uint64_t stored = static_cast<std::uint64_t>(image.vram_width) *
                                 static_cast<std::uint64_t>(image.height) * 2u;
    if (stored > 16u * 1024u * 1024u) {
        return Result<TimImage>::failure("TIM image is unreasonably large");
    }
    if (block < 12 || static_cast<std::uint64_t>(block) - 12 < stored ||
        stored > reader.remaining()) {
        return Result<TimImage>::failure("TIM pixel block does not match its dimensions");
    }
    const auto pixel_bytes = reader.bytes(static_cast<std::size_t>(stored));
    if (!reader.ok()) {
        return Result<TimImage>::failure("TIM pixels are truncated");
    }

    if (bpp == 4) {
        image.width = image.vram_width * 4;
    } else if (bpp == 8) {
        image.width = image.vram_width * 2;
    } else if (bpp == 16) {
        image.width = image.vram_width;
    } else {
        if ((image.vram_width * 2) % 3 != 0) {
            return Result<TimImage>::failure("24 bpp TIM width is not a multiple of 3 bytes");
        }
        image.width = (image.vram_width * 2) / 3;
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(image.width) * static_cast<std::uint64_t>(image.height);
    image.pixels.resize(static_cast<std::size_t>(pixel_count));

    if (bpp == 4 || bpp == 8) {
        std::size_t pixel = 0;
        for (std::uint8_t byte : pixel_bytes) {
            if (bpp == 4) {
                const int indexes[2] = {byte & 0x0F, (byte >> 4) & 0x0F};
                for (int index : indexes) {
                    if (pixel >= image.pixels.size()) {
                        break;
                    }
                    image.pixels[pixel++] = has_clut ? clut_at(image.clut, image.clut_width, index)
                                                     : Rgba8{static_cast<std::uint8_t>(index * 17),
                                                             static_cast<std::uint8_t>(index * 17),
                                                             static_cast<std::uint8_t>(index * 17),
                                                             255};
                }
            } else if (pixel < image.pixels.size()) {
                image.pixels[pixel++] = has_clut ? clut_at(image.clut, image.clut_width, byte)
                                                 : Rgba8{byte, byte, byte, 255};
            }
        }
    } else if (bpp == 16) {
        for (std::size_t i = 0; i + 1 < pixel_bytes.size() && i / 2 < image.pixels.size(); i += 2) {
            const std::uint16_t color =
                static_cast<std::uint16_t>(pixel_bytes[i] | (pixel_bytes[i + 1] << 8));
            image.pixels[i / 2] = from_1555(color);
        }
    } else {
        std::size_t pixel = 0;
        for (std::size_t i = 0; i + 2 < pixel_bytes.size() && pixel < image.pixels.size(); i += 3) {
            image.pixels[pixel++] =
                Rgba8{pixel_bytes[i], pixel_bytes[i + 1], pixel_bytes[i + 2], 255};
        }
    }
    return Result<TimImage>::success(std::move(image));
}

} // namespace oscilline
