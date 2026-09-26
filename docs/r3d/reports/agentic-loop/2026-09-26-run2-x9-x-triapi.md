# Run 2 - X9-X step 4: TriAPI (PR #149)

Reconstructed 2026-09-26 from the PR and its ten commits; the run's own
`.sweep/` logs were not kept. The owner had added "hard build gates" to
the run PROMPT (the loop files were unchanged since #119).

## Scope

- Prompt: X9-X step 4 of `fork-plan.md` item 14 (TriAPI).
- Topics: 1.
- Prior design: the item 14 inventory (step 4 was one paragraph; no
  separate design note).
- Result: merged, verified live in xemu by the loop's own last round.

## Cost

| | |
|---|---|
| Agents | not recorded |
| Output tokens | not recorded |
| Wall clock | ~3 h 50 min (previous merge 08:19 to PR open 12:09; first commit 08:50, last 11:49) |
| QA rounds | 2 codex rounds plus a final live round |

## Gates

| Gate | Required | Ran | Exit | Note |
|---|---|---|---|---|
| host test (81 checks incl. CHECK self-test) | yes | yes | 0 | |
| `xbox-ref-isolate.sh nv2a` | yes | yes | 0 | 9 ok, 0 leaked |
| `xbox-game-link-check.sh hl --include-client --include-freevgui` | yes | yes, late | 0 | two XBEs (baseline `e7086f40` and branch), 0 dup / 0 undefined - only after the main checkout's built nxdk was copied into the worktree by hand |
| xemu boot + `verify-image` | yes | yes | 0 | both builds |
| pixel diff baseline vs branch | yes | yes | 0 of 307200 differ | expected: the change is invisible until exercised |

## Review

- codex-review rounds: 2 of 2. Round 1: a host-test self-check that could
  hide a broken CHECK macro, and doc wording that understated the
  verification gap. Round 2: `GetMatrix` reported `ref/gl`'s near/far while
  the TriAPI draws with near=1/far=8192, and `CFrustum::CalculateFrustum`
  really reads it - a real bug, fixed at the root (`27f3a2ae`).

## Live

- `GetMatrix` hit every frame via particleman's frustum; 16 floats read
  back as a genuine near=1/far=8192 projection.
- `r_showhull 1` on screen: depth-occluded, back-face-culled hull polygons.
- `net_graph 1` not exercisable: the menu module in this build is a stub,
  so `host.allow_console` never opens - pre-existing, out of scope.
- Real hardware: owner-side, post-merge.

## Missed

- Nothing found by a human after the PR opened (as of merge).
- Inside the run: two docs commits (`8d2513ab`, `49c60572`) escalated
  "this environment cannot produce a bootable Xbox target - deps/nxdk/lib
  has no compiled .lib output". The final commit (`dd057133`) shows that
  was wrong: the main checkout's deps/nxdk, at the same pinned commit
  `29638d0b`, had every built lib, `cxbe` and `extract-xiso`; the worktree
  simply never got them. The same gap that silently failed run 1's gates
  cost run 2 an escalation cycle and two docs commits before the loop
  worked around it.

## What went well

- The build gate in the prompt did its job: no non-compiling code reached
  the PR, and the live round found a real result rather than assuming one.
- Codex round 2's `GetMatrix` finding was a genuine correctness bug with a
  real consumer.

## Lessons

The two entries run 1 earned were adopted in `docs/r3d/lessons.md` on
2026-09-26 with this run as their second data point. Not made a rule: "a
prompt-level gate works once; a script-level gate works every time" -
that is the loop's v2 design itself, recorded in
`docs/r3d/agentic-loop.md`, not a recurring mistake shape.
