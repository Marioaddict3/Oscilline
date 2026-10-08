// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// TMD and ANM viewer, the ANC path overlay, and the SFX browser.

#include "oscilline/anc.hpp"
#include "oscilline/anm.hpp"
#include "oscilline/audio/sfx.hpp"
#include "oscilline/disc/archive.hpp"
#include "oscilline/present/host.hpp"
#include "oscilline/present/sfx_out.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/tmd.hpp"
#include "oscilline/version.hpp"

#include <SDL3/SDL_main.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

oscilline::ModelView g_model_view = oscilline::ModelView::Side;

namespace {

void print_usage(std::ostream& out) {
    out << "Oscilline " << oscilline::version_string() << "\n"
        << "Usage: oscilline-viewer --disc <cue> [options]\n"
        << "       oscilline-viewer --sfx [--disc <cue>] [options]\n"
        << "\n"
        << "Draws TMD wireframes from a disc, with ANM playback.\n"
        << "  --disc <cue>     BIN/CUE or ISO you own\n"
        << "  --pak <path>     archive inside the disc (default: English, then Japanese)\n"
        << "  --tmd <name>     model path inside the archive\n"
        << "  --anm <name>     animation path inside the archive\n"
        << "  --anc <name>     camera path inside the archive (title CAM if the game pak lacks "
           "it)\n"
        << "  --anc-step N     eye-to-target stride (default aims at about 50; 1 draws every "
           "key)\n"
        << "  --frames N       present N frames and exit\n"
        << "  --view side|front  camera side (default side: the in-game profile)\n"
        << "  --sfx            list every SfxId and play the selected cue\n"
        << "\n"
        << "Left and right change the model and pick an animation from its folder.\n"
        << "Up and down change the animation.\n"
        << "Drag with the left mouse button to orbit around the framed model.\n"
        << "The wheel zooms. Q and E yaw, R and F pitch, - and = zoom. P resets.\n"
        << "Tab opens the SFX browser from the model view and the camera view.\n"
        << "Tab again returns. Escape closes the window.\n"
        << "\n"
        << "SFX browser (Tab, or --sfx): identified IDs are SfxId values, the\n"
        << "kSfxMap index. menu-move is 0. Cleared is Cleared plus the obstacle\n"
        << "kind, then Missed. Names come from sfx_name(). Unidentified samples\n"
        << "follow that list. Their IDs start with U, and each row says\n"
        << "unidentified plus the bank, the zero-based VAG index, and the\n"
        << "program/tone when a tone points at that sample. Up and down step by\n"
        << "one. Left and right jump by 10. Enter or Space plays. Identified rows\n"
        << "use kSfxMap. Unidentified rows play that bank sample at 11025 Hz.\n"
        << "With no disc, the SfxId list still shows and the unidentified list is\n"
        << "empty. --sfx starts in the browser. Tab returns only when the browser\n"
        << "was opened from the model or camera view.\n";
}

int refuse(const std::string& message) {
    std::cerr << "oscilline-viewer: " << message << '\n';
    return EXIT_FAILURE;
}

std::optional<int> parse_frames(std::string_view text) {
    int value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end || value < 0) {
        return std::nullopt;
    }
    return value;
}

std::string directory_of(std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return {};
    }
    return std::string(name.substr(0, slash));
}

