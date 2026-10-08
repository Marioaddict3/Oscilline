# Disc file map

PAL `SCES-02873` is the primary disc. Japanese `SCPS-45469` is also supported (Experimental). The notes below are format facts from the PAL image: how fields are laid out, not what the files contain. Do not add file sizes, hashes, asset counts, directory dumps, or file bytes to this document, and do not commit a disc image.

These serials are the PAL and Japanese (Experimental) discs of Vib-Ribbon. Boot files we accept are `SCES_028.73` and `SCPS_454.69`.

## Image

The data track stores no 150-sector pregap. The primary volume descriptor is at file sector 16. Audio tracks are separate BIN files, and each of those BINs carries its own `INDEX 00` pregap.

The ISO primary volume descriptor's volume space size covers every track, including the CD-DA tracks. It can be larger than the data track. Reads stay inside the data track. A directory record that points past the data track is kept and marked, and extract skips that entry.

## Top level

The top-level directories are `GAME/`, `KIOSK/`, and `TITLE/`. `GAME/` and `KIOSK/` each hold the six language-prefixed PAKs. `TITLE/` holds a single unprefixed `FILES.PAK`. Each of the three has an `AUDIO/` subfolder with one VH/VB pair.

| Prefix | Language |
| --- | --- |
| `01` | Japanese |
| `02` | English |
| `04` | German |
| `08` | Spanish |
| `10` | French |
| `20` | Italian |

`SYSTEM.CNF` uses CRLF. Its keys are `BOOT`, `TCB`, `EVENT`, and `STACK`.

Published executable names remain the boot file, `MAIN_T.EXE` (title), `MAIN_G.EXE` (game), and `MAIN_K.EXE` (tutorial). This note does not add paths for them.

## PAK

Offsets in a PAK are file-absolute. Each entry's name is NUL-terminated. When the character count is a multiple of 4, the terminator plus padding is four null bytes. Entries are 4-byte aligned.

`PIF` shows up inside the PAKs and has no parser yet. `ANC` is the camera-path type; see below.

## TMD

`FIXP` is 0 on this disc. Vertex, normal, and primitive offsets in each object entry are then relative to the object table, which starts at file offset `0x0C` (the byte after the 12-byte header). When `FIXP` is 1 those offsets are file-absolute.

A primitive packet's `ilen` byte counts body words only, not the 4-byte packet header. The packet is `(ilen + 1) * 4` bytes.

Primitives are almost all lines, with a few flat untextured triangles and quads.

## ANC

A camera file is a 6-byte header and then fixed 16-byte records. The header is three little-endian `uint16` fields: magic `0x8000`, a reserved word, and the record count. The file length is `6 + count * 16`. Any other magic is rejected. A count of zero, a count above 4096, or any other length is rejected. When the bytes after the header divide evenly by the count but the stride is not 16, the error names that stride.

Each record is eight little-endian `int16` fields. The order is confirmed: eye x, y, z, target x, y, z, roll, then a field-of-view channel. Roll uses the same 4096-unit turn as ANM rotation. Positive roll is a counter-clockwise image roll around the look axis. The channel is not an angle, and it is not the projection distance. `H` is `kAncProjectionScale / fov` with `kAncProjectionScale` = 250000, so the S01 play key (fov 499) keeps `H` about 501. Reading the channel as `H` directly made the figure too tall on later intro keys (about 85–150).

The second header word (`reserved`) is probably a playback rate in Hz, by analogy with ANM's second header word. 30 fits a long road path and 60 the short title cameras. That reading is medium confidence. The parser stores the word. Playback does not read it.

`--disc-camera` is the default. `--builtin-camera` keeps the built-in camera. When both are passed, the last one wins. The Options `disc_camera` setting is the same choice. Custom music with a loaded disc uses the disc camera when that setting is disc, and the built-in camera when the user picked it. `--no-disc-assets`, and custom music with no disc, have no camera file, so play falls back to the built-in pose.

