// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Command line and process entry for the oscilline window.

#include "oscilline/asset/character.hpp"
#include "oscilline/asset/menu.hpp"
#include "oscilline/asset/registry.hpp"
#include "oscilline/course/difficulty.hpp"
#include "oscilline/course/mapping.hpp"
#include "oscilline/disc/archive.hpp"
#include "oscilline/present/host.hpp"
#include "oscilline/settings.hpp"
#include "oscilline/version.hpp"
#include "session.hpp"
#include "sfx_stage.hpp"
#include "title.hpp"

#include <SDL3/SDL_main.h>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void print_usage(std::ostream& out) {
    out << "Oscilline " << oscilline::version_string() << "\n"
        << "Usage: oscilline [--version] [--help] [--frames N]\n"
        << "                 [--disc <cue>] [--course N] [--difficulty bronze|silver|gold]\n"
        << "                 [--music <file>] [--play]\n"
        << "                 [--no-disc-assets] [--asset-report] [--asset-confidence "
           "high|medium|low]\n"
        << "                 [--disc-camera] [--builtin-camera] [--ribbon-guides] [--language "
           "english|japanese|german|spanish|french|italian]\n"
        << "                 [--clips-test] [--sfx-test] [--sfx-experimental]\n"
        << "                 [--gen-experimental]\n"
        << "\n"
        << "Opens a title. Play Original opens a wheel when a disc is loaded and disc\n"
        << "assets are on: bronze, silver, gold, scores, and back. That row is hidden\n"
        << "with no disc, or when disc assets are off.\n"
        << "Load Disc asks for an image, or pass --disc. Play My Music charts a wav,\n"
        << "flac, or mp3. A difficulty loads two courses together and plays them\n"
        << "back to back. --play skips the title.\n"
        << "--play with no disc and no --music is an error.\n"
        << "Up and down move the wheel. Left and right do nothing there.\n"
        << "The title list, the language list, and scores stop at the ends.\n"
        << "Up and down move scores. Back leaves, the same as the other lists.\n"
        << "Q / left shoulder is block, E / right shoulder is loop,\n"
        << "X or Space / the south button is wave, S or Down / d-pad down is pit.\n"
        << "Those are the defaults; Options > Controls rebinds them.\n"
        << "Options > Interface sets the score display, timing hints, and control hints.\n"
        << "P, Start, or Escape pauses. Escape on the title quits.\n"
        << "--course N with --play starts that one course. N counts from 1, and "
        << oscilline::kPlayableCourses
        << " is the last. Without --play, --course leaves the title on the main menu.\n"
        << "--difficulty names a pair, or the density of a music chart. With --play it\n"
        << "starts that pair or chart. Without --play the title opens on that row.\n"
        << "Do not pass --course together with --difficulty or --music.\n"
        << "--music <file> plays that file. With --play it starts immediately.\n"
        << "--frames N presents N frames and exits, including loading frames\n"
        << "(CI uses SDL_VIDEODRIVER=dummy).\n"
        << "--no-disc-assets keeps placeholder art, the built-in camera, and silent\n"
        << "stand-in sounds for this run. It does not rewrite the saved disc_assets\n"
        << "flag. Options can save that same choice. The disc path can stay mounted\n"
        << "for courses and CD audio.\n"
        << "--asset-report prints each slot as a disc path or placeholder.\n"
        << "--asset-confidence sets which mapped slots may leave the placeholder.\n"
        << "High is the default. Medium and low stay off until you lower it.\n"
        << "Character clips and menu clips are high, so they play at that default.\n"
        << "--clips-test prints each action clip with its confidence, then exits.\n"
        << "Mapped clips are high and on at the default floor. A row below\n"
        << "--asset-confidence is gated, otherwise on. With --disc it also prints\n"
        << "the clip each form plays for each row, or none.\n"
        << "--sfx-test plays every event cue, then exits. Unmapped cues use placeholders.\n"
        << "It prints bank, program and tone or a VAG index, rate, and confidence.\n"
        << "Medium rows are marked gated.\n"
        << "--sfx-experimental also plays mapped low- and medium-confidence samples.\n"
        << "--gen-experimental opts into medium-confidence custom-music placement:\n"
        << "track density and a 2 second, 4-7 beat stride. The loudness gate and empty\n"
        << "noise and silence are on without it. Disc courses are unchanged. Bronze,\n"
        << "silver, and gold are easy, medium, and hard on a custom chart.\n"
        << "Triangle or the pad back button matches Escape.\n"
        << "--disc-camera is the default. Each script section stretches its road\n"
        << "file (S01, S02, or B01). Course 1 plays TV_SS, TV_BB, or TV_BS so\n"
        << "it ends as the next section starts. Courses 2-6 play that spin only\n"
        << "in an obstacle-free gap; a short gap holds the previous play key\n"
        << "and eases into the next. Custom music with a disc does the same.\n"
        << "--builtin-camera selects the built-in camera. The last of those two\n"
        << "flags wins. --no-disc-assets keeps the built-in camera. Custom music\n"
        << "uses the disc camera when a disc is loaded and the camera setting is\n"
        << "disc: equal sections on that tier's course row, and a spin only in an\n"
        << "obstacle-free gap. The built-in camera is used when that setting or\n"
        << "--builtin-camera is chosen, and when there is no disc.\n"
        << "--ribbon-guides draws tick marks on the ribbon, the hit marker, and\n"
        << "the hit windows. The ribbon is a plain line without that flag.\n"
        << "Options are grouped into Video, Audio, Interface, Controls, and Other. Reset\n"
        << "Settings restores defaults. The last loaded disc path is saved beside the\n"
        << "music scores and can be selected again from the title.\n"
        << "Other flags override saved values for this run; --disc updates the last-used path.\n"
        << "Load Disc, Play My Music, and scores stay on the font.\n"
        << "Browse disc models with oscilline-viewer --disc <cue>.\n"
        << "oscilline-viewer --anc <name> draws that camera path.\n"
        << "oscilline-viewer --sfx lists SfxId cues and plays one when a disc bank is loaded.\n"
        << "Tab in the model or camera view opens that browser without --sfx.\n";
}

