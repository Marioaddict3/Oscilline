// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// JSON for one parsed asset, including optional full PCM.

#include "oscilline/inspect.hpp"

#include "oscilline/anc.hpp"
#include "oscilline/anm.hpp"
#include "oscilline/fsl.hpp"
#include "oscilline/pak.hpp"
#include "oscilline/tim.hpp"
#include "oscilline/tmd.hpp"
#include "oscilline/vab.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace oscilline {
namespace {

constexpr std::size_t kMaxPixels = 4096;
constexpr std::size_t kPcmHead = 8;

class Json {
  public:
    void begin_object() { open('{'); }
    void end_object() { close('}'); }
    void begin_array() { open('['); }
    void end_array() { close(']'); }

    void key(std::string_view name) {
        comma();
        quote(name);
        out_ += ": ";
    }

    void item() { comma(); }

    void string(std::string_view text) { quote(text); }

    void number(std::int64_t value) { out_ += std::to_string(value); }

    void boolean(bool value) { out_ += value ? "true" : "false"; }

    std::string finish() {
        out_.push_back('\n');
        return std::move(out_);
    }

  private:
    void open(char bracket) {
        out_.push_back(bracket);
        first_.push_back(true);
    }

    void close(char bracket) {
        const bool empty = first_.back();
        first_.pop_back();
        if (!empty) {
            out_.push_back('\n');
            out_.append(first_.size() * 2, ' ');
        }
        out_.push_back(bracket);
    }

    void comma() {
        if (!first_.back()) {
            out_ += ",\n";
        } else {
            out_.push_back('\n');
            first_.back() = false;
        }
        out_.append(first_.size() * 2, ' ');
    }

    void quote(std::string_view text) {
        out_.push_back('"');
        for (unsigned char ch : text) {
            switch (ch) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if (ch < 0x20 || ch >= 0x80) {
                    constexpr char kHex[] = "0123456789abcdef";
                    out_ += "\\u00";
                    out_.push_back(kHex[ch >> 4]);
                    out_.push_back(kHex[ch & 0x0F]);
                } else {
                    out_.push_back(static_cast<char>(ch));
                }
                break;
            }
        }
        out_.push_back('"');
    }

    std::string out_;
    std::vector<char> first_;
};

std::string lower_copy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

void rgba(Json& json, const Rgba8& pixel) {
    json.item();
    json.begin_array();
    json.item();
    json.number(pixel.r);
    json.item();
    json.number(pixel.g);
    json.item();
    json.number(pixel.b);
    json.item();
    json.number(pixel.a);
    json.end_array();
}

