# X0 probe results: SDL2, video ownership, ref/soft and static linking

Output of a six-probe investigation with adversarial review, run 2026-09-05
against HEAD `1d5581b3`. Five probes survived review; **the static-linking
probe was refuted by all three reviewers** and the corrected version is in §4.

This supersedes the assumptions in `fork-plan.md` and `epics.md` wherever they
disagree. Corrections have been applied to those documents; this is the
evidence behind them.

---

**Decision document. Written 2026-09-05 against `resonance3d` HEAD `1d5581b3`.**

## 0. State of the tree, and what I could not check

Three facts bound everything below. State them first so nothing here reads as more settled than it is.

**Nothing in this repository compiles today.** `common/defaults.h:20` includes `build.h`. That file is not in the tree — `find . -name build.h` outside `deps/` returns nothing. It lives in `3rdparty/library_suffix` (`.gitmodules:43-45`, `wscript:109`), which `git submodule status` reports uninitialized (leading `-`, `e369abc23c…`) and whose directory is empty. Every `XASH_*` macro — `XASH_WIN32`, `XASH_SDL`, `XASH_LIB`, `XASH_VIDEO` — comes from that file. Any claim of the form "this branch is live on Xbox" is unverifiable in this checkout until that submodule is cloned. I worked around it for my own experiments by pointing `-I` at the sibling clone `G:/Proyectos/Ferrum56/xash3d-fwgs/3rdparty/library_suffix/include/build.h`, which `.gitmodules` says is the same upstream repo.

**`deps/nxdk` is fully cloned, clean, and currently unbuilt.** All eleven submodules show a leading space in `git submodule status`; `git -C deps/nxdk status --porcelain` is empty; `ls deps/nxdk/lib/*.lib` returns zero files. An earlier probe built the tree and then cleaned it up. So "nxdk is already built here" was true at one point today and is not true now.

**clang exists on this host** (`C:\Program Files\LLVM\bin\clang.exe`, 22.1.8), contrary to one probe's claim. I used it to run real experiments below rather than reasoning about them.

---

## 1. Verdict on SDL2

**`engine/platform/sdl2/` can host the port in part, not in whole. Split it by subsystem — the answer is different for each, and two of the five are outright broken.**

### 1.1 Video — usable for bring-up, must be replaced

`vid_sdl2.c` does **not** require a GL context. `R_Init_Video` branches on `ref_graphic_apis_t`: `case REF_SOFTWARE` only sets `glw_state.software = true` (`vid_sdl2.c:922-924`), while `SDL_GL_LoadLibrary` is reached only under `case REF_GL` (`vid_sdl2.c:932`). That flag gates the window flag (`if( !glw_state.software ) SetBits( flags, SDL_WINDOW_OPENGL );`, `vid_sdl2.c:672-673`) and context creation. The software presentation path is `SDL_GetWindowSurface` (`vid_sdl2.c:126`) → `SDL_LockSurface` → `SDL_UpdateWindowSurface` (`vid_sdl2.c:231`), which lands exactly on the three hooks nxdk's driver installs: `CreateWindowFramebuffer` / `UpdateWindowFramebuffer` / `DestroyWindowFramebuffer` (`SDL_xbvideo.c:129-131`). There are no GL hooks at all in that table (`SDL_xbvideo.c:124-133`).

One earlier probe claimed the engine's `SDL_GL_*` calls "do not exist on nxdk's SDL" and would fail to link. **That is wrong and was correctly refuted.** `SDL_GL_LoadLibrary` (`SDL_video.c:2867`), `SDL_GL_CreateContext` (`:3482`), `SDL_GL_SwapWindow` (`:3595`) are all defined unconditionally; the `#if SDL_VIDEO_OPENGL` guards are inside the bodies. GL fails at runtime with `"No dynamic GL support in current SDL video driver (%s)"` (`SDL_video.c:2881`), not at link time. GL is not the obstacle.

What *is* the obstacle is that SDL on Xbox is a passive consumer of a mode the application must set:

- `XBOX_SetDisplayMode` is `return 0;` — a no-op (`SDL_xbvideo.c:167-170`). `VID_SetScreenResolution` and `R_ChangeDisplaySettings` are inert.
- `XBOX_CreateWindow` overwrites `window->w/h` from `XVideoGetMode()` and forces `SDL_WINDOW_FULLSCREEN`, clearing `RESIZABLE`/`HIDDEN` (`SDL_xbvideo.c:71-79`). One window only (`:66-68`).
- The enumerable mode list is degenerate: `SDL_AddBasicVideoDisplay` (`SDL_xbvideo.c:155`) fills only `desktop_mode`/`current_mode`, and the only mode actually appended is a **zeroed** one (`SDL_zero(mode); SDL_AddDisplayMode(...)`, `SDL_xbvideo.c:159-160`). `vid_sdl2.c:273` rejects it on `mode.w < VID_MIN_WIDTH`, so `num_vidmodes` ends at **zero**, not one. The resolution menu is empty and `SDL_GetClosestDisplayMode` (`vid_sdl2.c:408`) has nothing but a 0×0 candidate to match.
- Every present costs a full-screen CPU convert out of a shadow surface: `SDL_CreateRGBSurface` at `SDL_xbframebuffer.c:52`, `SDL_ConvertPixels` into `XVideoGetFB()` at `:82,:94`, dirty rects ignored entirely. That is **1,228,800 bytes** of shadow RAM at 640×480×32 plus ~1.2 MB of CPU copy per frame, on top of the HAL's own framebuffer.

