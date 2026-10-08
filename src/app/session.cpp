// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Loads a course or a chart and runs play, retry, and results.

#include "session.hpp"

#include "course_draw.hpp"
#include "oscilline/asset/character.hpp"
#include "oscilline/asset/registry.hpp"
#include "oscilline/audio/cdda.hpp"
#include "oscilline/course/attack.hpp"
#include "oscilline/course/beats.hpp"
#include "oscilline/course/camera.hpp"
#include "oscilline/course/load.hpp"
#include "oscilline/course/mapping.hpp"
#include "oscilline/course/music.hpp"
#include "oscilline/course/obstacle.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/course/score.hpp"
#include "oscilline/disc/archive.hpp"
#include "oscilline/present/cdda_out.hpp"
#include "oscilline/present/host.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/render/viewport.hpp"
#include "sfx_stage.hpp"
#include "text_out.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oscilline {
namespace {

std::uint8_t action_edges(const HostInput& input) {
    std::uint8_t bits = 0;
    if (pressed(input, Key::Block)) {
        bits = static_cast<std::uint8_t>(bits | kActionBlock);
    }
    if (pressed(input, Key::Loop)) {
        bits = static_cast<std::uint8_t>(bits | kActionLoop);
    }
    if (pressed(input, Key::Wave)) {
        bits = static_cast<std::uint8_t>(bits | kActionWave);
    }
    if (pressed(input, Key::Pit)) {
        bits = static_cast<std::uint8_t>(bits | kActionPit);
    }
    return bits;
}

// The upcoming obstacle's beat. Scroll uses the same period against 120 BPM.
double rabbit_playback_rate(const CourseTimeline& course, std::int64_t now_ms) {
    if (course.events.empty()) {
        return 1.0;
    }
    auto it = std::lower_bound(
        course.events.begin(),
        course.events.end(),
        now_ms,
        [](const CourseEvent& event, std::int64_t time) { return event.hit_ms < time; });
    if (it == course.events.end()) {
        --it;
    }
    return static_cast<double>(rabbit_tempo_scale(it->beat_ms));
}

enum class PlayEnd { Closed, Quit, Retry, Cleared, LoadError };

struct PlayRequest {
    int course = 0;
    int round_number = 0;
    bool hold_on_clear = true;
    // Round 1's totals. A negative score means this screen is a single course.
    RoundTotals prior;
    PlayState carried{};
    bool carry_form = false;
    // Replaces the course number on the HUD (custom music).
    std::string heading;
    PlayOptions options;
};

// A charted course and its audio. Kept across retries, so a retry does not
// read the disc again.
struct ReadyCourse {
    CourseTimeline course;
    std::vector<std::int16_t> pcm;
    int frames = 0;
};

// Plays a loaded course. `assets` are placeholders when disc assets are off.
PlayEnd play_ready(Host& host,
                   const ReadyCourse& ready,
                   AssetRegistry& assets,
                   const PlayRequest& request,
                   PlayState& out_state,
                   SfxStage& sfx) {
    const CourseTimeline& course = ready.course;
    const PlayOptions& options = request.options;
    DebugTextPainter debug_text;
    std::optional<DiscTextPainter> disc_text;
    const TextPainter* text = &debug_text;
    if (options.disc_assets) {
        disc_text = font_painter_for(assets);
        if (disc_text) {
            text = &*disc_text;
        }
    }
    text->set_jitter_scale(text_shake_scale(options.text_shake));

    CddaOutput::Config audio_config;
    audio_config.interleaved = ready.pcm;
    audio_config.frames = ready.frames;
    audio_config.start_delay_ms = kCourseStartDelayMs;
    CddaOutput audio = CddaOutput::open(std::move(audio_config));
    sfx.set_playback(options.playback);

    PlayState play;
    if (request.carry_form) {
        carry_form_into(play, request.carried, kCarryFormAcrossRounds);
        carry_score_into(play, request.carried);
    }
    const CharacterRig rigs[] = {
        make_character_rig(assets, Form::Rabbit),
        make_character_rig(assets, Form::Frog),
        make_character_rig(assets, Form::Worm),
        make_character_rig(assets, Form::Super),
    };
    CharacterClock character;
    EyeCullState eye_cull;
    const TmdModel* eye_model = nullptr;
    int menu = 0;
    std::int64_t sim_ms = 0;
    std::int64_t last_frame_ms = -kCourseStartDelayMs;
    std::int64_t anim_ms = last_frame_ms;
    PlayEnd reason = PlayEnd::Closed;
    bool round_started = false;
    bool results_cue_posted = false;

    // Fixed for the whole run; the loop fills the menu, end screen, figure, and hint.
    const bool use_disc_camera = options.disc_camera && options.disc_assets;
    const auto camera = [&](Slot slot) { return use_disc_camera ? assets.camera(slot) : nullptr; };
    CourseView view;
    view.round_number = request.round_number;
    view.prior = request.prior;
    view.heading = request.heading;
    view.text = text;
    view.assets = options.disc_assets ? &assets : nullptr;
    view.disc_camera = use_disc_camera;
    view.cameras = {camera(Slot::CameraIntro),
                    camera(Slot::CameraPlay),
                    camera(Slot::CameraS02),
                    camera(Slot::CameraRoad),
                    camera(Slot::CameraTvBb),
                    camera(Slot::CameraTvBs)};
    view.ribbon_guides = options.ribbon_guides;
    view.hud.score_number = !options.hud.score_coupons;
    view.hud.timing_hints = options.hud.timing_hints;

    while (host.begin_frame()) {
        (void)host.sim_steps();
        const HostInput& input = host.input();
        if (!round_started) {
            // The sting follows the round-start voice by kMusicStartSfxDelayMs,
            // independent of the eight-second course delay. Pausing freezes both.
            sfx.post_sequence(course_start_sequence(), kMusicStartSfxDelayMs);
            round_started = true;
        }
        sfx.pump();
        sfx.apply_music_gain(audio);
        const std::int64_t clock = audio.position_ms();
        const bool failed = play.finished && play.form == Form::Out;
        const bool outro_done =
            play.finished && !failed &&
            clock >= static_cast<std::int64_t>(course.duration_ms) + kCameraOutroMs;
        const bool show_end = failed || (outro_done && request.hold_on_clear);
        std::uint8_t action_bits = 0;
        bool missed = false;
        bool simulating = false;
        ActionClipCue clip_cue;
        if (show_end) {
            if (pressed(input, Key::Up) && menu != 0) {
                menu = 0;
                sfx.post(SfxId::MenuMove);
            }
            if (pressed(input, Key::Down) && menu != 1) {
                menu = 1;
                sfx.post(SfxId::MenuMove);
            }
            if (pressed(input, Key::Confirm) || pressed(input, Key::Face)) {
                sfx.post(SfxId::MenuSelect);
                reason = menu == 0 ? PlayEnd::Retry : PlayEnd::Quit;
                break;
            }
            if (pressed(input, Key::Back)) {
                // Escape is Quit, so it sounds like choosing Quit.
                sfx.post(SfxId::MenuSelect);
                reason = PlayEnd::Quit;
                break;
            }
        } else if (play.paused) {
            if (pressed(input, Key::Up) && menu != 0) {
                menu = 0;
                sfx.post(SfxId::MenuMove);
            }
            if (pressed(input, Key::Down) && menu != 1) {
                menu = 1;
                sfx.post(SfxId::MenuMove);
            }
            if (pressed(input, Key::Confirm) || pressed(input, Key::Face)) {
                sfx.post(SfxId::MenuSelect);
                if (menu == 0) {
                    play.paused = false;
                    audio.set_paused(false);
                    sfx.set_paused(false);
                } else {
                    reason = PlayEnd::Quit;
                    break;
                }
            } else if (pressed(input, Key::Back) || pressed(input, Key::Start)) {
                // Escape is Resume, so it sounds like choosing Resume.
                sfx.post(SfxId::MenuSelect);
                play.paused = false;
                audio.set_paused(false);
                sfx.set_paused(false);
            }
        } else if (!play.finished && (pressed(input, Key::Start) || pressed(input, Key::Back))) {
            play.paused = true;
            menu = 0;
            audio.set_paused(true);
            sfx.set_paused(true);
        } else {
            simulating = true;
            const std::int64_t now = audio.position_ms();
            const bool was_finished = play.finished;
            std::vector<PlayHit> hits;
            PlayAdvanceResult step;
            if (now >= 0) {
                action_bits = action_edges(input);
                step = play_advance(play,
                                    course,
                                    sim_ms,
                                    now,
                                    action_bits,
                                    &hits,
                                    options.playback.timing_offset_ms);
                sim_ms = now;
                post_play_edges(sfx, step);
            }
            clip_cue = action_clip_cue(step, hits, action_bits);
            post_play_hits(sfx, hits);
            if (!was_finished && play.finished) {
                if (play.form == Form::Out) {
                    // Stop the course track as soon as the game-over state begins.
                    audio.set_paused(true);
                }
                if (const std::optional<SfxId> cue = course_failure_cue(play.form)) {
                    sfx.post(*cue);
                }
            }
            if (play.finished) {
                menu = 0;
            }
        }

        const std::int64_t now = audio.position_ms();
        const double alpha = host.alpha();
        const double span = static_cast<double>(now - last_frame_ms);
        const std::int64_t draw_ms = last_frame_ms + static_cast<std::int64_t>(span * alpha);
        last_frame_ms = now;

        const bool failed_now = play.finished && play.form == Form::Out;
        const bool outro_now =
            play.finished && !failed_now &&
            now >= static_cast<std::int64_t>(course.duration_ms) + kCameraOutroMs;
        const bool show_end_now = failed_now || (outro_now && request.hold_on_clear);
        if (show_end_now && !results_cue_posted) {
            results_cue_posted = true;
            if (const std::optional<SfxId> cue =
                    course_results_cue(play.form, request.hold_on_clear)) {
                sfx.post(*cue);
            }
        }
        double anim_dt = 0;
        if (simulating) {
            anim_dt = static_cast<double>(draw_ms - anim_ms) / 1000.0;
            if (anim_dt < 0) {
                anim_dt = 0;
            }
        }
        anim_ms = draw_ms;
        const CharacterRig* rig = nullptr;
        switch (play.form) {
        case Form::Super:
            rig = &rigs[3];
            break;
        case Form::Rabbit:
            rig = &rigs[0];
            break;
        case Form::Frog:
            rig = &rigs[1];
            break;
        case Form::Worm:
            rig = &rigs[2];
            break;
        case Form::Out:
            break;
        }
        DiscFigurePose disc_pose;
        const DiscFigurePose* disc_figure = nullptr;
        if (rig != nullptr && rig->disc) {
            const std::optional<Slot> slot = form_model_slot(play.form);
            const std::vector<AnmFile>* clips = slot ? assets.animations(*slot) : nullptr;
            if (clips != nullptr) {
                const double character_rate = rabbit_playback_rate(course, draw_ms);
                character_advance(character,
                                  rig->library,
                                  *clips,
                                  anim_dt,
                                  clip_cue.actions,
                                  missed,
                                  play.form,
                                  clip_cue.moment,
                                  character_rate);
                if (play.super_transform.start_ms == sim_ms &&
                    play.super_transform.active_at(sim_ms)) {
                    apply_super_transform_clip(
                        character, play.super_transform.clip_id, static_cast<int>(clips->size()));
                }
                const TmdModel* model = slot ? assets.model(*slot) : nullptr;
                if (model != nullptr && character.clip >= 0 &&
                    static_cast<std::size_t>(character.clip) < clips->size()) {
                    if (model != eye_model) {
                        eye_cull = EyeCullState{};
                        eye_model = model;
                    }
                    disc_pose.model = model;
                    disc_pose.clip = &(*clips)[static_cast<std::size_t>(character.clip)];
                    disc_pose.loop = !character.oneshot;
                    disc_pose.seconds = character.seconds;
                    disc_pose.placement = rig->placement;
                    disc_pose.eyes = &eye_cull;
                    disc_figure = &disc_pose;
                }
            }
        }
        const std::string hint = options.hud.control_hints
                                     ? control_hint_text(host.controls(), host.input().pad_active)
                                     : std::string();
        view.menu = menu;
        view.show_end = show_end_now;
        view.figure = disc_figure;
        view.hud.control_hint = hint;
        const CourseFrame frame = draw_course(course, play, draw_ms, view);
        host.end_frame(frame.triangles, frame.text);
        if (outro_now && !request.hold_on_clear) {
            reason = PlayEnd::Cleared;
            break;
        }
    }
    out_state = play;
    return reason;
}

bool hold_screen(Host& host,
                 const std::string& heading,
                 int duration_ms,
                 const TextPainter* painter) {
    double elapsed = 0.0;
    bool drew = false;
    while (host.begin_frame()) {
        const int steps = host.sim_steps();
        elapsed += static_cast<double>(steps) * host.step_seconds();
        TriangleList picture;
        std::vector<TextGlyph> text;
        TextTarget target;
        target.glyphs = &text;
        target.triangles = &picture;
        DebugTextPainter debug_text;
        const TextPainter& text_painter = painter != nullptr ? *painter : debug_text;
        text_painter.set_time_ms(static_cast<std::int64_t>(elapsed * 1000.0));
        const float x =
            (static_cast<float>(logical_width()) - text_painter.measure_width(heading)) * 0.5f;
        text_painter.line(target, x, 220.f, heading);
        host.end_frame(picture, text);
        drew = true;
        if (drew && elapsed >= static_cast<double>(duration_ms) / 1000.0) {
            return true;
        }
    }
    return false;
}

// Preloads every asset slot behind a progress bar. False when the window closed.
bool preload_assets(Host& host, AssetRegistry& assets, const TextPainter* text) {
    const int steps = assets.preload_steps();
    for (int step = 0; step < steps; ++step) {
        const float fraction =
            static_cast<float>(step + 1) / static_cast<float>(std::max(steps, 1));
        if (!present_loading(host, "ASSETS", fraction, text)) {
            return false;
        }
        assets.preload_step(step);
    }
    for (const std::string& warning : assets.warnings()) {
        std::cerr << "oscilline: asset: " << warning << '\n';
    }
    return true;
}

// Charts `request.course` and reads its CD audio into `ready`. With a non-null
// `assets` and disc assets on, also loads the disc assets there; the second
// round of a pair passes null and reuses the first round's.
// Returns the failure, or nothing when the course is ready.
std::optional<PlayEnd>
load_course(Host& host, const PlayRequest& request, ReadyCourse& ready, AssetRegistry* assets) {
    const PlayOptions& options = request.options;
    CourseTimeline course;
    std::vector<std::int16_t> pcm;
    std::vector<float> envelope;
    int frames = 0;
    {
        auto mounted = mount_disc(options.disc);
        if (!mounted) {
            std::cerr << "oscilline: " << mounted.error() << '\n';
            return PlayEnd::LoadError;
        }
        auto pak = load_language_pak(mounted.value().volume, options.language_pak);
        if (!pak && !options.language_pak.empty()) {
            std::cerr << "oscilline: " << pak.error() << '\n';
            pak = load_language_pak(mounted.value().volume);
        }
        if (!pak) {
            std::cerr << "oscilline: " << pak.error() << '\n';
            return PlayEnd::LoadError;
        }
        auto script = load_course_script(pak.value());
        if (!script) {
            std::cerr << "oscilline: " << script.error() << '\n';
            return PlayEnd::LoadError;
        }
        const int listed = static_cast<int>(script.value().track_index.size());
        const int playable = std::min(listed, kPlayableCourses);
        if (request.course < 0 || request.course >= playable) {
            std::cerr << "oscilline: course " << (request.course + 1)
                      << " is outside the playable courses (1-" << playable << ")\n";
            return PlayEnd::LoadError;
        }

        const int cdda_track = request.course + 2;
        std::int32_t audio_ms = 0;
        auto track = read_cdda_track(mounted.value().image, cdda_track);
        if (!track) {
            std::cerr << "oscilline: " << track.error() << "; continuing without the music\n";
        } else {
            frames = track.value().frames;
            audio_ms = cdda_duration_ms(frames);
            pcm = std::move(track.value().interleaved);
        }

        std::vector<std::int32_t> beats;
        std::vector<float> emphasis;
        if (frames > 0) {
            // The original charts disc courses from the playing audio: attacks
            // in the mono mix place the hits. The onset envelope still drives
            // the ribbon vibration. A quiet tail inside the file is not music,
            // so the chart ends at the last audible hop rather than the sector
            // length. Custom charts already close on loudness and long silence;
            // both paths then share this audio end for spawn and judgment.
            emphasis = attack_emphasis(pcm, frames);
            envelope = onset_envelope(pcm, frames);
            const std::int32_t audible = attack_audible_end_ms(pcm, frames);
            if (audible > 0 && audible < audio_ms) {
                audio_ms = audible;
            }
        } else {
            // No track. One silent sample selects attack mode, so the beat grid
            // does not fill the ribbon while nothing is playing.
            emphasis.assign(1, 0.f);
        }
        if (frames > 0 && emphasis.empty()) {
            std::vector<float> periods(envelope.size());
            bool periods_ok = !envelope.empty();
            for (std::size_t i = 0; i < envelope.size() && periods_ok; ++i) {
                auto period = course_beat_period_ms(
                    script.value(), request.course, static_cast<std::int32_t>(i) * kOnsetStepMs);
                periods_ok = static_cast<bool>(period);
                if (periods_ok) {
                    periods[i] = static_cast<float>(period.value());
                }
            }
            if (periods_ok) {
                beats = track_beats(envelope, periods);
            }
        }
        CourseMapOptions map_options;
        map_options.beat_ms = beats;
        map_options.emphasis = emphasis;
        auto timeline = build_course(script.value(), request.course, audio_ms, map_options);
        if (!timeline) {
            std::cerr << "oscilline: " << timeline.error() << '\n';
            return PlayEnd::LoadError;
        }
        std::cout << "course " << (request.course + 1) << ", CD track " << cdda_track << ", "
                  << timeline.value().events.size() << " obstacles\n";
        course = std::move(timeline.value());
        if (options.disc_assets && assets != nullptr) {
            *assets = AssetRegistry::from_disc(
                mounted.value(), false, options.flags.asset_confidence, options.language_pak);
            std::optional<DiscTextPainter> asset_font = font_painter_for(*assets);
            if (asset_font) {
                asset_font->set_jitter_scale(text_shake_scale(options.text_shake));
            }
            if (!preload_assets(host, *assets, asset_font ? &*asset_font : nullptr)) {
                return PlayEnd::Closed;
            }
        }
    }

    ready.course = std::move(course);
    ready.pcm = std::move(pcm);
    ready.frames = frames;
    return std::nullopt;
}

// Built-in presentation keeps the course and the CD audio, not the bank.
void load_bank(SfxStage& sfx, const PlayOptions& options) {
    if (options.disc_assets) {
        sfx.load_path(options.disc);
    }
}

} // namespace