std::optional<oscilline::Difficulty> parse_difficulty(std::string_view text) {
    std::string lower(text);
    for (char& letter : lower) {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    if (lower == "bronze") {
        return oscilline::Difficulty::Bronze;
    }
    if (lower == "silver") {
        return oscilline::Difficulty::Silver;
    }
    if (lower == "gold") {
        return oscilline::Difficulty::Gold;
    }
    return std::nullopt;
}

std::optional<oscilline::Confidence> parse_confidence(std::string_view text) {
    if (text == "high") {
        return oscilline::Confidence::High;
    }
    if (text == "medium") {
        return oscilline::Confidence::Medium;
    }
    if (text == "low") {
        return oscilline::Confidence::Low;
    }
    return std::nullopt;
}

bool parse_non_negative(std::string_view text, int& value) {
    if (text.empty()) {
        return false;
    }
    int parsed = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 0) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int frame_limit = -1;
    int course_number = 1;
    bool course_set = false;
    bool play_now = false;
    bool music_set = false;
    bool disc_assets = true;
    bool cli_no_disc_assets = false;
    bool asset_report = false;
    oscilline::Confidence asset_confidence = oscilline::kUsableConfidence;
    bool clips_test = false;
    bool sfx_test = false;
    bool sfx_experimental = false;
    bool gen_experimental = false;
    bool ribbon_flag = false;
    std::optional<bool> camera_flag;
    std::string language_pak;
    std::optional<oscilline::Difficulty> difficulty;
    std::filesystem::path disc;
    std::filesystem::path music;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--version") {
            std::cout << "Oscilline " << oscilline::version_string() << '\n';
            return EXIT_SUCCESS;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return EXIT_SUCCESS;
        }
        if (arg == "--frames" || arg == "--course") {
            if (i + 1 >= argc) {
                std::cerr << "oscilline: " << arg << " needs a number\n";
                return EXIT_FAILURE;
            }
            int value = 0;
            if (!parse_non_negative(argv[++i], value)) {
                std::cerr << "oscilline: " << arg << " must be a non-negative integer\n";
                return EXIT_FAILURE;
            }
            if (arg == "--frames") {
                frame_limit = value;
            } else if (value < 1 || value > oscilline::kPlayableCourses) {
                std::cerr << "oscilline: --course N counts from 1 to "
                          << oscilline::kPlayableCourses << '\n';
                return EXIT_FAILURE;
            } else {
                course_number = value;
                course_set = true;
            }
        } else if (arg == "--difficulty") {
            if (i + 1 >= argc) {
                std::cerr << "oscilline: --difficulty needs bronze, silver, or gold\n";
                return EXIT_FAILURE;
            }
            difficulty = parse_difficulty(argv[++i]);
            if (!difficulty) {
                std::cerr << "oscilline: --difficulty must be bronze, silver, or gold\n";
                return EXIT_FAILURE;
            }
        } else if (arg == "--disc") {
            if (i + 1 >= argc) {
                std::cerr << "oscilline: --disc needs a cue path\n";
                return EXIT_FAILURE;
            }
            disc = argv[++i];
        } else if (arg == "--music") {
            if (i + 1 >= argc) {
                std::cerr << "oscilline: --music needs a wav, flac, or mp3 path\n";
                return EXIT_FAILURE;
            }
            music = argv[++i];
            music_set = true;
        } else if (arg == "--play") {
            play_now = true;
        } else if (arg == "--no-disc-assets") {
            disc_assets = false;
            cli_no_disc_assets = true;
        } else if (arg == "--asset-report") {
            asset_report = true;
        } else if (arg == "--asset-confidence") {
            if (i + 1 >= argc) {
                std::cerr << "oscilline: --asset-confidence needs high, medium, or low\n";
                return EXIT_FAILURE;
            }
            const auto parsed = parse_confidence(argv[++i]);
            if (!parsed) {
                std::cerr << "oscilline: --asset-confidence must be high, medium, or low\n";
                return EXIT_FAILURE;
            }
            asset_confidence = parsed.value();
        } else if (arg == "--clips-test") {
            clips_test = true;
        } else if (arg == "--sfx-test") {
            sfx_test = true;
        } else if (arg == "--sfx-experimental") {
            sfx_experimental = true;
        } else if (arg == "--gen-experimental") {
            gen_experimental = true;
        } else if (arg == "--disc-camera") {
            camera_flag = true;
        } else if (arg == "--builtin-camera") {
            camera_flag = false;
        } else if (arg == "--ribbon-guides") {
            ribbon_flag = true;
        } else if (arg == "--language") {
            if (i + 1 >= argc) {
                std::cerr << "oscilline: --language needs a language name\n";
                return EXIT_FAILURE;
            }
            const auto pak = oscilline::language_pak_for(argv[++i]);
            if (!pak) {
                std::cerr << "oscilline: --language must be english, japanese, german, spanish, "
                             "french, or italian\n";
                return EXIT_FAILURE;
            }
            language_pak = pak.value();
        } else {
            std::cerr << "oscilline: unknown argument '" << arg << "'\n";
            print_usage(std::cerr);
            return EXIT_FAILURE;
        }
    }

    std::cout << "Oscilline " << oscilline::version_string() << '\n';

    if (clips_test) {
        std::cout << oscilline::character_clip_report(asset_confidence);
        if (disc.empty() || !disc_assets) {
            return EXIT_SUCCESS;
        }
        auto mounted = oscilline::mount_disc(disc);
        if (!mounted) {
            std::cerr << "oscilline: " << mounted.error() << '\n';
            return EXIT_FAILURE;
        }
        auto registry = oscilline::AssetRegistry::from_disc(
            mounted.value(), false, asset_confidence, language_pak);
        constexpr std::pair<oscilline::Form, const char*> kForms[] = {
            {oscilline::Form::Super, "super"},
            {oscilline::Form::Rabbit, "rabbit"},
            {oscilline::Form::Frog, "frog"},
            {oscilline::Form::Worm, "worm"}};
        for (const auto& [form, name] : kForms) {
            const oscilline::Slot slot = oscilline::form_model_slot(form).value();
            registry.load(slot);
            const std::vector<std::string>* names = registry.animation_names(slot);
            if (names == nullptr) {
                std::cout << name << " placeholder\n";
                continue;
            }
            std::cout << oscilline::character_clip_resolve(name, *names, asset_confidence);
        }
        return EXIT_SUCCESS;
    }

    const oscilline::SettingsLoad loaded = oscilline::load_settings(oscilline::settings_path());
    if (!loaded.error.empty()) {
        std::cerr << "oscilline: " << loaded.error << '\n';
    }
    oscilline::Settings stored = loaded.settings;
    if (disc.empty() && !stored.disc_path.empty()) {
        disc = stored.disc_path;
    }
    if (!disc.empty() && stored.disc_path != disc.string()) {
        stored.disc_path = disc.string();
        const auto saved = oscilline::save_settings(oscilline::settings_path(), stored);
        if (!saved) {
            std::cerr << "oscilline: " << saved.error() << '\n';
        }
    }

    if (course_set && (difficulty || music_set)) {
        std::cerr
            << "oscilline: --course is a single disc course. Leave off --difficulty and --music.\n";
        return EXIT_FAILURE;
    }
    if (course_set && !play_now) {
        std::cerr << "oscilline: --course " << course_number
                  << " without --play does not start that course. The title opens on the "
                     "main menu. Pass --play --course "
                  << course_number << " to play that course on its own.\n";
    }
    if (play_now && !sfx_test && disc.empty() && !music_set) {
        std::cerr << "oscilline: no disc. Pass --disc <cue> or --music <file>.\n";
        return EXIT_FAILURE;
    }
    if (disc.empty() && !music_set) {
        std::cerr << "oscilline: no disc. Pass --disc <cue> or choose Load Disc.\n";
    }

    if (asset_report) {
        oscilline::AssetRegistry registry = oscilline::AssetRegistry::placeholders();
        if (disc_assets && !disc.empty()) {
            auto mounted = oscilline::mount_disc(disc);
            if (!mounted) {
                std::cerr << "oscilline: " << mounted.error() << '\n';
            } else {
                registry = oscilline::AssetRegistry::from_disc(
                    mounted.value(), false, asset_confidence, language_pak);
                registry.preload_all();
            }
        }
        std::cout << registry.report();
        for (const std::string& warning : registry.warnings()) {
            std::cerr << "oscilline: asset: " << warning << '\n';
        }
    }

    oscilline::SettingsOverrides overrides;
    if (ribbon_flag) {
        overrides.ribbon_guides = true;
    }
    if (camera_flag) {
        overrides.disc_camera = camera_flag;
    }
    if (cli_no_disc_assets) {
        overrides.disc_assets = false;
    }
    const oscilline::Settings effective = oscilline::apply_settings_overrides(stored, overrides);
    const std::string session_language = language_pak.empty() ? stored.language_pak : language_pak;

    oscilline::Host::Config config;
    config.title = "Oscilline";
    config.frame_limit = frame_limit;
    config.aspect_ratio = effective.aspect_ratio;
    auto host = oscilline::Host::open(config);
    if (!host) {
        std::cerr << "oscilline: " << host.error() << '\n';
        return EXIT_FAILURE;
    }
    if (effective.window_scale > 0) {
        host.value().set_window_scale(effective.window_scale);
    }
    if (effective.fullscreen) {
        host.value().set_fullscreen(true);
    }
    host.value().set_classic_resolution(effective.classic_resolution);
    host.value().set_controls(effective.controls);

    if (sfx_test) {
        return oscilline::run_sfx_test(host.value(), disc, sfx_experimental);
    }
    oscilline::RunFlags flags;
    flags.asset_confidence = asset_confidence;
    flags.sfx_experimental = sfx_experimental;
    flags.gen_experimental = gen_experimental;
    if (play_now) {
        const oscilline::PlayOptions options =
            oscilline::play_options(effective, std::move(disc), session_language, flags);
        if (music_set) {
            const oscilline::Difficulty music_difficulty =
                difficulty ? difficulty.value() : oscilline::Difficulty::Bronze;
            return oscilline::run_music(host.value(), options, music, music_difficulty);
        }
        if (difficulty) {
            oscilline::ScoreBoard scores;
            return oscilline::run_difficulty(host.value(), options, difficulty.value(), scores);
        }
        return oscilline::run_session(host.value(), options, course_number - 1);
    }
    oscilline::TitleStart start;
    start.disc = std::move(disc);
    start.music = std::move(music);
    start.open_wheel = difficulty && !music_set ? static_cast<int>(difficulty.value()) : -1;
    start.open_music = music_set && difficulty ? static_cast<int>(difficulty.value()) : -1;
    return oscilline::run_title(host.value(),
                                std::move(start),
                                effective,
                                stored,
                                session_language,
                                flags,
                                cli_no_disc_assets);
}