**Verdict: SDL video is adequate for X5 and disqualified for X9.** But see §3 — it is not even the right choice for X5.

### 1.2 Input — gamepad yes, keyboard and mouse do not exist

`joy_sdl2.c` works. nxdk drives the pad through its own OHCI/XID stack (`SDL_xboxjoystick.c:36-40`, `:182-201`), exposes 6 axes / 1 hat / 10 buttons (`:321-324`), and `SDL_gamecontroller.c:1036-1043` compiles in a `"default,Original Xbox Controller,…"` mapping for every GUID, so `SDL_IsGameController` (`joy_sdl2.c:384`) returns true with no `gamecontrollerdb.txt` shipped. Rumble works: I grepped the fork's headers and `SDL_GameControllerRumble` **is present**, so the `#if SDL_VERSION_ATLEAST( 2, 0, 9 )` guard at `joy_sdl2.c:330` is true. One earlier probe listed rumble as a 2.0.14 casualty; that was wrong.

`in_sdl2.c` and the keyboard/mouse half of `host_sdl2.c` are dead weight. `XBOX_PumpEvents` is literally `/* do nothing. */` (`SDL_xbevents.c:33-37`). No keyboard or mouse backend is compiled (`Makefile.xbox:4-35` has `video/xbox`, `audio/xbox`, `joystick/xbox` and nothing else). Joystick events still flow because `SDL_PumpEvents` calls `SDL_JoystickUpdate()` unconditionally after the driver hook. This is not a crash — `SDL_CreateSystemCursor` returns NULL safely (`SDL_mouse.c:966-976`), clipboard falls back in-process (`SDL_clipboard.c:28-66`) — it simply produces nothing. Treat `in_sdl2.c` as compiled-and-inert.

### 1.3 Audio — works, with one guaranteed CPU cost and one dead feature

`s_sdl2.c` is structurally fine. The Xbox driver hard-forces its spec (`SDL_xboxaudio.c:81-84`: `freq = 48000; format = AUDIO_S16LSB; channels = 2; samples = 1024`), and because `s_sdl2.c:129` passes `allowed_changes = 0`, SDL returns the *desired* spec in `obtained` and converts internally — so the `AUDIO_S16SYS` and 1-2 channel checks at `s_sdl2.c:137-146` still pass.

But `SOUND_DMA_SPEED` is `SOUND_44k` = 44100 (`engine/client/sound.h:27,29`), a compile-time `#define` with no cvar escape. So a 44100→48000 `SDL_AudioStream` resample on a 733 MHz Celeron is **unconditional**, not contingent. Fix is one line: either `SOUND_DMA_SPEED = 48000` for the Xbox build, or pass `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE`.

Voice capture cannot work. `s_sdl2.c:283` opens a capture device; `SDL_xboxaudio.c:164` sets `impl->HasCaptureSupport = 0`. Compile it out.

### 1.4 Timer and threads — required, and they work

`XASH_SDL >= 2` forces `XASH_TIMER TIMER_SDL` (`common/defaults.h:75-77`), making `SDL_GetPerformanceCounter`/`SDL_Delay` non-optional; `SDL_TIMER_WINDOWS 1` (`SDL_config_xbox.h:131`) supplies them via nxdk's `QueryPerformanceCounter`.

Threads are **not** optional and are used outside `engine/platform/sdl2/`: `engine/common/net_ws.c:24-26` includes `<SDL_thread.h>` and `:299-307` maps the engine's mutex and thread primitives onto `SDL_CreateMutex`/`SDL_CreateThread`/`SDL_DetachThread` whenever `XASH_SDL == 2`. `SDL_THREAD_WINDOWS 1` (`SDL_config_xbox.h:128`) covers it. This kills the "trim SDL to input+audio only" idea if it means dropping `src/thread`.

### 1.5 Filesystem — broken in both directions. This is a real X2 blocker.

`SDL_FILESYSTEM_DUMMY 1` (`SDL_config_xbox.h:137`) makes `SDL_GetBasePath()` return NULL unconditionally (`src/filesystem/dummy/SDL_sysfilesystem.c:31-34`). `FS_DetermineRootDirectory` uses it as the *primary* mechanism when `( XASH_SDL >= 2 ) && !XASH_NSWITCH` (`engine/common/filesystem_engine.c:282-283`).

The fallback is worse than the probes reported. With `XASH_WIN32 == 1` the code reaches `if( getcwd( out, size ))` at `filesystem_engine.c:301`, else `Sys_Error` at `:303`. **nxdk has no `getcwd`.** It ships `_getcwd` (`deps/nxdk/lib/xboxrt/libc_extensions/direct.c:97`, declared `direct.h:9`) and its `unistd.h` is a three-line license stub with no declarations at all (`deps/nxdk/lib/xboxrt/libc_extensions/unistd.h`, `wc -l` = 3). So both branches fail and the engine `Sys_Error`s before a single frame. This needs a patched branch in `filesystem_engine.c`, not configuration.