With the flag, the road files replace that camera. `play_end` does not move it. A course with script sections follows the schedule below. Custom music has no script sections. When a disc is loaded and the camera setting is disc, it splits the track into three equal sections, each at least about 20 s, and uses fewer sections when the track is shorter. The built-in camera is used when the user picked it. Those spans drive this same schedule. Bronze uses course 1's road files, silver course 3's, and gold course 5's. The looping pan always plays at stage start, on every course and on a custom chart. It also plays in an obstacle-free gap of at least 4 s. The 240 keys stretch or compress to fill that gap, and the pan finishes at least 1 s before the next obstacle needs input. A gold custom chart also applies the Gold shift. With no section spans and no obstacles, playback is one `ROAD/TV_SS.ANC` intro from course start at 30 keys per second, then the first key of `ROAD/S01.ANC`. With obstacles, that stage-start pan is fitted to the opening instead, and a later quiet gap of at least 4 s gets the same variable-length spin.

The schedule is in `camera_schedule.cpp`. Section start and end come from the FSL pattern track (a playable segment ends at the next segment, and the last one ends with the audio). The seconds below are PAL audio-head readings of courses 1–5, kept for the tests. Playback does not hard-code them. The ANC timer is `key × 65536`.

- **In-play (high for the files that were watched).** The section's road file starts at key 0 when the section starts and reaches its last key when the section ends. S01 is 26 keys. S02 and B01 are 780 keys. After the section ends, that last key is held until the transition starts.
- **Transition (high).** `TV_SS` plays into an S camera (S01 or S02), `TV_BB` into a B camera, and `TV_BS` when the previous and next sections are different families. Each file is 240 keys. At 30 per second that is 8.0 s. The course intro is that transition into section 1. The watched windows, with the spin ending as the next section starts, were about 7.6–8.4 s. A transition's tilt is the unwrapped road angle, the same circle as the old intro: about 0° at the first key through −360° at the last. No `ROAD/` file sets the roll channel. With no obstacles, that spin is the watched 8.0 s clip and it ends exactly when the next section starts. With obstacles, the looping pan always plays at stage start, including on courses 2–6 and on a custom chart. It also plays in an obstacle-free gap of at least 4 s. The same 240 keys stretch or compress to fill the gap, and the pan finishes at least 1 s before the next obstacle needs input, leaving the play camera in place before the player has to act. A shorter gap skips the spin, holds the previous section's last key, and blends into the next play key over 400 ms (or the whole gap, when that is shorter) so the handoff does not snap. Reference playthroughs show a held side view through obstacle runs, with the orbit reserved for the start of a stage and for quiet stretches.
- **Which file (courses 1–4 high, course 5 mixed, course 6 a placeholder).** Three sections each: course 1 S01, S01, S01; course 2 S01, S02, S01; course 3 S02, S01, S02; course 4 B01, B01, B01. Course 5 section 1 is S02, then `TV_BS` was seen starting as that section ended. Sections 2 and 3 were not seen; the table guesses B01 then S02, which is what makes that boundary `TV_BS`. Course 6 was not yet observed. Its placeholder is S02, S01, S02, marked as a guess. A missing file holds S01 at key 0, which is the previous play pose.

| Course | Section 1 | Section 2 | Section 3 | Notes |
| --- | --- | --- | --- | --- |
| 1 | S01 | S01 | S01 | Intro and both later transitions are TV_SS. |
| 2 | S01 | S02 | S01 | TV_SS throughout. |
| 3 | S02 | S01 | S02 | TV_SS throughout. |
| 4 | B01 | B01 | B01 | Intro and both later transitions are TV_BB. |
| 5 | S02 | B01 | S02 | Section 1 and the TV_BS into section 2 were seen. Sections 2 and 3, and the TV_BS into section 3, are guesses. |
| 6 | S02 | S01 | S02 | Placeholder, all three guesses. Not observed. |

- **Gold shift (estimate).** On courses 5 and 6 the ribbon moves up and then back down, and the figure moves right, partway through the course. The times and the 512×286 offsets are `kGoldRibbonRiseStartMs`, `kGoldRibbonRisePs`, `kGoldFigureShiftStartMs`, `kGoldFigureShiftPs`, and the durations next to them. They are estimates from video, easy to tune, not a RAM measurement. The ribbon offset is applied and then removed. The figure offset stays once the move finishes. A gold custom chart on the disc camera uses the same shift. Other custom charts do not.

`ROAD/B01.ANC` is course 4's in-play camera, stretched across each of that course's sections. It is not a song-length dolly, and its tilt is the road angle of the sampled key, not the look vector's pitch.

