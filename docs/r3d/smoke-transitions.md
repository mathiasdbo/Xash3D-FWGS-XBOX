# Level-transition smoke test (real console)

Walks the Half-Life campaign in game order with `changelevel2 <next> <landmark>`,
stays 30 s on each map with the frame-time recorder running, and says from the
recording which map the game died or hung on. It catches crashes on load and on
transition early, before a release, without a person playing through.

Status: piloted on the console on 2026-11-09 (3 maps, 10 s each, label `smk-pilot1`): the
`_load.cfg` chain ran c0a0 -> c0a0a -> c0a0b with `changelevel2` and landmarks, the end marker
was recorded and the game quit cleanly. `wait N` counts about 120 a second at 60 fps there (the
command buffer runs more than once a frame), so the dwell is converted at that rate. The full
campaign run has not been done yet.

## Files

| File | What |
|---|---|
| `tools/xbox-smoke-transitions.py` | `stage`, `status`, `restore`, `fetch`, `check` |
| `tools/smoke/hl-campaign.txt` | The 96 maps from `c0a0` to the end, with the transition (landmark) from one to the next |
| `tools/smoke/hl-hazard-course.txt` | The training maps, `t0a0` onward |
| `tools/smoke/make-map-list.py` | Writes a list from the retail `valve/maps/*.bsp` |
| `tools/test_xbox_smoke_transitions.py` | Tests (run by the fast gates) |

The lists come from the maps themselves: the generator reads each BSP's entity
lump, collects its `trigger_changelevel` (map, landmark) and walks the graph from
the start map depth-first, the targets of a map in name order. A step is a real
`changelevel2` while the map the walk is on has a trigger to the next one. Where
the walk has to jump back to an earlier map (12 of the 95 steps in the campaign
list) the line has landmark `-` and the step is a plain `map` load, reported as
"direct". Multiplayer and demo maps are not in the lists.

## Run it

Put the build under test on the console as `default.xbe` first (the packager, or
`tools/xbox-ftp-route-bmi.py stage --xbe ...`). The run uses the console's own
settings (`50-packager.cfg`, the menu's `config.cfg`), so it runs at whatever
video mode the console has.

    # a pilot: 3 maps, 10 s each, to see the chain work
    python tools/xbox-smoke-transitions.py stage --host <ip> --count 3 --dwell 10
    # boot the game from the dashboard; it quits to the dashboard when done
    python tools/xbox-smoke-transitions.py fetch --host <ip>
    python tools/xbox-smoke-transitions.py check

    # the whole campaign, 30 s a map: about an hour
    python tools/xbox-smoke-transitions.py stage --host <ip> --dwell 30

`stage --dry-run` reads the console and prints what it would write. `restore`
removes everything a stage wrote and renames the cfgs it set aside back.

FTP is unavailable while the game runs and the console takes one connection at a
time, so everything is decided from the recording afterwards.

## How the chain runs

`stage` writes `valve/userconfig.d/90-smoke.cfg` (recorder on with a label,
`sv_cheats 1`, `map <first>`) and, for every map in the chain,
`valve/maps/<map>_load.cfg`. The server runs that file when the map loads. It
waits the dwell in frames, then runs `changelevel2 "<next>" "<landmark>"` (or
`map "<next>"`). A map reached by a plain load also gets `god` and `notarget`
after a short wait, since those commands toggle and the player carried through a
real transition already has them. Every map also runs `ent_fire trigger_changelevel solid 0`
three-quarters of a second after it loads: a level change the chain did not ask for (the tram
on `c0a0a` carries the player into its trigger by itself) leaves the earlier map's pending timer
behind, and two timers then alternate for the rest of the run. That was the first full run
(2026-11-09): every map from `c0a0b` on was visited twice. `ent_fire` changes at most
`sv_enttools_maxfire` (5) entities, so `c1a4b` (6 triggers) and `c3a2c` (7) can still bounce;
`check` reports a map played twice. The last map stops the recorder, records a
short second session `<label>-end`, and quits. `00-c1a0.cfg` and
`91-frametime.cfg` (which would load a map or set another label) are renamed to
`.off` for the run; `90-route-bmi.cfg` present is refused.

## Reading the result

`check` prints one row per map (arrival, seconds in play, result) and a verdict:

- **PASS**: every map loaded and stayed at least half the dwell in play, in order,
  and the end-marker recording exists.
- **The game stopped on `<map>`**: the last recording has no END chunk. The report
  says whether it died while loading (no in-play frames) or after N seconds in play.
- **The chain stopped before `<map>`**: the previous map's recording closed cleanly
  and the next never started, so the game died or hung loading `<map>`, or on the
  transition into it.

After a crash, power-cycle, `fetch`, `check`, then continue past it with the
command `check` prints: `stage --from <the map after the crashed one> --dwell 30`.
Each attempt has its own label, so recordings do not mix.

## Limits

- The player never walks to the trigger, so it arrives offset from the landmark.
  This tests loading and the transition and restore code, not gameplay.
- The recorder flushes every 5 s, so the last seconds before a crash can be
  missing. Segments start at each map start, so the failing map is still named.
- A crash back to the dashboard and a freeze look the same in the recording. The
  on-screen error text is not captured.
- Nothing watches the console live; a hang only shows when you notice the screen.
