// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Posts gameplay and menu cues onto the mixer.

#include "sfx_stage.hpp"

#include "oscilline/course/obstacle.hpp"
#include "oscilline/disc/archive.hpp"

#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace oscilline {
namespace {

const char* confidence_label(SfxConfidence confidence) {
    switch (confidence) {
    case SfxConfidence::High:
        return "high";
    case SfxConfidence::Medium:
        return "medium";
    case SfxConfidence::Low:
        return "low";
    case SfxConfidence::None:
        return "none";
    }
    return "none";
}

void post_hits(SfxStage& sfx, const std::vector<PlayHit>& hits) {
    for (const PlayHit& hit : hits) {
        if (!obstacle_known(hit.obstacle)) {
            continue;
        }
        if (hit.judgment == Judgment::Miss) {
            sfx.post(sfx_missed(hit.obstacle));
        } else if (hit.judgment == Judgment::Perfect || hit.judgment == Judgment::Good) {
            sfx.post(sfx_cleared(hit.obstacle));
        }
        if (hit.super_transform) {
            post_super_transform_cue(sfx, kSuperTransformSfxId, kSuperTransformClipId);
        } else if (hit.form_changed) {
            sfx.post(SfxId::FormChange);
        }
    }
}

} // namespace

SfxStage::SfxStage(bool experimental)
    : mixer_(&library_,
             std::span<const SfxMapEntry>(kSfxMap, static_cast<std::size_t>(SfxId::Count)),
             SfxConfig{experimental, SfxResample::Linear, 32767}),
      output_(SfxOutput::open(mixer_)) {}

void SfxStage::load(const IsoVolume& volume) {
    library_ = load_sfx_library(volume);
    mixer_.set_library(&library_);
}

void SfxStage::load_path(const std::filesystem::path& disc) {
    if (disc.empty()) {
        library_ = SfxLibrary{};
        mixer_.set_library(&library_);
        return;
    }
    auto mounted = mount_disc(disc);
    if (!mounted) {
        std::cerr << "oscilline: sfx disc: " << mounted.error() << '\n';
        library_ = SfxLibrary{};
        mixer_.set_library(&library_);
        return;
    }
    load(mounted.value().volume);
}

void SfxStage::post(SfxId id) {
    mixer_.post(id);
}

void SfxStage::post_sequence(std::span<const SfxId> ids, int inter_part_delay_ms) {
    mixer_.post_sequence(ids, inter_part_delay_ms);
}

void SfxStage::pump() {
    output_.pump();
}

void SfxStage::set_paused(bool paused) {
    output_.set_paused(paused);
}

void SfxStage::discard() {
    output_.discard();
}

void SfxStage::set_playback(const PlaybackPrefs& prefs) {
    music_q15_ = volume_percent_to_q15(prefs.music_volume);
    mixer_.set_sfx_gain_q15(volume_percent_to_q15(prefs.sfx_volume));
}

void SfxStage::apply_music_gain(CddaOutput& audio) {
    const int duck = mixer_.music_gain_q15();
    // User volume rides the existing duck gain. 100% leaves the duck alone.
    int gain = duck;
    if (music_q15_ != 32767) {
        gain = static_cast<int>((static_cast<std::int64_t>(duck) * music_q15_) >> 15);
    }
    if (gain == applied_gain_) {
        return;
    }
    applied_gain_ = gain;
    audio.set_music_gain(gain);
}

SfxMixer& SfxStage::mixer() {
    return mixer_;
}

int run_sfx_test(Host& host, const std::filesystem::path& disc, bool experimental) {
    SfxStage sfx(experimental);
    if (!disc.empty()) {
        sfx.load_path(disc);
    }
    std::cout << "sfx-test: " << static_cast<int>(SfxId::Count) << " events\n";
    int index = 0;
    int hold = 0;
    bool started = false;
    while (host.begin_frame()) {
        (void)host.sim_steps();
        if (!started) {
            started = true;
            hold = 0;
        }
        if (hold == 0) {
            if (index >= static_cast<int>(SfxId::Count)) {
                host.end_frame({}, {});
                break;
            }
            const auto id = static_cast<SfxId>(index);
            const SfxMapEntry& entry = kSfxMap[static_cast<std::size_t>(index)];
            const SfxChoice choice = sfx.mixer().resolve(id);
            std::cout << "sfx-test: " << sfx_name(id);
            if (entry.bank == kSfxBankNone) {
                std::cout << " placeholder";
            } else if (entry.vag != kSfxVagUnset) {
                std::cout << " bank " << static_cast<int>(entry.bank) << " vag "
                          << static_cast<int>(entry.vag);
                if (choice.placeholder) {
                    std::cout << " placeholder";
                }
            } else {
                std::cout << " bank " << static_cast<int>(entry.bank) << " program "
                          << static_cast<int>(entry.program) << " tone "
                          << static_cast<int>(entry.tone);
                if (choice.placeholder) {
                    std::cout << " placeholder";
                }
            }
            std::cout << " rate " << choice.rate_hz;
            if (!entry.rate_confirmed) {
                std::cout << " unconfirmed";
            }
            std::cout << ' ' << confidence_label(entry.confidence);
            if (entry.bank != kSfxBankNone && entry.confidence != SfxConfidence::High) {
                std::cout << " gated";
            }
            const bool music_cue = id == SfxId::MenuLoop || id == SfxId::RoundStart ||
                                   id == SfxId::GameOver || id == SfxId::LevelComplete;
            if (music_cue) {
                std::cout << " (music cue; skipped in sweep)";
            }
            std::cout << '\n';
            if (!music_cue) {
                sfx.post(id);
            }
            ++index;
            hold = 12;
        } else {
            --hold;
        }
        sfx.pump();
        host.end_frame({}, {});
    }
    return 0;
}

void post_super_transform_cue(SfxStage& sfx, int sfx_id, int clip_id) {
    (void)clip_id;
    if (sfx_id < 0 || sfx_id >= static_cast<int>(SfxId::Count)) {
        return;
    }
    sfx.post(static_cast<SfxId>(sfx_id));
}

void post_play_hits(SfxStage& sfx, const std::vector<PlayHit>& hits) {
    post_hits(sfx, hits);
}

void post_play_edges(SfxStage& sfx, const PlayAdvanceResult& step) {
    if (step.whiff) {
        sfx.post(SfxId::Whiff);
    }
    // Tone 15 is an alternate whiff. It is not played with tone 11.
    if (step.post_crash_press && !sfx.mixer().resolve(SfxId::PostCrashPress).placeholder) {
        sfx.post(SfxId::PostCrashPress);
    }
}

} // namespace oscilline
