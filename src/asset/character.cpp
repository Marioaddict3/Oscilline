// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Matches clip stems and places a disc model on the ribbon.

#include "oscilline/asset/character.hpp"

#include "oscilline/course/camera.hpp"
#include "oscilline/course/figure.hpp"
#include "oscilline/course/obstacle.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/render/viewport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>

namespace oscilline {
namespace {

constexpr int kActionSlots = 16;
constexpr std::uint8_t kActionMask = 0x0F;

std::string clip_key(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    const auto dot = path.rfind('.');
    if (dot != std::string_view::npos) {
        path = path.substr(0, dot);
    }
    std::string stem;
    stem.reserve(path.size());
    for (unsigned char ch : path) {
        stem.push_back(static_cast<char>(std::toupper(ch)));
    }
    // N00_SKIP, N03_L, N01_J: the leading index is not part of the role.
    if (stem.size() >= 3 && stem[0] == 'N' &&
        std::isdigit(static_cast<unsigned char>(stem[1])) != 0) {
        std::size_t index = 1;
        while (index < stem.size() && std::isdigit(static_cast<unsigned char>(stem[index])) != 0) {
            ++index;
        }
        if (index < stem.size() && (stem[index] == '_' || stem[index] == '-')) {
            stem.erase(0, index + 1);
        }
    }
    // N02_J_F and N00_SKIP_F: a trailing variant marker is not a role token.
    // The caller records that marker. The stem keeps its confidence.
    if (stem.size() >= 2) {
        const char separator = stem[stem.size() - 2];
        if ((separator == '_' || separator == '-') && stem.back() == 'F') {
            stem.resize(stem.size() - 2);
        }
    }
    std::string key;
    key.reserve(stem.size());
    for (unsigned char ch : stem) {
        if (ch == '_' || ch == '-' || ch == ' ') {
            continue;
        }
        key.push_back(static_cast<char>(ch));
    }
    return key;
}

bool trailing_f_variant(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    const auto dot = path.rfind('.');
    if (dot != std::string_view::npos) {
        path = path.substr(0, dot);
    }
    if (path.size() < 2) {
        return false;
    }
    const char tail = static_cast<char>(std::toupper(static_cast<unsigned char>(path.back())));
    const char separator = path[path.size() - 2];
    return tail == 'F' && (separator == '_' || separator == '-');
}

const ClipToken* longest_token(std::string_view key) {
    const ClipToken* found = nullptr;
    std::size_t best = 0;
    for (const ClipToken& token : character_clip_tokens()) {
        const std::size_t length = std::strlen(token.text);
        if (length == 0 || length > key.size() || length < best) {
            continue;
        }
        if (key.compare(0, length, token.text) != 0) {
            continue;
        }
        if (found == nullptr || length > best) {
            found = &token;
            best = length;
        }
    }
    return found;
}

void begin_idle(CharacterClock& clock, const CharacterLibrary& library) {
    clock.clip = library.idle;
    clock.actions = 0;
    clock.oneshot = false;
    clock.miss = false;
    clock.successful = false;
    clock.seconds = 0;
}

double clip_seconds(const AnmFile& animation) {
    const float rate = anm_playback_hz(animation.unk1);
    if (!(rate > 0.f) || animation.frames.empty()) {
        return 0;
    }
    return static_cast<double>(animation.frames.size()) / static_cast<double>(rate);
}

bool clip_ok(const std::vector<AnmFile>& anims, int index) {
    return index >= 0 && static_cast<std::size_t>(index) < anims.size();
}

void begin_clip(
    CharacterClock& clock, int index, std::uint8_t actions, bool miss, bool successful = false) {
    clock.clip = index;
    clock.actions = actions;
    clock.oneshot = true;
    clock.miss = miss;
    clock.successful = successful;
    clock.seconds = 0;
}

int miss_index(const CharacterLibrary& library, std::uint8_t edges) {
    if (edges < kActionSlots && library.miss[edges] >= 0) {
        return library.miss[edges];
    }
    return library.miss[0];
}

CharacterPlacement measure_placement(const TmdModel& model, const AnmFile& idle) {
    CharacterPlacement placement;
    float foot_sum = 0.f;
    int foot_frames = 0;
    ModelBounds bounds;
    const std::size_t objects = model.objects.size();
    const int frames = static_cast<int>(idle.frames.size());
    if (frames <= 0) {
        return placement;
    }
    for (int frame = 0; frame < frames; ++frame) {
        const std::vector<Pose> poses = poses_for_frame(idle, objects, frame, 0.f, false);
        const std::vector<ModelSegment> lines =
            to_view_space(tmd_wireframe(model, poses), ModelView::Side);
        float frame_min_y = 0.f;
        bool any = false;
        for (const ModelSegment& segment : lines) {
            for (const ModelVertex point : {segment.a, segment.b}) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
                    continue;
                }
                if (!any || point.y < frame_min_y) {
                    frame_min_y = point.y;
                }
                any = true;
            }
        }
        if (!any) {
            continue;
        }
        foot_sum += frame_min_y;
        ++foot_frames;
        extend_bounds(bounds, lines);
    }
    if (bounds.empty || foot_frames == 0) {
        return placement;
    }
    const float height = std::max(bounds.max.y - bounds.min.y, 1.f);
    const float width = std::max(bounds.max.x - bounds.min.x, 1.f);
    float scale = kFigureHeightPx / height;
    if (width * scale > kFigureMaxWidthPx) {
        scale = kFigureMaxWidthPx / width;
    }
    placement.valid = std::isfinite(scale) && scale > 0.f;
    placement.scale = placement.valid ? scale : 1.f;
    placement.anchor_x = (bounds.min.x + bounds.max.x) * 0.5f;
    placement.ground_y = foot_sum / static_cast<float>(foot_frames);
    return placement;
}

int action_clip_index(const CharacterLibrary& library, std::uint8_t actions, ClipMoment moment) {
    const int mask = actions & kActionMask;
    if (mask == 0 || moment == ClipMoment::Wrong || moment == ClipMoment::None) {
        return -1;
    }
    if (moment == ClipMoment::Whiff) {
        if (library.whiff[mask] >= 0) {
            return library.whiff[mask];
        }
        if (library.whiff_stem[mask]) {
            return -1;
        }
        return library.action[mask];
    }
    if (moment == ClipMoment::Clear) {
        return library.action[mask];
    }
    return -1;
}

const char* confidence_label(Confidence confidence) {
    switch (confidence) {
    case Confidence::High:
        return "high";
    case Confidence::Medium:
        return "medium";
    case Confidence::Low:
        return "low";
    }
    return "low";
}

} // namespace

std::optional<Slot> form_model_slot(Form form) {
    switch (form) {
    case Form::Super:
        return Slot::FormUpgraded;
    case Form::Rabbit:
        return Slot::FormBase;
    case Form::Frog:
        return Slot::FormDegradedA;
    case Form::Worm:
        return Slot::FormDegradedB;
    case Form::Out:
        return std::nullopt;
    }
    return std::nullopt;
}

