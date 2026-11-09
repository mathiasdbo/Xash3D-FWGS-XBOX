# Xash3D-FWGS-XBOX (formerly Resonance3D) — fork of Xash3D-FWGS

This repository **is** Xash3D-FWGS, with a bounded set of changes to bring it
to the Original Xbox (2001) without losing the PC build.

Fork base: `c7768c48` (upstream/master, 2026-08-08).

## Why it exists

The goal is an Original Xbox running Half-Life at feature parity. The previous
project (`ferrum56/`, Rust) reimplemented the engine from scratch: it reached
~35,000 lines and was still far from parity. This fork starts from the
complete engine (~221,000 lines) and adds what the console needs.

What survives from the previous project is the one part that was genuinely
original: the offline baker, now in `tools/asset-studio/`.

See `migration.md` for what was archived and why.

## Structure

```
<root>              ← upstream tree, nothing moved
  engine/  ref/  filesystem/  public/  common/  3rdparty/  wscript  ...

deps/               ← OURS. Pinned SDK submodules (see below)
tools/asset-studio/ ← OURS. Offline C++/CMake baker that produces .mpak
docs/r3d/           ← OURS. Plan, divergence ledger, migration notes
```

### `deps/` — pinned SDKs

| Path | Repository | Role |
|---|---|---|
| `deps/hlsdk` | `mathiasdbo/halflife-updated-xash3d` | Half-Life game code. Carried over from Ferrum56 at the same commit |
| `deps/hlsdk-opfor` | `mathiasdbo/halflife-op4-updated` | Opposing Force |
| `deps/hlsdk-bshift` | `mathiasdbo/halflife-bs-updated` | Blue Shift — see the caveat in `fork-plan.md` F0 |
| `deps/nxdk` | `XboxDev/nxdk` | Xbox toolchain, libc, pbkit. **Upstream, not a fork** — fork it only when a patch is actually needed, then change the URL here |

Xash3D-FWGS normally loads game code as DLLs at runtime, so on PC these are
only needed to build those DLLs. On Xbox there is no dynamic loading, so the
game code is compiled into the single static image and these submodules become
part of the build.

The upstream tree stays **exactly where it is**. Everything of ours lives in
directories upstream does not use (`tools/`, `docs/r3d/`), so that
`git merge upstream/master` never conflicts over layout. This is not
cosmetic: it is what decides whether the fork can be kept current at all.

> `scripts/` and `Documentation/` ALREADY EXIST upstream. Put nothing of ours
> there.

## Remotes

```
origin    git@github.com:mathiasdbo/Xash3D-FWGS-XBOX.git
upstream  https://github.com/FWGS/xash3d-fwgs.git   (fetch)
upstream  no-push                                    (push disabled on purpose)
```

`origin` is a detached mirror, not a GitHub fork: a fork of a public
repository is always public and cannot be made private afterwards. The cost is
that pull requests cannot be opened against upstream from here; the benefit is
that the repository can stay private. Upstream history is intact, so
`git log`, `git blame` and cherry-picks all still work.

Pulling upstream changes:

```bash
git fetch upstream
git merge upstream/master        # resolve against docs/r3d/divergences.md
```

## Fork rules

They live in `/AGENTS.md`, at the root. The central rule inverts what the Rust
project said: there the job was to **replicate** the reference, here we **are**
the reference, and the job is to **minimise and document every divergence**.
Any deviation from upstream goes into the ledger (`divergences.md`) the same
day it is written, not afterwards.

## Status

Nothing ported yet. The tree is clean upstream plus the baker. The phased plan
is in `fork-plan.md`.
