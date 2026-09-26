# Run 1 - X9-X steps 2-3: texture manager and 2D pass (PR #148)

Reconstructed 2026-09-26 from the PR, its commits and the owner's notes;
the run's own `.sweep/` logs were not kept.

## Scope

- Prompt: X9-X steps 2-3 of `fork-plan.md` item 14 (texture manager, 2D).
- Topics: 1 (`maxTasks` never bound; `pipeline()` across topics bought
  nothing).
- Prior design: `docs/r3d/design/x9-x-textures-and-2d.md` (161 lines, by
  nv2a-xbox-specialist, merged the day before in #147) and the item 14
  inventory. Two specialists were consulted anyway.
- Result: merged, after two owner fixes (below).

## Cost (owner's figures from the Workflow UI)

| | |
|---|---|
| Agents | 12 (setup, 2 specialists, research, implementer x3, QA x3, publisher, retrospector) |
| Output tokens | ~1.72 M |
| Tool calls | 684 |
| Wall clock | ~2 h 09 min |
| QA rounds | 3 (red, red, green) |

## Gates

| Gate | Required | Ran | Exit | Note |
|---|---|---|---|---|
| host test `nv2a_arena_host_test.c` | yes | yes | 0 | 49/49 |
| asset-studio ctest | yes | yes | 0 | |
| `xbox-ref-isolate.sh nv2a` | yes | **no** | - | "did not complete in this worktree: deps/nxdk's nested pdclib submodule is uninitialised" (PR body) |
| `xbox-link-check.sh` | yes | **no** | - | same cause |
| `xbox-game-link-check.sh --include-client --include-freevgui` | yes | **no** | - | same cause |
| xemu boot | yes (renderer change) | **no** | - | "owner-side, post-merge" |

QA reported **green** in round 3 with three required gates unrun. Round 3
had no codex round left and was code reading only. The real cause of the
unrun gates was not a nested submodule but that a fresh worktree has no
BUILT nxdk at all (see run 2, and `tools/xbox-link-check.sh`'s NXDK_DIR
comment).

## Review

- codex-review rounds: 2 of 2. Round 1 ran on a diff that did not compile
  for the Xbox. Round 2 found bugs that were already present in round 1's
  diff (Codex variance, not a loop defect; it means the diff a round sees
  should be as final as possible).
- Findings fixed: 5 real texture-lifetime bugs in `r_image.c` (stale
  hash-linked slot, update racing an in-flight batch, storage-less slot
  NULL write, unset `TF_IMG_UPLOADED`, unsynchronised deferred-free
  overflow). Recorded in `divergences.md` #118.

## Live

Nothing booted by the loop.

## Missed (found by the owner after the PR opened)

1. **It did not compile for the Xbox.** `r_image.c` lacked `crtlib.h` and
   `crclib.h`. Fixed in `4150bd14`. A ~10 s `xbox-ref-isolate.sh nv2a`
   would have caught it.
2. **HUD render modes were wrong.** A 2D batch was flushed under the NEXT
   draw's render mode, so additive HUD sprites drew opaque on black boxes.
   Found only by booting xemu and looking at the screen. Fixed in the same
   commit.

## What went well

- Research found that every map's WAD textures would have been decoded and
  uploaded for nothing (median 2.73 MiB, max 13.23 MiB per map) and fixed
  it in the baker - a finding neither the design note nor the specialists
  had.
- Codex's five findings were real and the review ran on Codex's quota.

## Lessons

Both candidates were adopted in `docs/r3d/lessons.md` on 2026-09-26:
"NEVER treat a fresh git worktree as a build environment" and "NEVER let
the agent that ran a check decide whether it passed". The HUD render-mode
bug is recorded as a second data point under "A structural pass ... Only
looking at the render does that".