Projection, in that 512-wide frame: screen x is not mirrored (camera-right is down × forward, as on the GTE), the center is `(256, 124)`, and `H` is `250000 / fov`:

```
ps_x = 256 + (camera_x × H) / z
ps_y = 124 + (camera_y × H) / z
```

The logical view stretches a 512×286 frame onto 640×480. Emulator captures of the course screen are 512×286, with the HUD gauge below line 240, and the play ribbon sits on line 191 (0.67 of the height). With S01 the figure lands at x = 256 − 2061 × 499 / 5204 ≈ 58 (0.114 of the width) and y ≈ 191. World +Y is down. Camera-right is down × forward. The ribbon direction is world +Z, which S01 (looking along −X) sees level with +Z to screen-right. Its screen angle, after that basis and the roll channel, is the tilt. Positive roll is one counter-clockwise turn per 4096 units. No `ROAD/` file sets it. Pitch and yaw do not tilt a level road by themselves; they do once the eye has yawed off the play view, which is the intro circle. The angle is unwrapped so the circle accumulates −360° instead of jumping at ±180.

**Play anchor (high).** The static play pose projects the world origin. That is the point S01 puts at x ≈ 0.114 and y ≈ 0.668.

**Intro anchor (assumption).** The origin is not the character while the eye circles. Projecting it through the middle of TV_SS drops the figure off the bottom and inflates scale to about 3.9×, and the real figure stays near mid-height at roughly play size. While the unwrapped road angle is 90° or more from level, the figure is placed at the horizontal center with its feet at the play ribbon height (about 0.67 of the height; PAL captures put the feet near 0.63–0.74 there). Zoom there is `(fov / look distance)` divided by the play camera's pixels per world unit, using the raw fov channel rather than `H`, clamped to 0.8–1.3. Inside 90° of level, that placement eases back to the origin when the origin is still in the playfield (about 15–78% of the height), or to the play pose when it is not. A dolly aimed at the origin (the origin stays on the look ray and in the playfield, and the ribbon is level) keeps its scale, so a real push-in is not clamped. The character's world position during the orbit is not known beyond this. The Gold shift is a separate screen offset, estimated above.

The ribbon and the obstacles scale and rotate around the figure anchor. A projected figure does not take that 2D tilt or a second zoom. A placeholder figure stays upright and scales with the camera. `TITLE/CAM/` holds menu cameras.

**Course line (high).** Ribbon, obstacles, the figure, and damage strokes use a 1.0 logical-pixel core and 0.5 px of feather on each side. A 1 px line in the 512×286 frame, shown 4:3, is about 1.25 logical px wide and 1.68 tall on the 640×480 view (`480/286` ≈ 1.678). The previous 2 px core plus 1 px feather laid about twice that ink. Text, the title, and the progress arc stay on the 2 px stroke. There is no glow. Packet colors stay, including gray `0xB2B2B2`. The ribbon is that one line. Tick marks every 24 px and the vertical hit marker are off unless `--ribbon-guides` is set.

**Figure polygons (high).** Flat unlit triangles (mode `0x21`) in the play models are black (`0x000000` or `0x010000`) and are occluders. The course figure fills them in packet color. A quad is two triangles, `(0,1,2)` and `(0,2,3)`. Lines and fills are drawn far to near in one list. In the side view, depth is the negated view-space z, and view-space z is model x, so a larger model x is nearer. `tmd_wireframe` still emits polygon edges for the viewer and the HUD.

**Figure through the disc camera (orientation high, mid-intro scale medium).** When the disc camera is on and a model is drawn, the posed segments and triangles go through the same ANC eye, target, and projection distance `H` as the road, before the side-view rotation. The basis is `right = normalize(cross((0,1,0), forward))` and `down = cross(forward, right)`, with no roll and no mirror. In the 512×286 frame, `x = 256 + H * right / z` and `y = 124 + H * down / z`, then the frame stretches by `640/512` and `480/286`, the same mapping as the road. The figure is then widened about its own center to the vertical scale, so it keeps square-pixel proportions at every aspect ratio (at 4:3 the frame alone would make it about 25% narrower). A vertical model line stays screen-vertical, including when the roll channel is set and when the pitch is steep. The figure stands at the world origin, feet at `y = 0`, facing `+Z`, at 0.71 world units per model unit. That scale is a 49-row rabbit in the 286-row frame; the original measures 49 rows, p10–p90 44–54. At the held play pose the projected feet meet the side-view feet. Mid-intro scale and the anchor between keys about 105–135 are medium confidence: 49 rows times `480/286` is about 82 logical px, and the side-view height is 88, about 6.6% apart.