ClipMatch match_character_clip(std::string_view path) {
    ClipMatch match;
    const bool variant = trailing_f_variant(path);
    const std::string key = clip_key(path);
    if (key.empty()) {
        return match;
    }
    bool idle = false;
    bool miss = false;
    int action_tokens = 0;
    Confidence worst = Confidence::High;
    std::string_view rest = key;
    while (!rest.empty()) {
        const ClipToken* token = longest_token(rest);
        if (token == nullptr) {
            return {};
        }
        const std::size_t length = std::strlen(token->text);
        rest.remove_prefix(length);
        if (static_cast<int>(token->confidence) < static_cast<int>(worst)) {
            worst = token->confidence;
        }
        if (token->kind == ClipKind::Idle) {
            idle = true;
        } else if (token->kind == ClipKind::Miss) {
            miss = true;
        } else {
            if ((match.actions & token->actions) != 0) {
                return {};
            }
            match.actions = static_cast<std::uint8_t>(match.actions | token->actions);
            ++action_tokens;
        }
    }
    if (idle && (miss || match.actions != 0)) {
        return {};
    }
    if (miss && idle) {
        return {};
    }
    match.matched = true;
    if (idle) {
        match.kind = ClipKind::Idle;
        match.actions = 0;
        // Idle, including a trailing _F, stays at the token confidence. Every
        // idle token is high.
        match.confidence = worst;
        return match;
    }
    if (miss) {
        match.kind = ClipKind::Miss;
        match.confidence = worst;
        return match;
    }
    if (action_tokens < 1 || action_tokens > 2 || match.actions == 0) {
        return {};
    }
    match.kind = ClipKind::Action;
    match.whiff = variant;
    // Singles, pairs (JH, JL, JW, HL, HW, LW, and any other two-letter stem),
    // and trailing _F whiffs all use the token confidence. Every action token
    // is high, so these play at the default floor.
    match.confidence = worst;
    return match;
}

CharacterLibrary::CharacterLibrary() {
    for (int& slot : action) {
        slot = -1;
    }
    for (int& slot : whiff) {
        slot = -1;
    }
    for (int& slot : miss) {
        slot = -1;
    }
}

CharacterLibrary character_library(const std::vector<std::string>& names, Confidence minimum) {
    CharacterLibrary library;
    Confidence idle_confidence = Confidence::Low;
    Confidence action_confidence[kActionSlots]{};
    Confidence whiff_confidence[kActionSlots]{};
    Confidence miss_confidence[kActionSlots]{};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const ClipMatch match = match_character_clip(names[index]);
        if (match.matched && match.kind == ClipKind::Action && match.whiff) {
            library.whiff_stem[match.actions & kActionMask] = true;
        }
        if (!match.matched || static_cast<int>(match.confidence) < static_cast<int>(minimum)) {
            continue;
        }
        const int slot = static_cast<int>(index);
        if (match.kind == ClipKind::Idle) {
            if (library.idle < 0 ||
                static_cast<int>(match.confidence) > static_cast<int>(idle_confidence)) {
                library.idle = slot;
                idle_confidence = match.confidence;
            }
            continue;
        }
        const int mask = match.actions & kActionMask;
        if (match.kind == ClipKind::Miss) {
            if (library.miss[mask] < 0 ||
                static_cast<int>(match.confidence) > static_cast<int>(miss_confidence[mask])) {
                library.miss[mask] = slot;
                miss_confidence[mask] = match.confidence;
            }
            continue;
        }
        if (match.whiff) {
            if (library.whiff[mask] < 0 ||
                static_cast<int>(match.confidence) > static_cast<int>(whiff_confidence[mask])) {
                library.whiff[mask] = slot;
                whiff_confidence[mask] = match.confidence;
            }
            continue;
        }
        if (library.action[mask] < 0 ||
            static_cast<int>(match.confidence) > static_cast<int>(action_confidence[mask])) {
            library.action[mask] = slot;
            action_confidence[mask] = match.confidence;
        }
    }
    library.usable = library.idle >= 0;
    return library;
}

CharacterRig make_character_rig(AssetRegistry& assets, Form form) {
    CharacterRig rig;
    const std::optional<Slot> slot = form_model_slot(form);
    if (!slot || assets.origin(*slot) != AssetOrigin::Disc) {
        return rig;
    }
    const TmdModel* model = assets.model(*slot);
    const std::vector<AnmFile>* anims = assets.animations(*slot);
    const std::vector<std::string>* names = assets.animation_names(*slot);
    if (model == nullptr || anims == nullptr || names == nullptr ||
        names->size() != anims->size()) {
        return rig;
    }
    rig.library = character_library(*names, assets.minimum());
    if (!rig.library.usable || !clip_ok(*anims, rig.library.idle)) {
        return rig;
    }
    rig.placement = measure_placement(*model, (*anims)[static_cast<std::size_t>(rig.library.idle)]);
    rig.disc = rig.placement.valid;
    return rig;
}

ActionClipCue action_clip_cue(const PlayAdvanceResult& step,
                              const std::vector<PlayHit>& hits,
                              std::uint8_t edges) {
    ActionClipCue cue;
    edges = static_cast<std::uint8_t>(edges & kActionMask);
    if (step.whiff) {
        cue.moment = ClipMoment::Whiff;
        cue.actions = edges;
        return cue;
    }
    for (const PlayHit& hit : hits) {
        if (hit.judgment == Judgment::Perfect || hit.judgment == Judgment::Good) {
            cue.moment = ClipMoment::Clear;
            cue.actions = obstacle_actions(hit.obstacle);
        }
    }
    if (cue.moment == ClipMoment::None && step.wrong) {
        cue.moment = ClipMoment::Wrong;
        cue.actions = edges;
    }
    return cue;
}

namespace {

struct ClipRow {
    const char* name;
    const char* situation;
    // nullptr is the wrong-press row: high confidence that no clip plays.
    const char* stem;
    std::uint8_t actions;
    ClipMoment moment;
};

// A whiff row whose stem has no _F uses that plain clip's confidence.
// Pair rows are J block, H pit, L loop, and W wave. Every row is high.
constexpr ClipRow kClipRows[] = {
    {"J", "clear", "J", kActionBlock, ClipMoment::Clear},
    {"J_F", "whiff", "J_F", kActionBlock, ClipMoment::Whiff},
    {"L", "clear", "L", kActionLoop, ClipMoment::Clear},
    {"L_F", "whiff", "L_F", kActionLoop, ClipMoment::Whiff},
    {"W", "clear", "W", kActionWave, ClipMoment::Clear},
    {"W", "whiff", "W", kActionWave, ClipMoment::Whiff},
    {"H", "clear", "H", kActionPit, ClipMoment::Clear},
    {"H", "whiff", "H", kActionPit, ClipMoment::Whiff},
    {"JL", "pair", "JL", kActionBlock | kActionLoop, ClipMoment::Clear},
    {"JW", "pair", "JW", kActionBlock | kActionWave, ClipMoment::Clear},
    {"HL", "pair", "HL", kActionPit | kActionLoop, ClipMoment::Clear},
    {"HW", "pair", "HW", kActionPit | kActionWave, ClipMoment::Clear},
    {"LW", "pair", "LW", kActionLoop | kActionWave, ClipMoment::Clear},
    {"JH", "pair", "JH", kActionBlock | kActionPit, ClipMoment::Clear},
    {"wrong-press", "none", nullptr, kActionBlock, ClipMoment::Wrong},
};

} // namespace

std::string character_clip_report(Confidence minimum) {
    std::string out;
    for (const ClipRow& row : kClipRows) {
        Confidence confidence = Confidence::High;
        if (row.stem != nullptr) {
            const ClipMatch match = match_character_clip(row.stem);
            confidence = match.matched ? match.confidence : Confidence::Low;
        }
        const bool on = static_cast<int>(confidence) >= static_cast<int>(minimum);
        out += std::string(row.name) + ' ' + row.situation + ' ' + confidence_label(confidence) +
               (on ? " on\n" : " gated\n");
    }
    return out;
}

