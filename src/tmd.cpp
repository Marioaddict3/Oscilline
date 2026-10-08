// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Objects, primitives, and the FIXP offset base.

#include "oscilline/tmd.hpp"

#include "oscilline/byte_reader.hpp"

namespace oscilline {
namespace {

constexpr std::uint32_t kMaxObjects = 4096;
constexpr std::uint32_t kMaxElements = 200000;

// The object table begins at file offset 0x0C. FIXP = 0 adds that base to
// every object entry's vertex, normal, and primitive offsets. FIXP = 1 leaves
// the offsets file-absolute.
constexpr std::uint32_t kObjectTable = 0x0C;

std::uint64_t located(bool fixp, std::uint32_t offset) {
    const std::uint64_t base = fixp ? 0 : kObjectTable;
    return base + offset;
}

std::string parse_polygon(ByteReader& body, TmdPrimitive& primitive) {
    const bool quad = (primitive.mode & 0x08) != 0;
    const bool textured = (primitive.mode & 0x04) != 0;
    const int vertices = quad ? 4 : 3;
    primitive.quad = quad;
    primitive.textured = textured;
    primitive.color = body.u32();
    if (textured) {
        primitive.texcoord_words.resize(static_cast<std::size_t>(vertices));
        for (std::uint32_t& word : primitive.texcoord_words) {
            word = body.u32();
        }
    }
    // Flag bit 2: extra colors after the first (color 2..N).
    if ((primitive.flag & 0x04) != 0) {
        primitive.extra_colors.resize(static_cast<std::size_t>(vertices - 1));
        for (std::uint32_t& color : primitive.extra_colors) {
            color = body.u32();
        }
    }
    primitive.vertex_indices.resize(static_cast<std::size_t>(vertices));
    for (int i = 0; i < vertices; ++i) {
        // Normal 1 is present unless lighting is off. Later normals follow Mode bit 4.
        const bool has_normal =
            i == 0 ? (primitive.flag & 0x01) == 0 : (primitive.mode & 0x10) != 0;
        if (has_normal) {
            primitive.normal_indices.push_back(body.u16());
        }
        primitive.vertex_indices[static_cast<std::size_t>(i)] = body.u16();
    }
    if (!body.ok()) {
        return "TMD polygon packet is shorter than its mode";
    }
    return {};
}

std::string parse_line(ByteReader& body, TmdPrimitive& primitive) {
    primitive.color = body.u32();
    // psx-spx line packets: a second color only when Mode bit 4 is set. No normals.
    if ((primitive.mode & 0x10) != 0) {
        primitive.gouraud = true;
        primitive.extra_colors.push_back(body.u32());
    }
    primitive.vertex_indices.push_back(body.u16());
    primitive.vertex_indices.push_back(body.u16());
    if (!body.ok()) {
        return "TMD line packet is shorter than its mode";
    }
    return {};
}

std::string read_vectors(ByteReader& reader,
                         std::uint32_t count,
                         std::vector<TmdVertex>& out,
                         const char* what) {
    if (count > kMaxElements) {
        return std::string("TMD ") + what + " count is unreasonable";
    }
    if (static_cast<std::uint64_t>(count) * 8u > reader.remaining()) {
        return std::string("TMD ") + what + " list is truncated";
    }
    out.resize(count);
    for (TmdVertex& vertex : out) {
        vertex.x = reader.i16();
        vertex.y = reader.i16();
        vertex.z = reader.i16();
        reader.u16();
    }
    if (!reader.ok()) {
        return std::string("TMD ") + what + " list is truncated";
    }
    return {};
}

} // namespace

Result<TmdModel> parse_tmd(std::span<const std::uint8_t> bytes) {
    ByteReader reader(bytes);
    const std::uint32_t id = reader.u32();
    const std::uint32_t flags = reader.u32();
    const std::uint32_t object_count = reader.u32();
    if (!reader.ok() || id != 0x41) {
        return Result<TmdModel>::failure("TMD id is not 0x41");
    }
    if (object_count > kMaxObjects) {
        return Result<TmdModel>::failure("TMD object count is unreasonable");
    }

    TmdModel model;
    model.flags = flags;
    model.fixp = (flags & 1u) != 0;
    model.objects.resize(object_count);

    for (std::uint32_t index = 0; index < object_count; ++index) {
        const std::uint32_t entry = 0x0Cu + index * 0x1Cu;
        reader.seek(entry);
        const std::uint32_t vertex_offset = reader.u32();
        const std::uint32_t vertex_count = reader.u32();
        const std::uint32_t normal_offset = reader.u32();
        const std::uint32_t normal_count = reader.u32();
        const std::uint32_t primitive_offset = reader.u32();
        const std::uint32_t primitive_count = reader.u32();
        const std::int32_t scale = reader.i32();
        if (!reader.ok()) {
            return Result<TmdModel>::failure("TMD object entry is truncated");
        }
        if (primitive_count > kMaxElements) {
            return Result<TmdModel>::failure("TMD primitive count is unreasonable");
        }

        TmdObject& object = model.objects[index];
        object.scale = scale;
        const auto place = [&](std::uint32_t offset, std::uint32_t count) -> std::uint64_t {
            if (count == 0) {
                return 0;
            }
            return located(model.fixp, offset);
        };
        const std::uint64_t vertices_at = place(vertex_offset, vertex_count);
        const std::uint64_t normals_at = place(normal_offset, normal_count);
        const std::uint64_t primitives_at = place(primitive_offset, primitive_count);
        object.vertex_offset = static_cast<std::uint32_t>(vertices_at);
        object.normal_offset = static_cast<std::uint32_t>(normals_at);
        object.primitive_offset = static_cast<std::uint32_t>(primitives_at);

        if (vertex_count != 0) {
            if (vertices_at > bytes.size()) {
                return Result<TmdModel>::failure("TMD vertex list is past the end of the file");
            }
            reader.seek(static_cast<std::size_t>(vertices_at));
            const std::string error = read_vectors(reader, vertex_count, object.vertices, "vertex");
            if (!error.empty()) {
                return Result<TmdModel>::failure(error);
            }
        }
        if (normal_count != 0) {
            if (normals_at > bytes.size()) {
                return Result<TmdModel>::failure("TMD normal list is past the end of the file");
            }
            reader.seek(static_cast<std::size_t>(normals_at));
            const std::string error = read_vectors(reader, normal_count, object.normals, "normal");
            if (!error.empty()) {
                return Result<TmdModel>::failure(error);
            }
        }
        if (primitive_count == 0) {
            continue;
        }
        if (primitives_at > bytes.size()) {
            return Result<TmdModel>::failure("TMD primitive list is past the end of the file");
        }
        reader.seek(static_cast<std::size_t>(primitives_at));
        object.primitives.reserve(primitive_count);
        for (std::uint32_t primitive_index = 0; primitive_index < primitive_count;
             ++primitive_index) {
            const std::size_t start = reader.position();
            TmdPrimitive primitive;
            primitive.output_words = reader.u8();
            primitive.input_words = reader.u8();
            primitive.flag = reader.u8();
            primitive.mode = reader.u8();
            if (!reader.ok()) {
                return Result<TmdModel>::failure("TMD primitive header is truncated");
            }
            // ilen is the number of body words. The 4-byte header is not included,
            // so the packet is (ilen + 1) * 4 bytes. ilen 0 is a header-only packet.
            const std::size_t body_size = static_cast<std::size_t>(primitive.input_words) * 4u;
            const std::size_t packet_bytes = body_size + 4u;
            if (reader.remaining() < body_size) {
                return Result<TmdModel>::failure("TMD primitive packet is truncated");
            }
            const auto body_bytes = reader.bytes(body_size);
            if (!reader.ok() || start + packet_bytes > bytes.size()) {
                return Result<TmdModel>::failure("TMD primitive packet is truncated");
            }
            primitive.raw.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(start + packet_bytes));
            primitive.unlit = (primitive.flag & 0x01) != 0;
            primitive.no_backface_clip = (primitive.flag & 0x02) != 0;
            primitive.gouraud = (primitive.flag & 0x04) != 0;

            const int klass = primitive.mode >> 5;
            std::string error;
            if (klass == 1) {
                primitive.kind = PrimitiveKind::Polygon;
                ByteReader body(body_bytes);
                error = parse_polygon(body, primitive);
            } else if (klass == 2) {
                primitive.kind = PrimitiveKind::Line;
                ByteReader body(body_bytes);
                error = parse_line(body, primitive);
            } else if (klass == 3) {
                // psx-spx: rectangle packet body is "Unknown". Keep the raw packet.
                primitive.kind = PrimitiveKind::Rectangle;
            } else {
                primitive.kind = PrimitiveKind::Unknown;
            }
            if (!error.empty()) {
                return Result<TmdModel>::failure(error);
            }
            object.primitives.push_back(std::move(primitive));
        }
    }
    return Result<TmdModel>::success(std::move(model));
}

} // namespace oscilline
