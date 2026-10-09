# Score coupons

The top HUD shows seven colored coupons. Their values depend on their slot,
rather than behaving like decimal digits. `coupon_value` implements the table in
[Score Coupons](https://vibribbon.fandom.com/wiki/Score_Coupons); the tests check
all 116,280 representable totals and the table's row boundaries. Gameplay coupons
saturate at 116,279. Numeric totals appear on the results screen. Options >
Interface > Score shows the score as a number instead of coupons.

## Awards

A successful obstacle awards the next clear-streak value, capped at 11. Super
(the original game's Queen form) doubles that base. Rabbit, Frog, and Worm have
the same base award. A miss resets this scoring streak. It is independent of the
evolution ring, so promoting a form does not reset it. The promoting obstacle
uses the form before promotion. A two-button obstacle earns one clear award.

The wiki documents accuracy bonuses of one or two points but does not specify
timing boundaries. Oscilline maps Good to +1 and Perfect to +2 using its existing
judgment windows. The wiki documents freestyle without a numeric formula;
[falsehead's FAQ](https://www.digitpress.com/faq/vibribbon.htm) corroborates that
an action not required by the upcoming obstacle earns a bonus only if that
obstacle is then cleared. Oscilline credits +1 for each distinct non-required
action pressed before that obstacle's Good window. Repeating the same action
does not multiply the reward. A miss discards pending freestyle. These bonus
mappings are implementation choices, pending measurement of the original; they
are not confirmed original-game formulas.

Each completed stage adds a bonus, rounded down: Super 30%, Rabbit 20%, Frog
15%, Worm 0%. Failed and unfinished runs receive no clear bonus. The percentage
uses the uncapped earned total, so the bonus can grow after coupons saturate.
Round two carries the first stage's coupons, including its bonus, as the FAQ
describes, and the figure's form carries over too. Its final bonus is
calculated on that carried total plus the second stage's earnings. Results and
saved high scores use the same totals. Retry resets all scoring state.
Exceptionally long custom tracks saturate instead of reproducing the original's
signed-integer overflow. High scores saved before this scoring scheme are not
comparable; Options > Other > Reset High Scores clears them.

## Disc graphics and motion

The PAL disc has 15 ordered colored line objects in `FONT/MARK.TMD` and a
240-frame single-object motion track in `METERS/SCORE.ANM`. The `score.coupons`
slot resolves these exact paths, without enabling the unverified results
`rank.marks` slot. Each coupon selects one model object and samples the motion
track at seven evenly spaced positions across half the cycle, leaving the other
half empty. Coupon strokes use a one-pixel core and a half-pixel feather. The
screen-space HUD uses a fixed-size top band centered on the logical viewport,
independent of course camera motion and aspect ratio. The carousel's highest
edge sits one coupon height (22 px) below the top of the screen. Pausing freezes
its course clock.

Coupons travel counter-clockwise around the semicircle. Rotation follows the upcoming obstacle's effective stage-scroll travel time,
with one revolution per screen crossing (four seconds at a four-second
crossing). Its phase integrates speed changes rather than jumping when speed
changes; the final speed continues through the stage tail. The eight-second
prelude turns at the first obstacle's speed, so the carousel is already moving
when the stage starts. The same phase drives the disc animation and the
procedural fallback. Missing or malformed assets use the procedural fallback;
disc bytes are loaded locally and are not committed.

Placement, scale, phase spacing, and the no-disc procedural symbols are
approximate. They have not been matched frame by frame against footage of the
original. The coupon geometry and motion track come directly from the disc, and
the layout is checked at 4:3 and wider aspect ratios.