std::string character_clip_resolve(std::string_view form,
                                   const std::vector<std::string>& names,
                                   Confidence minimum) {
    const CharacterLibrary library = character_library(names, minimum);
    std::string out;
    for (const ClipRow& row : kClipRows) {
        const int index = action_clip_index(library, row.actions, row.moment);
        std::string clip = "none";
        if (index >= 0 && static_cast<std::size_t>(index) < names.size()) {
            const std::string& path = names[static_cast<std::size_t>(index)];
            const std::size_t slash = path.find_last_of("/\\");
            clip = slash == std::string::npos ? path : path.substr(slash + 1);
        }
        out += std::string(form) + ' ' + row.name + ' ' + row.situation + ' ' + clip + '\n';
    }
    return out;
}

void apply_super_transform_clip(CharacterClock& clock, int clip_id, int clip_count) {
    if (clock.successful || clip_id < 0 || clip_count <= 0 || clip_id >= clip_count) {
        return;
    }
    if (clock.clip == clip_id && clock.oneshot) {
        return;
    }
    clock.clip = clip_id;
    clock.actions = 0;
    clock.oneshot = true;
    clock.miss = false;
    clock.successful = false;
    clock.seconds = 0;
    clock.started = true;
}

double character_clip_duration_seconds(const AnmFile& animation) {
    return clip_seconds(animation);
}

void character_advance(CharacterClock& clock,
                       const CharacterLibrary& library,
                       const std::vector<AnmFile>& anims,
                       double dt_seconds,
                       std::uint8_t edges,
                       bool missed,
                       Form form,
                       ClipMoment moment,
                       double playback_rate) {
    if (!library.usable) {
        return;
    }
    if (!std::isfinite(dt_seconds) || dt_seconds < 0) {
        dt_seconds = 0;
    }
    if (!std::isfinite(playback_rate) || playback_rate < 0.0) {
        playback_rate = 1.0;
    }
    edges = static_cast<std::uint8_t>(edges & kActionMask);
    const Form previous = clock.form;
    const bool switched = clock.started && previous != form;
    // The promoting clear keeps the pose it already has. Other form changes
    // still return to idle unless a successful obstacle clip is in progress.
    const bool keep_phase = switched && previous == Form::Rabbit && form == Form::Super;
    const bool preserve_success = clock.successful && clock.oneshot && moment != ClipMoment::Clear;
    clock.form = form;
    if (!clock.started) {
        begin_idle(clock, library);
        clock.started = true;
    } else if (switched && !keep_phase && !preserve_success) {
        begin_idle(clock, library);
        if (moment != ClipMoment::Clear) {
            return;
        }
    } else if (switched && preserve_success) {
        const int continued = action_clip_index(library, clock.actions, ClipMoment::Clear);
        if (clip_ok(anims, continued)) {
            clock.clip = continued;
        }
    }
    if (!preserve_success && moment == ClipMoment::Wrong) {
        const int missed_clip = missed ? miss_index(library, edges) : -1;
        if (missed && clip_ok(anims, missed_clip)) {
            begin_clip(clock, missed_clip, edges, true);
            return;
        }
    } else if (!preserve_success && (moment == ClipMoment::Clear || moment == ClipMoment::Whiff)) {
        const int index = action_clip_index(library, edges, moment);
        if (clip_ok(anims, index)) {
            begin_clip(clock, index, edges, false, moment == ClipMoment::Clear);
            return;
        }
    } else if (!preserve_success && (edges != 0 || missed)) {
        const int missed_clip = missed ? miss_index(library, edges) : -1;
        if (missed && clip_ok(anims, missed_clip)) {
            begin_clip(clock, missed_clip, edges, true);
            return;
        }
        if (edges != 0 && clip_ok(anims, library.action[edges])) {
            begin_clip(clock, library.action[edges], edges, false);
            return;
        }
    }
    if (!clip_ok(anims, clock.clip)) {
        begin_idle(clock, library);
        return;
    }
    clock.seconds += dt_seconds * playback_rate;
    const double duration = clip_seconds(anims[static_cast<std::size_t>(clock.clip)]);
    if (clock.oneshot) {
        if (!(duration > 0) || clock.seconds >= duration) {
            begin_idle(clock, library);
        }
        return;
    }
    if (duration > 0 && clock.seconds >= duration) {
        clock.seconds = std::fmod(clock.seconds, duration);
    }
}

std::vector<Pose>
poses_at_time(const AnmFile& animation, std::size_t object_count, double seconds, bool loop) {
    const float rate = anm_playback_hz(animation.unk1);
    const int count = static_cast<int>(animation.frames.size());
    if (count <= 0 || !(rate > 0.f)) {
        return std::vector<Pose>(object_count);
    }
    double cursor = seconds * static_cast<double>(rate);
    if (!std::isfinite(cursor) || cursor < 0) {
        cursor = 0;
    }
    int frame = 0;
    float fraction = 0.f;
    bool interpolate = true;
    if (loop) {
        double wrapped = std::fmod(cursor, static_cast<double>(count));
        if (wrapped < 0) {
            wrapped += count;
        }
        if (wrapped >= static_cast<double>(count)) {
            wrapped = 0;
        }
        frame = static_cast<int>(wrapped);
        fraction = static_cast<float>(wrapped - static_cast<double>(frame));
    } else if (cursor >= static_cast<double>(count)) {
        frame = count - 1;
        fraction = 0.f;
        interpolate = false;
    } else {
        frame = static_cast<int>(cursor);
        fraction = static_cast<float>(cursor - static_cast<double>(frame));
        if (frame >= count - 1) {
            interpolate = false;
        }
    }
    return poses_for_frame(animation, object_count, frame, fraction, interpolate);
}

