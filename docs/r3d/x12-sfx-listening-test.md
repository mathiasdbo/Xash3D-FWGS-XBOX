# X12 step 3: the interpolated-mix listening test (console)

Divergence #174 gives the Xbox mixer upstream's `s_lerping` (linear
interpolation between source samples) on #145's fixed-point counter. It is
off by default, as upstream has it; this test decides whether the console
should turn it on. The owner runs it on the console with headphones and the
TV, never during a recording session.

## What is expected to differ

Every Half-Life sound effect and voice is 8-bit mono at 11 or 22 kHz
(survey, 2026-10-12: 582 files at 11 kHz, 129 at 22 kHz, none 16-bit or
44 kHz). Mixed at 48 kHz (#173), the point loop repeats each source sample
4 or 5 times (11 kHz) or 2 or 3 times (22 kHz), unevenly. Measured on a tone in an 11 kHz sound
(`tools/host-tests/s_mix_host_test.c`, dB under the tone):

| tone | point, 48 kHz: inharmonic / the sound's own images | interpolated, 48 kHz |
|---|---|---|
| 500 Hz | -36 / -26 | -72 / -53 |
| 1 kHz | -30 / -20 | -61 / -40 |
| 2 kHz | -24 / -13 | -47 / -26 |
| 4 kHz | -18 / -5 | -35 / -10 |

The images (the 11 kHz "staircase", at 11025 +- f, 22050 +- f ...) were
already there at 44.1 kHz and in the original game; the interpolation
takes them down too. What it costs: the sounds lose a little top end (an
11 kHz sound's 4 kHz content comes out about 2 dB softer), so expect
voices and the announcer to sound slightly duller and much cleaner. The
music (`.48k.xwav`) and the room effects do not change.

## The disc

The step-3 build (`sfx.xbe`) as `default.xbe`. Copy this file to
`E:\UDATA\<title id>\valve\valve\userconfig.d\95-sfx-ab.cfg` (or into the
disc tree's `valve/userconfig.d/` before packing). Delete it afterwards: it
loads a map at every boot.

```
// X12 step 3 listening test (docs/r3d/x12-sfx-listening-test.md). Remove after the test.
// D-pad: UP = A/B the interpolation (prints "s_lerping is ..."),
//        RIGHT = next map, LEFT = previous map, DOWN = this map again from its start.
s_lerping 0
alias ab_on "s_lerping 1; s_lerping; alias lerp_ab ab_off"
alias ab_off "s_lerping 0; s_lerping; alias lerp_ab ab_on"
alias lerp_ab ab_on
bind DPAD_UP "lerp_ab"
alias lt1 "map c0a0; alias lt_next lt2; alias lt_prev lt3; alias lt_again lt1; echo listening 1/3 c0a0 - the announcer, the tram"
alias lt2 "map c1a1; alias lt_next lt3; alias lt_prev lt1; alias lt_again lt2; echo listening 2/3 c1a1 - voices, sparks, alarms, rooms"
alias lt3 "map t0a0; alias lt_next lt1; alias lt_prev lt2; alias lt_again lt3; echo listening 3/3 t0a0 - the training voice, footsteps, echoes"
alias lt_next lt2
alias lt_prev lt3
alias lt_again lt1
bind DPAD_RIGHT "lt_next"
bind DPAD_LEFT "lt_prev"
bind DPAD_DOWN "lt_again"
lt1
```

`s_lerping` is archived (upstream's `FCVAR_ARCHIVE`), so whatever it is
left at is saved to `config.cfg`; the cfg above sets it back to 0 at every
boot while it is installed. After the test, `s_lerping 1` (or 0) in the
console, or in a cfg, keeps the verdict.

## What to check

For each item, the verdict with the interpolation on against off: "same",
"better" or "worse".

- **Voices and the announcer** (11 kHz): the metallic edge and grit at 48
  kHz should be gone; listen also for a duller, muffled voice. c0a0's
  announcements, c1a1's scientists, t0a0's hologram.
- **Effects** (11 and 22 kHz): sparks, alarms, the tram, footsteps,
  weapons. Bright effects (sparks, glass) are where any loss of top end
  shows first.
- **Pitch and timing:** the same at both settings. Any difference is a bug.
- **Stability:** five minutes with it on, without a click, a dropout or a
  stuck sound.

The frame-time cost is measured separately (below); the verdict here is
only about the sound.

## The console recordings (frame-time cost)

Black Mesa Inbound, `tools/xemu-route-bmi.sh`'s two route files copied to
the console as its header describes (`90-route-bmi.cfg` in
`userconfig.d`, `c0a0e_load.cfg` in `valve\maps`), with no listening cfg
installed:

1. `bmi-sfx-a`: the main build as `default.xbe`; the route cfg as the
   script writes it with the label `bmi-sfx-a`.
2. `bmi-sfx-b`: `sfx.xbe`; label `bmi-sfx-b`, and the line `s_lerping 1`
   before `map c0a0`.
3. Optional, on `sfx.xbe` at the menu before either route: `r3d_mixbench`
   in the console (or as a cfg line) - Pentium III cycles an output for
   the point and the interpolating loops, appended to `mixbench.txt` in
   the write directory.

Then `tools/frametime-report.py fetch --host <ip> --label bmi-sfx-a` (and
`-b`), and `summary --compare bmi-sfx-a bmi-sfx-b`: `k_snd_mix` in the
audio table is the number. Afterwards delete both route files and set
`s_lerping 0` (it is archived) unless the listening test kept it on.