int matching_anm(const oscilline::ModelList& models, std::string_view tmd_name) {
    const std::string folder = directory_of(tmd_name);
    for (std::size_t i = 0; i < models.anm.size(); ++i) {
        if (directory_of(models.anm[i]) == folder) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct View {
    oscilline::TmdModel model;
    std::optional<oscilline::AnmFile> animation;
    std::string error;
    double time = 0;
    oscilline::Camera camera;
};

// Orbit and zoom on top of the framing camera. Yaw and pitch of zero, and a
// zoom of 1, leave that camera's picture unchanged.
struct Inspect {
    float yaw = 0.f;
    float pitch = 0.f;
    float zoom = 1.f;
};

constexpr float kZoomMin = 0.05f;
constexpr float kZoomMax = 40.f;
constexpr float kKeyOrbit = 1.6f;
constexpr float kMouseOrbit = 0.007f;
constexpr float kWheelZoom = 0.18f;
constexpr float kKeyZoom = 1.4f;

// Yaw around +Y, then pitch around +X, both about the framing target.
// Dragging right yaws so the front swings right. Dragging down pitches the
// top toward the camera, which is the camera rising around the model.
oscilline::ModelVertex orbit_vertex(oscilline::ModelVertex point,
                                    const oscilline::Camera& camera,
                                    const Inspect& inspect) {
    float x = point.x - camera.target_x;
    float y = point.y - camera.target_y;
    float z = point.z - camera.target_z;
    const float cy = std::cos(inspect.yaw);
    const float sy = std::sin(inspect.yaw);
    const float x1 = x * cy + z * sy;
    const float z1 = -x * sy + z * cy;
    const float cp = std::cos(inspect.pitch);
    const float sp = std::sin(inspect.pitch);
    const float y2 = y * cp - z1 * sp;
    const float z2 = y * sp + z1 * cp;
    point.x = x1 + camera.target_x;
    point.y = y2 + camera.target_y;
    point.z = z2 + camera.target_z;
    return point;
}

void update_inspect(Inspect& inspect, const oscilline::HostInput& input, double seconds) {
    const float dt = static_cast<float>(seconds);
    if (oscilline::held(input, oscilline::Key::LeftShoulder)) {
        inspect.yaw -= kKeyOrbit * dt;
    }
    if (oscilline::held(input, oscilline::Key::RightShoulder)) {
        inspect.yaw += kKeyOrbit * dt;
    }
    if (oscilline::held(input, oscilline::Key::LookUp)) {
        inspect.pitch -= kKeyOrbit * dt;
    }
    if (oscilline::held(input, oscilline::Key::LookDown)) {
        inspect.pitch += kKeyOrbit * dt;
    }
    inspect.yaw += input.drag_x * kMouseOrbit;
    inspect.pitch += input.drag_y * kMouseOrbit;
    if (input.wheel_y != 0.f) {
        inspect.zoom *= std::exp(-input.wheel_y * kWheelZoom);
    }
    if (oscilline::held(input, oscilline::Key::ZoomIn)) {
        inspect.zoom *= std::exp(-kKeyZoom * dt);
    }
    if (oscilline::held(input, oscilline::Key::ZoomOut)) {
        inspect.zoom *= std::exp(kKeyZoom * dt);
    }
    if (inspect.zoom < kZoomMin) {
        inspect.zoom = kZoomMin;
    }
    if (inspect.zoom > kZoomMax) {
        inspect.zoom = kZoomMax;
    }
    if (oscilline::pressed(input, oscilline::Key::Start)) {
        inspect = {};
    }
}

std::string inspect_text(const Inspect& inspect) {
    const float deg = 180.f / std::numbers::pi_v<float>;
    int yaw = static_cast<int>(std::lround(inspect.yaw * deg));
    int pitch = static_cast<int>(std::lround(inspect.pitch * deg));
    yaw %= 360;
    if (yaw > 180) {
        yaw -= 360;
    }
    if (yaw < -180) {
        yaw += 360;
    }
    pitch %= 360;
    if (pitch > 180) {
        pitch -= 360;
    }
    if (pitch < -180) {
        pitch += 360;
    }
    int hundredths = static_cast<int>(std::lround(inspect.zoom * 100.f));
    if (hundredths < 0) {
        hundredths = 0;
    }
    const int whole = hundredths / 100;
    const int frac = hundredths % 100;
    std::string text = "yaw " + std::to_string(yaw) + "  pitch " + std::to_string(pitch) +
                       "  zoom " + std::to_string(whole) + ".";
    if (frac < 10) {
        text += "0";
    }
    text += std::to_string(frac);
    return text;
}

// Frames the model once per view, over every frame of its animation, so the
// camera does not chase the figure while it moves.
void frame_view(View& view) {
    oscilline::ModelBounds bounds;
    if (view.animation && !view.animation->frames.empty()) {
        for (std::size_t frame = 0; frame < view.animation->frames.size(); ++frame) {
            const std::vector<oscilline::Pose> poses = oscilline::poses_for_frame(
                *view.animation, view.model.objects.size(), static_cast<int>(frame), 0.f, false);
            const std::vector<oscilline::ModelSegment> lines =
                oscilline::to_view_space(oscilline::tmd_wireframe(view.model, poses), g_model_view);
            oscilline::extend_bounds(bounds, lines);
        }
    } else {
        const std::vector<oscilline::ModelSegment> lines =
            oscilline::to_view_space(oscilline::tmd_wireframe(view.model), g_model_view);
        oscilline::extend_bounds(bounds, lines);
    }
    view.camera = oscilline::framing_camera(bounds);
}

View load_view(const oscilline::PakArchive& archive,
               const oscilline::ModelList& models,
               int tmd_index,
               int anm_index) {
    View view;
    if (tmd_index < 0 || static_cast<std::size_t>(tmd_index) >= models.tmd.size()) {
        view.error = "no TMD in this archive";
        return view;
    }
    const oscilline::PakEntry* tmd_entry =
        oscilline::find_entry(archive, models.tmd[static_cast<std::size_t>(tmd_index)]);
    if (tmd_entry == nullptr) {
        view.error = "missing " + models.tmd[static_cast<std::size_t>(tmd_index)];
        return view;
    }
    auto parsed = oscilline::parse_tmd(tmd_entry->data);
    if (!parsed) {
        view.error = parsed.error();
        return view;
    }
    view.model = std::move(parsed.value());
    if (anm_index < 0) {
        frame_view(view);
        return view;
    }
    if (static_cast<std::size_t>(anm_index) >= models.anm.size()) {
        view.error = "animation index is out of range";
        return view;
    }
    const oscilline::PakEntry* anm_entry =
        oscilline::find_entry(archive, models.anm[static_cast<std::size_t>(anm_index)]);
    if (anm_entry == nullptr) {
        view.error = "missing " + models.anm[static_cast<std::size_t>(anm_index)];
        return view;
    }
    auto animation = oscilline::parse_anm(anm_entry->data);
    if (!animation) {
        view.error = animation.error();
        return view;
    }
    view.animation = std::move(animation.value());
    frame_view(view);
    return view;
}

oscilline::TriangleList draw_view(const View& view, const Inspect& inspect) {
    oscilline::TriangleList triangles;
    if (!view.error.empty()) {
        return triangles;
    }
    std::vector<oscilline::Pose> poses;
    if (view.animation && !view.animation->frames.empty()) {
        const float rate = oscilline::anm_playback_hz(view.animation->unk1);
        const float cursor = static_cast<float>(view.time) * rate;
        const int count = static_cast<int>(view.animation->frames.size());
        const int frame = static_cast<int>(cursor) % count;
        const float fraction = cursor - static_cast<float>(static_cast<int>(cursor));
        poses = oscilline::poses_for_frame(
            *view.animation, view.model.objects.size(), frame, fraction, true);
    }
    auto model_lines =
        oscilline::to_view_space(oscilline::tmd_wireframe(view.model, poses), g_model_view);
    if (inspect.yaw != 0.f || inspect.pitch != 0.f) {
        for (oscilline::ModelSegment& segment : model_lines) {
            segment.a = orbit_vertex(segment.a, view.camera, inspect);
            segment.b = orbit_vertex(segment.b, view.camera, inspect);
        }
    }
    oscilline::Camera camera = view.camera;
    if (inspect.zoom != 1.f) {
        camera.eye_z *= inspect.zoom;
        if (!(camera.eye_z > camera.near_plane + 1.f)) {
            camera.eye_z = camera.near_plane + 1.f;
        }
    }
    const auto screen = oscilline::project_segments(model_lines, camera);
    oscilline::append_strokes(triangles, screen);
    return triangles;
}

int named_index(const oscilline::PakArchive& archive,
                const std::vector<std::string>& names,
                const std::string& wanted) {
    const oscilline::PakEntry* needle = oscilline::find_entry(archive, wanted);
    if (needle == nullptr) {
        return -1;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (oscilline::find_entry(archive, names[i]) == needle) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

oscilline::ModelVertex anc_vertex(std::int16_t x, std::int16_t y, std::int16_t z) {
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
}

oscilline::TriangleList
draw_anc(const oscilline::AncFile& file, float t, oscilline::Camera camera, int look_step) {
    std::vector<oscilline::ModelSegment> lines;
    // Eye and target polylines stay dense, so the path shape is intact.
    // Eye-to-target segments are strided: a road file's looks share a target
    // and would otherwise fill the view. Bounds still cover both ends.
    const std::size_t key_count = file.keys.size();
    for (std::size_t i = 0; i < key_count; ++i) {
        const oscilline::AncKeyframe& key = file.keys[i];
        if (oscilline::anc_overlay_draws_look(i, key_count, look_step)) {
            oscilline::ModelSegment look;
            look.a = anc_vertex(key.eye_x, key.eye_y, key.eye_z);
            look.b = anc_vertex(key.target_x, key.target_y, key.target_z);
            lines.push_back(look);
        }
        if (i == 0) {
            continue;
        }
        const oscilline::AncKeyframe& prev = file.keys[i - 1];
        oscilline::ModelSegment eyes;
        eyes.a = anc_vertex(prev.eye_x, prev.eye_y, prev.eye_z);
        eyes.b = anc_vertex(key.eye_x, key.eye_y, key.eye_z);
        lines.push_back(eyes);
        oscilline::ModelSegment targets;
        targets.a = anc_vertex(prev.target_x, prev.target_y, prev.target_z);
        targets.b = anc_vertex(key.target_x, key.target_y, key.target_z);
        lines.push_back(targets);
    }
    const oscilline::AncSample sample = oscilline::sample_anc(file, t, false);
    oscilline::ModelSegment playhead;
    playhead.a = {sample.eye_x, sample.eye_y, sample.eye_z};
    playhead.b = {sample.target_x, sample.target_y, sample.target_z};
    lines.push_back(playhead);
    // Short cross so a collapsed look still has a playhead.
    constexpr float kArm = 32.f;
    const auto arm = [&](float x, float y, float z) {
        oscilline::ModelSegment segment;
        segment.a = {sample.eye_x, sample.eye_y, sample.eye_z};
        segment.b = {sample.eye_x + x, sample.eye_y + y, sample.eye_z + z};
        lines.push_back(segment);
    };
    arm(kArm, 0.f, 0.f);
    arm(0.f, kArm, 0.f);
    arm(0.f, 0.f, kArm);
    const auto viewed = oscilline::to_view_space(lines, g_model_view);
    oscilline::ModelBounds bounds;
    oscilline::extend_bounds(bounds, viewed);
    camera = oscilline::framing_camera(bounds, camera);
    const auto screen = oscilline::project_segments(viewed, camera);
    oscilline::TriangleList triangles;
    oscilline::append_strokes(triangles, screen);
    return triangles;
}

enum class SfxLeave { Quit, Back };

void print_sfx_catalog(const oscilline::SfxLibrary* library);
SfxLeave
run_sfx_browser(oscilline::Host& host, const oscilline::SfxLibrary* library, bool allow_back);

// Banks stay unloaded until the browser opens, so a model session does not
// decode VH/VB until Tab asks for it.
struct SfxSession {
    std::optional<oscilline::SfxLibrary> library;
    const oscilline::IsoVolume* volume = nullptr;
    bool catalog_printed = false;

    const oscilline::SfxLibrary* loaded() {
        if (!library && volume != nullptr) {
            library = oscilline::load_sfx_library(*volume);
        }
        return library ? &*library : nullptr;
    }

    void print_once() {
        if (catalog_printed) {
            return;
        }
        print_sfx_catalog(loaded());
        catalog_printed = true;
    }

    SfxLeave open(oscilline::Host& host, bool allow_back) {
        print_once();
        return run_sfx_browser(host, loaded(), allow_back);
    }
};

int run_anc_view(oscilline::Host& host,
                 const oscilline::PakArchive& archive,
                 const std::vector<std::string>& names,
                 int index,
                 int look_step,
                 SfxSession& sfx) {
    if (names.empty() || index < 0 || static_cast<std::size_t>(index) >= names.size()) {
        return refuse("the archive has no ANC files");
    }
    float along = 0.f;
    int shown = -1;
    while (host.begin_frame()) {
        const oscilline::HostInput& input = host.input();
        if (oscilline::pressed(input, oscilline::Key::Back)) {
            break;
        }
        const bool open_sfx = oscilline::pressed(input, oscilline::Key::Sfx);
        const bool scrub = oscilline::pressed(input, oscilline::Key::Up) ||
                           oscilline::pressed(input, oscilline::Key::Down);
        if (oscilline::pressed(input, oscilline::Key::Right) && names.size() > 1) {
            index = (index + 1) % static_cast<int>(names.size());
            along = 0.f;
        }
        if (oscilline::pressed(input, oscilline::Key::Left) && names.size() > 1) {
            index = (index + static_cast<int>(names.size()) - 1) % static_cast<int>(names.size());
            along = 0.f;
        }
        if (oscilline::pressed(input, oscilline::Key::Up)) {
            along += 0.05f;
        }
        if (oscilline::pressed(input, oscilline::Key::Down)) {
            along -= 0.05f;
        }
        const int steps = host.sim_steps();
        if (!scrub) {
            along += static_cast<float>(static_cast<double>(steps) * host.step_seconds()) * 0.15f;
        }
        if (along < 0.f) {
            along = 0.f;
        }
        if (along > 1.f) {
            along = std::fmod(along, 1.f);
        }
        const oscilline::PakEntry* entry =
            oscilline::find_entry(archive, names[static_cast<std::size_t>(index)]);
        oscilline::TriangleList triangles;
        std::string error;
        oscilline::AncFile file;
        bool parsed = false;
        int step = 1;
        if (entry == nullptr) {
            error = "missing " + names[static_cast<std::size_t>(index)];
        } else {
            auto camera_file = oscilline::parse_anc(entry->data);
            if (!camera_file) {
                error = camera_file.error();
            } else {
                file = std::move(camera_file.value());
                parsed = true;
                step = look_step > 0 ? look_step : oscilline::anc_look_step(file.keys.size());
                triangles = draw_anc(file, along, {}, step);
            }
        }
        std::string summary = error;
        if (parsed) {
            summary = std::to_string(file.keys.size()) + " keys, step " + std::to_string(step);
        }
        if (index != shown) {
            shown = index;
            std::cout << names[static_cast<std::size_t>(index)];
            if (parsed) {
                std::cout << "  " << summary;
            }
            std::cout << '\n';
        }
        std::vector<oscilline::TextGlyph> text;
        text.push_back({16.f, 16.f, names[static_cast<std::size_t>(index)]});
        text.push_back({16.f, 28.f, summary});
        text.push_back({16.f, 464.f, "Tab sfx"});
        host.end_frame(triangles, text);
        if (open_sfx && sfx.open(host, true) == SfxLeave::Quit) {
            break;
        }
    }
    return EXIT_SUCCESS;
}

const char* sfx_bank_label(std::uint8_t bank) {
    if (bank == oscilline::kSfxBankGame) {
        return "game";
    }
    if (bank == oscilline::kSfxBankTitle) {
        return "title";
    }
    if (bank == oscilline::kSfxBankTutorial) {
        return "tutorial";
    }
    return "none";
}

bool sfx_can_play(const oscilline::SfxMixer& mixer, oscilline::SfxId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(oscilline::SfxId::Count)) {
        return false;
    }
    if (oscilline::kSfxMap[index].bank == oscilline::kSfxBankNone) {
        return false;
    }
    return !mixer.resolve(id).placeholder;
}

std::string sfx_status(const oscilline::SfxMixer& mixer, int index) {
    const auto id = static_cast<oscilline::SfxId>(index);
    const oscilline::SfxMapEntry& entry = oscilline::kSfxMap[static_cast<std::size_t>(index)];
    if (entry.bank == oscilline::kSfxBankNone) {
        return "unmapped";
    }
    if (sfx_can_play(mixer, id)) {
        return sfx_bank_label(entry.bank);
    }
    return "no audio";
}

// Direct notes from unidentified rows. SfxId voices use tag id+1, which stays
// below this value.
constexpr std::uint32_t kUnidentifiedTag = 0x10000u;

struct BrowserRow {
    bool identified = true;
    int id = 0;
    oscilline::SfxBankSample sample{};
};

std::string pad_front(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.insert(0, width - text.size(), ' ');
    }
    return text;
}

std::string unidentified_label(const oscilline::SfxBankSample& sample) {
    std::string line = "unidentified ";
    line += sfx_bank_label(sample.bank);
    line += " vag ";
    line += std::to_string(sample.vag);
    if (sample.has_tone) {
        line += ' ';
        line += std::to_string(sample.program);
        line += '/';
        line += std::to_string(sample.tone);
    }
    return line;
}

std::string unidentified_console(int ordinal, const oscilline::SfxBankSample& sample) {
    std::string line = "unidentified ";
    line += std::to_string(ordinal);
    line += ' ';
    line += sfx_bank_label(sample.bank);
    line += " vag ";
    line += std::to_string(sample.vag);
    if (sample.has_tone) {
        line += ' ';
        line += std::to_string(sample.program);
        line += '/';
        line += std::to_string(sample.tone);
    }
    return line;
}

std::vector<BrowserRow> browser_rows(const oscilline::SfxLibrary* library) {
    std::vector<BrowserRow> rows;
    const int count = static_cast<int>(oscilline::SfxId::Count);
    rows.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        BrowserRow row;
        row.identified = true;
        row.id = index;
        rows.push_back(row);
    }
    if (library == nullptr) {
        return rows;
    }
    const std::vector<oscilline::SfxBankSample> extra = oscilline::unidentified_sfx(*library);
    for (int index = 0; index < static_cast<int>(extra.size()); ++index) {
        BrowserRow row;
        row.identified = false;
        row.id = index;
        row.sample = extra[static_cast<std::size_t>(index)];
        rows.push_back(row);
    }
    return rows;
}

std::string sfx_row(const oscilline::SfxMixer& mixer, const BrowserRow& row, bool selected) {
    std::string line = selected ? ">" : " ";
    if (row.identified) {
        line += pad_front(std::to_string(row.id), 4);
        line += "  ";
        line += oscilline::sfx_name(static_cast<oscilline::SfxId>(row.id));
        line += "  ";
        line += sfx_status(mixer, row.id);
        return line;
    }
    line += pad_front("U" + std::to_string(row.id), 4);
    line += "  ";
    line += unidentified_label(row.sample);
    return line;
}

void print_sfx_catalog(const oscilline::SfxLibrary* library) {
    const int count = static_cast<int>(oscilline::SfxId::Count);
    const std::vector<oscilline::SfxBankSample> extra =
        library != nullptr ? oscilline::unidentified_sfx(*library)
                           : std::vector<oscilline::SfxBankSample>{};
    std::cout << "sfx catalog " << count << '\n';
    bool loaded = false;
    if (library != nullptr) {
        for (std::uint8_t bank = 0; bank < oscilline::kSfxBankCount; ++bank) {
            if (!library->banks[bank].loaded) {
                continue;
            }
            loaded = true;
            std::cout << "sfx bank " << sfx_bank_label(bank) << " loaded\n";
        }
    }
    if (!loaded) {
        std::cout << "sfx audio not loaded\n";
    }
    std::cout << "sfx unidentified count " << extra.size() << '\n';
    for (int index = 0; index < count; ++index) {
        std::cout << "sfx " << index << ' '
                  << oscilline::sfx_name(static_cast<oscilline::SfxId>(index)) << '\n';
    }
    for (int index = 0; index < static_cast<int>(extra.size()); ++index) {
        std::cout << "sfx " << unidentified_console(index, extra[static_cast<std::size_t>(index)])
                  << '\n';
    }
}

void stop_browser_voice(oscilline::SfxMixer& mixer,
                        const std::vector<BrowserRow>& rows,
                        int voiced) {
    if (voiced < 0 || static_cast<std::size_t>(voiced) >= rows.size()) {
        return;
    }
    const BrowserRow& row = rows[static_cast<std::size_t>(voiced)];
    if (row.identified) {
        mixer.stop(static_cast<oscilline::SfxId>(row.id));
    } else {
        mixer.stop_tag(kUnidentifiedTag);
    }
}

SfxLeave
run_sfx_browser(oscilline::Host& host, const oscilline::SfxLibrary* library, bool allow_back) {
    // A viewer plays every mapped sample, including low and medium confidence.
    // Unmapped SfxId rows and a missing bank stay silent instead of a placeholder beep.
    // Unidentified rows are bank samples that kSfxMap does not play.
    oscilline::SfxConfig config;
    config.experimental = true;
    oscilline::SfxMixer mixer(
        library,
        std::span<const oscilline::SfxMapEntry>(oscilline::kSfxMap,
                                                static_cast<std::size_t>(oscilline::SfxId::Count)),
        config);
    oscilline::SfxOutput output = oscilline::SfxOutput::open(mixer);
    const std::vector<BrowserRow> rows = browser_rows(library);
    const int count = static_cast<int>(rows.size());
    int index = 0;
    int shown = index;
    int voiced = -1;
    constexpr int kVisible = 40;
    const char* hint = allow_back
                           ? "Up/Down step  Left/Right jump  Enter plays  Tab back  Esc closes"
                           : "Up/Down step  Left/Right jump  Enter plays  Esc closes";
    while (host.begin_frame()) {
        (void)host.sim_steps();
        const oscilline::HostInput& input = host.input();
        if (oscilline::pressed(input, oscilline::Key::Back) ||
            (allow_back && oscilline::pressed(input, oscilline::Key::Sfx))) {
            const bool back = allow_back && oscilline::pressed(input, oscilline::Key::Sfx) &&
                              !oscilline::pressed(input, oscilline::Key::Back);
            stop_browser_voice(mixer, rows, voiced);
            output.pump();
            return back ? SfxLeave::Back : SfxLeave::Quit;
        }
        if (count > 0) {
            if (oscilline::pressed(input, oscilline::Key::Up)) {
                index = (index + count - 1) % count;
            }
            if (oscilline::pressed(input, oscilline::Key::Down)) {
                index = (index + 1) % count;
            }
            if (oscilline::pressed(input, oscilline::Key::Left)) {
                index = (index + count - 10) % count;
            }
            if (oscilline::pressed(input, oscilline::Key::Right)) {
                index = (index + 10) % count;
            }
        }
        if (count > 0 && index != shown) {
            shown = index;
            stop_browser_voice(mixer, rows, voiced);
            voiced = -1;
            const BrowserRow& row = rows[static_cast<std::size_t>(index)];
            if (row.identified) {
                std::cout << "sfx " << row.id << ' '
                          << oscilline::sfx_name(static_cast<oscilline::SfxId>(row.id)) << '\n';
            } else {
                std::cout << "sfx " << unidentified_console(row.id, row.sample) << '\n';
            }
        }
        const bool play = oscilline::pressed(input, oscilline::Key::Confirm) ||
                          oscilline::pressed(input, oscilline::Key::Face);
        if (play && count > 0) {
            const BrowserRow& row = rows[static_cast<std::size_t>(index)];
            if (row.identified) {
                const auto id = static_cast<oscilline::SfxId>(row.id);
                if (sfx_can_play(mixer, id)) {
                    stop_browser_voice(mixer, rows, voiced);
                    mixer.post(id);
                    voiced = index;
                    std::cout << "sfx play " << row.id << ' ' << oscilline::sfx_name(id) << '\n';
                } else {
                    std::cout << "sfx play " << row.id << ' ' << oscilline::sfx_name(id)
                              << " skipped\n";
                }
            } else if (library != nullptr) {
                oscilline::SfxNote note;
                if (oscilline::sfx_bank_note(*library, row.sample.bank, row.sample.vag, note)) {
                    note.tag = kUnidentifiedTag;
                    stop_browser_voice(mixer, rows, voiced);
                    mixer.post_note(note);
                    voiced = index;
                    std::cout << "sfx play " << unidentified_console(row.id, row.sample) << '\n';
                } else {
                    std::cout << "sfx play " << unidentified_console(row.id, row.sample)
                              << " skipped\n";
                }
            }
        }
        output.pump();
        std::vector<oscilline::TextGlyph> text;
        std::string header = "SfxId";
        if (count > 0) {
            const BrowserRow& row = rows[static_cast<std::size_t>(index)];
            if (row.identified) {
                header = "SfxId " + std::to_string(row.id) + "  " +
                         std::string(oscilline::sfx_name(static_cast<oscilline::SfxId>(row.id))) +
                         "  " + sfx_status(mixer, row.id);
            } else {
                header = "U" + std::to_string(row.id) + "  " + unidentified_label(row.sample);
            }
        }
        text.push_back({8.f, 8.f, header});
        text.push_back({8.f, 18.f, hint});
        int first = 0;
        if (count > kVisible) {
            first = index - kVisible / 2;
            if (first < 0) {
                first = 0;
            }
            if (first > count - kVisible) {
                first = count - kVisible;
            }
        }
        const int last = std::min(count, first + kVisible);
        for (int row = first; row < last; ++row) {
            const float y = 36.f + static_cast<float>(row - first) * 10.f;
            text.push_back(
                {8.f, y, sfx_row(mixer, rows[static_cast<std::size_t>(row)], row == index)});
        }
        host.end_frame({}, text);
    }
    return SfxLeave::Quit;
}

} // namespace