namespace {

ModelVertex figure_world(ModelVertex model, const CharacterPlacement& placement) {
    // Feet (view-space ground) sit on world y = 0. The side-view anchor sits on
    // world z = 0 so the body faces +Z from the origin. +X is model right.
    ModelVertex world;
    world.x = model.x * kFigureWorldPerModel;
    world.y = (model.y + placement.ground_y) * kFigureWorldPerModel;
    world.z = (model.z - placement.anchor_x) * kFigureWorldPerModel;
    return world;
}

void place_side(ModelVertex model,
                const CharacterPlacement& placement,
                float figure_x,
                float ribbon_y,
                float& x,
                float& y,
                float& view_z) {
    const ModelVertex view = to_view_space(model, ModelView::Side);
    x = figure_x + (view.x - placement.anchor_x) * placement.scale;
    // ground_y still plants the feet on the ribbon. Clearance lifts the ink.
    y = ribbon_y - kFigureRibbonClearancePx - (view.y - placement.ground_y) * placement.scale;
    view_z = view.z;
}

// Painter's sort uses each primitive's average depth, so a line drawn on a
// surface (an eye outline on the head) can sort behind the fill it sits on
// and be painted over. Lines are pulled toward the camera by this fraction
// of the model's largest extent, in model units. Far-side parts, such as the
// far eye behind the head, are much deeper than this and stay hidden.
constexpr float kLineDepthBiasFraction = 0.03f;

float line_depth_bias(const FigureMesh& mesh) {
    bool any = false;
    ModelVertex low;
    ModelVertex high;
    const auto include = [&](const ModelVertex& point) {
        if (!any) {
            low = high = point;
            any = true;
            return;
        }
        low.x = std::min(low.x, point.x);
        low.y = std::min(low.y, point.y);
        low.z = std::min(low.z, point.z);
        high.x = std::max(high.x, point.x);
        high.y = std::max(high.y, point.y);
        high.z = std::max(high.z, point.z);
    };
    for (const ModelSegment& line : mesh.lines) {
        include(line.a);
        include(line.b);
    }
    for (const ModelTriangle& triangle : mesh.triangles) {
        include(triangle.a);
        include(triangle.b);
        include(triangle.c);
    }
    if (!any) {
        return 0.f;
    }
    const float extent = std::max({high.x - low.x, high.y - low.y, high.z - low.z});
    return std::isfinite(extent) ? extent * kLineDepthBiasFraction : 0.f;
}

// An eye fill is drawn behind every line of its own outline. The eye is a flat
// disc with one black fill inside it; tilted, some outline lines average
// deeper than the fill's center and were painted under it. `deepest` holds
// each object's farthest line endpoint, or -inf.
void keep_lines_over_own_fill(float& fill_depth,
                              const ModelTriangle& triangle,
                              const std::vector<float>& deepest) {
    const std::uint32_t object = triangle.object;
    if (triangle.under_own_lines && object < deepest.size() && std::isfinite(deepest[object])) {
        fill_depth = std::max(fill_depth, deepest[object] + 1.e-3f);
    }
}

void note_deepest(std::vector<float>& deepest, std::uint32_t object, float depth) {
    if (object >= deepest.size()) {
        deepest.resize(object + 1u, -std::numeric_limits<float>::infinity());
    }
    deepest[object] = std::max(deepest[object], depth);
}

bool project_figure_mesh(std::vector<Segment>& lines,
                         std::vector<FilledTriangle>& fills,
                         const FigureMesh& mesh,
                         const CharacterPlacement& placement,
                         const AncSample& camera) {
    DiscView view;
    view.eye_x = camera.eye_x;
    view.eye_y = camera.eye_y;
    view.eye_z = camera.eye_z;
    view.target_x = camera.target_x;
    view.target_y = camera.target_y;
    view.target_z = camera.target_z;
    view.projection_h = anc_projection_h(camera.fov);
    const float screen_w = static_cast<float>(logical_width());
    const float screen_h = static_cast<float>(kLogicalHeight);
    bool any = false;
    const std::size_t first_line = lines.size();
    const std::size_t first_fill = fills.size();
    const float line_bias = line_depth_bias(mesh) * kFigureWorldPerModel;
    std::vector<float> deepest;
    constexpr float kFigureNearPlane = 1.1e-3f;
    const auto interpolate = [](const ModelVertex& from, const ModelVertex& to, float t) {
        ModelVertex point;
        point.x = from.x + (to.x - from.x) * t;
        point.y = from.y + (to.y - from.y) * t;
        point.z = from.z + (to.z - from.z) * t;
        return point;
    };
    for (const ModelSegment& line : mesh.lines) {
        ModelVertex world_a = figure_world(line.a, placement);
        ModelVertex world_b = figure_world(line.b, placement);
        float depth_a = 0.f;
        float depth_b = 0.f;
        if (!disc_view_depth(view, world_a, depth_a) || !disc_view_depth(view, world_b, depth_b) ||
            (depth_a <= kFigureNearPlane && depth_b <= kFigureNearPlane)) {
            continue;
        }
        if (depth_a <= kFigureNearPlane) {
            const float t = (kFigureNearPlane - depth_a) / (depth_b - depth_a);
            world_a = interpolate(world_a, world_b, t);
        } else if (depth_b <= kFigureNearPlane) {
            const float t = (kFigureNearPlane - depth_b) / (depth_a - depth_b);
            world_b = interpolate(world_b, world_a, t);
        }
        DiscProjected a;
        DiscProjected b;
        if (!project_disc_world(view, world_a, screen_w, screen_h, a) ||
            !project_disc_world(view, world_b, screen_w, screen_h, b)) {
            continue;
        }
        Segment segment;
        segment.x0 = a.x;
        segment.y0 = a.y - kFigureRibbonClearancePx;
        segment.x1 = b.x;
        segment.y1 = b.y - kFigureRibbonClearancePx;
        segment.depth = (a.depth + b.depth) * 0.5f - line_bias;
        note_deepest(deepest, line.object, std::max(a.depth, b.depth));
        segment.color = line.color;
        lines.push_back(segment);
        any = true;
    }
    for (const ModelTriangle& triangle : mesh.triangles) {
        std::vector<ModelVertex> polygon = {
            figure_world(triangle.a, placement),
            figure_world(triangle.b, placement),
            figure_world(triangle.c, placement),
        };
        std::vector<ModelVertex> clipped;
        clipped.reserve(4);
        ModelVertex previous = polygon.back();
        float previous_depth = 0.f;
        if (!disc_view_depth(view, previous, previous_depth)) {
            continue;
        }
        bool previous_inside = previous_depth > kFigureNearPlane;
        for (const ModelVertex& current : polygon) {
            float current_depth = 0.f;
            if (!disc_view_depth(view, current, current_depth)) {
                clipped.clear();
                break;
            }
            const bool current_inside = current_depth > kFigureNearPlane;
            if (current_inside != previous_inside) {
                const float t =
                    (kFigureNearPlane - previous_depth) / (current_depth - previous_depth);
                clipped.push_back(interpolate(previous, current, t));
            }
            if (current_inside) {
                clipped.push_back(current);
            }
            previous = current;
            previous_depth = current_depth;
            previous_inside = current_inside;
        }
        if (clipped.size() < 3) {
            continue;
        }

        std::vector<DiscProjected> projected;
        projected.reserve(clipped.size());
        bool valid = true;
        for (const ModelVertex& point : clipped) {
            DiscProjected screen;
            if (!project_disc_world(view, point, screen_w, screen_h, screen)) {
                valid = false;
                break;
            }
            projected.push_back(screen);
        }
        if (!valid) {
            continue;
        }
        for (std::size_t i = 1; i + 1 < projected.size(); ++i) {
            const DiscProjected& a = projected[0];
            const DiscProjected& b = projected[i];
            const DiscProjected& c = projected[i + 1];
            FilledTriangle fill;
            fill.x0 = a.x;
            fill.y0 = a.y - kFigureRibbonClearancePx;
            fill.x1 = b.x;
            fill.y1 = b.y - kFigureRibbonClearancePx;
            fill.x2 = c.x;
            fill.y2 = c.y - kFigureRibbonClearancePx;
            fill.depth = (a.depth + b.depth + c.depth) / 3.f;
            keep_lines_over_own_fill(fill.depth, triangle, deepest);
            fill.color = triangle.color;
            fills.push_back(fill);
        }
        any = true;
    }
    // The 512x286 frame is stretched to the logical view: 1.25x across but
    // 1.68x down at 4:3, which made her narrow. 16:9 is close to square.
    // Widen her about her own center so she has the vertical scale both ways
    // at every aspect ratio. The road keeps the frame's mapping.
    const float widen = (screen_h / kDiscBufferHeight) / (screen_w / kDiscBufferWidth);
    if (any && std::isfinite(widen) && widen > 0.f) {
        float min_x = std::numeric_limits<float>::infinity();
        float max_x = -std::numeric_limits<float>::infinity();
        for (std::size_t i = first_line; i < lines.size(); ++i) {
            min_x = std::min({min_x, lines[i].x0, lines[i].x1});
            max_x = std::max({max_x, lines[i].x0, lines[i].x1});
        }
        for (std::size_t i = first_fill; i < fills.size(); ++i) {
            min_x = std::min({min_x, fills[i].x0, fills[i].x1, fills[i].x2});
            max_x = std::max({max_x, fills[i].x0, fills[i].x1, fills[i].x2});
        }
        const float center = (min_x + max_x) * 0.5f;
        const auto square = [&](float& x) { x = center + (x - center) * widen; };
        for (std::size_t i = first_line; i < lines.size(); ++i) {
            square(lines[i].x0);
            square(lines[i].x1);
        }
        for (std::size_t i = first_fill; i < fills.size(); ++i) {
            square(fills[i].x0);
            square(fills[i].x1);
            square(fills[i].x2);
        }
    }
    return any;
}

void paint_side_mesh(std::vector<Segment>& lines,
                     std::vector<FilledTriangle>& fills,
                     const FigureMesh& mesh,
                     const CharacterPlacement& placement,
                     float figure_x,
                     float ribbon_y) {
    const float line_bias = line_depth_bias(mesh);
    std::vector<float> deepest;
    for (const ModelSegment& line : mesh.lines) {
        float ax = 0.f;
        float ay = 0.f;
        float az = 0.f;
        float bx = 0.f;
        float by = 0.f;
        float bz = 0.f;
        place_side(line.a, placement, figure_x, ribbon_y, ax, ay, az);
        place_side(line.b, placement, figure_x, ribbon_y, bx, by, bz);
        Segment segment;
        segment.x0 = ax;
        segment.y0 = ay;
        segment.x1 = bx;
        segment.y1 = by;
        // View-space z is model x. Larger model x is nearer, so depth is negated.
        segment.depth = -(az + bz) * 0.5f - line_bias;
        note_deepest(deepest, line.object, std::max(-az, -bz));
        segment.color = line.color;
        lines.push_back(segment);
    }
    for (const ModelTriangle& triangle : mesh.triangles) {
        float xs[3] = {};
        float ys[3] = {};
        float zs[3] = {};
        const ModelVertex verts[3] = {triangle.a, triangle.b, triangle.c};
        for (int i = 0; i < 3; ++i) {
            place_side(verts[i], placement, figure_x, ribbon_y, xs[i], ys[i], zs[i]);
        }
        FilledTriangle fill;
        fill.x0 = xs[0];
        fill.y0 = ys[0];
        fill.x1 = xs[1];
        fill.y1 = ys[1];
        fill.x2 = xs[2];
        fill.y2 = ys[2];
        fill.depth = -(zs[0] + zs[1] + zs[2]) / 3.f;
        keep_lines_over_own_fill(fill.depth, triangle, deepest);
        fill.color = triangle.color;
        fills.push_back(fill);
    }
}

float shifted_coord(std::int16_t coordinate, std::int32_t scale) {
    // Same bit shift tmd_figure_mesh applies before the pose.
    constexpr int kMaxShift = 16;
    int shift = static_cast<int>(scale);
    if (shift > kMaxShift) {
        shift = kMaxShift;
    }
    if (shift < -kMaxShift) {
        shift = -kMaxShift;
    }
    const float value = static_cast<float>(coordinate);
    if (shift >= 0) {
        return value * static_cast<float>(1u << static_cast<unsigned>(shift));
    }
    return value / static_cast<float>(1u << static_cast<unsigned>(-shift));
}

bool bind_point(const TmdObject& object, std::uint16_t index, float& x, float& y, float& z) {
    if (index >= object.vertices.size()) {
        return false;
    }
    const TmdVertex& vertex = object.vertices[index];
    x = shifted_coord(vertex.x, object.scale);
    y = shifted_coord(vertex.y, object.scale);
    z = shifted_coord(vertex.z, object.scale);
    return true;
}

// Same Rx, then Ry, then Rz order as the mesh poser.
ModelVertex pose_point(ModelVertex point, const Pose& pose) {
    point.x *= pose.scale_x;
    point.y *= pose.scale_y;
    point.z *= pose.scale_z;

    const float rx_c = std::cos(pose.rotation_x);
    const float rx_s = std::sin(pose.rotation_x);
    const float y_after_x = point.y * rx_c - point.z * rx_s;
    const float z_after_x = point.y * rx_s + point.z * rx_c;
    point.y = y_after_x;
    point.z = z_after_x;

    const float ry_c = std::cos(pose.rotation_y);
    const float ry_s = std::sin(pose.rotation_y);
    const float x_after_y = point.x * ry_c + point.z * ry_s;
    const float z_after_y = -point.x * ry_s + point.z * ry_c;
    point.x = x_after_y;
    point.z = z_after_y;

    const float rz_c = std::cos(pose.rotation_z);
    const float rz_s = std::sin(pose.rotation_z);
    const float x_after_z = point.x * rz_c - point.y * rz_s;
    const float y_after_z = point.x * rz_s + point.y * rz_c;
    point.x = x_after_z;
    point.y = y_after_z;

    point.x += pose.position_x;
    point.y += pose.position_y;
    point.z += pose.position_z;
    return point;
}

bool object_drawn(std::span<const Pose> poses, std::size_t index) {
    if (poses.empty()) {
        return true;
    }
    return index < poses.size() && poses[index].visible;
}

bool placed_point(const TmdObject& object,
                  std::span<const Pose> poses,
                  std::size_t object_index,
                  std::uint16_t vertex_index,
                  float& x,
                  float& y,
                  float& z) {
    if (!bind_point(object, vertex_index, x, y, z)) {
        return false;
    }
    if (!poses.empty() && object_index < poses.size()) {
        const ModelVertex posed = pose_point(ModelVertex{x, y, z}, poses[object_index]);
        x = posed.x;
        y = posed.y;
        z = posed.z;
    }
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

struct PlacedVertex {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

std::vector<PlacedVertex> primitive_points(const TmdObject& object,
                                           const TmdPrimitive& primitive,
                                           std::span<const Pose> poses,
                                           std::size_t object_index) {
    std::vector<PlacedVertex> points;
    for (std::uint16_t vertex_index : primitive.vertex_indices) {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        if (!placed_point(object, poses, object_index, vertex_index, x, y, z)) {
            return {};
        }
        points.push_back(PlacedVertex{x, y, z});
    }
    return points;
}

struct ObjectExtent {
    bool usable = false;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float radius = 0.f;
    bool polygon = false;
    bool line = false;
};

ObjectExtent
measure_object(const TmdObject& object, std::span<const Pose> poses, std::size_t index) {
    ObjectExtent extent;
    std::vector<PlacedVertex> points;
    for (const TmdPrimitive& primitive : object.primitives) {
        const bool polygon =
            primitive.kind == PrimitiveKind::Polygon && primitive.vertex_indices.size() >= 3;
        const bool line =
            primitive.kind == PrimitiveKind::Line && primitive.vertex_indices.size() >= 2;
        if (!polygon && !line) {
            continue;
        }
        const std::vector<PlacedVertex> gathered =
            primitive_points(object, primitive, poses, index);
        if (gathered.size() != primitive.vertex_indices.size()) {
            continue;
        }
        extent.polygon = extent.polygon || polygon;
        extent.line = extent.line || line;
        points.insert(points.end(), gathered.begin(), gathered.end());
    }
    if (points.size() < 2 || (!extent.polygon && !extent.line)) {
        return extent;
    }
    float sum_x = 0.f;
    float sum_y = 0.f;
    float sum_z = 0.f;
    for (const PlacedVertex& point : points) {
        sum_x += point.x;
        sum_y += point.y;
        sum_z += point.z;
    }
    const float count = static_cast<float>(points.size());
    extent.x = sum_x / count;
    extent.y = sum_y / count;
    extent.z = sum_z / count;
    for (const PlacedVertex& point : points) {
        const float dx = point.x - extent.x;
        const float dy = point.y - extent.y;
        // Radius stays in the head plane. Depth would reject a protruding eye
        // that the flat fill test used to accept.
        extent.radius = std::max(extent.radius, std::sqrt(dx * dx + dy * dy));
    }
    extent.usable = true;
    return extent;
}

} // namespace

namespace {

bool wrapped_yaw(float yaw_radians, float& wrapped) {
    if (!std::isfinite(yaw_radians)) {
        return false;
    }
    const float turn = std::numbers::pi_v<float> * 2.f;
    wrapped = std::remainder(yaw_radians, turn);
    return std::isfinite(wrapped);
}

// Far eye: hide above 45°, show again only below 40°. The band keeps `hidden`.
void hold_far_eye(bool& hidden, float abs_yaw) {
    if (hidden) {
        if (abs_yaw < kEyeBothShowRadians) {
            hidden = false;
        }
        return;
    }
    if (abs_yaw > kEyeBothMaxRadians) {
        hidden = true;
    }
}

bool hides_eye(EyeVisibility visibility, FigureEye eye) {
    if (eye == FigureEye::Left) {
        return !visibility.left;
    }
    if (eye == FigureEye::Right) {
        return !visibility.right;
    }
    return false;
}

// The fill's own vertices sit on this disc. The pad only covers that
// boundary. It is not wide enough to take in a neighboring ear or brow.
constexpr float kEyeDiscScale = 1.05f;
constexpr float kEyeDiscSlack = 0.5f;

struct EyeDisc {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float radius = 0.f;
    FigureEye side = FigureEye::None;
    bool bind = false;
};

struct EyeScan {
    std::vector<EyeDisc> discs;
    // Head polygons that are not the eye pair (ears). Bind identity only.
    std::vector<std::vector<char>> other_head;
};

EyeScan scan_eye_discs(const TmdModel& model, std::span<const Pose> poses, bool bind_space) {
    EyeScan scan;
    scan.other_head.resize(model.objects.size());
    float min_x = 0.f;
    float max_x = 0.f;
    float min_y = 0.f;
    float max_y = 0.f;
    bool any = false;
    const auto take = [&](float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) {
            return;
        }
        if (!any) {
            min_x = max_x = x;
            min_y = max_y = y;
            any = true;
            return;
        }
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    };
    for (std::size_t index = 0; index < model.objects.size(); ++index) {
        if (!object_drawn(poses, index)) {
            continue;
        }
        const TmdObject& object = model.objects[index];
        // Primitives index vertices with 16 bits, so later vertices are unreachable.
        const std::size_t vertex_count = std::min<std::size_t>(object.vertices.size(), 0x10000);
        for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
            float x = 0.f;
            float y = 0.f;
            float z = 0.f;
            if (!placed_point(object, poses, index, static_cast<std::uint16_t>(vertex), x, y, z)) {
                continue;
            }
            take(x, y);
        }
    }
    const float height = max_y - min_y;
    if (!any || !(height > 1.f)) {
        return scan;
    }
    const float head_limit = min_y + height * 0.42f;
    const float max_radius = height * 0.18f;
    const float min_offset = height * 0.04f;
    const float center_x = (min_x + max_x) * 0.5f;

    struct Candidate {
        std::size_t object = 0;
        std::size_t primitive = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        float radius = 0.f;
    };
    std::vector<Candidate> candidates;
    for (std::size_t index = 0; index < model.objects.size(); ++index) {
        if (!object_drawn(poses, index)) {
            continue;
        }
        const TmdObject& object = model.objects[index];
        for (std::size_t primitive_index = 0; primitive_index < object.primitives.size();
             ++primitive_index) {
            const TmdPrimitive& primitive = object.primitives[primitive_index];
            if (primitive.kind != PrimitiveKind::Polygon || primitive.vertex_indices.size() < 3) {
                continue;
            }
            const std::vector<PlacedVertex> points =
                primitive_points(object, primitive, poses, index);
            if (points.size() != primitive.vertex_indices.size()) {
                continue;
            }
            Candidate candidate;
            candidate.object = index;
            candidate.primitive = primitive_index;
            for (const PlacedVertex& point : points) {
                candidate.x += point.x;
                candidate.y += point.y;
                candidate.z += point.z;
            }
            const float count = static_cast<float>(points.size());
            candidate.x /= count;
            candidate.y /= count;
            candidate.z /= count;
            for (const PlacedVertex& point : points) {
                const float dx = point.x - candidate.x;
                const float dy = point.y - candidate.y;
                candidate.radius = std::max(candidate.radius, std::sqrt(dx * dx + dy * dy));
            }
            if (!(candidate.radius > 0.5f) || candidate.radius > max_radius ||
                candidate.y > head_limit || std::fabs(candidate.x - center_x) < min_offset) {
                continue;
            }
            candidates.push_back(candidate);
        }
    }

    const std::size_t none = candidates.size();
    std::size_t left = none;
    std::size_t right = none;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const Candidate& candidate = candidates[index];
        if (candidate.x < center_x) {
            if (left == none || (center_x - candidate.x) < (center_x - candidates[left].x)) {
                left = index;
            }
        } else if (right == none || (candidate.x - center_x) < (candidates[right].x - center_x)) {
            right = index;
        }
    }
    if (left == none || right == none) {
        return scan;
    }
    if (std::fabs(candidates[left].y - candidates[right].y) > height * 0.25f) {
        return scan;
    }