### 1.6 What it takes to make it compile — measured, not guessed

I ran the experiment the probes proposed but could not run. Predefined macros under `nxdk-cc`'s exact flags (`deps/nxdk/bin/nxdk-cc:7-24`):

```
clang -target i386-pc-win32 -march=pentium3 -ffreestanding -nostdlib -fno-builtin -DNXDK -D__STDC__=1 -dM -E -x c /dev/null
  → #define NXDK 1   #define _MSC_VER 1950   #define _WIN32 1   #define __i386__ 1
```

`XBOX` and `__XBOX__` are **not** defined. Three consequences, in the order they bite:

1. **`-DXBOX` is a hard requirement on every engine TU, and nothing supplies it today.** `SDL_platform.h:133,165-167` sets `__WIN32__` from `_WIN32` with no Xbox exclusion, so `SDL_config.h:32` (`#if defined(__WIN32__) && !defined(__XBOX__)`) selects `SDL_config_windows.h`, not `SDL_config_xbox.h`. Worse, `SDL_DISABLE_ANALYZE_MACROS` is defined only inside `SDL_platform.h:68-79`'s `#if defined(XBOX) || defined(__XBOX__)` block, so `SDL_stdinc.h:281-282` does `#include <sal.h>` — and `find deps/nxdk -iname sal.h` returns nothing. **I reproduced this**: syntax-checking `vid_sdl2.c`, `sys_sdl2.c`, `joy_sdl2.c` under nxdk flags without `-DXBOX` gives exactly one fatal error each, `SDL_stdinc.h:282:10: fatal error: 'sal.h' file not found`. Adding `-DXBOX` clears it. `-DXBOX` comes only from `deps/nxdk/lib/pkgconfig/sdl2.pc:7` and `Makefile.xbox:41,44` — so the waf integration must use the pkg-config branch (`scripts/waifulib/sdl2.py:109`) or add `-DXBOX` by hand. `--sdl2=deps/nxdk/lib/sdl/SDL2` takes the path branch (`sdl2.py:43-84`), never adds `-DXBOX`, and additionally appends an `i686-w64-mingw32` triplet on `DEST_OS == 'win32'` (`sdl2.py:66-71`) that does not exist in the nxdk tree. That experiment would have produced a confidently wrong answer.

2. **nxdk has no `sys/types.h` at all.** With `-DXBOX` applied, the next fatal error on all seven `sdl2/*.c` files is `common/xash3d_types.h:6: fatal error: 'sys/types.h' file not found` (or `engine/common/common.h:28` on the `XASH_WIN32` path). `find deps/nxdk -path "*sys/types.h"` returns nothing; nxdk's libc extensions are flat (`stat.h`, `wchar.h`, `direct.h` with no `sys/` directory). The engine needs `off_t` there — `typedef off_t fs_offset_t;` (`xash3d_types.h:48`). A shim header is required before any engine TU compiles. With a two-line shim in place, the SDL and libc layers pass and the remaining failures are ordinary engine include-path wiring.

3. **Five SDL hint constants are missing.** I grepped each against the fork's `include/*.h` (SDL **2.0.9**, `SDL_version.h:60-62`, `XboxDev/nxdk-sdl` at `2554902f7`). Zero hits for: `SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO` (`sys_sdl2.c:119`, unguarded), `SDL_HINT_VIDEO_X11_FORCE_EGL` (`vid_sdl2.c:912`, unguarded), `SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE` (`joy_sdl2.c:367`, unguarded), plus `SDL_HINT_WINDOWS_DPI_AWARENESS` (`sys_sdl2.c:112`) and `SDL_HINT_AUDIODRIVER` (`s_sdl2.c:102`), which fire because `XASH_WIN32 = 1`. `SDL_GetWindowSizeInPixels` is absent but correctly gated at 2.26. Five one-line patches or `#ifdef`s.

4. **`XASH_WIN32 = 1` drags in the whole Win32 platform layer.** `engine/wscript:173` globs `platform/%s/*.c % DEST_OS`; `:196-197` links `USER32, SHELL32, GDI32, ADVAPI32, DBGHELP, PSAPI, WS2_32` for `DEST_OS == 'win32'`. None of those import libraries exist in nxdk. The Xbox target must not be classified as `DEST_OS == 'win32'`, which means the backend directory must be named after whatever `DEST_OS` the toolchain sets — and `common/defaults.h:32-53` needs a new branch, since `XASH_VIDEO` is selected only from `XASH_SDL`/`XASH_LINUX`/`XASH_DOS4GW`/`XASH_PSP`, and `common/backends.h:18-23` has no `VIDEO_XBOX` enum.

**Bottom line for §1: adopt `joy_sdl2.c` and `s_sdl2.c` and the timer/thread dependency. Compile `in_sdl2.c` as inert. Do not adopt `vid_sdl2.c` (see §3). Patch `filesystem_engine.c`. Budget roughly a day for the `-DXBOX` / `sys/types.h` / five-hint plumbing — it is mechanical, but it is X0 work that the plan currently does not name.**

---