PlayOptions play_options(const Settings& effective,
                         std::filesystem::path disc,
                         std::string_view language_pak,
                         const RunFlags& flags) {
    PlayOptions options;
    options.disc = std::move(disc);
    options.disc_assets = effective.disc_assets;
    options.disc_camera = effective.disc_camera;
    options.ribbon_guides = effective.ribbon_guides;
    options.language_pak = std::string(language_pak);
    options.playback = playback_from(effective);
    options.text_shake = effective.text_shake;
    options.hud = effective.hud;
    options.flags = flags;
    return options;
}

int run_session(Host& host, const PlayOptions& options, int course) {
    PlayRequest request;
    request.course = course;
    request.options = options;
    ReadyCourse ready;
    AssetRegistry assets = AssetRegistry::placeholders();
    if (const auto failed = load_course(host, request, ready, &assets)) {
        return *failed == PlayEnd::LoadError ? 1 : 0;
    }
    for (;;) {
        PlayState state;
        SfxStage sfx(options.flags.sfx_experimental);
        load_bank(sfx, options);
        if (play_ready(host, ready, assets, request, state, sfx) != PlayEnd::Retry) {
            return 0;
        }
    }
}

int run_difficulty(Host& host,
                   const PlayOptions& options,
                   Difficulty difficulty,
                   ScoreBoard& scores) {
    const CoursePair pair = courses_for(difficulty);
    // One registry serves the loading card's font and both rounds.
    AssetRegistry assets = AssetRegistry::placeholders();
    std::optional<DiscTextPainter> card_font;
    const TextPainter* card_text = nullptr;
    if (options.disc_assets) {
        if (auto mounted = mount_disc(options.disc)) {
            assets = AssetRegistry::from_disc(
                mounted.value(), false, options.flags.asset_confidence, options.language_pak);
            card_font = font_painter_for(assets);
            if (card_font) {
                card_font->set_jitter_scale(text_shake_scale(options.text_shake));
                card_text = &*card_font;
            }
        }
    }
    if (!hold_screen(host, "LOADING", kLoadingMs, card_text)) {
        return 0;
    }
    if (options.disc_assets && !preload_assets(host, assets, card_text)) {
        return 0;
    }
    // Both rounds load up front, so round 2 follows round 1 with no loading
    // screen, and a retry replays without reading the disc again.
    PlayRequest requests[2];
    ReadyCourse ready[2];
    for (int round = 0; round < 2; ++round) {
        PlayRequest& request = requests[round];
        request.course = round == 0 ? pair.first : pair.second;
        request.round_number = round + 1;
        request.hold_on_clear = round == 1;
        request.options = options;
        if (round == 1 && !present_loading(host, "ROUND 2", 1.f, card_text)) {
            return 0;
        }
        if (const auto failed = load_course(host, request, ready[round], nullptr)) {
            return *failed == PlayEnd::LoadError ? 1 : 0;
        }
    }
    for (;;) {
        int total = 0;
        PlayState carried;
        SfxStage sfx(options.flags.sfx_experimental);
        load_bank(sfx, options);
        PlayEnd end = PlayEnd::Cleared;
        for (int round = 0; round < 2 && end == PlayEnd::Cleared; ++round) {
            PlayRequest& request = requests[round];
            if (round == 1) {
                request.prior = {
                    result_score(carried), carried.perfects, carried.goods, carried.misses};
                request.carried = carried;
                request.carry_form = true;
            }
            PlayState state;
            end = play_ready(host, ready[round], assets, request, state, sfx);
            total += result_score(state);
            carried = state;
        }
        if (end == PlayEnd::Closed) {
            return 0;
        }
        note_score(scores, difficulty, total);
        if (end != PlayEnd::Retry) {
            return 0;
        }
    }
}