    const auto take_side = [&](std::size_t primary, FigureEye side) {
        const Candidate& eye = candidates[primary];
        for (const Candidate& candidate : candidates) {
            const bool on_side =
                side == FigureEye::Left ? candidate.x < center_x : candidate.x >= center_x;
            if (!on_side) {
                continue;
            }
            const float dx = candidate.x - eye.x;
            const float dy = candidate.y - eye.y;
            const float reach = eye.radius + candidate.radius;
            if (dx * dx + dy * dy > reach * reach) {
                std::vector<char>& marks = scan.other_head[candidate.object];
                if (marks.size() <= candidate.primitive) {
                    marks.resize(candidate.primitive + 1, 0);
                }
                marks[candidate.primitive] = 1;
                continue;
            }
            scan.discs.push_back(
                EyeDisc{candidate.x, candidate.y, candidate.z, candidate.radius, side, bind_space});
        }
    };
    take_side(left, FigureEye::Left);
    take_side(right, FigureEye::Right);
    return scan;
}

bool other_head_part(const std::vector<std::vector<char>>& marks,
                     std::size_t object,
                     std::size_t primitive) {
    if (object >= marks.size() || primitive >= marks[object].size()) {
        return false;
    }
    return marks[object][primitive] != 0;
}

FigureEye match_eye_disc(const std::vector<PlacedVertex>& points,
                         bool bind_space,
                         const std::vector<EyeDisc>& discs) {
    FigureEye best = FigureEye::None;
    float best_distance = 0.f;
    if (points.empty()) {
        return best;
    }
    for (const EyeDisc& disc : discs) {
        if (disc.bind != bind_space || !(disc.radius > 0.f)) {
            continue;
        }
        const float limit = disc.radius * kEyeDiscScale + kEyeDiscSlack;
        const float limit_sq = limit * limit;
        float distance = 0.f;
        bool all = true;
        for (const PlacedVertex& point : points) {
            const float dx = point.x - disc.x;
            const float dy = point.y - disc.y;
            const float squared = dx * dx + dy * dy;
            if (squared > limit_sq) {
                all = false;
                break;
            }
            distance += squared;
        }
        if (!all) {
            continue;
        }
        if (best == FigureEye::None || distance < best_distance) {
            best = disc.side;
            best_distance = distance;
        }
    }
    return best;
}

} // namespace

