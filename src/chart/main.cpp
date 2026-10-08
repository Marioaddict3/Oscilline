// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Prints one disc course chart. --attacks prints attack times.

// Prints the chart a disc course would play: one line per obstacle with the
// hit time in milliseconds on the CD clock and the obstacle id. Used to compare
// hit times against captures of the original without opening a window.

#include "oscilline/audio/cdda.hpp"
#include "oscilline/course/attack.hpp"
#include "oscilline/course/load.hpp"
#include "oscilline/course/mapping.hpp"
#include "oscilline/disc/archive.hpp"
#include "oscilline/version.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int usage() {
    std::cerr << "usage: oscilline-chart [--attacks] <disc.cue> <course 1-6>\n"
              << "  prints \"hit_ms obstacle\" per obstacle; --attacks prints the picked attack\n"
              << "  times (ms, evaluation stamps) before the lead-in and audio-end cuts instead\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    bool attacks = false;
    std::string disc;
    int course = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--version") {
            std::cout << "oscilline-chart " << oscilline::version_string() << '\n';
            return 0;
        }
        if (arg == "--attacks") {
            attacks = true;
        } else if (disc.empty()) {
            disc = std::string(arg);
        } else if (course == 0) {
            course = std::atoi(argv[i]);
        } else {
            return usage();
        }
    }
    if (disc.empty() || course < 1 || course > oscilline::kPlayableCourses) {
        return usage();
    }
    auto mounted = oscilline::mount_disc(disc);
    if (!mounted) {
        std::cerr << "oscilline-chart: " << mounted.error() << '\n';
        return 1;
    }
    auto pak = oscilline::load_language_pak(mounted.value().volume);
    if (!pak) {
        std::cerr << "oscilline-chart: " << pak.error() << '\n';
        return 1;
    }
    auto script = oscilline::load_course_script(pak.value());
    if (!script) {
        std::cerr << "oscilline-chart: " << script.error() << '\n';
        return 1;
    }
    auto track = oscilline::read_cdda_track(mounted.value().image, course + 1);
    if (!track) {
        std::cerr << "oscilline-chart: " << track.error() << '\n';
        return 1;
    }
    const std::vector<float> emphasis =
        oscilline::attack_emphasis(track.value().interleaved, track.value().frames);
    std::int32_t audio_ms = oscilline::cdda_duration_ms(track.value().frames);
    const std::int32_t audible =
        oscilline::attack_audible_end_ms(track.value().interleaved, track.value().frames);
    if (audible > 0 && audible < audio_ms) {
        audio_ms = audible;
    }
    if (attacks) {
        auto picked =
            oscilline::course_attack_ms(script.value(), course - 1, emphasis, {}, audio_ms);
        if (!picked) {
            std::cerr << "oscilline-chart: " << picked.error() << '\n';
            return 1;
        }
        for (const double ms : picked.value()) {
            std::cout << ms << '\n';
        }
        return 0;
    }
    oscilline::CourseMapOptions options;
    options.emphasis = emphasis;
    auto timeline = oscilline::build_course(script.value(), course - 1, audio_ms, options);
    if (!timeline) {
        std::cerr << "oscilline-chart: " << timeline.error() << '\n';
        return 1;
    }
    for (const oscilline::CourseEvent& event : timeline.value().events) {
        std::cout << event.hit_ms << ' ' << static_cast<int>(event.obstacle) << '\n';
    }
    return 0;
}