int main(int argc, char** argv) {
    std::string disc;
    std::string pak;
    std::string tmd_name;
    std::string anm_name;
    std::string anc_name;
    int frame_limit = -1;
    int anc_step = 0;
    bool anm_set = false;
    bool anc_set = false;
    bool anc_step_set = false;
    bool sfx_mode = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto need = [&]() -> const char* {
            if (i + 1 >= argc) {
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--version") {
            std::cout << "Oscilline " << oscilline::version_string() << '\n';
            return EXIT_SUCCESS;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return EXIT_SUCCESS;
        }
        if (arg == "--sfx") {
            sfx_mode = true;
            continue;
        }
        if (arg == "--view") {
            const char* value = need();
            if (value == nullptr) {
                return refuse("--view needs a value");
            }
            const std::string_view mode{value};
            if (mode == "side") {
                g_model_view = oscilline::ModelView::Side;
            } else if (mode == "front") {
                g_model_view = oscilline::ModelView::Front;
            } else {
                return refuse("--view must be side or front");
            }
            continue;
        }
        if (arg == "--anc-step") {
            const char* value = need();
            if (value == nullptr) {
                return refuse("--anc-step needs a value");
            }
            const auto step = parse_frames(value);
            if (!step || *step < 1) {
                return refuse("--anc-step must be a positive integer");
            }
            anc_step = *step;
            anc_step_set = true;
            continue;
        }
        if (arg == "--disc" || arg == "--pak" || arg == "--tmd" || arg == "--anm" ||
            arg == "--anc" || arg == "--frames") {
            const char* value = need();
            if (value == nullptr) {
                return refuse(std::string(arg) + " needs a value");
            }
            if (arg == "--disc") {
                disc = value;
            } else if (arg == "--pak") {
                pak = value;
            } else if (arg == "--tmd") {
                tmd_name = value;
            } else if (arg == "--anm") {
                anm_name = value;
                anm_set = true;
            } else if (arg == "--anc") {
                anc_name = value;
                anc_set = true;
            } else {
                const auto frames = parse_frames(value);
                if (!frames) {
                    return refuse("--frames must be a non-negative integer");
                }
                frame_limit = *frames;
            }
        } else {
            std::cerr << "oscilline-viewer: unknown argument '" << arg << "'\n";
            print_usage(std::cerr);
            return EXIT_FAILURE;
        }
    }

    if (anc_step_set && !anc_set) {
        return refuse("--anc-step needs --anc");
    }
    if (sfx_mode && (anc_set || anc_step_set || anm_set || !tmd_name.empty())) {
        return refuse("--sfx cannot be combined with --tmd, --anm, or --anc");
    }
    if (sfx_mode) {
        SfxSession session;
        if (!disc.empty()) {
            auto mounted = oscilline::mount_disc(disc);
            if (!mounted) {
                return refuse(mounted.error());
            }
            session.library = oscilline::load_sfx_library(mounted.value().volume);
        }
        oscilline::Host::Config config;
        config.title = "Oscilline SFX";
        config.frame_limit = frame_limit;
        auto host = oscilline::Host::open(config);
        if (!host) {
            return refuse(host.error());
        }
        session.open(host.value(), false);
        return EXIT_SUCCESS;
    }

    if (disc.empty()) {
        print_usage(std::cerr);
        return refuse("pass --disc <cue> for a disc image you own");
    }

    auto mounted = oscilline::mount_disc(disc);
    if (!mounted) {
        return refuse(mounted.error());
    }
    SfxSession session;
    session.volume = &mounted.value().volume;
    auto archive = oscilline::load_language_pak(mounted.value().volume, pak);
    if (!archive) {
        return refuse(archive.error());
    }
    oscilline::ModelList models = oscilline::list_models(archive.value());
    if (anc_set && named_index(archive.value(), models.anc, anc_name) < 0 && pak.empty()) {
        auto title = oscilline::load_language_pak(mounted.value().volume, oscilline::kTitlePakPath);
        if (title) {
            const oscilline::ModelList title_models = oscilline::list_models(title.value());
            if (named_index(title.value(), title_models.anc, anc_name) >= 0 || models.anc.empty()) {
                archive = std::move(title);
                models = oscilline::list_models(archive.value());
            }
        }
    }
    if (anc_set) {
        if (models.anc.empty()) {
            return refuse("the archive has no ANC files");
        }
        int anc_index = named_index(archive.value(), models.anc, anc_name);
        if (anc_index < 0) {
            return refuse("ANC '" + anc_name + "' is not in the archive");
        }
        std::cout << "Oscilline " << oscilline::version_string() << "  "
                  << mounted.value().identity.serial << "\n"
                  << models.anc.size() << " cameras\n";
        oscilline::Host::Config config;
        config.title = "Oscilline";
        config.frame_limit = frame_limit;
        auto host = oscilline::Host::open(config);
        if (!host) {
            return refuse(host.error());
        }
        return run_anc_view(
            host.value(), archive.value(), models.anc, anc_index, anc_step, session);
    }
    if (models.tmd.empty()) {
        return refuse("the archive has no TMD files");
    }

    int tmd_index = 0;
    if (!tmd_name.empty()) {
        tmd_index = -1;
        for (std::size_t i = 0; i < models.tmd.size(); ++i) {
            if (oscilline::find_entry(archive.value(), models.tmd[i]) != nullptr &&
                oscilline::find_entry(archive.value(), tmd_name) ==
                    oscilline::find_entry(archive.value(), models.tmd[i])) {
                tmd_index = static_cast<int>(i);
                break;
            }
        }
        if (tmd_index < 0) {
            return refuse("TMD '" + tmd_name + "' is not in the archive");
        }
    }
    int anm_index =
        anm_set ? -1 : matching_anm(models, models.tmd[static_cast<std::size_t>(tmd_index)]);
    if (anm_set && !anm_name.empty()) {
        for (std::size_t i = 0; i < models.anm.size(); ++i) {
            if (oscilline::find_entry(archive.value(), anm_name) ==
                oscilline::find_entry(archive.value(), models.anm[i])) {
                anm_index = static_cast<int>(i);
                break;
            }
        }
        if (anm_index < 0) {
            return refuse("ANM '" + anm_name + "' is not in the archive");
        }
    }

    std::cout << "Oscilline " << oscilline::version_string() << "  "
              << mounted.value().identity.serial << "\n"
              << models.tmd.size() << " models, " << models.anm.size() << " animations\n";

    oscilline::Host::Config config;
    config.title = "Oscilline";
    config.frame_limit = frame_limit;
    auto host = oscilline::Host::open(config);
    if (!host) {
        return refuse(host.error());
    }

    View view = load_view(archive.value(), models, tmd_index, anm_index);
    Inspect inspect;
    int shown_tmd = -2;
    int shown_anm = -2;
    while (host.value().begin_frame()) {
        const oscilline::HostInput& input = host.value().input();
        if (oscilline::pressed(input, oscilline::Key::Back)) {
            break;
        }
        const bool open_sfx = oscilline::pressed(input, oscilline::Key::Sfx);
        if (oscilline::pressed(input, oscilline::Key::Right) && !models.tmd.empty()) {
            tmd_index = (tmd_index + 1) % static_cast<int>(models.tmd.size());
            if (!anm_set) {
                anm_index = matching_anm(models, models.tmd[static_cast<std::size_t>(tmd_index)]);
            }
            view = load_view(archive.value(), models, tmd_index, anm_index);
        }
        if (oscilline::pressed(input, oscilline::Key::Left) && !models.tmd.empty()) {
            tmd_index = (tmd_index + static_cast<int>(models.tmd.size()) - 1) %
                        static_cast<int>(models.tmd.size());
            if (!anm_set) {
                anm_index = matching_anm(models, models.tmd[static_cast<std::size_t>(tmd_index)]);
            }
            view = load_view(archive.value(), models, tmd_index, anm_index);
        }
        const int anm_slots = static_cast<int>(models.anm.size()) + 1;
        if (oscilline::pressed(input, oscilline::Key::Up)) {
            const int slot = anm_index + 1;
            anm_index = ((slot + 1) % anm_slots) - 1;
            view = load_view(archive.value(), models, tmd_index, anm_index);
        }
        if (oscilline::pressed(input, oscilline::Key::Down)) {
            const int slot = anm_index + 1;
            anm_index = ((slot + anm_slots - 1) % anm_slots) - 1;
            view = load_view(archive.value(), models, tmd_index, anm_index);
        }
        if (tmd_index != shown_tmd || anm_index != shown_anm) {
            shown_tmd = tmd_index;
            shown_anm = anm_index;
            std::cout << models.tmd[static_cast<std::size_t>(tmd_index)];
            if (anm_index >= 0) {
                std::cout << "  " << models.anm[static_cast<std::size_t>(anm_index)];
            } else {
                std::cout << "  (bind pose)";
            }
            std::cout << '\n';
            view.time = 0;
        }

        const int steps = host.value().sim_steps();
        const double seconds = static_cast<double>(steps) * host.value().step_seconds();
        update_inspect(inspect, input, seconds);
        view.time += seconds;
        const oscilline::TriangleList triangles = draw_view(view, inspect);
        std::vector<oscilline::TextGlyph> text;
        text.push_back({16.f, 16.f, models.tmd[static_cast<std::size_t>(tmd_index)]});
        if (anm_index >= 0) {
            text.push_back({16.f, 28.f, models.anm[static_cast<std::size_t>(anm_index)]});
        } else {
            text.push_back({16.f, 28.f, "(bind pose)"});
        }
        text.push_back({16.f, view.error.empty() ? 40.f : 56.f, inspect_text(inspect)});
        if (!view.error.empty()) {
            text.push_back({16.f, 44.f, view.error});
        }
        text.push_back(
            {16.f, 464.f, "drag orbit   wheel zooms   Q/E R/F   -/= zoom   P reset   Tab sfx"});
        host.value().end_frame(triangles, text);
        if (open_sfx && session.open(host.value(), true) == SfxLeave::Quit) {
            break;
        }
    }
    return EXIT_SUCCESS;
}