float figure_yaw_to_camera(float camera_x, float camera_z) {
    if (!std::isfinite(camera_x) || !std::isfinite(camera_z)) {
        return 0.f;
    }
    if (std::fabs(camera_x) < 1.e-3f && std::fabs(camera_z) < 1.e-3f) {
        return 0.f;
    }
    return std::atan2(camera_x, camera_z);
}

EyeVisibility eye_visibility_for_yaw(float yaw_radians) {
    EyeVisibility visibility;
    float wrapped = 0.f;
    if (!wrapped_yaw(yaw_radians, wrapped)) {
        return visibility;
    }
    const float abs_yaw = std::fabs(wrapped);
    if (abs_yaw > kEyeAwayEnterRadians) {
        visibility.left = false;
        visibility.right = false;
        return visibility;
    }
    if (abs_yaw > kEyeBothMaxRadians) {
        // Positive yaw is the camera on model +X, so the far eye is -X.
        if (wrapped > 0.f) {
            visibility.left = false;
        } else {
            visibility.right = false;
        }
    }
    return visibility;
}

EyeVisibility eye_visibility_for_yaw(float yaw_radians, EyeCullState& state) {
    EyeVisibility visibility;
    float wrapped = 0.f;
    if (!wrapped_yaw(yaw_radians, wrapped)) {
        visibility.left = !state.left_hidden;
        visibility.right = !state.right_hidden;
        return visibility;
    }
    const float abs_yaw = std::fabs(wrapped);
    // Already facing away: stay hidden until yaw is back inside 146°. Otherwise
    // hide both only past 150°.
    const bool both_hidden = state.left_hidden && state.right_hidden;
    const bool both = both_hidden ? abs_yaw > kEyeAwayRadians : abs_yaw > kEyeAwayEnterRadians;
    if (both) {
        state.left_hidden = true;
        state.right_hidden = true;
        visibility.left = false;
        visibility.right = false;
        return visibility;
    }
    if (wrapped > 0.f) {
        hold_far_eye(state.left_hidden, abs_yaw);
        state.right_hidden = false;
    } else if (wrapped < 0.f) {
        hold_far_eye(state.right_hidden, abs_yaw);
        state.left_hidden = false;
    } else {
        state.left_hidden = false;
        state.right_hidden = false;
    }
    visibility.left = !state.left_hidden;
    visibility.right = !state.right_hidden;
    return visibility;
}