## 2. The video ownership question

**Viable. Not a dead end. The handover mechanism is explicit, documented in pbkit's own source, and shipped as a CI-built sample.**

Neither SDL nor pbkit owns the video mode — **the application does**. `XVideoSetMode` (`deps/nxdk/lib/hal/video.c:397`) is called from `main()` in 16 of the 17 samples and from **zero** places in `lib/sdl` or `lib/pbkit`. SDL reads the mode (`SDL_xbvideo.c:147`) and cannot change it (`:167-170`). pbkit reads it too (`pbkit.c:2870`) and allocates its own framebuffers, deliberately leaving the HAL's alone. The comment is unambiguous (`pbkit.c:2859-2864`):

> `//Even if it's a waste of memory, for now, we will leave the openxdk (& SDL) default frame buffer untouched. debugPrint (& SDL) will still target it. We will provide functions pb_show_debug_screen() and pb_show_front_screen() in order to let user (developper) toggle between screens at will.`

**The handover is one register, `PCRTC_START`, arbitrated by one flag.** `pb_show_front_screen()` points scanout at pbkit's buffer and clears `pb_debug_screen_active` (`pbkit.c:1914-1918`); `pb_show_debug_screen()` points it at `XVideoGetFB()` — the exact buffer SDL blits into — and sets the flag (`pbkit.c:1920-1924`). pbkit's per-VBlank flip is suppressed while that flag is set (`pbkit.c:811-812`). `pb_init` saves the HAL address (`pbkit.c:2345`) and `pb_kill` restores it (`pbkit.c:2173`). This is a designed handback, not a race.

**Concrete recipe:**

1. `main()` calls `XVideoSetMode(w, h, bpp, REFRESH_DEFAULT)` before anything else.
2. X5: engine uses the HAL framebuffer directly (§3). No pbkit.
3. X9: `pb_init()` at renderer init, `pb_show_front_screen()` immediately after, `pb_kill()` at shutdown. Never both renderers live.
4. SDL is initialised **without** `SDL_INIT_VIDEO`. `SDL.c:116-123,167` confirms `GAMECONTROLLER` implies `JOYSTICK` implies `EVENTS`, and never `VIDEO`; `SDL_PumpEvents` guards the video hook with `if (_this)` (`SDL_events.c:672-677`). `sys_sdl2.c:130` currently passes `SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_EVENTS` and must drop `VIDEO` for the Xbox build — note the engine demotes itself to `HOST_DEDICATED` on `SDL_Init` failure (`sys_sdl2.c:133`), so this must be an explicit change, not a hope.
5. `deps/nxdk/samples/sdl_gamecontroller/main.c` is exactly this shape and is built by upstream CI (`.ci_build_samples.sh:22-29`).

**Two things not to do.** First, do not plan on `XVideoSetFB((unsigned char*)pb_back_buffer())` to make SDL's surface composite into a pbkit buffer. It is publicly exposed (`hal/video.h:71`, `video.c:308-313`) and would work in principle, but it has **zero callers anywhere in the tree**, it writes `PCRTC_START` without setting `pb_debug_screen_active` so it fights pbkit's VBlank flip every frame, and SDL's `dst_pitch = vm.width * bpp` (`SDL_xbframebuffer.c:85`) diverges from pbkit's 64-byte-aligned tiled pitch at any width other than 640×32bpp. Second, `pb_debug_screen_active` is never reset by `pb_init` — it appears only at `pbkit.c:169,811,1917,1923,1929`. After a `pb_kill()`/`pb_init()` cycle following a debug-screen switch it stays 1 and flips stay suppressed. **Call `pb_show_front_screen()` explicitly after every `pb_init()`.**

IRQ contention is a non-issue for this shape: pbkit takes IRQ 3 (`pbkit.c:46`), hal/video takes IRQ 3 only lazily inside `XVideoWaitForVBlank` (`video.c:66`, `:471`) and returns without waiting if it loses the hook (`video.c:473-477`); audio is IRQ 6 and USB is IRQ 1. Nothing in SDL calls `XVideoWaitForVBlank`.

---

## 3. `ref/soft` to screen — cheapest path and per-frame cost

**Write `engine/platform/xbox/vid_xbox.c` against nxdk's `hal/video` directly. Do not route `ref/soft` through SDL.**

`ref/soft` renders into a 16-bit private buffer (`typedef unsigned short pixel_t;`, `ref/soft/r_local.h:84`) whose high byte is RGB332 and low byte holds interleaved low bits (`r_image.c:265-271`). It is not palettised. Presentation is one function: `R_EndFrame` → `R_BlitScreen` (`r_glblit.c:591`), which locks, runs `dst[i] = vid.screen[vid.buffer[i]]` through a 65536-entry LUT, and unlocks (`r_glblit.c:667-680` for the 2-byte case). Crucially the LUT is **rebuilt from whatever masks the platform declares** (`R_BuildScreenMap`, `r_glblit.c:347-407`) — `ref/soft` adapts to the platform, it does not demand a format. The platform contract is three functions: `SW_CreateBuffer` / `SW_LockBuffer` / `SW_UnlockBuffer` (`engine/platform/platform.h:371-373`, `engine/ref_api.h:477-479`). Only bpp 2, 3 and 4 have blit branches, so 8-bit output is impossible.