bool present_loading(Host& host,
                     std::string_view stage,
                     float fraction,
                     const TextPainter* painter) {
    if (!host.begin_frame()) {
        return false;
    }
    const float t = std::clamp(fraction, 0.f, 1.f);
    std::vector<Segment> segments;
    const float center_x = static_cast<float>(logical_width()) * 0.5f;
    const float kLeft = center_x - 160.f;
    const float kRight = center_x + 160.f;
    constexpr float kY = 260.f;
    const auto add = [&](float x0, float y0, float x1, float y1) {
        Segment segment;
        segment.x0 = x0;
        segment.y0 = y0;
        segment.x1 = x1;
        segment.y1 = y1;
        segments.push_back(segment);
    };
    add(kLeft, kY - 8.f, kRight, kY - 8.f);
    add(kRight, kY - 8.f, kRight, kY + 8.f);
    add(kRight, kY + 8.f, kLeft, kY + 8.f);
    add(kLeft, kY + 8.f, kLeft, kY - 8.f);
    add(kLeft, kY, kLeft + (kRight - kLeft) * t, kY);
    TriangleList triangles;
    append_strokes(triangles, segments);
    std::vector<TextGlyph> text;
    DebugTextPainter debug_text;
    const TextPainter& text_painter = painter != nullptr ? *painter : debug_text;
    text_painter.set_time_ms(static_cast<std::int64_t>(SDL_GetTicks()));
    const auto glyph = [&](float y, std::string value) {
        const float x =
            (static_cast<float>(logical_width()) - text_painter.measure_width(value)) * 0.5f;
        TextTarget target;
        target.glyphs = &text;
        target.triangles = &triangles;
        text_painter.line(target, x, y, std::move(value));
    };
    glyph(200.f, "LOADING");
    glyph(220.f, std::string(stage));
    glyph(284.f, std::to_string(static_cast<int>(t * 100.f)) + "%");
    host.end_frame(triangles, text);
    return true;
}

