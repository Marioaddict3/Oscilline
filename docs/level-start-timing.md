# Level-start timing

Behavioral reference: the [European World of Longplays recording](https://archive.org/details/psx-longplay-vib-ribbon-eu).
No recordings, disc audio, screenshots, or game code are committed.

The start cue was located by matching GAME VAG 083 at 16000 Hz. The CD music was
located independently by matching seconds 1–6 of each of the disc's audio tracks
against the recording, then subtracting that one-second template offset. Track
positions are measured from CUE INDEX 01, excluding each track's pregap.

| Course | Start cue (s) | Music INDEX 01 (s) | Gap (s) |
| --- | ---: | ---: | ---: |
| 1 | 185.296 | 193.287 | 7.991 |
| 2 | 352.313 | 360.331 | 8.018 |
| 3 | 547.485 | 555.488 | 8.003 |
| 4 | 707.550 | 715.567 | 8.017 |
| 5 | 908.288 | 916.293 | 8.005 |
| 6 | 1083.324 | 1091.342 | 8.018 |

Music correlation scores range from 0.946 to 0.990. The approximately one-frame
spread supports an eight-second prelude. Before/after frames show a round
caption early in that interval and the camera settling before music begins.
These measurements verify cue-to-track latency, not the complete zoom path.
The built-in pan is still an approximation. The eight-second prelude is still
the gap from the start cue to the music. When the course script has section
spans and no obstacles, the disc intro is the transition that ends at section 1
on the audio clock (about 8 s into the song on the courses that were watched),
not a spin that fills this prelude. With obstacles, the stage-start pan always
plays and is stretched or compressed so it finishes at least 1 s before the
first obstacle needs input. A later obstacle-free gap of at least 4 s gets the
same variable-length pan. With no spans and no obstacles, the intro is still
`ROAD/TV_SS.ANC` across the prelude at 30 keys per second, and play then holds
`ROAD/S01.ANC`. Custom music with a disc supplies equal section spans instead,
so its stage-start pan is the transition into the first slice.
`--builtin-camera` keeps the built-in pan. See [Disc file map](disc-map.md).
The pan in the next paragraph is that built-in camera.

Oscilline posts the round-start cue at entry and holds CD/file playback for
8000 ms. The audio clock reports negative track time during this prelude,
reaches zero when music starts, and then follows consumed audio samples.
`music-start` (SfxId 47, TITLE bank VAG index 17, program 9 tone 7) is the
second cue. The mixer waits 1000 ms (`kMusicStartSfxDelayMs`) after the round-start voice ends before
playing it, during the prelude and before music. That gap is measured on the
SFX output clock, not the music clock. The eight-second prelude, the course
music start, and obstacle timing are unchanged. Missing-device or silent
playback uses the same delayed wall clock. Pausing freezes both the prelude and
the SFX mixer, so the voice and gap resume where they stopped. Retries and
subsequent courses, including a custom chart, each receive a new prelude and
cue chain.

The existing camera pan runs in the final four seconds before track zero, with
the round caption in the first four seconds. Obstacle hit times, chart duration,
and onset-envelope indices remain relative to music; inputs during the prelude
cannot judge obstacles. This avoids shifting the chart or letting music run
silently in the background before gameplay.