const char* primitive_kind(PrimitiveKind kind) {
    switch (kind) {
    case PrimitiveKind::Polygon:
        return "polygon";
    case PrimitiveKind::Line:
        return "line";
    case PrimitiveKind::Rectangle:
        return "rectangle";
    case PrimitiveKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string pak_json(const PakArchive& archive) {
    Json json;
    json.begin_object();
    json.key("type");
    json.string("pak");
    json.key("count");
    json.number(static_cast<std::int64_t>(archive.entries.size()));
    json.key("entries");
    json.begin_array();
    for (const PakEntry& entry : archive.entries) {
        json.item();
        json.begin_object();
        json.key("name");
        json.string(entry.name);
        json.key("offset");
        json.number(entry.offset);
        json.key("size");
        json.number(entry.size);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return json.finish();
}

std::string tim_json(const TimImage& image) {
    Json json;
    json.begin_object();
    json.key("type");
    json.string("tim");
    json.key("bpp");
    json.number(image.bpp);
    json.key("has_clut");
    json.boolean(image.has_clut);
    json.key("flags");
    json.number(image.flags);
    json.key("width");
    json.number(image.width);
    json.key("height");
    json.number(image.height);
    json.key("vram_width");
    json.number(image.vram_width);
    json.key("clut_width");
    json.number(image.clut_width);
    json.key("clut_height");
    json.number(image.clut_height);
    json.key("clut");
    json.begin_array();
    const std::size_t clut_count = std::min(image.clut.size(), kMaxPixels);
    for (std::size_t i = 0; i < clut_count; ++i) {
        rgba(json, image.clut[i]);
    }
    json.end_array();
    json.key("pixels");
    json.begin_array();
    const std::size_t pixel_count = std::min(image.pixels.size(), kMaxPixels);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        rgba(json, image.pixels[i]);
    }
    json.end_array();
    json.key("pixels_truncated");
    json.boolean(image.pixels.size() > kMaxPixels);
    json.end_object();
    return json.finish();
}

std::string tmd_json(const TmdModel& model) {
    Json json;
    json.begin_object();
    json.key("type");
    json.string("tmd");
    json.key("flags");
    json.number(model.flags);
    json.key("fixp");
    json.boolean(model.fixp);
    json.key("objects");
    json.begin_array();
    for (const TmdObject& object : model.objects) {
        json.item();
        json.begin_object();
        json.key("scale");
        json.number(object.scale);
        json.key("vertex_offset");
        json.number(object.vertex_offset);
        json.key("vertices");
        json.begin_array();
        for (const TmdVertex& vertex : object.vertices) {
            json.item();
            json.begin_array();
            json.item();
            json.number(vertex.x);
            json.item();
            json.number(vertex.y);
            json.item();
            json.number(vertex.z);
            json.end_array();
        }
        json.end_array();
        json.key("normals");
        json.begin_array();
        for (const TmdVertex& normal : object.normals) {
            json.item();
            json.begin_array();
            json.item();
            json.number(normal.x);
            json.item();
            json.number(normal.y);
            json.item();
            json.number(normal.z);
            json.end_array();
        }
        json.end_array();
        json.key("primitives");
        json.begin_array();
        for (const TmdPrimitive& primitive : object.primitives) {
            json.item();
            json.begin_object();
            json.key("kind");
            json.string(primitive_kind(primitive.kind));
            json.key("mode");
            json.number(primitive.mode);
            json.key("flag");
            json.number(primitive.flag);
            json.key("input_words");
            json.number(primitive.input_words);
            json.key("textured");
            json.boolean(primitive.textured);
            json.key("quad");
            json.boolean(primitive.quad);
            json.key("unlit");
            json.boolean(primitive.unlit);
            json.key("gouraud");
            json.boolean(primitive.gouraud);
            json.key("color");
            json.number(primitive.color);
            json.key("vertex_indices");
            json.begin_array();
            for (std::uint16_t index : primitive.vertex_indices) {
                json.item();
                json.number(index);
            }
            json.end_array();
            json.key("normal_indices");
            json.begin_array();
            for (std::uint16_t index : primitive.normal_indices) {
                json.item();
                json.number(index);
            }
            json.end_array();
            json.key("raw_bytes");
            json.number(static_cast<std::int64_t>(primitive.raw.size()));
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return json.finish();
}

std::string anc_json(const AncFile& file) {
    Json json;
    json.begin_object();
    json.key("type");
    json.string("anc");
    json.key("magic");
    json.number(file.magic);
    json.key("key_count");
    json.number(file.key_count);
    json.key("reserved");
    json.number(file.reserved);
    json.key("record_bytes");
    json.number(kAncRecordBytes);
    json.key("keys");
    json.begin_array();
    for (const AncKeyframe& key : file.keys) {
        json.item();
        json.begin_object();
        json.key("eye");
        json.begin_array();
        json.item();
        json.number(key.eye_x);
        json.item();
        json.number(key.eye_y);
        json.item();
        json.number(key.eye_z);
        json.end_array();
        json.key("target");
        json.begin_array();
        json.item();
        json.number(key.target_x);
        json.item();
        json.number(key.target_y);
        json.item();
        json.number(key.target_z);
        json.end_array();
        json.key("roll");
        json.number(key.roll);
        json.key("fov");
        json.number(key.fov);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return json.finish();
}

std::string anm_json(const AnmFile& file) {
    Json json;
    json.begin_object();
    json.key("type");
    json.string("anm");
    json.key("unk0");
    json.number(file.unk0);
    json.key("unk1");
    json.number(file.unk1);
    json.key("frame_count");
    json.number(file.frame_count);
    json.key("frame_offset_table");
    json.begin_array();
    for (std::uint16_t offset : file.frame_offset_table) {
        json.item();
        json.number(offset);
    }
    json.end_array();
    json.key("frames");
    json.begin_array();
    for (const AnmFrame& frame : file.frames) {
        json.item();
        json.begin_object();
        json.key("keys");
        json.begin_array();
        for (const AnmKeyframe& key : frame.keys) {
            json.item();
            json.begin_object();
            json.key("object");
            json.number(key.object_index);
            json.key("flags");
            json.number(key.flags);
            json.key("rotation");
            json.begin_array();
            json.item();
            json.number(key.rotation_x);
            json.item();
            json.number(key.rotation_y);
            json.item();
            json.number(key.rotation_z);
            json.end_array();
            json.key("scale");
            json.begin_array();
            json.item();
            json.number(key.scale_x);
            json.item();
            json.number(key.scale_y);
            json.item();
            json.number(key.scale_z);
            json.end_array();
            json.key("position");
            json.begin_array();
            json.item();
            json.number(key.position_x);
            json.item();
            json.number(key.position_y);
            json.item();
            json.number(key.position_z);
            json.end_array();
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return json.finish();
}

void write_vags(Json& json, const VabBank& bank, bool full_pcm) {
    json.key("vags");
    json.begin_array();
    for (std::size_t index = 0; index < bank.vags.size(); ++index) {
        const DecodedVag& vag = bank.vags[index];
        if (vag.size_units == 0) {
            continue;
        }
        json.item();
        json.begin_object();
        json.key("index");
        json.number(static_cast<std::int64_t>(index));
        json.key("size_units");
        json.number(vag.size_units);
        json.key("sample_count");
        json.number(static_cast<std::int64_t>(vag.pcm.size()));
        json.key("last_flags");
        json.number(vag.last_flags);
        json.key("pcm_head");
        json.begin_array();
        const std::size_t head = std::min(vag.pcm.size(), kPcmHead);
        for (std::size_t sample = 0; sample < head; ++sample) {
            json.item();
            json.number(vag.pcm[sample]);
        }
        json.end_array();
        if (full_pcm) {
            json.key("pcm");
            json.begin_array();
            for (std::int16_t sample : vag.pcm) {
                json.item();
                json.number(sample);
            }
            json.end_array();
        }
        json.end_object();
    }
    json.end_array();
}

std::string vab_json(const VabBank& bank, bool full_pcm) {
    Json json;
    json.begin_object();
    json.key("type");
    json.string("vh");
    json.key("version");
    json.number(bank.header.version);
    json.key("bank_id");
    json.number(bank.header.bank_id);
    json.key("total_size");
    json.number(bank.header.total_size);
    json.key("program_count");
    json.number(bank.header.program_count);
    json.key("program_count_field");
    json.number(bank.header.program_count_field);
    json.key("tone_count_field");
    json.number(bank.header.tone_count_field);
    json.key("vag_count_field");
    json.number(bank.header.vag_count_field);
    json.key("master_volume");
    json.number(bank.header.master_volume);
    json.key("master_pan");
    json.number(bank.header.master_pan);
    json.key("has_body");
    json.boolean(bank.has_body);
    json.key("programs");
    json.begin_array();
    // The VH always stores 128 program slots. Only the live prefix is useful.
    const std::size_t live_programs =
        std::min(bank.header.programs.size(), static_cast<std::size_t>(bank.header.program_count));
    for (std::size_t index = 0; index < live_programs; ++index) {
        const VabProgram& program = bank.header.programs[index];
        json.item();
        json.begin_object();
        json.key("tone_count");
        json.number(program.tone_count);
        json.key("volume");
        json.number(program.volume);
        json.key("priority");
        json.number(program.priority);
        json.key("mode");
        json.number(program.mode);
        json.key("pan");
        json.number(program.pan);
        json.key("attribute");
        json.number(program.attribute);
        json.end_object();
    }
    json.end_array();
    json.key("tones");
    json.begin_array();
    for (const VabTone& tone : bank.header.tones) {
        json.item();
        json.begin_object();
        json.key("priority");
        json.number(tone.priority);
        json.key("mode");
        json.number(tone.mode);
        json.key("volume");
        json.number(tone.volume);
        json.key("pan");
        json.number(tone.pan);
        json.key("center");
        json.number(tone.center);
        json.key("shift");
        json.number(tone.shift);
        json.key("note_min");
        json.number(tone.note_min);
        json.key("note_max");
        json.number(tone.note_max);
        json.key("vib_w");
        json.number(tone.vib_w);
        json.key("vib_t");
        json.number(tone.vib_t);
        json.key("por_w");
        json.number(tone.por_w);
        json.key("por_t");
        json.number(tone.por_t);
        json.key("pitch_bend_min");
        json.number(tone.pitch_bend_min);
        json.key("pitch_bend_max");
        json.number(tone.pitch_bend_max);
        json.key("adsr1");
        json.number(tone.adsr1);
        json.key("adsr2");
        json.number(tone.adsr2);
        json.key("program");
        json.number(tone.program);
        json.key("vag");
        json.number(tone.vag);
        json.end_object();
    }
    json.end_array();
    if (bank.has_body) {
        write_vags(json, bank, full_pcm);
    }
    json.end_object();
    return json.finish();
}

std::string fsl_json(const FslFile& file) {
    Json json;
    json.begin_object();
    json.key("type");
    json.string("fsl");
    json.key("unk1");
    json.number(file.header.unk1);
    json.key("unk2");
    json.number(file.header.unk2);
    json.key("unk3");
    json.number(file.header.unk3);
    json.key("fixed_pattern_offset");
    json.number(file.fixed_pattern_offset);
    json.key("distribution_pattern_offset");
    json.number(file.distribution_pattern_offset);
    json.key("control_segment_offset");
    json.number(file.control_segment_offset);
    json.key("pattern_segment_offset");
    json.number(file.pattern_segment_offset);
    json.key("event_segment_offset");
    json.number(file.event_segment_offset);
    json.key("track_index_offset");
    json.number(file.track_index_offset);
    json.key("fixed_patterns");
    json.begin_array();
    for (const FslFixedPattern& pattern : file.fixed_patterns) {
        json.item();
        json.begin_array();
        for (std::uint32_t obstacle : pattern.obstacles) {
            json.item();
            json.number(obstacle);
        }
        json.end_array();
    }
    json.end_array();
    json.key("distribution_patterns");
    json.begin_array();
    for (const FslDistributionPattern& pattern : file.distribution_patterns) {
        json.item();
        json.begin_object();
        json.key("count");
        json.number(pattern.count);
        json.key("random_seed");
        json.number(pattern.random_seed);
        json.key("max_prob");
        json.number(pattern.max_prob);
        json.end_object();
    }
    json.end_array();
    json.key("control_tracks");
    json.begin_array();
    for (const FslControlTrack& track : file.control_tracks) {
        json.item();
        json.begin_array();
        for (const FslControlSegment& segment : track.segments) {
            json.item();
            json.begin_object();
            json.key("start_time");
            json.number(segment.start_time);
            json.key("base_speed");
            json.number(segment.base_speed);
            json.end_object();
        }
        json.end_array();
    }
    json.end_array();
    json.key("pattern_tracks");
    json.begin_array();
    for (const FslPatternTrack& track : file.pattern_tracks) {
        json.item();
        json.begin_array();
        for (const FslPatternSegment& segment : track.segments) {
            json.item();
            json.begin_object();
            json.key("start_time");
            json.number(segment.start_time);
            json.key("pattern_type");
            json.number(segment.pattern_type);
            json.key("index");
            json.number(segment.index);
            json.end_object();
        }
        json.end_array();
    }
    json.end_array();
    json.key("event_tracks");
    json.begin_array();
    for (const FslEventTrack& track : file.event_tracks) {
        json.item();
        json.begin_array();
        for (const FslEventSegment& segment : track.segments) {
            json.item();
            json.begin_object();
            json.key("start_time");
            json.number(segment.start_time);
            json.key("camera_event");
            json.number(segment.camera_event);
            json.end_object();
        }
        json.end_array();
    }
    json.end_array();
    json.key("track_index");
    json.begin_array();
    for (const FslTrackIndex& index : file.track_index) {
        json.item();
        json.begin_array();
        json.item();
        json.number(index.control_segment_index);
        json.item();
        json.number(index.pattern_segment_index);
        json.item();
        json.number(index.event_segment_index);
        json.end_array();
    }
    json.end_array();
    json.key("trailing_bytes");
    json.number(static_cast<std::int64_t>(file.trailing.size()));
    json.end_object();
    return json.finish();
}

Result<std::string> vab_from_request(const InspectRequest& request, bool body_required) {
    if (body_required && request.body.empty()) {
        return Result<std::string>::failure("a VB file needs the matching VH via --vh");
    }
    if (request.body.empty()) {
        auto header = parse_vh(request.bytes);
        if (!header) {
            return Result<std::string>::failure(header.error());
        }
        VabBank bank;
        bank.header = std::move(header.value());
        return Result<std::string>::success(vab_json(bank, false));
    }
    auto bank = parse_vab(request.bytes, request.body);
    if (!bank) {
        return Result<std::string>::failure(bank.error());
    }
    return Result<std::string>::success(vab_json(bank.value(), request.full_pcm));
}

} // namespace

Result<std::string> inspect_to_json(const InspectRequest& request) {
    const std::string type = lower_copy(request.type);
    if (type == "pak") {
        auto parsed = parse_pak(request.bytes);
        if (!parsed) {
            return Result<std::string>::failure(parsed.error());
        }
        return Result<std::string>::success(pak_json(parsed.value()));
    }
    if (type == "tim") {
        auto parsed = parse_tim(request.bytes);
        if (!parsed) {
            return Result<std::string>::failure(parsed.error());
        }
        return Result<std::string>::success(tim_json(parsed.value()));
    }
    if (type == "tmd") {
        auto parsed = parse_tmd(request.bytes);
        if (!parsed) {
            return Result<std::string>::failure(parsed.error());
        }
        return Result<std::string>::success(tmd_json(parsed.value()));
    }
    if (type == "anm") {
        auto parsed = parse_anm(request.bytes);
        if (!parsed) {
            return Result<std::string>::failure(parsed.error());
        }
        return Result<std::string>::success(anm_json(parsed.value()));
    }
    if (type == "anc") {
        auto parsed = parse_anc(request.bytes);
        if (!parsed) {
            return Result<std::string>::failure(parsed.error());
        }
        return Result<std::string>::success(anc_json(parsed.value()));
    }
    if (type == "vh") {
        return vab_from_request(request, false);
    }
    if (type == "vb") {
        return vab_from_request(request, true);
    }
    if (type == "fsl") {
        auto parsed = parse_fsl(request.bytes);
        if (!parsed) {
            return Result<std::string>::failure(parsed.error());
        }
        return Result<std::string>::success(fsl_json(parsed.value()));
    }
    return Result<std::string>::failure("unknown inspect type '" + request.type + "'");
}

} // namespace oscilline
