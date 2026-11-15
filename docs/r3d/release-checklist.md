# Releasing an Xbox build (`xbe-<commit>`) and the packager

Owner rule: no build may make an existing saved game unloadable. Saves are on players' consoles and cannot be regenerated.
The gates are scripts that fail; this is the order to run them in. Fork plan section XS has the reasons.

## 1. The XBE

1. Build it from a commit on `origin/main` with `tools/xbox-game-link-check.sh` (it runs `tools/save_rules_check.py source`, the
   callback table check and the `-opt:noicf` link, and writes `default.map` and `default.xbe.identity` beside the XBE;
   the identity records the commit, whether the tracked tree was clean, and the hashes of the XBE and the map). Build from
   a clean checkout of the commit you will release.
2. Run the live save gate on exactly those bytes. It needs xemu and the fixture saves, which stay outside the repository:

       set R3D_SAVE_FIXTURES=<folder of the fixture saves>
       python tools/save_live_gate.py run --xbe default.xbe --stage <a COPY of the disc tree, outside the repo> --out <folder>

   Every fixture must load and play. Commit `<folder>/live-gate.txt` as `tools/save-live-evidence/<sha256 of the XBE>.txt`
   (hashes, names, maps and PASS lines only; no save content). A new fixture goes in `tools/save-fixtures-expected.txt`
   first (`save_fixture_gate.py scan`, review the diff).
3. `python tools/xbe_release.py check --xbe default.xbe --commit <40-hex>` prints every reason the XBE must not be released:
   the title id, the save format and limits, the live evidence for these bytes, the map and its identity.
4. `python tools/xbe_release.py publish ... --publish` creates the `xbe-<commit8>` pre-release with `default.xbe`,
   `default.map` and `save-live-evidence.txt`. Without `--publish` it only prints.

## 2. The packager

`tools/packager/make_payload.py` (CI and `release.py` both call it) refuses an XBE without committed live evidence for its
bytes, and with a title id other than `0xFFFF0002`. A changed server layout is only noted: saves are written by name and old
offsets are read through the frozen `tools/save-legacy-table.txt`, which is never regenerated or shrunk.

## 3. After

- Changelog section closed; site updated (`tools/site/build.py`, then the Pages workflow once merged).
- Before replacing a build on a console, the packager copies the console's saves to the PC; `--restore-build` and
  `--restore-saves` are the way back. Never write to a console's save folder by hand.