std::vector<FigureEye> classify_figure_eyes(const TmdModel& model, std::span<const Pose> poses) {
    std::vector<FigureEye> eyes(model.objects.size(), FigureEye::None);
    float min_x = 0.f;
    float max_x = 0.f;
    float min_y = 0.f;
    float max_y = 0.f;
    bool any = false;
    const auto take = [&](float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) {
            return;
        }
        if (!any) {
            min_x = max_x = x;
            min_y = max_y = y;
            any = true;
            return;
        }
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    };
    for (std::size_t index = 0; index < model.objects.size(); ++index) {
        if (!object_drawn(poses, index)) {
            continue;
        }
        const TmdObject& object = model.objects[index];
        // Primitives index vertices with 16 bits, so later vertices are unreachable.
        const std::size_t vertex_count = std::min<std::size_t>(object.vertices.size(), 0x10000);
        for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
            float x = 0.f;
            float y = 0.f;
            float z = 0.f;
            if (!placed_point(object, poses, index, static_cast<std::uint16_t>(vertex), x, y, z)) {
                continue;
            }
            take(x, y);
        }
    }
    const float height = max_y - min_y;
    if (!any || !(height > 1.f)) {
        return eyes;
    }
    // PlayStation +Y is down, so the head is the smaller Y.
    const float head_limit = min_y + height * 0.42f;
    const float max_radius = height * 0.18f;
    const float min_offset = height * 0.04f;
    const float center_x = (min_x + max_x) * 0.5f;

    struct Part {
        std::size_t index = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        float radius = 0.f;
        bool polygon = false;
    };
    std::vector<Part> parts;
    std::vector<Part> cores;
    for (std::size_t index = 0; index < model.objects.size(); ++index) {
        if (!object_drawn(poses, index)) {
            continue;
        }
        const ObjectExtent extent = measure_object(model.objects[index], poses, index);
        if (!extent.usable || extent.radius > max_radius * 2.f) {
            continue;
        }
        Part part{index, extent.x, extent.y, extent.z, extent.radius, extent.polygon};
        parts.push_back(part);
        // The fill may share its object with the outline. A line does not
        // throw the object out.
        if (!extent.polygon || extent.radius > max_radius || extent.y > head_limit ||
            std::fabs(extent.x - center_x) < min_offset) {
            continue;
        }
        cores.push_back(part);
    }

    float left_y = 0.f;
    float right_y = 0.f;
    int left_count = 0;
    int right_count = 0;
    for (const Part& core : cores) {
        if (core.x < center_x) {
            left_y += core.y;
            ++left_count;
        } else {
            right_y += core.y;
            ++right_count;
        }
    }
    if (left_count == 0 || right_count == 0) {
        return eyes;
    }
    left_y /= static_cast<float>(left_count);
    right_y /= static_cast<float>(right_count);
    if (std::fabs(left_y - right_y) > height * 0.25f) {
        return eyes;
    }
    for (const Part& core : cores) {
        eyes[core.index] = core.x < center_x ? FigureEye::Left : FigureEye::Right;
    }
    // A second polygon or the outline can be its own object, posed onto the
    // fill. It hides with that eye instead of staying as one leftover part.
    for (const Part& part : parts) {
        if (eyes[part.index] != FigureEye::None || std::fabs(part.x - center_x) < min_offset) {
            continue;
        }
        const bool left = part.x < center_x;
        for (const Part& core : cores) {
            if ((core.x < center_x) != left) {
                continue;
            }
            const float dx = part.x - core.x;
            const float dy = part.y - core.y;
            const float dz = part.z - core.z;
            const float limit = std::max(core.radius * 2.f, height * 0.05f);
            if (dx * dx + dy * dy + dz * dz <= limit * limit) {
                eyes[part.index] = left ? FigureEye::Left : FigureEye::Right;
                break;
            }
        }
    }
    return eyes;
}

