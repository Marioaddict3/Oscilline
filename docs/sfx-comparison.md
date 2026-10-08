# SFX comparison notes

This is a behavioral comparison, not a reconstruction of game code. No BIOS,
disc data, decoded audio, recordings, or screenshots are in the repository.
Tests use synthetic fixtures. Sample identifiers refer to the user's disc at
runtime and do not embed its contents.

## Sources and method

- The European disc, identified by DuckStation as SCES-02873.
- Direct DuckStation 0.1-12070 recordings with the interpreter CPU, the software
  renderer, and DuckStation's built-in audio and video capture, booted normally
  through the menus into a Bronze course.
- [World of Longplays Japanese recording by ScHlAuChi](https://longplays.org/infusions/longplays/longplays.php?cat_id=21&longplay_id=13571),
  [original submission and media link](https://longplays.org/infusions/forum/viewthread.php?pid=30824&thread_id=12751).
  The Japanese recording is corroboration, not PAL verification.
- [World of Longplays European recording](https://archive.org/details/psx-longplay-vib-ribbon-eu),
  uploaded by World of Longplays; original Matroska file
  `PSX_Longplay_-_Vib-Ribbon_-_EU.mkv`.

Decoded samples were compared with mono recording audio using normalized
cross-correlation. The coarse search resampled both to 2205 Hz and tried 11025,
16000, 16537, and 22050 Hz. Candidate event frames were inspected before and
after peaks. These scores are screening evidence, not probabilities: related
samples, music, and overlapping effects produce misleading peaks. In particular,
several success samples correlate with the same miss/lifecycle moments; those
peaks do not establish a new mapping.

## Results

Recording times below are seconds from the start of the named recording. VAG
identifiers are one-based; program and tone indices are zero-based.

| Event | GAME program/tone | VAG | Evidence | Status |
| --- | --- | --- | --- | --- |
| Loop success | 1/2 | 006 | PAL loop successes at 212.807 and 222.151, coarse scores 0.787/0.758; full-resolution scores 0.746/0.786 at 11025 Hz. JP loop frames at 324.305 and 429.944 corroborate. | High; 11025 Hz confirmed; default playback |
| Wave success | 1/3 | 007 | PAL wave successes at 203.201 and 1039.193, coarse scores 0.780/0.576; full-resolution scores 0.704/0.606 at 11025 Hz. JP wave frames at 314.576 and 483.533 corroborate. | High; 11025 Hz confirmed; default playback |
| Block+loop success | 1/5 | 009 | PAL paired successes at 1008.928 and 1240.334, coarse scores 0.581/0.874; full-resolution scores 0.659/0.815 at 11025 Hz. JP paired frames at 1105.520 and 1202.183 corroborate. | High; 11025 Hz confirmed; default playback |
| Block+pit success | 1/4 | 008 | PAL paired shapes at 948.621 and 955.714; 80 ms prefix scores 0.614/0.463 at 11025 Hz. The PAL tutorial at 134.270 also shows block+pit button prompts. | High; 11025 Hz confirmed; default playback |
| Block+wave success | 1/6 | 010 | PAL paired successes at 1042.308 and 1212.019; prefix scores 0.627/0.510 at 11025 Hz. JP success animation at 1146.355 and two direct PAL Auto-mode passes corroborate. | High; 11025 Hz confirmed; default playback |
| Pit+loop success | 1/7 | 011 | PAL paired successes at 1187.100 and 1066.906; prefix scores 0.503/0.578 at 11025 Hz. JP paired moments at 1341.236 and 1288.550 corroborate. | High; 11025 Hz confirmed; default playback |
| Pit+wave success | 1/8 | 012 | PAL paired successes at 1213.867 and 1216.721; raw prefix scores 0.535/0.411, CD-music-adjusted scores 0.723/0.823 at 11025 Hz. JP success at 1320.694 corroborates. | High; 11025 Hz confirmed; default playback |
| Loop+wave success | 1/9 | 013 | PAL paired successes at 921.813 and 923.400; prefix scores 0.644/0.543 at 11025 Hz. JP paired moments at 1035.901 and 1025.906 corroborate. | High; 11025 Hz confirmed; default playback |
| Round start | 0/11 | 083 | Direct PAL course-start transition at recording time 64.728: scores 0.959/0.993 at 16000 Hz in two captures; PAL longplay matches reach 0.990; JP reaches 0.997 | High; 16000 Hz confirmed |
| Level complete | 0/13 | 003 | PAL longplay Clear transitions at 514.588, 870.829, and 1244.594: coarse scores 0.940/0.940/0.938 at 16000 Hz; full-cue scores 0.759/0.759/0.830 | High; rate corrected to 16000 Hz |

Full-resolution success comparisons used the complete sample and recording at
11025 Hz, without the coarse search's downsampling. Alternative source rates
10500, 11500, and 16000 Hz scored at most 0.249 for these same events, compared
with 0.606–0.815 at 11025 Hz. Before/after frames show the corresponding obstacle
traversal and success animation. These repeated PAL observations establish the
three success identities and rates, so they play by default.

The level-complete comparison uses the entire cue. Alternative rates 11025,
15800, 16200, and 16537 Hz score below 0.10 at all three clear transitions,
compared with 0.759–0.830 at 16000 Hz. A finer direct PAL round-start search from
15800 to 16200 Hz in 10 Hz steps also selects 16000 Hz.

All ten obstacle-success cues now have confirmed rates and play by default.

The remaining paired samples were also screened at full 11025 Hz resolution.
For the event-local check, each candidate's first 80 ms was compared against the
recording within ±20 ms of its full-sample peak, and against all other success
samples at that event. The expected sample is distinctive: its prefix scores
0.463–0.644, while the strongest competing success sample scores at most 0.289
at those PAL events. Tested alternative source rates 10000, 10500, 11500, 12000,
16000, and 22050 Hz score at most 0.308. A finer 10800–11200 Hz sweep in 25 Hz steps also selects 11025 Hz at the
strongest PAL event for each of these five samples. Short-prefix scores supplement the
complete-sample comparison and inspected animations; they are not used alone.
Pit+wave initially stayed provisional because its second raw PAL prefix peak
at 1216.721 scores only 0.411. The follow-up below establishes the repeated
success after accounting for the CD music. Peaks at menu/voice moments were
not used as paired successes.

### Pit+wave follow-up

In the European recording, frames at 1213.8–1214.8 and
1216.6–1217.8 show two distinct pit+wave obstacles, the traversal animation,
and the figure returning to the ribbon. The second event is not a menu,
miss, or lifecycle transition.

The final course's background matches the disc's Track 7, excluding its
INDEX 00 pregap. A three-second music window scores 0.948. Independent local
alignment around each success, excluding the effect itself, scores 0.982 and
0.979 and selects the same alignment to the nearest 11025 Hz sample.

To account for overlapping music without assuming a fixed ducking gain,
project the recording window and each candidate PCM template onto the
orthogonal complement of the aligned CD music plus a constant offset. Then
correlate those residuals. This is a two-variable linear nuisance fit, not a
fit of the candidate sound to the recording. Use an 80 ms source prefix and
search ±30 samples around the independently located raw cue peak. Repeat for
every competing success sample and alternative source rate.

| PAL event (s) | Raw prefix | Music-adjusted prefix | Strongest competing success | Whole-sample adjusted |
| --- | ---: | ---: | ---: | ---: |
| 1213.867 | 0.535 | 0.723 | 0.317 | 0.636 |
| 1216.721 | 0.411 | 0.823 | 0.328 | 0.701 |

Alternative rates 10000, 10500, 11500, 12000, 16000, and 22050 Hz reach at most
0.203 after the same music adjustment. Both raw and adjusted 10800–11200 Hz
sweeps in 25 Hz steps select 11025 Hz independently at both events. Combined
with the two inspected PAL successes and Japanese corroboration, this confirms
GAME VAG 012 at 11025 Hz and promotes pit+wave to default playback. Scores
remain comparison measures, not probabilities. No recording or disc-derived
PCM is included in the repository.

The additional PAL captures used the Play your own CD menu's Auto mode with
audio CD tracks and capped fast-forward. Both block+wave peaks align
with the paired obstacle animation; the Japanese frame also shows a success
animation. These were initially candidate evidence; the two normal-play PAL longplay
events above now corroborate the block+wave identity and rate.
A third PAL start transition also correlates with VAG 083 at 16000 Hz (0.968).

Repeated unsuccessful manual loop inputs were not counted as loop successes.
A further frame-step sweep of R1 around the first Bronze loop, including a
no-input control with CD audio muted, still did not produce a clean success.
The button does trigger an animation, but these misses do not verify a success
mapping. The PAL confirmations above come from the European longplay, not those
manual trials. High-scoring shared miss/voice peaks were rejected. A peak at
613.693 for VAG 007 coincides with a different paired shape, so it was not used
as wave-success evidence.

## SPU key-on follow-up

A black-box SPU pass recorded program and tone indices only. Rates are in
Hz. High means the same key-on repeated. These rows do not replace the ten
success cues at 11025 Hz, or round start and level complete at 16000 Hz.

| Event | Bank | Program/tone | Rate | Confidence | Playback |
| --- | --- | --- | --- | --- | --- |
| Miss / crash | GAME | 1/10 | 11025 | High | Default. Same row as every missed-obstacle cue. Wrong press or no press into an obstacle. |
| Empty press | GAME | 1/11 | 11025 | High | Default. A press while no obstacle window is open. |
| Alternate whiff | GAME | 1/15 | 12371 | Medium | Gated. Not played together with the empty-press whiff. |
| Press after crash | GAME | 1/12 | 12371 | Medium | Gated. Posted on the first press after a miss only when `--sfx-experimental` resolves the sample. |
| Title confirm | TITLE | 0/0 | 9991 | High | Default. Confirm on the title list or the language list. SPU pitch `0x03A0`. |
| Menu select | TITLE | 0/1 | 9991 | High | Default. The earlier 11025 Hz rate is replaced. |
| Menu-select follow | PSJ_SE | 2/0 | 11025 | High | Default. The menu-loop sample. PSJ_SE is the GAME/AUDIO bank. |
| Wheel CROSS | TITLE then PSJ_SE | 0/1, then VAG index 83 | 9991, then unconfirmed | High, then medium | The first key-on stays program 0 tone 1 (TITLE VAG 19 and PSJ_SE VAG 16 are the same sample). difficulty-confirm-b is the round-start sample (PSJ_SE 0/11), not re-observed, and gated. |
| Menu back | PSJ_SE | VAG index 20 (6/0) | 9991 | High | Mapped, but not played: the original plays it for triangle on the wheel. Oscilline plays the Back row's own confirm sound for Escape and triangle instead, so both ways out of a page sound the same. |
| Menu move | TITLE | 0/6 | 9991 | High | Default. One voice. An edge, or left/right on a vertical list, is silent. |

`--sfx-test` prints each of these rows. Medium rows say `gated`. A VAG-index row prints `vag` instead of program and tone.

## Wheel announcements

A pass on the PAL disc recorded the wheel lines. Parts of one line start when the previous part ends. They are never keyed together. Every part is PSJ_SE at 9991 Hz and high confidence. Program and tone are zero-based.

| Line | Parts |
| --- | --- |
| Vib-ribbon disc | 15/0, then 15/1, then 7/14 |
| Own CD | 7/13, then 15/3, then 7/14 |
| Options | 7/7 |
| Back | 6/0 |
| Bronze | 7/2 |
| Silver | 7/1 |
| Gold | 7/0 |
| High scores | 8/0 |

The title list and the language list use the cursor blip, not these lines. The difficulty wheel speaks bronze, silver, gold, high scores, and back when the cursor lands on that row. High scores use up and down, stop at the ends, and play 7/2, 7/1, or 7/0 for bronze, silver, and gold. Back (Escape, the pad Back button, or triangle) leaves for the wheel and plays 6/0. The game-wheel lines (vib-ribbon disc, own CD, options) are in the event table. The title list is not that wheel, so those three lines are not posted from it. Back is posted from the difficulty wheel and from high scores.

## Remaining comparisons

The rabbit → super cue is PSJ_SE VAG index 19 (program 1 tone 14) at 11025 Hz, one-shot, with no music duck. Confidence: high, from footage of the original. Confidence in the dash color of that burst is low. Other form changes still need event-specific captures. Game-over identity rests on earlier
captures; attempts to isolate a new game-over event did not yield a verified
VAG 002 event and rate match. Playback uses 16000 Hz as an estimate, the
same in-between rate as round start and level complete, and the row stays
unconfirmed. The crash key-on is game program 1 tone 10 at 11025 Hz. Pitch across character forms
was not compared separately.

Level-start measurements and clock behavior are recorded in
[level-start timing](level-start-timing.md).

`music-start` (SfxId 47) is TITLE VAG index 17, the viewer row that was
unidentified 115 (`title vag 17 9/7`). It plays 1000 ms
(`kMusicStartSfxDelayMs`) after the round-start voice ends, during the prelude and before music. The rate is the 11025 Hz
unidentified-row audition rate and stays unconfirmed. Confidence is high, so
it plays without `--sfx-experimental`. The course-music clock is unchanged.