**Why not SDL:** at 640×480×32 the SDL route costs a 1.2 MB shadow surface plus a full-frame `SDL_ConvertPixels` on top of the LUT blit (`SDL_xbframebuffer.c:52,:94`), and the mode you get is whatever `main()` already set — so it is not "zero new code" either. The bare-HAL route removes both.

**The cheap path, concretely:**

- `XVideoSetMode(640, 480, 16, REFRESH_DEFAULT)` selects `VIDEO_R5G6B5` (`hal/video.c:339-341`).
- Pitch is exactly `width * bytesPerPixel` with no padding (`hal/video.c:332`), so `SW_CreateBuffer` reports `stride = width`, `bpp = 2`, masks `0xF800/0x07E0/0x001F`. `ref/soft` then writes native R5G6B5 with **zero conversion**.
- The framebuffer is contiguous `PAGE_WRITECOMBINE` memory (`hal/video.c:363-368`); `XVideoGetFB()` is the lock (`:303-306`); `XVideoFlushFB()` is a bare `sfence` (`:320-323`).
- `engine/platform/linux/vid_fbdev.c` (253 lines) is the in-tree template, including the five GL no-ops a non-GL backend must supply (`GL_SetAttribute`→0, `GL_GetAttribute`→0, `GL_GetProcAddress`→NULL, `GL_SwapBuffers`→`{}`, `GL_UpdateSwapInterval`) and the `if( type != REF_SOFTWARE ) return false;` at `vid_fbdev.c:57-58`. `ref/soft` itself needs no GL: `r_glblit.c:2` defines `APIENTRY_LINKAGE static` before including `../gl/gl_export.h`, whose `GL_FUNCTION` macro expands to function *pointers*, so the whole GL blitter is null pointers and `ref/soft/wscript:17-22` links no GL library. `GL_ClearExtensions` is the only GL entry called unconditionally and it is empty (`r_glblit.c:185-188`).

**Per-frame cost.** 307,200 iterations of {16-bit load, gather from a 128 KiB LUT (`vid.screen`, `r_local.h:103`), 16-bit store} = 600 KB of write-combined stores. **This has never been measured and it is the single number that decides whether X5 is a 15 fps reference or a 3 fps slideshow.** The LUT is large relative to any plausible L2 on a 733 MHz Celeron, so a cache-miss-per-pixel worst case is not absurd.

**The lever if it is too slow is not `vid_scale`.** `R_SetDisplayTransform` rejects any scale ≠ 1.0 (`r_glblit.c:36-41`). The real lever is `R_SaveVideoMode( w, h, render_w, render_h, maximized )` (`engine/client/vid_common.c:41-79`), which sets `refState.width/height` from `render_w/h` — and `refState` is what `GetRefAPI` hands `ref/soft` as `gpGlobals`. So `R_SaveVideoMode(640, 480, 320, 240, false)` renders at 320×240 with **zero `ref/soft` changes**, and the 2× expand goes in `SW_UnlockBuffer`. That also shrinks the surface cache, which is the biggest single allocation.

**Two costs the plan should carry explicitly:**

- **Tearing.** `XVideoInit` allocates exactly one framebuffer (`hal/video.c:363-368`, `_fb = framebufferMemory` at `:381`). Blitting into `XVideoGetFB()` races the CRTC. Double buffering is a second `MmAllocateContiguousMemoryEx` plus `XVideoSetFB()` (`video.c:308-313`, one register write) in `SW_UnlockBuffer`, paced by `XVideoWaitForVBlank()` (`video.c:471`). Cost: +600 KB at 16bpp. `vid_fbdev.c:175-197` is the model.
- **Memory.** Static `viddef_t` tables (`r_local.h:96-108`) = 2,621,440 B; `vid.buffer` 600 KB; z-buffer 600 KB; surface cache 3,227,648 B at 640×480 (`r_surf.c:778-786`, `r_local.h:615`). ≈ **6.8 MiB before map data**, ~10.6% of the 64 MB budget. `sw_surfcacheoverride` (`r_main.c:65`) tunes the largest term from a cvar. Also budget stack: `R_DrawBrushModel` and `R_EdgeDrawing` each declare ~256 KB of `edge_t`/`surf_t` arrays on the stack (`r_main.c:865-868`, `:971-975`) and they nest — ~512 KB live during a frame, against whatever `lld-link` defaults `-stack:` to.

**Two engine-side gaps that will stop the link before any of this runs:** `VID_Info_f` is referenced unconditionally at `engine/client/vid_common.c:223` and declared at `platform.h:376`, but defined **only** in `vid_sdl2.c:1163` and `vid_sdl3.c:207` — not in `vid_fbdev.c`, not in `vid_dos.c`. A new backend must supply it. And `ref/soft` must be statically linked, which is §4.

---

## 4. Static linking — the verdict

**The probe on this was REFUTED by all three reviewers, and correctly so. I am going with the corrected version, which differs from the probe on both the diagnosis and the prescription.**

### What was wrong in the probe