TmdModel cull_figure_eyes(std::vector<Pose>& poses,
                          const TmdModel& model,
                          EyeVisibility visibility,
                          EyeCullState* state) {
    const bool have_pose = !poses.empty();
    const EyeScan bind_scan = scan_eye_discs(model, {}, true);
    EyeScan posed_scan;
    if (have_pose) {
        posed_scan = scan_eye_discs(model, poses, false);
    }
    std::vector<EyeDisc> discs = bind_scan.discs;
    discs.insert(discs.end(), posed_scan.discs.begin(), posed_scan.discs.end());
    if (state != nullptr && state->primitives.size() != model.objects.size()) {
        state->primitives.assign(model.objects.size(), {});
    }

    TmdModel drawn = model;
    for (std::size_t index = 0; index < drawn.objects.size(); ++index) {
        if (!object_drawn(poses, index)) {
            continue;
        }
        std::vector<TmdPrimitive>& primitives = drawn.objects[index].primitives;
        std::vector<TmdPrimitive> kept;
        kept.reserve(primitives.size());
        bool removed = false;
        for (std::size_t primitive_index = 0; primitive_index < primitives.size();
             ++primitive_index) {
            const TmdPrimitive& primitive = primitives[primitive_index];
            const bool polygon =
                primitive.kind == PrimitiveKind::Polygon && primitive.vertex_indices.size() >= 3;
            const bool line =
                primitive.kind == PrimitiveKind::Line && primitive.vertex_indices.size() >= 2;
            if (!polygon && !line) {
                kept.push_back(primitive);
                continue;
            }
            // Ears are other head polygons. They are not eyes, even if a pose
            // later parks them on the fill.
            const bool foreign_head =
                polygon && other_head_part(bind_scan.other_head, index, primitive_index);
            FigureEye which = FigureEye::None;
            if (!foreign_head) {
                const std::vector<PlacedVertex> bind_points =
                    primitive_points(model.objects[index], primitive, {}, index);
                const std::vector<PlacedVertex> posed_points =
                    primitive_points(model.objects[index], primitive, poses, index);
                if (bind_points.size() == primitive.vertex_indices.size()) {
                    which = match_eye_disc(bind_points, true, discs);
                }
                // Lines join an eye only in bind pose, so a limb that swings
                // past the face is not latched as an outline. A pupil is a
                // polygon that lands on the fill after the pose.
                if (which == FigureEye::None && polygon && have_pose &&
                    posed_points.size() == primitive.vertex_indices.size()) {
                    which = match_eye_disc(posed_points, false, discs);
                }
            }
            if (state != nullptr && !foreign_head && index < state->primitives.size()) {
                std::vector<FigureEye>& latched = state->primitives[index];
                if (latched.size() <= primitive_index) {
                    latched.resize(primitive_index + 1, FigureEye::None);
                }
                if (which != FigureEye::None) {
                    latched[primitive_index] = which;
                } else {
                    which = latched[primitive_index];
                }
            }
            if (hides_eye(visibility, which)) {
                removed = true;
                continue;
            }
            kept.push_back(primitive);
        }
        bool drawable = false;
        for (const TmdPrimitive& primitive : kept) {
            const bool polygon =
                primitive.kind == PrimitiveKind::Polygon && primitive.vertex_indices.size() >= 3;
            const bool line =
                primitive.kind == PrimitiveKind::Line && primitive.vertex_indices.size() >= 2;
            drawable = drawable || polygon || line;
        }
        primitives = std::move(kept);
        if (removed && !drawable && index < poses.size()) {
            poses[index].visible = false;
        }
    }
    return drawn;
}

namespace {

// The play models draw each eye as its own object, and the eyes are the only
// objects with a pure black (0x000000) fill. Head fills are 0x010000. The
// packet's top byte is the primitive code, so only the color is compared.
std::vector<std::size_t> eye_objects(const TmdModel& model) {
    std::vector<std::size_t> eyes;
    for (std::size_t index = 0; index < model.objects.size(); ++index) {
        for (const TmdPrimitive& primitive : model.objects[index].primitives) {
            if (primitive.kind == PrimitiveKind::Polygon &&
                (primitive.color & 0x00FFFFFFu) == 0x000000u) {
                eyes.push_back(index);
                break;
            }
        }
    }
    return eyes;
}

// Posed center of one object, as distance from the viewer. Larger is farther.
// With no camera this is the side view, where a larger model x is nearer.
bool object_depth(const TmdObject& object,
                  std::span<const Pose> poses,
                  std::size_t index,
                  const CharacterPlacement& placement,
                  const AncSample* camera,
                  float& depth) {
    ModelVertex sum;
    int count = 0;
    const std::size_t vertex_count = std::min<std::size_t>(object.vertices.size(), 0x10000);
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        if (placed_point(object, poses, index, static_cast<std::uint16_t>(vertex), x, y, z)) {
            sum.x += x;
            sum.y += y;
            sum.z += z;
            ++count;
        }
    }
    if (count == 0) {
        return false;
    }
    const float inv = 1.f / static_cast<float>(count);
    const ModelVertex center{sum.x * inv, sum.y * inv, sum.z * inv};
    if (camera == nullptr) {
        depth = -to_view_space(center, ModelView::Side).z;
        return true;
    }
    DiscView view;
    view.eye_x = camera->eye_x;
    view.eye_y = camera->eye_y;
    view.eye_z = camera->eye_z;
    view.target_x = camera->target_x;
    view.target_y = camera->target_y;
    view.target_z = camera->target_z;
    return disc_view_depth(view, figure_world(center, placement), depth);
}

// Facing the camera shows both eyes. Turned more than 45° away hides the eye
// farther from the camera, until the turn is back inside 40°. Facing away,
// past 150°, hides both until the turn is back inside 146°. The yaw rules decide how many
// eyes show; the posed depth decides which one is the far eye.
void hide_eyes(std::vector<Pose>& poses,
               const TmdModel& model,
               const CharacterPlacement& placement,
               const AncSample* camera,
               EyeCullState* state) {
    const std::vector<std::size_t> eyes = eye_objects(model);
    if (eyes.size() != 2 || poses.size() < model.objects.size()) {
        return;
    }
    const float yaw =
        camera != nullptr ? figure_yaw_to_camera(camera->eye_x, camera->eye_z) : kEyeSideYawRadians;
    const EyeVisibility visibility =
        state != nullptr ? eye_visibility_for_yaw(yaw, *state) : eye_visibility_for_yaw(yaw);
    const int hidden = (visibility.left ? 0 : 1) + (visibility.right ? 0 : 1);
    if (hidden == 0) {
        return;
    }
    if (hidden == 2) {
        poses[eyes[0]].visible = false;
        poses[eyes[1]].visible = false;
        return;
    }
    float depth[2] = {};
    for (int i = 0; i < 2; ++i) {
        if (!object_depth(model.objects[eyes[i]], poses, eyes[i], placement, camera, depth[i])) {
            return;
        }
    }
    poses[depth[0] > depth[1] ? eyes[0] : eyes[1]].visible = false;
}

} // namespace

bool paint_disc_figure(std::vector<Segment>& out,
                       const DiscFigurePose& pose,
                       float figure_x,
                       float ribbon_y,
                       std::vector<FilledTriangle>* fills,
                       const AncSample* camera,
                       bool* disc_projected) {
    if (disc_projected != nullptr) {
        *disc_projected = false;
    }
    if (pose.model == nullptr || pose.clip == nullptr || !pose.placement.valid) {
        return false;
    }
    std::vector<Pose> poses =
        poses_at_time(*pose.clip, pose.model->objects.size(), pose.seconds, pose.loop);
    hide_eyes(poses, *pose.model, pose.placement, camera, pose.eyes);
    WireframeOptions options;
    options.packet_color = true;
    options.skip_black_occluders = true;
    const FigureMesh mesh = tmd_figure_mesh(*pose.model, poses, options);
    std::vector<FilledTriangle> local_fills;
    std::vector<FilledTriangle>& fill_out = fills != nullptr ? *fills : local_fills;
    if (camera != nullptr) {
        std::vector<Segment> projected;
        std::vector<FilledTriangle> projected_fills;
        if (project_figure_mesh(projected, projected_fills, mesh, pose.placement, *camera)) {
            out.insert(out.end(), projected.begin(), projected.end());
            fill_out.insert(fill_out.end(), projected_fills.begin(), projected_fills.end());
            if (disc_projected != nullptr) {
                *disc_projected = true;
            }
            return true;
        }
    }
    paint_side_mesh(out, fill_out, mesh, pose.placement, figure_x, ribbon_y);
    return true;
}

} // namespace oscilline
