# Xbox bring-up plan

Modelled on [xashPS3](https://github.com/Mayo1970/xash3d-fwgs)'s goal stack,
which is the only ordering for this work validated end to end on real console
hardware — it reached a full campaign at a locked 60 FPS.

Their sequence is not the obvious one, and reading it changed four things
here. Those are marked **[from PS3]**.

Each phase is a branch, with micro-commits, specialist review and a merged PR
before advancing (see `/AGENTS.md`). Divergences go into `divergences.md` the
same day.

## What their ordering teaches

**Get logs off the box before anything else.** Their goal 1 was toolchain
bring-up *and* UDP debug logging, together. A console has no stdout to read.
Without a log channel every later failure is silent, and silent failure on
unfamiliar hardware is the expensive kind.

**Prove video outside the engine.** Their goal 4 built RSX video
infrastructure as a **standalone tool**. A triangle on screen from a 200-line
program separates "does the GPU work" from "does the engine work". Inside the
engine you debug both at once.

**Wire the software renderer to the display before writing the hardware one.**
Their goal 5 put `ref_soft` on screen; the RSX `ref_gl` did not arrive until
goal 11, six goals later. This is the item this plan had wrong. `ref/soft` is
17,422 lines that already exist and are already correct — putting it on the
framebuffer validates the filesystem, BSP loading, PVS, entity handling and
the frame loop **before** a single pushbuffer command is written. The hardware
backend then replaces a known-good picture instead of producing the first one.

**Stub the game module first.** Their goal 6 ran the main loop against a stub
client, decoupling engine bring-up from SDK bring-up. Two hard problems, one
at a time.

## Where Xbox differs from PS3

**Easier:**

- **Endianness is free.** x86 little-endian, same as the host. Their goal 13
  was a whole endianness audit across networking, demos, saves and HPK. We
  skip it.
- **Same architecture as the host**, so much numerical behaviour matches the
  PC build by construction.

**Harder:**

- **The linker.** Their static linking worked because PPC/ELF suits
  `scripts/waifulib/xshlib.py`. Ours does not — though **not for the reason
  this document gave until 2026-09-05**, see `x0-probe-results.md` §4. The
  `-melf_i386` in `xshlib.py:49` is appended only `if '-m32' in LINKFLAGS`,
  and nxdk never passes `-m32`, so `ld -r` would run with *no emulation flag
  at all*, which is a subtler failure than an ELF/COFF mismatch. What
  genuinely breaks: `objcopy` does not exist in nxdk, `llvm-objcopy` rejects
  `-G` on COFF, and `lld -flavor link -r` cannot do partial links. Their goal
  2 was solved; ours is open with three known fallbacks.
- **The GPU.** They wired `ref_gl` to the RSX, a G70 derivative with real
  shaders. The NV2A is fixed-function T&L plus VP20 and register combiners.
  There is no `ref_gl` path for us — X9 is a new backend, not an adaptation.
- **Memory.** The PS3 has 256 MB XDR plus 256 MB GDDR3. The Xbox has **64 MB
  total, shared with the GPU**. They still hit memory pressure hard enough to
  cap listen servers at four players. We have a quarter of their budget.

---

## What nxdk provides, and what it does not

Checked in `deps/nxdk`, because the plan should not assume anything that is
not there.

**There is no OpenGL.** None, at any version, in any wrapper. `pbkit` is raw
NV2A register access — `lib/pbkit/nv_regs.h` and `nv_objects.h` are register
and object definitions, not a graphics API. Upstream's `3rdparty/gl4es`
translates GL to GLES and there is no GLES here either. `ref/gl` has no path
to this hardware, and `ref/nv2a` is written against the hardware directly.

**But nxdk vendors SDL2**, as a first-class submodule alongside `SDL2_image`
and `SDL_ttf`, with working samples for video, `sdl_gamecontroller`, images
and TrueType text. It also provides `hal` (video, audio, Xbox APIs), `usb`,
`net` (lwIP, with an httpd sample), `xaudio`, `libjpeg`, `libpng` and `zlib`.

That matters because **the engine already has a complete SDL2 platform
backend**: `engine/platform/sdl2/` is 2,980 lines across `host_sdl2.c`,
`vid_sdl2.c`, `in_sdl2.c`, `joy_sdl2.c`, `s_sdl2.c`, `sensor_sdl2.c` and
`sys_sdl2.c`. If nxdk's SDL2 port is functional, most of `engine/platform/
xbox/` does not need writing — video surface, events, controller and **audio**
come from code that already exists and is already maintained.

The honest caveats, none of them settled:

- **SDL2 video on Xbox is a software framebuffer, not a GL context**, and X5
  does not use it either — see X5. Coexistence with pbkit is **not** an open
  question: the application owns the mode, arbitration is `PCRTC_START` via
  `pb_show_front_screen()` / `pb_show_debug_screen()`, and it ships as an
  upstream sample. Initialise SDL without `SDL_INIT_VIDEO` and call
  `pb_show_front_screen()` after every `pb_init()`.
- **SDL2 costs memory and binary size** on a 64 MB budget. Measure before
  committing to it as permanent.
- **Its completeness here is unverified.** The submodules are not initialised.
  Verifying it is an early, cheap task with a large payoff either way.

If SDL2 audio works, **X8 may largely disappear** — `s_sdl2.c` already exists,
and the audio abstraction described there becomes a nice-to-have for upstream
rather than a prerequisite for us.

---

## X0 — Toolchain and a log channel

**[from PS3]** The log channel ships with the toolchain, not after it.

- nxdk in the waf build: root `wscript`, toolchain
  `clang -target i386-pc-win32 -march=pentium3 -fuse-ld=nxdk-link`.
- **Answered per subsystem** — `x0-probe-results.md` §1. What remains is an
  enumerated list, not an investigation:
  - Clone `3rdparty/library_suffix` and add an Xbox case to `build.h`.
    **Until this exists nothing in the tree compiles at all**:
    `common/defaults.h:20` includes it and every `XASH_*` macro comes from it.
    It is a third-party submodule, so plan for a fork.
  - Plumb `-DXBOX` into every engine TU; add a `sys/types.h` shim; patch the
    five hint constants; pick a `DEST_OS` that is not `win32`; add
    `VIDEO_XBOX`/`INPUT_XBOX` to `common/backends.h:18-23` and a branch to
    `common/defaults.h:32-53`; drop `SDL_INIT_VIDEO` from `sys_sdl2.c:130`.
  - `public/miniz.c` does not compile for nxdk — `<sys/stat.h>`, `errno_t`,
    `localtime_s`, and `_MSC_VER` steering it into the Win32 branch.
  - ~~**Build nxdk on the dev host.**~~ **Done 2026-09-05.** 19 libraries,
    7.1 MB, including libSDL2 (1.5 MB), libpbkit, libpdclib, libnxdk_hal,
    libSDL_ttf and libc++. Recipe in `tools/xbox-env.sh`; single-TU
    compiles go through `tools/xbox-cc.sh`.
  - ~~**Prove the toolchain reaches an XBE.**~~ **Done.** nxdk's `hello`
    sample builds end to end — `nxdk-cc` → `nxdk-link` → `cxbe` →
    `extract-xiso` — producing a 102,400-byte `default.xbe` and an ISO.
- **A working log channel before anything else** — nxdk's debug channel, then
  network logging, so failures on hardware are readable.
- **A ruler in the same channel.** Per-subsystem `rdtsc` counters and a
  frame-time line, here rather than in X11. Numbers are logs. Every claim in
  `optimization-transfer.md` is a hypothesis until a phase that draws prints
  one, and until 2026-09-06 this plan had the instrument scheduled last.
- XBE packaging with `cxbe`.


### Networking is parked, deliberately

Six files — `net_ws.c`, `net_chan.c`, `xrcon.c`, `cl_steam.c`, the two
`http/` files — do not compile, and are being left that way.

Both routes were priced:

**`XASH_NO_NETWORK=1`** selects `engine/platform/stub/net_stub.h`
(`net_ws_private.h:17`), which is upstream's own no-network mode and the
direction X3 takes anyway. It is not self-contained: the stub declares the
socket structures opaquely, while `net_ws.c` uses `sockaddr_in6`,
`sockaddr_storage` and `in6_addr` **by value**. On Linux the real headers
are present regardless of the flag, so upstream never hits this. On a
platform with no BSD headers at all, it does not build.

**Routing through nxdk's lwIP** would give the real types — it vendors lwIP
with a `compat/posix` layer carrying exactly those structs. But nxdk ships
no `lwipopts.h` and no `arch/cc.h`; its `lib/net/Makefile` compiles the
sources and leaves the port layer to the application. Writing one is
perhaps a hundred lines, and it would be written for code X3 deletes.

So: parked, not forgotten. Two things worth doing later, in this order:

1. **Complete upstream's stub** so `XASH_NO_NETWORK` is self-contained.
   That is a small, generic patch that helps any platform without BSD
   headers, and it is the kind of thing FWGS would plausibly take. It also
   makes X3's trim trivial instead of surgical.
2. Only if the console build ever wants real networking, write the lwIP
   port layer.

**Exit:** an XBE that boots in xemu, prints, and exits cleanly.

## X0.5 — Root directory detection

**A blocker that was not in this plan.** `SDL_GetBasePath` returns NULL on
nxdk and `getcwd` does not exist, so `filesystem_engine.c` cannot determine
the root directory and the engine `Sys_Error`s before X2 begins. A small
patch, but a hard gate, and it lived nowhere until now.
See `x0-probe-results.md` §1.5.

### The exit above is NOT sufficient. Found in review, 2026-09-06.

Finding the root directory is half of it. `FS_InitStdio` then mounts
everything through **relative** paths, and nxdk has no current directory to
resolve them against:

- `FS_SetCurrentDirectory` is one of the ten `!XASH_XBOX` widenings in
  divergence #6, so Xbox falls into `filesystem/sys.c:511`, which prints
  "not implemented, ignoring..." and **returns true**. `compat/xbox`'s
  `chdir()` — which fails honestly with `ENOSYS` — is never reached. The one
  place in the ledger where "compat/xbox supplies what that branch needs" is
  false.
- `FS_InitStdio` then uses the literal `"./"`: `FS_ValidateDirectories` at
  `searchpath.c:623`, `FS_AddGameDirectory` at `:649`, `listdirectory` at
  `:677`.
- `FS_AddGameHierarchy` builds every real searchpath relatively too —
  `"valve/"` (`:378`), `_downloaded` (`:374`), `_hd` (`:386`), `_addon`
  (`:392`), `_lv` (`:398`), the language dir (`:404`), `custom` (`:410`).
  `fs_rootdir` is consumed at `dll.c:78,83`, `gameinfo.c:487` and
  `searchpath.c:507,851,952`, and **never to prefix a searchpath**.

Every nxdk path API resolves against `ObDosDevicesDirectory()` with no CWD in
the stack: `CreateFileA` (`fileio.c:32`), `GetFileAttributesA`
(`filemanip.c:26`), `FindFirstFileA` (`findfile.c:74`). So `opendir("./")`
builds `".\*"`, nxdk splits it into dirPath `".\"` + mask `"*"`
(`findfile.c:51-63`) and `NtOpenFile`s `"."` as a DOS device name, which it is
not. `STATUS_OBJECT_NAME_NOT_FOUND`.

The consequence is worse than "no pak0.pak": `FS_ParseGameInfo` never reads
`valve/gameinfo.txt`, `FI.numgames` stays 0, and the engine has no game to
run. Handing it an absolute `rootdir` does not fix it, because the searchpaths
built from that point on are relative regardless.

**Two ways out, and the choice belongs to this phase:**

1. **Resolve a process-wide CWD inside `r3d_native_path`.** One place, no
   upstream edit, and `compat/xbox/xbox_posix_io.c:615-617` currently argues
   against it ("fail rather than pretending to succeed"). That argument was
   written about `chdir` as an API, not about path resolution, and it is worth
   revisiting: an emulated CWD is a lie only if something can observe the
   difference, and on a platform with exactly one process nothing can.
2. **Make `filesystem/` prefix searchpaths with `fs_rootdir`.** Correct
   everywhere, larger diff, and it touches a file upstream changes often.

Route 1 is smaller and reversible; route 2 is the one that could go upstream.
Neither is written yet.

**Exit:** the engine finds its game directory on Xbox **and mounts it** —
`FI.numgames > 0` and `pak0.pak` opens.

## X1 — Static linking proven

### MEASURED, 2026-09-06: ten duplicate symbols

The measurement this phase existed to demand has been run. 157 objects —
every non-networking, non-test translation unit in `engine/`,
`filesystem/`, `public/`, `common/` and `compat/` — linked with the exact
`nxdk-link` flags plus `-errorlimit:0`, **no `ld -r`, no objcopy**:

```
_FI              _FS_LoadFile       __Mem_Alloc
_FS_Close        _FS_LoadGameInfo   __Mem_Free
_FS_FindLibrary  _FS_Open
_FS_LoadDirectFile  _FS_Search
```

**Ten.** All of them one boundary: the filesystem module defines the FS API,
and `engine/common/filesystem_engine.c` re-exports the same names as thin
wrappers. `_FI` is the known struct-versus-pointer collision
(`filesystem/filesystem.c:30` against `filesystem_engine.c:41`), and the
`_Mem_*` pair is `zone.c` against the filesystem's own allocator.

A first run reported 21. Eleven of those were `_main`, from
`filesystem/tests/` and `public/tests/` — unit-test programs that have no
business in an engine link at all. Worth recording because it is the sort of
number that gets quoted: the real figure is ten, and the difference was a
glob, not a problem.

**This is design-stage option 1 + 4, the cheapest branch.** Ten names is
`llvm-objcopy --redefine-sym` territory — the one symbol option that works
on COFF — or ten `#define`s. `xshlib.py` is bypassed entirely on Xbox: no
`ld -r`, no `objcopy -G`, no GNU binutils in the toolchain, none of the
COMDAT-folding hazard the earlier probe found. The largest risk this project
recorded is retired, and it was retired by measuring rather than by
building the machinery to survive it.

**Not yet known:** the link reported zero undefined symbols, which cannot be
true — no SDL library, no game module and no nxdk libraries were on the
command line. lld almost certainly stopped at the duplicates. The undefined
picture is a separate measurement, and it belongs to the real link, not this
probe.

**Still the gate, no longer the top risk.** Downgraded from "this may not
work at all" to "three known fallbacks, one needing no new tools" — but the
measurement that would close it has never been run.

Static linking is **mandatory**, not preferred: nxdk's `LoadLibraryExA`
unconditionally returns NULL (`deps/nxdk/lib/winapi/libloaderapi.c:16-23`) and
`SDL_LOADSO_DISABLED 1`. There is no dynamic path for anything.

**Measure before choosing.** Compile `engine/`, `filesystem/` and `ref/soft/`
with the real `nxdk-cc` line, archive `public` with `llvm-lib`, link with the
exact `nxdk-link` flags plus `-errorlimit:0`, **no `ld -r`, no objcopy**, and
count the genuine duplicates. Nobody has done this; every figure so far came
from synthetic modules or from linking one object twice.

- A handful of real conflicts → `llvm-objcopy --redefine-sym`, the one symbol
  option that works on COFF, and `xshlib.py` is bypassed on Xbox entirely.
- A large structural list → GNU `ld -r -m i386pe` run **twice** (one pass
  folds duplicate COMDATs but leaves a dangling undefined that binds silently
  to the engine at final link, exit 0, no diagnostic), then `objcopy` with
  `.data` included in `--set-section-flags`, then the missing `_` prefix at
  `xshlib.py:36`.

One genuine conflict is already known: `fs_globals_t FI;`
(`filesystem/filesystem.c:30`) against `fs_globals_t *FI;`
(`filesystem_engine.c:41`) — struct versus pointer, same name.

Also required here: **`exports.txt` does not exist for `ref_soft`, `menu`,
`server` or `client`** — only `filesystem/` and `ref/gl/` have one, and
`xshlib.py:86` reads it unconditionally, so `--static-linking=ref_soft` fails
today even on Linux. And note `XASH_STATIC_LIBS` implies
`XASH_ALLOW_SAVERESTORE_OFFSETS`, which makes the **save format depend on link
layout**.

Settle here which HL SDK base compiles under nxdk. **[from PS3]** Their goal
10 used `FWGS/hlsdk-portable`, not the `halflife-updated` family pinned in
`deps/`. Evidence, not proof, but it points one way.

**Exit:** engine, filesystem, menu and a stub game module link into one XBE.

## X2 — Filesystem and headless boot

**Bigger than it looked.** The `filesystem/` module rests on POSIX that
nxdk does not have. Measured by compiling it: `fcntl.h` is included 8
times across `filesystem/`, `sys/types.h` 7, `sys/stat.h` 7, `unistd.h` 5.
Of those nxdk supplies only a `stat.h` declaring `stat`/`fstat`; its
`unistd.h` is an empty licence stub. There is no `io.h` and no `_commit`,
both of which `io.c` reaches for under `XASH_WIN32` — which is true for
us, since `XASH_XBOX` is a sub-platform of it.

**Strategy: a compat include directory, not `#ifdef`s.** Put `fcntl.h`,
`sys/types.h`, `sys/stat.h` and `io.h` shims in a directory of ours, added
to the Xbox include path only. Most forward to the flat headers nxdk
already ships under `xboxrt/libc_extensions`; the rest supply `O_*`
constants and a few underscore-prefixed CRT functions. That keeps this on
the cheapest rung of the divergence ladder — new files in our own tree,
zero edits to upstream — instead of dozens of guards through a module
upstream actively maintains.

- `filesystem/mpak.c` as a `searchpath_t` (7 pointers, modelled on `pak.c`),
  loading **per resource** through `pfnLoadFile`'s custom `pfnAlloc`/`pfnFree`.
- Read from the Xbox HDD or a mounted ISO.
- **[from PS3]** Headless mode: boot, mount, load a map's data, run server
  frames, no video at all. Asset-path bugs found here are found without a
  renderer in the picture.

**Exit:** the engine loads a map from `.mpak` and runs the server loop with
`ref/null`.

## X3 — Memory budget

Earlier than the PS3 ordering, because 64 MB is a quarter of their budget and
finding the ceiling late is not survivable.

- `tools/asset-studio` emits a per-archive memory manifest and **fails the
  build** when `global + max(map)` exceeds the declared budget.
- `zone.c` gains a per-pool cap and a high-water mark; attribution exists.
- **One ledger, two allocators**: NV2A textures need physically contiguous
  memory outside the normal heap, so a budget counting only the heap is blind
  to half of it.
- Lightmaps to RGB565 — 8 MB to 4 MB on the worst map, and `MpakHeader.h:94`
  already reserves the field.

**Exit:** a declared budget, enforced at bake time, measured at runtime across
level transitions.

## X4 — Video bring-up, standalone

**[from PS3]** Outside the engine. A separate XBE that initialises pbkit, sets
a video mode, clears, and draws one textured triangle.

This is where the NV2A is learned: pushbuffer submission, DMA fencing in
unified memory, texture swizzling, and `fp20compiler`/`vp20compiler` from
`deps/nxdk/tools/`. Getting it wrong here costs an afternoon; getting it wrong
inside the engine costs a week of not knowing which half is broken.

**Exit:** a triangle, on hardware, from a program small enough to read in one
sitting.

## X5 — `ref/soft` on the framebuffer

**[from PS3]** The change their roadmap forced in this plan.

**Do not route this through SDL's window surface.** Write
`engine/platform/xbox/vid_xbox.c` — roughly 250 lines, modelled on
`vid_fbdev.c` — against `hal/video` at 16 bpp. That deletes the 1.2 MB shadow
surface, the per-frame `SDL_ConvertPixels`, and ~350 KiB of SDL software-2D
code from the image. Add double buffering, `XVideoWaitForVBlank`, and
`VID_Info_f`, which exists only in the `sdl2`/`sdl3` backends.

The render-resolution lever is `R_SaveVideoMode(640,480,320,240,…)`, not
`vid_scale`.

This puts a **complete and correct** image on screen — world, entities,
lightmaps, HUD — validating the filesystem, PVS, BSP loading, entity handling
and frame loop before any hardware renderer exists. Everything after compares
against a known-good picture instead of guessing.

It will be slow. That is fine; it is a reference, not a shipping path.

**Exit:** a recognisable Half-Life map on screen, rendered in software — and
its frame time on `c1a3` and `crossfire` at 640×480 and 320×240, from the X0
counters. This is the baseline X9 has to beat; without it, "the hardware path
is faster" is an opinion.

## X6 — Input and menu

- **Gamepad is free.** SDL's controller layer works on nxdk through its own
  OHCI/XID stack, with a `"default,Original Xbox Controller,…"` mapping
  compiled in for every GUID, so no `gamecontrollerdb.txt` needs shipping.
  Rumble works.
- **Keyboard and mouse do not exist, and are not needed.** `XBOX_PumpEvents`
  is an empty function and `in_sdl2.c` compiles inert. Nothing crashes; it
  simply produces nothing.
- Sequence the menu half **after** X1 picks a mechanism — `3rdparty/mainui` is
  C++ and multi-TU, which is exactly where the `ld -r` hazard bites.
- `3rdparty/mainui` compiled in. **[from PS3]** Their goal 7 had the Half-Life
  menu visible before input worked, which confirms the menu is cheap and makes
  a good early validation checkpoint.
- Gamepad navigation per `ui-architecture.md` §7.

**Exit:** the menu is navigable with a controller.

## X7 — Game code

The real game module statically linked, replacing X1's stub, on whichever SDK
base X1 settled.

`vgui_support` to a null stub: `engine/client/vgui/vgui.c:345` reports the
load failure and continues, and single-player Half-Life does not use VGUI.

**Exit:** a map playable in software rendering, with HUD and game logic.

## X8 — Audio

**Largely resolved.** `s_sdl2.c` works on nxdk's Xbox audio driver
(`XAudioInit(16, 2, …)`, `XAudioProvideSamples`, `XAudioPlay`). Two one-line
fixes and it is done:

- `SOUND_DMA_SPEED` is a compile-time `#define` of 44100
  (`engine/client/sound.h:27,29`) with no cvar escape, and the Xbox driver
  hard-forces 48000. That makes an `SDL_AudioStream` resample
  **unconditional** on a 733 MHz CPU. Set 48000 for this build, or pass
  `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE`.
- Voice capture cannot work — `HasCaptureSupport = 0`. Compile out
  `VoiceCapture_Init`.

The NVAPU/ADPCM backend and the `engine/client/sound/` abstraction come off
the Xbox critical path entirely. Keep them as an upstream nice-to-have.

**Exit:** positional sound and sentences.

## XB — Baker output for the NV2A

Between X5 and X9, and runnable alongside X6–X8: it touches only
`tools/asset-studio` and the PC build. Seven of the nineteen real items in
`xbox-performance-review.md` §3 are baker work, and until this phase existed
none of them had an owner.

**Two censuses first, because they decide the rest.** Both are one-minute
scripts over the retail assets, and both answer questions the plan had been
carrying as opinions:

- **Lightstyles.** How many faces per map carry `styles[1..3] != 255`. The
  baker bakes style 0 only (`BspParser.cpp:514-523`), so "RGB565 pages, no
  runtime conversion" currently means no light in the game switches or
  flickers on Xbox. The number picks between per-style pages combined at
  runtime, a baked *on* state with binary switching, or an accepted loss —
  and the choice is written down here, not arrived at.
- **Bones.** Which retail models reference more bones per mesh than a VP20
  palette holds — 192 constants (`deps/nxdk/tools/vp20compiler/config.h:11`),
  three per bone, roughly 50 after the view-projection and light constants,
  against `MAXSTUDIOBONES 128`. Every model over the line needs partitioning
  by bone subset; the code must exist for the ones that do.

Then the format, decided once:

- **Vertex layout** the NV2A reads natively: FLOAT, normalised SHORT,
  D3DCOLOR. No integer attributes. `base_vertex`-chunked u16 indices — the
  decision the Rust port already made, carried over rather than re-derived.
- **Draw lists per cluster, per material**, so runtime submission is a walk in
  baked order. Per leaf alone explodes the draw count, and on pbkit every
  submission is CPU time writing pushbuffer words into write-combined memory.
- **Lightmap pages in RGB565**, with whatever the census decided.
- **Bone partitioning** for the models the census named.
- **DXT laid out linear by block row.** `swizzleNV2A`
  (`TextureOpt.cpp:110-132`) applies a Morton order to DXT blocks, which is
  wrong for `D3DFMT_DXT1` on this hardware. Verified against a known-good
  surface, not reasoned about.
- **Post-T&L cache ordering** of indices. Free offline, invisible to the
  engine.

**The exit below was wrong as first written. Found in review, 2026-09-07.**
"`ref/gl` renders the baked render-half" cannot mean what it sounds like it
means, because `ref/gl` has no consumer for most of what this phase bakes.
Its default world path is `glpoly2_t` fans drawn per polygon
(`R_RenderBrushPoly`, `gl_rsurf.c:1479`) built from `surf->samples`
(`GL_BuildLightmaps`, `:3898`); its one indexed, batched path is `R_DrawVBO`,
which §1 of `xbox-performance-review.md` says not to port — it is off by
default and not a base to build on. `ref/gl` has no concept of "per-cluster,
per-material draw list" or "post-T&L cache order" at all: it walks its own
texture chain every frame regardless of what order anything was baked in.

So there are two different things to validate, and only one of them is
`ref/gl`'s job:

1. **Data correctness — is what got baked right?** Vertex positions, UVs,
   lightmap texel values, the lightstyle policy's output, texture and bone
   assignment. **Corrected 2026-09-07 — the wrong layer was named.**
   `filesystem/mpak.c` is a `searchpath_t` VFS shim (`divergences.md:66`),
   raw bytes in, raw bytes out, the same job `pak.c` does for `.pak` — it
   does not touch `glpoly2_t` or build anything. The reconstruction is the
   render-half loader rewrite `divergences.md` already anticipates
   (`Mod_LoadVertexes/Edges/SurfEdges/Surfaces/Textures/Lighting`, six
   functions in `mod_bmodel.c`, high merge risk): those six read the bytes
   `mpak.c` hands them and populate the same `msurface_t`/`glpoly2_t`
   structures the loose-folder path does, which `ref/gl` then consumes
   unmodified via `GL_BuildPolygonFromSurface` (`gl_rsurf.c:381`) and
   `samples` (assigned at `mod_bmodel.c:3952`). A pixel-compare against the
   loose-folder path is a real, meaningful test of those six functions, not
   of `mpak.c`.
2. **Draw-order correctness — is the per-cluster, per-material list and the
   index chunking actually right for pushbuffer submission?** `ref/gl` cannot
   exercise this; nothing in this codebase can except a pushbuffer consumer.
   That consumer is `ref/nv2a`, and it does not exist until X9.

**Exit, split honestly:** `ref/gl` on PC renders a map from the baked
render-half — vertex, UV, lightmap and material data — pixel-compared against
the loose-folder path. This validates (1). It does **not** validate the
per-cluster/per-material draw-list order, the `base_vertex` index chunking, or
the post-T&L cache ordering — those are unverified until X9 builds their only
consumer, on hardware, and X9's exit criterion is where that verification
actually happens. The D4 principle (the console path runs first on the machine
with a debugger) holds for the data; it does not hold for the submission
shape, and this document should not have implied it did.

## X9 — `ref/nv2a`

The hardware renderer, replacing X5's software path against a picture already
known to be correct, consuming the format XB decided.

- Per-map atlas instead of texture arrays — the baker already produces it.
- Vertex format with no integer attributes: FLOAT1-4, D3DCOLOR and normalised
  SHORT only.
- Register combiners for `diffuse × lightmap × 2` and native alpha test.
- Matrix-palette skinning in VP20, within the 192-register palette, on meshes
  XB has already partitioned.
- **Memoised bone setup.** The reference is eager every frame
  (`R_StudioSetupBones`, `gl_studio.c:815`); this is new work, and it is the
  half of the skinning cost VP20 does not take.
- **Dynamic lighting — two small items, not a redesign.** The reference has no
  per-pixel lights (`xbox-performance-review.md` §1.3): studio models are
  per-vertex with `MAX_LOCALLIGHTS 4`, so raise to the eight the T&L unit
  gives; world dlights are lightmap-space sub-uploads, so measure linear
  (`LU_IMAGE_R5G6B5`) against swizzled pages and pick by number. The "real
  variance" this plan used to name here lives in XB's lightstyle policy.
- `r_alphasort`'s job is already done by `R_TransEntityCompare`
  (`gl_rmain.c:86`); keep that order when the entity list becomes a baked
  walk.
- ~~Fix `swizzleNV2A` on DXT blocks (`TextureOpt.cpp:110-132`)~~ **Moved to
  XB, 2026-09-07: this line duplicated XB's DXT block-row fix and left the
  plan asserting two owners for the same bug.** `TextureOpt.cpp` is
  `tools/asset-studio`, the PC baker — X9 never touches it, and XB already
  lists this fix. What belongs here instead: verify on hardware that XB's
  fix produced a correct swizzle, since xemu is not validation for this.
- Aspect ratio: implement `R_SetDisplayTransform`'s separate `scale_x`/
  `scale_y` and feed a display aspect rather than pixel dimensions.
  `V_AdjustFov` (`cl_view.c:252`) returns early on 640×480, which is exactly
  wrong for anamorphic widescreen. See `ui-architecture.md` §8.

**Exit:** a map drawn on the NV2A, compared frame by frame against X5's
software output and the PC capture — and a hardware frame time on `c1a3` and
`crossfire` against X5's baseline. This is also, per XB's corrected exit
criterion, **the first time the per-cluster/per-material draw-list order, the
`base_vertex` index chunking and the post-T&L cache ordering are verified at
all** — nothing before this phase has a consumer for that shape. Treat the
first hardware run of this phase as a validation of XB's baked format, not
only of the backend, and be ready for the bug to be in the bake.

## X10 — First playable

Full campaign start to finish on real hardware, with HUD, audio, input and
level transitions.

**Exit:** someone finishes Half-Life on an Xbox.

## X11 — Performance hardening

**[from PS3]** Their goal 14 is where 720p and a locked 60 FPS came from, and
several of its findings were transcription bugs and undersized buffers rather
than algorithmic problems. Expect the same shape.

- The counters come from X0 and the baseline from X5. This phase is what
  remains after X9, not the first time anyone measures.
- Measure on `c1a3` and `crossfire`, not `c0a0` — one of the emptiest maps.
- The Rust port's lessons: no allocation in the frame loop; no unconditional
  logging on a serial-console platform. (Memoised bones moved to X9.)
- The rows of `xbox-performance-review.md` §3 that no earlier phase owns:
  the transient-buffer allocation audit; threaded save and load I/O, which
  transfers because the hitch is I/O-bound, not because there is a second
  core; `R_CullSurface` backface rejection against deliberate under-culling,
  measured both ways rather than ported on the strength of the reference
  doing it; and SSE intrinsics only for a loop a hardware profile has named.
- Memory audit: per-map peak, fragmentation after repeated `changelevel`.

**Exit:** a documented frame budget and peak memory figure on real hardware.

---

## Out of scope

- **VGUI1 widgets** — null stub until multiplayer UI is wanted, then adopt
  `FWGS/freevgui` and write only the Surface backend.
- **VGUI2** — its own epic, after X10. It unlocks Counter-Strike, Day of
  Defeat and Blue Shift, and moves the first playable build no closer.
- **Ironwail's GPU-driven techniques** — Vulkan only, structurally.
- **Multiplayer on console.**

## Validation discipline

Borrowed from their AGENTS.md, and it is why their roadmap worked:

- One goal at a time. Do not implement ahead of the current phase.
- Do not speculate about hardware behaviour; confirm it.
- On failure, isolate to a minimal case rather than rewriting.
- Escalate after two failed attempts with diagnostics, not a new hypothesis.
- **xemu is not validation.** DMA timing, the serial channel and unified
  memory behaviour are not faithfully emulated.
