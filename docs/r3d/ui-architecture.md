# UI architecture: replacing VGUI and VGUI2

How Resonance3D draws panels without the proprietary `vgui.dll`, and what a
VGUI2 replacement would take.

Everything here was measured in this tree. Line numbers are citations, not
estimates.

## 0. The console does not need any of this

Worth stating first, because it is the usual reason people reach for VGUI.

`engine/client/console.c` is 2,391 lines and contains **zero** references to
VGUI. It draws with `ref.dllFuncs.R_DrawStretchPic` (`:1916-1917`),
`Color4ub` (`:1834`), `GL_SetRenderMode` (`:1913`) and the engine's own font
renderer, `CL_DrawString` (`:757, 769, 1276, 1857`).

**The command console works on Xbox the moment `ref/nv2a` implements the 2D
primitives.** No UI library is involved, and none needs to be.

## 1. What actually needs VGUI

| Target | Needs | Size |
|---|---|---|
| Half-Life single player | Nothing | — |
| Half-Life multiplayer | VGUI1 | 7,842 lines across 9 `cl_dll/vgui_*.cpp` files: scoreboard, MOTD, spectator panel, team and class menus |
| Counter-Strike, CS:CZ, Day of Defeat, Blue Shift | VGUI2 | See §5 |

The first playable Xbox build is single-player Half-Life. It needs neither.
This is the single most important scoping fact in this document: VGUI is not
on the critical path to a booting game.

## 2. The layer map

Every UI layer in the engine bottoms out in the same handful of `ref`
primitives. `engine/client/vgui/vgui_draw.c` proves it — the 28 function
pointers of `engine/vgui_api.h` collapse into:

```c
ref.dllFuncs.R_DrawStretchPic( x, y, w, h, s1, t1, s2, t2, texnum ); // ref_api.h:566
ref.dllFuncs.FillRGBA( rendermode, x, y, w, h, r, g, b, a );         // ref_api.h:567
ref.dllFuncs.GL_Bind( XASH_TEXTURE0, texnum );                       // ref_api.h:625
ref.dllFuncs.Color4ub( r, g, b, a );                                 // ref_api.h:645
ref.dllFuncs.VGUI_SetupDrawing( rect );                              // ref_api.h:655
```

plus texture upload and release (`GL_LoadTextureFromBuffer`,
`GL_FreeTexture`).

```
freevgui (widgets)   ─┐
mainui (menu)        ─┼─→ engine 2D primitives ─→ ref_interface_s ─→ ref/nv2a
console + HUD        ─┘
```

Consequence: **`ref/nv2a` has to implement these anyway for the HUD.** Once it
does, every UI layer draws. There is no separate 2D backend to write, and no
reason to add an immediate-mode GUI library — the role an immediate-mode
library would play is already filled three times over.

## 3. VGUI1 — adopt `freevgui`, write one class

### The surface we must satisfy

The SDK ships 79 VGUI headers in `deps/hlsdk/utils/vgui/include/`. The
Half-Life client includes **35 of them**. That set is the acceptance test:

- **Core** — `VGUI.h`, `Panel`, `App`, `MiniApp`, `Surface`, `SurfaceBase`,
  `Scheme`, `Font`, `Cursor`, `String`, `Image`, `Dar`
- **Widgets** — `Button`, `Label`, `TextEntry`, `TextPanel`, `TextImage`,
  `TextGrid`, `ScrollBar`, `ScrollPanel`, `Slider`, `Frame`, `ImagePanel`,
  `HeaderPanel`, `TablePanel`, `MenuSeparator`, `DesktopIcon`
- **Layout** — `BorderLayout`, `StackLayout`
- **Borders** — `LineBorder`, `LoweredBorder`
- **Events** — `ActionSignal`, `InputSignal`, `MouseCode`
- **Assets** — `BitmapTGA`, `FileInputStream`, `BuildGroup`

Note also that `deps/hlsdk/utils/vgui/lib/win32_vc16/` is **empty**. The fork
cannot link against the proprietary library today even on Windows, so there is
no working state to preserve.

### The simplification that makes this cheap

We compile the game code ourselves, from `deps/hlsdk`. That means we need
**source compatibility, not ABI compatibility**. No matching of vtable layout,
no matching of MSVC name mangling, no binary-compatible struct offsets. This
is the difference between a research project and a week of work, and it is why
this is worth doing at all.

### What to adopt