bool pump_music_load(void* user, float fraction, const char* stage) {
    auto* pump = static_cast<MusicLoadPump*>(user);
    if (pump == nullptr || pump->host == nullptr || !pump->alive) {
        return false;
    }
    const std::uint64_t now = SDL_GetTicks();
    if (fraction < 1.f && pump->last_ms != 0 && now - pump->last_ms < 100) {
        return true;
    }
    pump->last_ms = now;
    if (!present_loading(*pump->host, stage == nullptr ? "LOADING" : stage, fraction, pump->text)) {
        pump->alive = false;
        return false;
    }
    return true;
}

namespace {

struct ChartJob {
    std::filesystem::path path;
    Difficulty difficulty = Difficulty::Bronze;
    const DecodedMusic* cached = nullptr;
    bool gen_experimental = false;
    std::atomic<float> fraction{0.f};
    std::atomic<int> stage{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    bool have_audio = false;
    DecodedMusic audio;
    Result<MusicChart> chart = Result<MusicChart>::failure("closed");
    std::string decode_error;
};

bool chart_job_progress(void* user, float fraction, const char* stage) {
    auto* job = static_cast<ChartJob*>(user);
    if (job->cancel.load(std::memory_order_relaxed)) {
        return false;
    }
    job->fraction.store(fraction, std::memory_order_relaxed);
    job->stage.store(stage != nullptr && stage[0] == 'C' ? 1 : 0, std::memory_order_relaxed);
    return true;
}

int chart_job_main(void* raw) {
    auto* job = static_cast<ChartJob*>(raw);
    const DecodedMusic* audio = job->cached;
    if (audio == nullptr) {
        auto decoded = decode_music_file(job->path, {}, chart_job_progress, job);
        if (!decoded) {
            job->decode_error = decoded.error();
            job->finished.store(true, std::memory_order_release);
            return 0;
        }
        job->audio = std::move(decoded.value());
        audio = &job->audio;
    }
    job->have_audio = true;
    job->stage.store(1, std::memory_order_relaxed);
    MusicChartOptions chart_options;
    chart_options.experimental = job->gen_experimental;
    job->chart = chart_music(
        audio->interleaved, audio->frames, job->difficulty, chart_job_progress, job, chart_options);
    job->finished.store(true, std::memory_order_release);
    return 0;
}

} // namespace

int run_music(Host& host,
              const PlayOptions& options,
              const std::filesystem::path& music,
              Difficulty difficulty,
              const DecodedMusic* cached) {
    // The disc is mounted first so the loading screens use its font.
    std::optional<MountedDisc> mounted;
    if (options.disc_assets && !options.disc.empty()) {
        auto opened = mount_disc(options.disc);
        if (!opened) {
            std::cerr << "oscilline: " << opened.error() << '\n';
        } else {
            mounted = std::move(opened.value());
        }
    }
    const bool use_disc = mounted.has_value();
    AssetRegistry assets = assets_for_custom_play(use_disc ? &*mounted : nullptr,
                                                  use_disc,
                                                  options.flags.asset_confidence,
                                                  options.language_pak);
    std::optional<DiscTextPainter> asset_font;
    if (use_disc) {
        asset_font = font_painter_for(assets);
        if (asset_font) {
            asset_font->set_jitter_scale(text_shake_scale(options.text_shake));
        }
    }
    const TextPainter* loading_text = asset_font ? &*asset_font : nullptr;

    ChartJob job;
    job.path = music;
    job.difficulty = difficulty;
    job.cached = cached;
    job.gen_experimental = options.flags.gen_experimental;
    SDL_Thread* thread = SDL_CreateThread(chart_job_main, "oscilline-chart", &job);
    bool alive = true;
    if (thread == nullptr) {
        MusicLoadPump pump;
        pump.host = &host;
        pump.text = loading_text;
        if (cached == nullptr) {
            auto decoded = decode_music_file(music, {}, pump_music_load, &pump);
            if (!decoded) {
                if (!pump.alive || decoded.error() == "closed") {
                    return 0;
                }
                std::cerr << "oscilline: " << decoded.error() << '\n';
                return 1;
            }
            job.audio = std::move(decoded.value());
        }
        job.have_audio = true;
        const DecodedMusic* audio = cached != nullptr ? cached : &job.audio;
        MusicChartOptions chart_options;
        chart_options.experimental = options.flags.gen_experimental;
        job.chart = chart_music(
            audio->interleaved, audio->frames, difficulty, pump_music_load, &pump, chart_options);
        alive = pump.alive;
        job.finished.store(true, std::memory_order_release);
    } else {
        std::uint64_t last_ms = 0;
        while (!job.finished.load(std::memory_order_acquire)) {
            const std::uint64_t now = SDL_GetTicks();
            if (last_ms == 0 || now - last_ms >= 100) {
                last_ms = now;
                const char* stage =
                    job.stage.load(std::memory_order_relaxed) == 0 ? "READING" : "CHARTING";
                if (!present_loading(
                        host, stage, job.fraction.load(std::memory_order_relaxed), loading_text)) {
                    job.cancel.store(true, std::memory_order_relaxed);
                    alive = false;
                    break;
                }
            }
            SDL_Delay(10);
        }
        SDL_WaitThread(thread, nullptr);
    }
    if (!alive || job.decode_error == "closed") {
        return 0;
    }
    if (!job.decode_error.empty()) {
        std::cerr << "oscilline: " << job.decode_error << '\n';
        return 1;
    }
    const DecodedMusic* audio = cached != nullptr ? cached : &job.audio;
    if (!job.have_audio || audio == nullptr) {
        return 0;
    }
    if (audio->truncated) {
        std::cerr << "oscilline: that track is long, so this plays the first "
                  << kMaxMusicSeconds / 60 << " minutes\n";
    }
    if (!job.chart) {
        if (!alive || job.chart.error() == "closed") {
            return 0;
        }
        std::cerr << "oscilline: " << job.chart.error() << '\n';
        return 1;
    }
    std::cout << "music " << music.filename().string() << ", "
              << job.chart.value().timeline.events.size() << " obstacles\n";

    std::string heading = music.stem().string();
    if (heading.empty()) {
        heading = "MUSIC";
    }
    for (char& letter : heading) {
        letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    }
    if (heading.size() > 42) {
        heading.resize(42);
    }

    const std::filesystem::path score_file = music_score_path();
    auto loaded = load_music_scores(score_file);
    MusicScoreBook book = loaded ? std::move(loaded.value()) : MusicScoreBook{};
    const std::string fingerprint = audio->fingerprint;

    if (use_disc) {
        if (!preload_assets(host, assets, loading_text)) {
            return 0;
        }
    } else {
        for (const std::string& warning : assets.warnings()) {
            std::cerr << "oscilline: asset: " << warning << '\n';
        }
    }

    PlayRequest request;
    request.hold_on_clear = true;
    request.heading = heading;
    request.options = options;
    request.options.disc_assets = use_disc;
    // No FSL spans. Equal slices drive the tier's camera row.
    request.options.disc_camera = use_disc && options.disc_camera;
    ReadyCourse ready;
    ready.course = job.chart.value().timeline;
    if (request.options.disc_camera) {
        ready.course.camera_sections =
            custom_music_camera_sections(cdda_duration_ms(audio->frames));
        ready.course.track_index = custom_music_camera_course(difficulty);
        ready.course.gold_shift = custom_music_gold_shift(difficulty);
    }
    ready.pcm = audio->interleaved;
    ready.frames = audio->frames;
    for (;;) {
        PlayState state;
        SfxStage sfx(options.flags.sfx_experimental);
        if (use_disc) {
            sfx.load(mounted->volume);
        }
        // Course audio lives in play_ready and stops when that call returns.
        const PlayEnd end = play_ready(host, ready, assets, request, state, sfx);
        music_note(book, fingerprint, difficulty, result_score(state));
        if (auto saved = save_music_scores(score_file, book); !saved) {
            std::cerr << "oscilline: " << saved.error() << '\n';
        }
        if (end == PlayEnd::Retry) {
            continue;
        }
        return end == PlayEnd::LoadError ? 1 : 0;
    }
}

} // namespace oscilline
