# X12 item 4, Stage A: the console runs for the dialogue stream

Divergence #177 (branch `x9-audio-vox-stream`). The host test proves the
stream's output is bit-identical to upstream's whole loads; these runs
measure what it buys on the console and check it holds up on the real
disk. Prepared 2026-10-13, not yet run: the owner runs them.

## What to put on the console

- **A** (the baseline): main's XBE, the build before #177.
- **B**: `voxstream.xbe` from `.sweep/pending-xbox/` (this branch).
- The disc tree is the same for both (no new assets: Stage A streams the
  retail `valve/sound` WAVs where they already are).

`r3d_vox_stream` is not archived, so a run that ends at 0 cannot leak into
the next one through `config.cfg`. Every recording below is labelled by
its cfg, so nobody types a label.

## 1. Black Mesa Inbound: `bmi-vox-a`, `bmi-vox-b` (and `bmi-vox-c`)

The fixed audio benchmark, `tools/xemu-route-bmi.sh`. On the console, copy
its two files with these lines (the script prints them; `<label>` is the
run's):

- `E:\UDATA\<title id>\valve\valve\userconfig.d\90-route-bmi.cfg`:

  ```
  host_frametime_label "<label>"
  host_frametime_analysis 2
  map c0a0
  ```
- `E:\UDATA\<title id>\valve\valve\maps\c0a0e_load.cfg`:

  ```
  sv_enttools_enable 1
  wait 2700
  ent_fire barnwalk3 use
  wait 1200
  host_frametime_analysis 0
  wait 10
  host_frametime_label "<label>-end"
  host_frametime_analysis 1
  wait 30
  host_frametime_analysis 0
  echo route-bmi: done
  ```

  The last five lines record the end marker, a short session labelled
  `<label>-end`: its presence says the cfg ran to its end.

Runs: `bmi-vox-a` on A, `bmi-vox-b` on B, and `bmi-vox-c` on B with the
line `r3d_vox_stream 0` added after `host_frametime_analysis 2` (the same
binary, upstream's path: the per-frame counters show which side ran).
Boot, wait about 7 minutes, then:

```
python tools/frametime-report.py fetch --host <ip> --label bmi-vox-b
python tools/frametime-report.py fetch --host <ip> --label bmi-vox-b-end
python tools/frametime-report.py summary <out> --label bmi-vox-b
python tools/frametime-report.py check <out> --label bmi-vox-b --end-label bmi-vox-b-end \
    --maps c0a0,c0a0a,c0a0b,c0a0c,c0a0d,c0a0e --min c0a0e:vox_starts:1?
```

`check` exits nonzero, naming what is missing, unless the session ends
cleanly, plays the six maps in order, has the end marker and reaches the
counter minimums; the xemu scripts run the same check. Use a label not
recorded on that console before (the newest session of a label is read).

Delete both files afterwards. BMI has 12-13 word loads and none over
60 KiB: it checks the semantics and that nothing regressed, not the big
stalls.

## 2. The scientist route: `route-vox-a`, `route-vox-b`

### Scripted (unattended)

`tools/xemu-route-vox.sh` writes four files; copy them, with the lines it
prints, to the same places on the console:

- `...\valve\userconfig.d\90-route-vox.cfg` (the label, the recorder,
  the c1a0e steps as aliases, `map c1a0`);
- `...\valve\maps\c1a0_load.cfg`, `c1a0e_load.cfg`, `c1a1_load.cfg`.

It plays Barney's desk line (182 KiB), an announcement and the suit's
logon on c1a0; on c1a0e two scientists at once (`c1a0_sci_dis1a`,
194 KiB, and `dis2a`), a save in the middle of their lines (`route_vox_mid`)
and its load, two more lines, and the change to c1a1 while the last one
still speaks; then c1a1's alarms and the end marker (`<label>-end`, as in
section 1). About 6 minutes. Fetch both labels, then check:

```
python tools/frametime-report.py check <out> --label route-vox-b --end-label route-vox-b-end \
    --maps c1a0,c1a0e,c1a1 --min c1a0:vox_starts:1 --min c1a0e:vox_starts:4
```

(on A, upstream's build, leave out the `--min` arguments: it has no
`vox_starts`). Delete the four files and the `route_vox_mid` save
afterwards.

### Played by hand (the real scenes)

The scripted lines play on the player's own channel. The NPC scenes - lip
sync, several speakers placed in the world, the channel picks - need the
route played: from a new game, c1a0 (Barney at the desk, `BA_DESK`) to
c1a1, as route-pr193 was. Use a cfg with only the label and the recorder:

```
host_frametime_label "route-vox-a"
host_frametime_analysis 2
```

and:

1. walk past Barney's desk and wait for his line;
2. in c1a0c, the HEV suit's logon and the scan (`c1a1_sci_4scan`, 188 KiB);
3. in c1a0e, stand in the scientists' room while they talk (`dis1a`...)
   and, **in the middle of a line**, quick-save, keep listening for a few
   seconds, then quick-load: the restored line must carry on from where
   it was saved, with the mouths moving;
4. walk into the test chamber and on to c1a1 (the level change while a
   scientist is still speaking, if one is);
5. stop the recorder (`host_frametime_analysis 0`) on c1a1.

Run it once on A (`route-vox-a`) and once on B (`route-vox-b`), the same
way. Two more scenes for the performance review's open questions, in the
same sessions if they fit: a fight where scientists or grunts are hurt
(their pain sounds are precached speech), and an autosave while someone
speaks (vox reads queue behind the save's writes).

## What to read

`summary` prints, per map, **"dialogue per map"** (B only): streams
started and prefetch hits, files left whole for size or another reason,
opens, reads (queued / at once), waits, jumps, and the time in the starts,
opens, waits and file calls; then **"sounds S_LoadSound loaded in play, by
caller"**: the `sound` notes, now tagged with why the load ran (w a word,
t `SND_GetChannelTimeLeft`, s a sound started, m the mixer, r a
registration) and their opens.

Exit criteria (the spec's):

- no `sound` note for a word that streamed (on B, the w and t columns
  hold only words under 8 KiB, other formats and fallbacks);
- `vox_waits` about 0, and the `vox` notes (fallbacks) explained;
- no frame over 8.3 ms from a sound load, and none from the streams'
  own synchronous work: read the per-frame peaks of `vox_first_kc`,
  `vox_open_kc` and `vox_wait_kc` (the frames list), not their sums;
- `music_waits` no higher than on A (the music's reads now share the
  disk with the voices');
- `r3d_vox_crc all` clean on the console (below);
- the A/B listening check: B must sound identical to A (the output is
  bit-exact; any difference is a bug).

Stage B (the ordered speech pack, `docs/r3d/fork-plan.md`, X12 item 4) is
the next phase whatever these show; the first-word wait (`start ms` per
start) and the opens tell how much it buys on the HDD.

## `r3d_vox_crc all` on the console

From the console (or a cfg line), with a map loaded or at the menu:

```
r3d_vox_crc all
```

It plays every sentence of `sentences.txt` through both paths - upstream's
whole loads, then the stream, word by word as the mixer advances, and each
word again from a random restore position - and compares the trimmed
start and end, the flags and the CRC of the samples; each mismatch is a
`DIFFERS` line. A sentence that built words in one pass and not the
other, or loaded a word in one and not the other, is a `LOAD FAILED,
DIFFERS` line, counted in `differ` and in its own `failed to load`; a
sentence upstream's pass has no words for is a `SKIPPED` line. The
summary line ends `SAME` only when every sentence was compared and none
differs, `DIFFERS` when one does, `INCOMPLETE` when nothing differs but a
sentence was skipped (or none was compared); it gives the counts of each,
and the total load time of each path (a ratio on the console as in xemu). The
output is also appended to `vox_crc.txt` in the write directory
(`E:\UDATA\<title id>\valve\valve\vox_crc.txt`). It takes a few minutes
(every word file is read twice).