## Ribbon vibration

Measured black-box on Bronze course 1 with no input, through rabbit, frog, and worm. PS pixels are the 512×286 frame. The logical view is 640×480: horizontal ×1.25, vertical ×1.678. The rounded logical sizes below are that conversion. Geometry is high confidence. The worm peak, the raised floor, and the two times are medium: the emulator course clock ran about 3.2× slow, and the times are already converted to course time.

The ribbon is not one rigid offset. Break points are the left screen edge, the right screen edge, and both attachment points of every on-screen obstacle, where its outline leaves and rejoins the ribbon. The figure does not split the ribbon. Each piece between neighboring breaks is one straight line (no interior vertices). Each break takes an independent vertical offset, uniform in [−A, +A], freshly random every frame. Frame-to-frame correlation was about 0.2, so the draw treats it as white noise and does not use an oscillation frequency. The picture holds one sample for 20 ms (50 Hz).

Obstacle outline vertices jitter independently by up to A in x and in y. An obstacle's ends stay on its break points, so the line stays connected. The figure is not displaced as a whole. Her line vertices jitter by up to A − A0, and they are still when A is A0.

A hit below is a miss. h is misses since the last form change.

| Quantity | PS px | Logical y | Confidence |
| --- | --- | --- | --- |
| A0, course start and any form change | 1 | 1.7 | high |
| Peak, rabbit or frog, h = 1–3 | 6 | 10 | high |
| Peak, rabbit or frog, h ≥ 4 | 9.5 | 16 | high |
| Peak, worm, any h | 3.5 | 6 | medium |
| Floor after the fade, h ≤ 3 | 1 (A0) | 1.7 | high |
| Floor after the fade, h ≥ 4 | 2.5 | 4 | medium |
| Hold near the peak | 1.6 s | | medium |
| Linear fade from the peak to the floor | 0.8 s | | medium |

A form change, drop or promotion, resets A to A0 and h to 0 immediately. A clear does not change A. The onset envelope does not translate the stage. Intro tilt and the disc camera still move the ribbon and the obstacles around the figure.

## ANM

Frame-table entries multiplied by 2 are byte offsets from the start of the file. The second header field is unused; observed values include 1, 10, 15, 20, 30, and 60. The first header field is `0x8000`.

## VH / VB

The program-count field is the actual count, not count minus one. Layout:

1. A `0x20`-byte header.
2. A 128-entry program table, 16 bytes each (`0x800` bytes). This table is always present, whatever the program count.
3. `program_count` tone tables, `0x200` bytes each.
4. The 256-entry VAG size table. Each entry is a `uint16`, and the value is a size in 8-byte units. The table ends at the end of the VH file.

The sum of each VAG size times 8 equals the VB length.

Each program is a kit of tone slots (16 per program). A sample is chosen by program index and tone index. The tone's `vag` field is the zero-based slot in this size table. An event row uses that program and tone, or the VAG index itself when a capture names the slot and not a program. The tone's shift byte is the fine tune; center, note min, and note max are the note range; pan and mode are stored on both the program and the tone. Those fields do not state the playback rate. The mixer default is 11025 Hz and each event row can override it. `GAME/AUDIO/PSJ_SE` is the game bank when that pair is present; the wheels play from it. Cursor move, title confirm, menu select, the first wheel CROSS key-on, menu back, and the wheel announcements play at 9991 Hz. That rate is SPU pitch `0x03A0`: `44100 * 0x03A0 / 4096`. The main-menu loop, menu-select follow, and missed-obstacle cues stay at 11025 Hz. Round start and level complete stay at 16000 Hz. Menu back is PSJ_SE VAG index 20. Game over plays at an estimated 16000 Hz, the same in-between rate as round start and level complete; that rate is not a confirmed capture match. Difficulty-confirm-b (PSJ_SE VAG index 83, medium confidence) keeps the 11025 Hz default because that capture did not confirm a rate. See [SFX comparison notes](sfx-comparison.md). ADPCM flag bit 2 marks a loop start and bit 0 a loop end. Bit 1 set with the end flag repeats; an end flag without it stops the voice.

