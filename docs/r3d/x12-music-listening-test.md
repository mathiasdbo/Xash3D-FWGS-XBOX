# X12 step 1: the baked-music listening test (console)

Divergence #171 replaces the music's mp3 decode on the console with tracks
baked offline to Xbox ADPCM (`media/<Track>.xwav`). The bake's numbers
(SNR against libmpg's own decode, `music_manifest.txt`) catch a broken or
unusual track; they do not say whether a track sounds the same. This test
does. The owner runs it on the console with headphones and the TV, never
during a recording session.

## The disc

Build the disc with the bake (`tools/xbox-xiso.sh hl ... --music-bake
.sweep/music-bake`); the mp3s stay on it. Copy this file to
`E:\UDATA\<title id>\valve\valve\userconfig.d\95-music-ab.cfg` (or into the
disc tree's `valve/userconfig.d/` before packing). Delete it afterwards: it
loads a map at every boot.

```
// X12 music listening test (docs/r3d/x12-music-listening-test.md). Remove after the test.
// D-pad: UP = A/B (the same track at the same moment, mp3 <-> baked),
//        RIGHT = next track, LEFT = previous track, DOWN = this track from the start.
// The console prints which side is playing ("music now BAKED" / "MP3").
r3d_music_baked 1
map c1a0d
bind DPAD_UP "r3d_music_ab"
alias mt1 "music media/Half-Life02.mp3 media/Half-Life02.mp3; alias mt_next mt2; alias mt_prev mt9; alias mt_again mt1; echo music test 1/9 Half-Life02 - loudest, lowest SNR"
alias mt2 "music media/Half-Life12.mp3 media/Half-Life12.mp3; alias mt_next mt3; alias mt_prev mt1; alias mt_again mt2; echo music test 2/9 Half-Life12 - worst 50 ms at 58 s"
alias mt3 "music media/Half-Life01.mp3 media/Half-Life01.mp3; alias mt_next mt4; alias mt_prev mt2; alias mt_again mt3; echo music test 3/9 Half-Life01 - dense rock, worst at 0.5 s"
alias mt4 "music media/Suspense07.mp3 media/Suspense07.mp3; alias mt_next mt5; alias mt_prev mt3; alias mt_again mt4; echo music test 4/9 Suspense07 - short, worst at 0.2 s"
alias mt5 "music media/Half-Life17.mp3 media/Half-Life17.mp3; alias mt_next mt6; alias mt_prev mt4; alias mt_again mt5; echo music test 5/9 Half-Life17 - worst at 54 s"
alias mt6 "music media/Half-Life16.mp3 media/Half-Life16.mp3; alias mt_next mt7; alias mt_prev mt5; alias mt_again mt6; echo music test 6/9 Half-Life16 - the CBR 128 track"
alias mt7 "music media/Prospero01.mp3 media/Prospero01.mp3; alias mt_next mt8; alias mt_prev mt6; alias mt_again mt7; echo music test 7/9 Prospero01 - quiet ambient"
alias mt8 "music media/Suspense03.mp3 media/Suspense03.mp3; alias mt_next mt9; alias mt_prev mt7; alias mt_again mt8; echo music test 8/9 Suspense03 - quiet ambient"
alias mt9 "music media/Half-Life07.mp3 media/Half-Life07.mp3; alias mt_next mt1; alias mt_prev mt8; alias mt_again mt9; echo music test 9/9 Half-Life07 - worst at 2.8 s"
alias mt_next mt1
alias mt_prev mt9
alias mt_again mt1
bind DPAD_RIGHT "mt_next"
bind DPAD_LEFT "mt_prev"
bind DPAD_DOWN "mt_again"
```

On c1a0d the only music trigger is the HEV suit's (`trigger_cdaudio`
`suit_cd_audio`): do not pick the suit up. Each track is started with
itself as the loop, so its end also tests the loop seam.

## The tracks and why

The bake measured every track against libmpg's decode (encoder 1; overall
SNR, and the worst 50 ms window louder than -60 dBFS):

| # | Track | Overall | Worst 50 ms (at) | Why it is on the list |
|---|---|---|---|---|
| 1 | Half-Life02 | 29.3 dB | 23.9 dB (0.7 s) | lowest overall SNR of the 27 |
| 2 | Half-Life12 | 38.8 dB | 19.2 dB (58.4 s) | lowest worst window |
| 3 | Half-Life01 | 31.2 dB | 19.4 dB (0.5 s) | dense rock, second-lowest window |
| 4 | Suspense07 | 33.7 dB | 20.1 dB (0.2 s) | short, low window at the start |
| 5 | Half-Life17 | 33.3 dB | 21.2 dB (53.7 s) | low on both |
| 6 | Half-Life16 | 37.6 dB | 25.8 dB (60.9 s) | the one constant-bitrate (128k) source |
| 7 | Prospero01 | 47.0 dB | 34.8 dB (0.6 s) | quiet ambient: where hiss would show |
| 8 | Suspense03 | 49.3 dB | 40.7 dB (8.8 s) | quiet ambient |
| 9 | Half-Life07 | 48.2 dB | 20.9 dB (2.8 s) | high overall but a low window near the start |

## What to check

- **Per track, A/B with D-pad UP** around the "worst" second in the table
  and anywhere else; verdict "same" or "worse". Each press restarts the
  track at the position it had reached, on the other path.
- **Loop seam:** let one track run to its end (D-pad DOWN restarts).
- **Save/restore:** in a map with music, save mid-track, load it: it
  resumes at the same point. Save on one side of r3d_music_ab and load on
  the other: the position carries over. (After a load the mp3 path's own
  seek can land a few hundredths of a second off its target; the baked
  one lands exactly - the bake test measures both.)
- **Pause and menu:** pause and unpause; the music stops and resumes
  where it was.
- **Volume:** the menu's music slider at 0, half and full; at 0 the track
  must hold its position, as upstream does.

Any "worse" track: rebake it as PCM, `--music-pcm <Track>` (3.6x its
ADPCM size), and repeat for that track.