- **"`LD_RELOCATABLE_FLAGS=-melf_i386` forces ELF" is a misread.** `xshlib.py:47-50` appends `-melf_i386` only `if '-m32' in self.env.LINKFLAGS`. I grepped every `wscript` and every `scripts/waifulib/*.py`: `-m32` appears in the build system at exactly two places, `scripts/gha/build_linux.sh:21,24` (a Linux i386 cross build) and `xshlib.py:49` itself. On an nxdk target — `-target i386-pc-win32`, never `-m32` (`deps/nxdk/bin/nxdk-cc:8`) — the list stays empty and `ld -r` runs with **no emulation flag at all**, defaulting to whatever `find_program('ld')` picked. That is a different and more insidious failure. **`docs/r3d/fork-plan.md:124` and `docs/r3d/epics.md:135` both repeat this error and should be corrected**, since it is cited as the project's top risk.
- **"115 duplicate symbols from `public/miniz.c`" is a measurement artifact.** It came from linking the *same object* twice. `public/wscript:147` builds `public` with `bld.stlib`, and lld-link resolves archive members lazily — three modules pulling miniz through one `public.lib` produce zero duplicates. Separately, `public/miniz.c` does not compile for nxdk at all (`#include <sys/stat.h>` at `miniz.c:3068` against nxdk's flat `stat.h`; `errno_t`/`localtime_s` at `:3464`; `_MSC_VER` steering it into the Win32 branch). That is a real X0 finding, but it is not evidence about symbol collisions.
- **The "proven working replacement" was proven on a single-TU C++ module.** Two reviewers independently reproduced the failure: `ld -r -m i386pe` folds duplicate COMDATs across TUs but leaves a **dangling undefined** for the same mangled name beside the folded definition. While the definition is global, it resolves internally. After `objcopy -G` localizes it, the undefined binds to whatever defines that name at final link — the engine — with **link exit 0 and zero diagnostics**. Runnable A/B: 2-TU module ran the engine's implementation; 1-TU module ran its own. The fix is running `ld -r -m i386pe` a **second time** over its own output; `--allow-multiple-definition` does not help. The `--set-section-flags` pass also needs `.data` added, not just `.text`/`.rdata`, or the link dies with `unknown comdat type 0 for .data`.

### What stands

- `objcopy` does not exist anywhere in nxdk (`deps/nxdk/bin/` has 8 wrapper scripts, none of them objcopy).
- `llvm-objcopy` rejects `-G`, `--localize-symbol`, `--globalize-symbol`, `--weaken-symbol` and `--prefix-symbols` on COFF: `option is not supported for COFF`. **`--redefine-sym` is the one symbol option that works.**
- `xshlib.py:36` omits the i386 `_` prefix (`-G lib_${NAME}_exports`, not `-G _lib_${NAME}_exports`). GNU objcopy exits 0 and localizes the export table itself — a silent failure that surfaces as an undefined symbol at final link.
- `lld -flavor link -r` cannot do partial links, so the merge cannot use `nxdk-link`.
- The machinery is needed for at least one genuine conflict: `fs_globals_t FI;` (`filesystem/filesystem.c:30`) versus `fs_globals_t *FI;` (`engine/common/filesystem_engine.c:41`) — a struct-vs-pointer type confusion that reproduces as `lld: error: duplicate symbol: _FI` under the exact nxdk-link flags.
- Static linking is **mandatory**, not a preference: nxdk's `LoadLibraryExA` unconditionally does `SetLastError(ERROR_MOD_NOT_FOUND); return NULL;` (`deps/nxdk/lib/winapi/libloaderapi.c:16-23`), and `SDL_LOADSO_DISABLED 1` (`SDL_config_xbox.h:122`). There is no dynamic path for `ref_soft`, the filesystem module or the game DLL.
- `ref/soft/exports.txt` **does not exist** — only `filesystem/exports.txt` and `ref/gl/exports.txt` do. `xshlib.py:86` reads it unconditionally, so `--static-linking=ref_soft` fails today even on Linux. Same for `menu`, `server`, `client` in `README.md:43`.
- `XASH_STATIC_LIBS` is not only a link switch. `common/defaults.h:102-105` makes it imply `XASH_LIB LIB_STATIC` **plus** `XASH_INTERNAL_GAMELIBS` **plus** `XASH_ALLOW_SAVERESTORE_OFFSETS` — the last of which routes `COM_NameForFunction` through offsets relative to `svgame.dllFuncs.pfnGameInit`, making the save format depend on link layout. Note this also means `LIB_STATIC` wins over `XASH_WIN32`, so the dead `LoadLibraryExA` path is avoided *as long as every module is static*.

### The mechanism to use

**Follow `docs/r3d/design-stage.md:18-31`, which already lays out the correct order and which the probe never opened:** *no action needed; a COFF equivalent of `objcopy -G`; scoped `/FORCE:MULTIPLE`; per-module symbol renaming by `#define`.* The probe tested only option 2 and proposed a fifth option — importing GNU binutils into the toolchain — which is the most invasive available.

**Do this, in order:**

1. **Measure first.** Compile `engine/`, `filesystem/` and `ref/soft/` objects with the real `nxdk-cc` line, archive `libpublic` with `llvm-lib`, link everything with the exact `nxdk-link` flags, **no `ld -r`, no objcopy**, and count with `-errorlimit:0`. This has never been done. If the residue is a handful of genuine name conflicts, you are at design-stage option 1+4.
2. **For each genuine conflict, use `--redefine-sym` or a `#define`.** `FI` is the known one. `llvm-objcopy --redefine-sym` works on COFF and needs no new toolchain.
3. **Only if the residue is large and structural**, adopt the binutils route — GNU `ld -r -m i386pe` **twice**, then a single `objcopy --set-section-flags .text=… --set-section-flags .rdata=… --set-section-flags .data=contents,alloc,load,data -G _lib_${NAME}_exports`, then `nxdk-link`. Drive it via `$LD`, `$OBJCOPY`, `$LD_RELOCATABLE_FLAGS` (waf's `add_os_flags`/`find_program` honour all three without patching), plus the underscore fix at `xshlib.py:36`. Add a post-`objcopy` assertion that no symbol appears as both `U` and a lowercase local, or the C++ menu will silently call the engine's code.

### Is the risk retired?

**No. Downgraded, not retired.** It has moved from *"this approach may not work at all"* to *"there are three known-good fallbacks, one of which requires no new tools"* — which is a real reduction and justifies removing "highest risk in the project" from `fork-plan.md:118-120`. But the one measurement that would actually close it (step 1 above) has never been run by anyone, and it cannot be run until `3rdparty/library_suffix` is cloned and `build.h` exists. **Keep X1 as a gate; change its content from "port xshlib.py" to "measure, then choose".**

---

## 5. What this changes in `docs/r3d/fork-plan.md`

**X0 (`fork-plan.md:103-116`) — grows, and gains a concrete definition of done.** Today it says "establish whether `engine/platform/sdl2/` works on nxdk's SDL2". That question is now answered per subsystem (§1), so X0 becomes an enumerated list instead of an investigation: clone `3rdparty/library_suffix` and add an Xbox case to `build.h` (a change inside a third-party submodule — plan for a fork); plumb `-DXBOX` into every engine TU via the pkg-config branch of `sdl2.py`; add a `sys/types.h` shim; patch five hint constants; pick a `DEST_OS` that is not `win32` and add `VIDEO_XBOX`/`INPUT_XBOX` to `common/backends.h:18-23` and a branch to `common/defaults.h:32-53`; drop `SDL_INIT_VIDEO` from `sys_sdl2.c:130`. Add "nxdk builds on the dev host" as an explicit X0 exit — nothing under `deps/nxdk/lib` is built right now.

**A new X0.5 is needed and is not in the plan: `filesystem_engine.c` root-directory detection.** `SDL_GetBasePath` returns NULL and `getcwd` does not exist on nxdk (§1.5). Without it the engine `Sys_Error`s before X2 starts. It is a small patch but it is a *blocker*, and it currently lives nowhere.

**X1 (`:118-134`) — same position, different content, lower risk.** Replace "its mechanism is ELF-specific" (`:124`) with the corrected diagnosis, and replace "port xshlib" with "measure the real duplicate count first". Correct the `-melf_i386` claim at `fork-plan.md:52` and `epics.md:135`. Add: create `exports.txt` for every module named in `--static-linking`; none of `ref_soft`, `menu`, `server`, `client` has one. Add: `XASH_STATIC_LIBS` changes the save format.

**X2 (`:136-147`) — grows.** Absorbs X0.5. Also: `filesystem/exports.txt` exists, so the filesystem module is the natural first static-linking test case.

**X5 (`:178-192`) — shrinks in effort, changes in method.** Do **not** wire `ref/soft` to SDL's window surface. Write `engine/platform/xbox/vid_xbox.c` (~250 lines, modelled on `vid_fbdev.c`) against `hal/video` at 16bpp. That deletes the 1.2 MB shadow surface, the per-frame `SDL_ConvertPixels`, and ~350 KiB of SDL software-2D code from the image. Add the double-buffer + `XVideoWaitForVBlank` work, and add `VID_Info_f` (absent outside `sdl2`/`sdl3`). Note the render-resolution lever is `R_SaveVideoMode(640,480,320,240,…)`, not `vid_scale`.

**X6 (`:194-203`) — splits cleanly.** Gamepad is **free** — SDL's controller layer works on nxdk with a compiled-in mapping, and rumble works. Keyboard and mouse **do not exist and are not needed**; strike any implication that they might come from SDL. `in_sdl2.c` compiles inert. The menu half of X6 is unchanged and carries the C++ multi-TU static-linking hazard from §4, so sequence it after X1's mechanism is chosen.

**X8 (`:215-228`) — mostly disappears, as the plan hoped.** `s_sdl2.c` works. Two one-line fixes (48 kHz to avoid a guaranteed `SDL_AudioStream` resample; compile out `VoiceCapture_Init`). The NVAPU/ADPCM fallback and the `engine/client/sound/` abstraction extraction can be struck from the Xbox critical path — keep them as an upstream nice-to-have as `:222-227` already suggests.

**X9 (`:230-252`) — unchanged in scope, de-risked in one specific way.** `:88-91`'s caveat ("SDL owning it too is a known-awkward combination") can be replaced with a mechanism: `PCRTC_START` arbitration via `pb_show_front_screen()`/`pb_show_debug_screen()`, SDL initialised without `SDL_INIT_VIDEO`, `pb_show_front_screen()` after every `pb_init()`. The coexistence is a shipped upstream sample, not an open question. Note `ref/nv2a` does not exist yet — `ref/` holds `common`, `gl`, `null`, `soft`.

**One thing to add to `:66-71`'s inventory:** the SDL2 dependency is ~810 KiB of linked image at stock flags, ~418 KiB with `-O2 -ffunction-sections -fdata-sections /OPT:REF /OPT:ICF`, of which ~350 KiB is the software-2D stack that a pbkit-owning-video design never uses. That trim requires patching `Makefile.xbox` and stubbing symbols in `SDL_video.c`, not a linker flag. It is worth doing but it is X11 work, not X0 work. LTO buys 0.6% and is not a lever.

---

## 6. Still unknown — ranked by how much each blocks

**1. How many real duplicate symbols does the whole engine produce under `nxdk-link`?** *Blocks X1, which blocks everything.* Nobody has ever linked real project objects; every number so far came from synthetic modules or from linking one object twice. **Experiment:** clone `3rdparty/library_suffix`; compile `engine/`, `filesystem/`, `ref/soft/` objects with the exact `deps/nxdk/bin/nxdk-cc` line; `llvm-lib` the `public` objects into `public.lib`; link with the exact `nxdk-link` flags plus `-errorlimit:0` and no `ld -r`/objcopy; count. A small list of genuine conflicts → `--redefine-sym`, and `xshlib.py` is bypassed on Xbox entirely. A large structural list → the four-step binutils pipeline from §4.

**2. Does `build.h` need an `XASH_XBOX` case, and what does the submodule already do with `_WIN32 && NXDK`?** *Blocks X0 and therefore item 1.* The sibling clone's `build.h:100-101` is `#if defined _WIN32 → #define XASH_WIN32 1` with no nxdk case, but `resonance3d`'s pin may differ. **Experiment:** `git submodule update --init 3rdparty/library_suffix`, then read `include/build.h` for any Xbox branch and for how `DEST_OS` is expected to be set.

**3. What is `R_BlitScreen`'s actual cost at 640×480 and 320×240?** *Decides whether X5 is a usable reference or a slideshow, and whether the 2× upscale is mandatory.* **Experiment:** in the X4 standalone XBE, fill a `vid.buffer`-shaped array, run the `bpp == 2` loop from `r_glblit.c:667-680` 100 times against a 128 KiB LUT, read RDTSC. Do both resolutions, and do a variant that stages a scanline in cached RAM before `memcpy` into write-combined memory.

**4. Does `VIDEO_R5G6B5` actually work on hardware and in xemu?** *Decides whether §3's zero-conversion path exists.* It is coded (`hal/video.c:339-341`) but **every one of the 16 samples that sets a mode uses 32bpp** — nothing in nxdk has ever exercised 16bpp. **Experiment:** ~30-line XBE: `XVideoSetMode(640,480,16,REFRESH_DEFAULT)`, read back `XVideoGetMode()` to confirm `vm.bpp` survived, write a 565 gradient into `XVideoGetFB()`, `XVideoFlushFB()`, loop. If it fails, `VIDEO_A1R5G5B5` (bpp 15) still reports `bytesPerPixel = 2` and `R_BuildScreenMap` handles 555 without bleed — but note `Get8888PixelAt` hardcodes 565 shifts (`r_glblit.c:729-733`), so screenshots would be wrong.

**5. Does `pb_init()` succeed beside a live SDL at runtime?** *Blocks X9's design, not X5.* The source says yes and `samples/sdl_gamecontroller` is CI-linked, but nobody has run it. **Experiment:** build `deps/nxdk/samples/sdl_gamecontroller` and run it in xemu; confirm `pb_init()` returns 0 and the controller enumerates. Then extend it to `SDL_VideoInit(NULL)` + a window, alternate `pb_show_debug_screen()`/`pb_show_front_screen()` once a second, and watch for tearing or a hung DMA.

**6. Does the C++ multi-TU hijack actually bite `3rdparty/mainui`?** *Blocks X6 only, and only if the binutils route is chosen.* Two reviewers reproduced it on synthetic modules; `3rdparty/mainui` is an uninitialized submodule here so it has never been tried on real code. **Experiment:** after item 1 decides the mechanism, if it is `ld -r` + `objcopy -G`, clone mainui, run the pipeline, and assert with `llvm-nm` that no symbol appears as both `U` and a lowercase local before linking.

**Not worth chasing yet:** exact peak RSS on hardware (derivable to ±10% from the allocation sites); XBE header overhead (measured at a flat 8-12 KB, irrelevant against 64 MB); the `SDL_DISABLE_JOYSTICK_INIT_DELAY` 500 ms boot cost (`SDL_xboxjoystick.c:192-200`) — a boot-time annoyance, not a design question; the SDL event-queue high-water mark (`SDL_MAX_QUEUED_EVENTS 65535`, `SDL_events.c:37`), which only matters if the engine stops pumping, and it does not.