## Course file

The published FSL section offsets match the PAL file. Obstacle values are 32-bit. The parser still walks the header counts rather than hard-coding the offsets.

| Section | Offset |
| --- | --- |
| Fixed patterns | `0x40` |
| Distribution patterns | `0xEC` |
| Control segments | `0xDB8` |
| Pattern segments | `0x56CC` |
| Event segments | `0x5EC8` |
| Track index | `0x68D4` |

## Super form

`CHARA/SUPER` is the super model (`Slot::FormUpgraded`, high, by folder name). It has `MODEL.TMD` and the same clip stems as `CHARA/PEELOO`: `N00_SKIP`, `N01_J`, `N01_J_F`, `N02_H`, `N03_L`, `N03_L_F`, `N04_W`, `N05_JH`, `N06_JL`, `N07_JW`, `N08_HL`, `N09_HW`, `N10_LW`. Clip choice uses the rabbit's stems. Every stem is high, including the pairs JL, JW, HL, HW, and LW and the `_F` whiffs. The model is a crowned, winged figure (private render). Those bytes are not in this tree.

The ladder is worm → frog → rabbit → super. Eighteen consecutive clears raise one form on every rung. Confidence: high for rabbit → super, from footage of the original. Clears 1 through 17 each light one ring slot. The 18th clear promotes, and no 18th dot is drawn. A miss empties the streak.

Super drops to rabbit after the rabbit miss limit (10). A separate super miss count was not measured. The documented scoring model doubles the capped clear-streak award in Super; other forms share a multiplier of one. See [scoring.md](scoring.md). The hit that promotes is still scored as the form it left. Round 2 carries the form, the damage, and the clear run, including super. Out is not carried.

The streak ring is drawn, and it is on by default. Confidence: high. H is the 4:3 game-area height. The ring is a near-circle centered on Rabbit, `0.104 H` above the ribbon line, with radii `0.133 H` by `0.145 H`. There are 17 slots at `360/17` degrees (about 21.18). Slot 0 is at 12 o'clock. Each next dot goes counter-clockwise on screen. One pink dot (RGB 208, 88, 176) per consecutive clear. The dot is about 2 PS px. A new dot appears in its slot. A PS px is one pixel of the 512×286 PAL frame. Logical x is `1.25 × PS px` and logical y is `1.678 × PS px`. A miss clears the ring. Super form leaves the ring empty: there is no super streak display.

On the 18th clear the model switches from rabbit to super (`CHARA/SUPER`) on that same frame. Confidence: high. There is no morph, flash, scale change, or spin. The run pose and animation phase stay, and the clear clip plays on the super model. The same frame turns each of the 17 dots into a radial dash on its slot angle. The center is fixed at the figure's screen position at t = 0. With `u = t / 0.28 s`, the radius is `R0 * (1 + 3.8 * (1 - (1 - u)^2))`, where `R0` is the ring radius on that angle. Dash length is `0.16 * r`, about `0.02 H` growing to about `0.10 H`. All dashes vanish together at t = 0.28 s, with no fade. Dash color is RGB 232, 170, 214. Confidence: low. The stroke uses the ribbon's line width. Nothing else is added: no screen flash, popup text, score popup, shake, or hit-stop. The ribbon and the obstacles keep scrolling. Drops still use the scribble burst.

The promoting clear plays the normal clear cue for that obstacle and the promotion cue on the same frame. Confidence: high. The cue is the GAME bank (`PSJ_SE`) VAG index 19 (program 1, tone 14) at exactly 11025 Hz, one-shot, with no music ducking. `kSuperTransformSfxId` is that row. It is resolved from the disc like the other cues, and it is skipped when the disc is absent. There is no transform clip.

The evolution meter fill steps evenly from super (0) through rabbit (1/3) and frog (2/3) to worm and out (1). Confidence: medium. The frames were not measured for four forms. The direction matches the previous meter, where a lower form filled more of the strip.