[FWGS/freevgui](https://github.com/FWGS/freevgui) — a from-scratch
reimplementation of the Half-Life 1 GUI library in C++, **BSD 3-clause**,
written from a written specification rather than by copying the original.
Upstream's `freevgui` branch already wires it in as `3rdparty/freevgui` and
deletes `scripts/waifulib/vgui.py`, the 97-line build glue for the
proprietary library. That branch is current: one commit ahead of a master
29 commits old, dated 2026-08-04.

It provides `panel`, `surface`, `scheme`, `layout`, `font`, `image`,
`signals`, `input` and a `controls/` directory.

**Do not write our own widget library.** The widget hierarchy is the expensive
part and it already exists, under a compatible licence, from the same people
who maintain the engine.

### What we write

One thing: **the Surface backend**. `Surface` is the bridge between freevgui's
widget tree and the engine, and it is the only class whose implementation is
platform-specific. It maps onto the five primitives in §2.

Everything else we contribute is glue.

## 4. Fonts — nothing new is needed

VGUI's `Surface::createFont( name, size, ... )` implies a system font
rasteriser. The Xbox has none, and adding one (stb_truetype or similar) would
cost RAM and CPU on the tightest budget in the project.

It is not necessary. `engine/client/cl_font.c` is 393 lines of **bitmap** font
handling with the whole API already present:

```
CL_DrawCharacter:192   CL_DrawString:246      CL_DrawStringf:303
CL_DrawCharacterLen:315  CL_DrawStringLen:327
CL_SetFontColor:82     CL_SetFontRendermode:77
CL_FreeFont:171        CL_FixedFont:35        CL_LoadFontTexture:40
```

`CL_LoadFontTexture` loads a texture, so fonts are already glyph atlases —
exactly the right shape for a console.

**Design:** freevgui's `Font` delegates to `cl_font.c`. Do not build a second
font system.

**The one mismatch to resolve:** VGUI addresses fonts by name and point size;
`cl_font.c` addresses them by texture. Bridge it with a scheme-to-font-asset
mapping table, resolved when the scheme is loaded rather than per call. VGUI
schemes already declare font names, so the table is data, and
`tools/asset-studio` is the natural place to bake it alongside everything
else.

## 5. VGUI2 — our own, and later

### Why upstream's route is closed to us

`Documentation/vgui2-support.md`, on upstream's `new_vgui_support_api` branch,
is unambiguous about its own state:

> **Support status: Doesn't draw anything, doesn't take any input.**

and about its dependencies:

> This support also relies on a number of proprietary libraries, that we
> better not redistribute [...] compiled only for 32-bit Windows, Linux and
> OSX, you shouldn't expect them running anywhere else.

The list is `vgui2.dll`, `tier0.dll`, `vstdlib.dll`, `steam_api.dll`,
`chromehtml.dll` and **`libcef.dll`** — Chromium Embedded Framework, for the
HTML MOTD. On a 64 MB console that is not difficult, it is out of the
question.

That branch is also dead: last commit 2023-05-02, **4,005 commits behind
master**. Nobody is finishing it.

So an own implementation is not a preference, it is the only route. But it
buys a different thing than VGUI1 does.

### Shape of the problem

VGUI2 is a Source-era interface-factory system, not a class library. Game code
does not link classes; it calls `CreateInterface( "VGUI_Panel009", ... )` and
gets a pointer to an abstract interface. The surface to implement is
`IPanel`, `ISurface`, `IScheme`, `IInput`, `ILocalize` and `IVGui`, plus the
`tier0`/`vstdlib` primitives those interfaces assume.

That factory shape is, for once, in our favour: we implement only the
interfaces and only the versions the games actually query, and the games
cannot see what is behind them.

### Design

- **Same retained core.** Back the VGUI2 interfaces with the same panel tree,
  scheme system and Surface that VGUI1 uses. Two APIs, one implementation.
  If they diverge, this becomes two UI toolkits and stops being viable.
- **No CEF.** The HTML MOTD degrades to plain text. Anything that genuinely
  requires a browser is out of scope, permanently.
- **No Steam API.** Stub `steam_api` to the minimum the games touch.
- **Scope by game.** Implement what Counter-Strike queries first; it is the
  most-wanted and the best documented. Day of Defeat and Blue Shift follow.

### Scheduling

**After Half-Life runs on real hardware.** VGUI2 unlocks other games; it does
not move the first playable build one day closer. Keeping it separate turns
"we support CS and Blue Shift" into a measurable goal instead of a hidden
requirement inflating F0.

This also settles the earlier open question about `deps/hlsdk-bshift`: Blue
Shift is not blocked by the SDK, it is blocked by VGUI2. Same wall.

## 6. Build and adopt

| Component | Source | Effort |
|---|---|---|
| Command console | Already works, engine-drawn | None |
| Widget library (VGUI1) | Adopt `FWGS/freevgui`, BSD-3 | Track the branch |
| Surface backend | **Ours** | The real work, and it is small |
| Fonts | Delegate to `cl_font.c` | Glue + a scheme mapping table |
| Scheme font table | **Ours**, baked in `tools/asset-studio` | Data, not code |
| Gamepad input | **Ours**, modelled on `3rdparty/mainui/menus/Gamepad.cpp` | See §7 |
| Game menu | `3rdparty/mainui`, already gamepad-capable | None |
| VGUI2 | **Ours**, on the same core | A phase of its own |

## 7. Open questions

**Input.** VGUI is mouse-driven and the Xbox has no mouse. The choice is a
virtual cursor driven by the analog stick, or D-pad focus navigation through
the widget tree, or both. `mainui` already solved this for the menu
(`menus/Gamepad.cpp`); read it before deciding. Focus navigation is better on
a TV but needs every panel to declare a sensible tab order, which HL's
multiplayer panels were never written to do.

**Memory.** VGUI's `Dar` is a growing dynamic array and the panel tree
allocates freely. Under the F2 budget this needs a cap and a pool, and the
answer should come from `zone.c`'s named pools rather than a new allocator.
Measure before deciding a number.

**Scheme fidelity.** HL's schemes assume specific font metrics. Panels laid
out against a different glyph atlas will not be pixel-identical to retail.
Decide early whether that matters — chasing it is expensive and probably
pointless on a TV at 640×480.
