# X12 step 2: the 48 kHz mix listening test (console)

Divergence #173 mixes the console's sound at 48 kHz, the AC97's own rate,
instead of upstream's 44.1 kHz, which SDL then converted on its audio
thread. The output is no longer bit-identical to upstream's. The host
tests prove that sounds last as long and play at the same pitch, and that
the room effects keep their delays in milliseconds; they cannot say
whether it sounds the same. This test does. The owner runs it on the
console with headphones and the TV, never during a recording session.

## What is expected to differ

- **11 and 22 kHz sounds** (most of Half-Life's effects and voices, the
  announcer's sentences). The mixer repeats each source sample to fill the
  output. At 44.1 kHz that is exactly 4 (or 2) times per sample; at 48 kHz
  it is 4 or 5 times, unevenly, and that adds faint inharmonic tones: on a
  test tone in an 11 kHz sound, 28 dB under it at 500 Hz, 22 dB at 1 kHz,
  16 dB at 2 kHz, 10 dB at 4 kHz (measured; nothing at 44.1 kHz). The
  sounds already carry their own 11 kHz "staircase" images at 3 to 23 dB
  under the tone on both sides. Sounds the game plays at another pitch
  than normal (weapons, which vary theirs, and pitched voices) already
  had these tones at 44.1 kHz, at the same levels; at 48 kHz the sounds at
  normal pitch get them too. Listen for extra grit or a metallic edge on
  voices and the announcer at 48 kHz.
- **Music** at 48 kHz plays `media/<Track>.48k.xwav`, resampled offline
  with a high-quality filter; at 44.1 kHz the original bake plays and SDL
  converts it. Both should sound the same.
- **Room effects** (echoes in tunnels, halls, vents): the same delays in
  milliseconds. Their smoothing filters are counted in samples, so at 48
  kHz they let through slightly higher frequencies (8.8%).

## Verdict

The owner's first session on the console (2026-10-11, the live-switch build, before its sound died - see below): **48 kHz sounds better than 44.1 kHz.** 48 kHz is the default and stays so; 44.1 kHz is a debug A/B only. Normal play and map changes at 48 kHz never reopen the device.

## The disc

The step-2 build (`mix48.xbe`) as `default.xbe`, with both music sets in
`valve/media`: the 27 `<Track>.xwav` files already on the console from
#171 and the 27 `<Track>.48k.xwav` files of the new bake.

**The rate is chosen at boot, so the A/B takes two boots.** A first
version switched the rate live with the D-pad; on the console one live
switch left the sound dead for the rest of the session (recorder session
`ebbee2dd`). `r3d_snd_rate` now applies only before the first map loads
- from a cfg at boot - and a later change only prints that it applies at
the next boot.

Copy this file to `E:\UDATA\<title id>\valve\valve\userconfig.d\95-mix48-ab.cfg`
(or into the disc tree's `valve/userconfig.d/` before packing). Delete
it afterwards: it loads a map at every boot.

```
// X12 step 2 listening test (docs/r3d/x12-mix48-listening-test.md). Remove after the test.
// The mix rate is set at boot: this line as it is for 44.1 kHz; comment it out
// ("// r3d_snd_rate 44100") for 48 kHz, then reboot. The console prints "r3d_snd_rate: mixing at ...".
r3d_snd_rate 44100
// D-pad: RIGHT = next map, LEFT = previous map, DOWN = this map again from its start.
alias lt1 "map c0a0; alias lt_next lt2; alias lt_prev lt3; alias lt_again lt1; echo listening 1/3 c0a0 - the announcer, the opening music, the tram"
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

The engine reads the rate from the userconfig.d files before it opens the device, so the line may sit in any cfg there, even one that runs after another file's `map` and `wait`s; before that fix (x9-audio-rate-latch) a `96-rate44.cfg` behind a `00-c1a0.cfg` that loads a map came too late and the console stayed at 48 kHz. Keep the line a plain command: one inside an alias or a bind is only seen when it runs.

Go through the three maps at one rate, change the cfg's first command
line, reboot, and go through them at the other; then back if a
difference needs a second listen. Without the cfg the console mixes at
48 kHz.

## What to check

For each item, the verdict at 48 kHz against 44.1 kHz: "same", "better"
or "worse".

- **Pitch and timing:** a sound plays at the same pitch and for the same
  time at both rates (the announcer's sentences, a door, a footstep). Any
  difference here is a bug.
- **Voices and the announcer** (11 kHz): grit or a metallic edge at 48
  kHz - the expected difference above. Listen on c0a0's announcements and
  c1a1's scientists.
- **Effects** (22 kHz and 11 kHz): sparks, alarms, the tram, weapons.
- **Music:** the opening track on c0a0, A/B'd several times.
- **Echoes:** in a tunnel or a large room, the echo arrives at the same
  moment at both rates and has the same colour.
- **Stability:** at 48 kHz, five minutes of play without a click, a
  dropout or a stuck sound.

If voices or effects are worse at 48 kHz, X12 step 3's interpolation (or
baking effects at the mix rate) is the remedy; `r3d_snd_rate 44100` in a
cfg restores upstream's mix until then.
