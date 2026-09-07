# E0 — Solution design

The stage before implementation. Its output is decisions, not code.

Nothing here is a research project. Every item is answerable in days, most of
them by reading code or running one experiment, and each one is a question
that gets more expensive to answer the later it is asked.

## Why this stage exists

Three of the four open questions in `epics.md` are load-bearing for work that
would otherwise start immediately, and all three have the same shape: an
assumption that looks safe and is checkable in an afternoon. The previous
project's Xbox target accumulated six stale fields over seven months because
nothing exercised it — the cost of an unasked question compounds quietly.

## D1 — The linker question

**Question:** does `lld-link` tolerate the symbol collisions that
`scripts/waifulib/xshlib.py` exists to prevent?

**Why it is first:** every phase of the Xbox plan sits behind it, and it is
the only question whose answer might be "this approach does not work".

**How to answer:** compile two modules that both define a common symbol,
link them with `lld-link`, observe. Then, in order of preference: no action
needed; a COFF equivalent of `objcopy -G`; scoped `/FORCE:MULTIPLE`;
per-module symbol renaming by `#define`.

**Output:** a written mechanism, and a one-page note if it required anything
unusual. Days, not weeks.

## D2 — Which HL SDK base

**Question:** `deps/hlsdk` (the pinned `halflife-updated` forks) or
`FWGS/hlsdk-portable`?

**Evidence so far:** upstream's README points at `hlsdk-portable` for every
non-desktop target; the Android, iOS and PS3 ports all use it; it is the base
already known to build as a static library under `XASH_INTERNAL_GAMELIBS`. The
pinned forks descend from `halflife-updated`, which the Rust project built as
a Windows DLL and called over FFI — a different job.

**How to answer:** try to compile both under nxdk. This is cheap and the
answer is binary.

**Output:** a decision, and if it is `hlsdk-portable`, a note on what the
pinned forks carry that would need rebasing.

## D3 — The scene interface

**Question:** what does a backend receive, once per frame?

**Why it matters most:** it blocks the NV2A backend, the Vulkan backend and
anything modern, and `epics.md` §0 explains why `ref_interface_s` cannot serve
all three. This is the decision the project is least able to revisit later.

**Constraints it must satisfy simultaneously:**

- A **fixed-function pushbuffer** needs a command list it can encode directly,
  with state sorted by the producer, because it cannot re-batch.
- A **path tracer** needs a scene — geometry, materials, emissive surfaces,
  lights as objects — and does not want draw order at all.
- A **GPU-driven rasteriser** needs the visible set as data it can cull and
  index on the GPU, not as calls already made on the CPU.

The shape those three agree on is: **submit a described scene, not a call
stream.** Visible surface set, material references, light list, entity
transforms and skinning matrices, submitted once. What the backend does with
it is the backend's business.

**How to answer:** write the interface as a header with no implementation, and
sketch — on paper, not in code — how each of the three consumes it. If any of
them has to reconstruct information the interface threw away, the interface is
wrong.

**Output:** a header and a rationale document. `ref/gl` is then adapted to it
before any new backend is written, because `ref/gl` working is the only proof
the interface is complete.

## D4 — The asset path split

**Question:** PC reads loose folders, console reads `.mpak`. How do the two
stay honest?

**Settled already:** PC needs no repack. `filesystem/dir.c` exists and
searchpaths are a chain; `valve/`, `gearbox/` and `bshift/` work today. This
is a feature to not break rather than one to build.

**What is not settled:** the `.mpak` path will be exercised far less than the
loose-folder path, and it is the harder one to debug. Left alone, it rots.

**Proposal to validate:** run the `.mpak` path on PC in CI, on every change.
The console then runs code that has already been exercised on a machine with
a debugger.

**Output:** a decision on how both paths are tested, before either is
depended on.

## D5 — Map manifests

**Question:** do maps need a manifest of assets that live outside their
archive?

**The evidence says yes**, and it comes from a real failure. The Rust
project's baker discovered a map's assets by regex-scraping the BSP entity
lump. That cannot see anything precached by a fixed string in game code, so it
silently omitted five combat sprites. The effects compiled, passed their unit
tests, booted, and drew nothing. The bug was found by a code review, not by
running the game.

**The design conclusion:** a manifest must be produced by **observing what the
engine actually requests**, not by parsing what a map appears to reference.
Instrument a playthrough, record every resolved asset, diff against the
archive.

**Second use:** searchpaths are a chain, so a manifest also gives a map a way
to name assets resolved from outside its own archive — patches, additional
content, or anything the bake did not know about.

**Output:** the manifest format, and where it is generated.

## D6 — The memory model

**Question:** what is the budget, per target, and who enforces it?

**Settled in shape:** the baker decides and rejects, `zone.c` measures, the
resource manager allocates and frees. One ledger, two allocators, because
NV2A textures live outside the normal heap and a heap-only budget is blind to
half the consumption.

**Not settled:** the actual numbers. They come from measurement, not from
choosing round figures. The starting data point is the previous project's
`global.mpak` at 62.9 MB — 99.1 % of it one lump — against a 64 MB machine.

**Output:** a per-target budget table with a named owner for each line, and
the enforcement point for each.

## D7 — Scope confirmation

Two questions in `epics.md` §0 are for the project owner, not for
engineering, and they change the plan materially:

- **Is DX12 backed by a named target?** If there is no Series X/S plan, it is
  a second Windows-only renderer for no additional coverage.
- **Is PS3 a port we build or an abstraction we validate?** xashPS3 already
  reached a locked 60 FPS at 720p on real hardware. Building it again spends
  months arriving where someone already is.

**Output:** an answer to each, written down, so the epic list stops carrying
work nobody has committed to.

## D8 — Lightstyles and bone palettes, by census

**Question:** what does the console lose, and what does the baker have to
partition?

Two facts the plan carried as opinions until 2026-09-06. A face carries up to
four lightstyle layers (`MAXLIGHTMAPS 4`) combined every frame, and the baker
bakes only the first — so "final-format lightmaps" as written turns off every
switchable and flickering light in the game. And a VP20 program holds roughly
50 bone matrices against `MAXSTUDIOBONES 128`, so some models must be split by
bone subset before GPU skinning can touch them.

**How to answer:** two scripts over the retail assets. Count faces per map
with `styles[1..3] != 255`; count bones per mesh per model. Minutes each.

**Output:** the lightstyle policy, written down with the number that chose it,
and the list of models the partitioner must handle. Both are inputs to
`fork-plan.md` XB.

## Exit criteria for E0

- D1 has a proven mechanism, not a plan to find one.
- D2 is decided by compilation, not by preference.
- D3 has a header and three sketched consumers.
- D4, D5 and D6 have written decisions.
- D7 is answered by the project owner.
- D8 is answered by the two censuses, before XB starts.

E1 may begin as soon as D1 and D2 are answered. E4 may not begin before D3.
XB may not begin before D8.
