# Menu loading investigation, 2026-09-27

Branch: `fix/xbox-menu-loading`, based on the release-enabled PR #153
revision `2ed0e17e`; the original branch is unchanged.

## Evidence

The initial investigation attributed a grey screen to ref/nv2a's texture
free path. That attribution was not established. The later investigation
stopped at `Sys_Error` (`0x000f89b0` in its own link), called from
`__Mem_Alloc+0x255`. The captured stack included `FS_LoadFileFromArchive`,
`FS_LoadSound`, `S_EndRegistration` and `CL_PrecacheResources`. Physical
memory at that stop had only nine available pages (0.04 MiB). These are
historical observations from the September 27 investigation, not addresses
to reuse against another build.

The emulator's `dvd_path` still selected
`agentic-x9-x-menu-texture-unload-fix4.iso` while the test repacked `hl.iso`.
After correcting the disc selection, the investigation recorded successful,
image-verified release-enabled loads: c1a0 with 19.68 MiB free and disposal
with 6.55 MiB free. No ref/nv2a change was needed to obtain those results.
The later screenshot sequence also shows fonts and the menu after returning
from a map. The old report remains a historical record, not current status.

Before this follow-up build, the running release-enabled image was checked
again: all 6,698,499 file-backed read-only bytes matched its linked PE, with
zero mismatches and zero unreadable bytes. Emulator launches for this fix
use an explicit `-dvd_path` and a separate configuration copy. Never infer
the loaded disc from the file most recently packed.

## Changes and limits

- Keep the release-enabled menu lifecycle. Font atlases and font-file data
  are released before a map, and restored on demand. Background textures
  are similarly released and restored.
- Restore fonts before ellipsis measurement as well as glyph measurement.
  Otherwise the first string after release gets a zero ellipsis width.
- Process background preference changes before restoring released art.
  A failed reload of the old profile must not prevent choosing another.
- Correct the changelog, design note, plan and divergence #127: neither a
  proven renderer deadlock nor disabled release describes the current code.

Pause restores fonts. Resuming retains them until another map loads; this
change does not promise the full saving throughout gameplay. Compressing
files on disc alone would not reduce the current RGBA32 menu texture
storage; a GPU-format change would need separate image-quality and memory
measurements. No such change is included here.

## Verification

- Engine compile sweep matched its existing 29-failure platform baseline.
- Xbox mainui compile: 86 attempted, 86 passed, zero failures.
- Xbox static Half-Life link passed: server, client, freevgui and mainui,
  with zero duplicate or undefined symbols. XBE SHA-256:
  `13e081716bfdb6c015a51b0528bbbb3d4a74e34dab021231728771f17d6fdc0f`.
- Fresh xemu boot: all 6,698,467 read-only image bytes matched the linked
  executable. Main menu and c1a0 pause screen screenshots were inspected:
  readable fonts, menu art and no grey blank screen. c1a0 reached map-ready
  and advanced 74 frames over four seconds; 15.11 MiB physical memory free
  before pausing, 11.79 MiB after resuming (restored fonts remain resident).
- Host regression tests passed (arena, skin cache, printf). Desktop MinGW
  compilation of the two changed translation units passed; a full PC
  engine build/boot has not been performed.
- Memory and reference specialists reviewed the source. The conclusive
  adversarial mainui review exited zero and approved without findings.
  An earlier outer-tree review exited nonzero and is not counted as a pass.
  The standard review wrapper could not locate the plugin under the sandbox
  profile; its installed companion was invoked directly with project focus.
- Return to the main menu restored readable fonts and background art
  (`menu-return.png`, inspected).
- c1a0 -> disposal did NOT pass the readiness check: map-ready stayed zero
  for 60 seconds, despite active message processing and 7.11 MiB free.
  This remains an unresolved renderer/map-transition limitation; it is not
  evidence of a font restore failure or a successful disposal load.
- Final expanded XISO fresh boot passed full executable identity (6,698,467
  matching read-only bytes). c0a0 reached renderer readiness and advanced
  92 frames over three seconds. Its 124 baked archives include c0a0 and
  training; the unbaked rocket_frenzy loose BSP is not included. This is a
  smoke test, not validation of every packaged map.
- Real Xbox boot and DMA/memory behavior: unverified; this does not close
  a hardware phase.
