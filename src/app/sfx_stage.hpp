// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Cue stage used by play and by the sfx tests.

#pragma once

#include "oscilline/audio/sfx.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/present/cdda_out.hpp"
#include "oscilline/present/host.hpp"
#include "oscilline/present/sfx_out.hpp"
#include "oscilline/settings.hpp"

#include <filesystem>
#include <vector>

namespace oscilline {

class IsoVolume;

// One mixer for a screen. Disc banks are optional. With none loaded, every
// event plays a placeholder. The music clock is only touched by set_music_gain
// on the existing CD-DA / custom-music output.
class SfxStage {
  public:
    explicit SfxStage(bool experimental);

    void load(const IsoVolume& volume);
    void load_path(const std::filesystem::path& disc);

    void post(SfxId id);
    void post_sequence(std::span<const SfxId> ids, int inter_part_delay_ms = 0);
    void pump();
    void set_paused(bool paused);
    void discard();
    // Music scales the course stream. SFX scales the mixer. Both take effect on the next mix.
    void set_playback(const PlaybackPrefs& prefs);
    void apply_music_gain(CddaOutput& audio);

    [[nodiscard]] SfxMixer& mixer();

  private:
    SfxLibrary library_;
    SfxMixer mixer_;
    SfxOutput output_;
    int applied_gain_ = 32767;
    int music_q15_ = 32767;
};

// Plays each mapped event once, then returns. `--frames` still ends the loop.
int run_sfx_test(Host& host, const std::filesystem::path& disc, bool experimental);

// Posts the clear, miss, and form-change notes for hits just resolved.
// Miss rows are the crash key-on (GAME program 1 tone 10).
// The rabbit → super rung calls `post_super_transform_cue` instead of
// the generic form-change cue. The clear for that obstacle is still posted.
void post_play_hits(SfxStage& sfx, const std::vector<PlayHit>& hits);

// Dedicated rabbit → super cue, on the same frame as the clear. A negative
// id plays nothing. The mixer resolves `sfx_id` from the disc and stays silent
// when that sample is missing. `clip_id` is unused; there is no transform clip.
void post_super_transform_cue(SfxStage& sfx, int sfx_id, int clip_id);

// Empty press plays the high-confidence whiff. The alternate whiff stays in
// the table. The post-crash press is posted only when its sample resolves.
void post_play_edges(SfxStage& sfx, const PlayAdvanceResult& step);

} // namespace oscilline
