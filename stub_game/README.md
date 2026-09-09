# stub_game/

The fourth piece X1's own exit line names: "engine, filesystem, menu and a
**stub game module** link into one XBE." Deliberately not `deps/hlsdk` (the
real, ~160-file Half-Life SDK already vendored for a later phase) — this
proves the engine's static-linking mechanism resolves a real game module by
name, not that any gameplay logic works. Real game code is X7's job.

## Why a stub, not the real SDK

Xbox has no dynamic loading (`deps/nxdk/lib/winapi/libloaderapi.c`'s
`LoadLibraryExA` unconditionally returns `NULL`), so `-DXASH_STATIC_LIBS=1`
routes `COM_LoadLibrary`/`COM_GetProcAddress` through
`engine/platform/misc/lib_static.c`'s `Lib_Find` — a compile-time table
lookup by name, not a real OS library load. `tools/gen-library-tables.sh`
generates that table (mirroring `scripts/waifulib/xshlib.py:74-89`, which the
Xbox build bypasses entirely — no GNU binutils in this toolchain). A "game
module" here just needs to exist as a named entry in that table with the
right exports; nothing requires it to be the real 160-file SDK to prove the
mechanism works.

## `server/` vs `client/` — why one has code and the other doesn't

`engine/server/sv_game.c` performs two separate, equally hard checks before
it will use a server module at all: `:5235` requires at least one of
`GetEntityAPI`/`GetEntityAPI2` to exist ("missing GetEntityAPI and
GetEntityAPI2 exports" otherwise), and `:5245-5255` — found in review,
missed in this file's first version — separately requires
`GiveFnptrsToDll` ("missing GiveFnptrsToDll export"), failing and freeing
the library before `GetEntityAPI2` is ever even called if it's absent.
`server/server_stub.c` provides both: `GetEntityAPI2` (the newer,
preferred one upstream itself checks first) returns a
validated-but-entirely-zeroed `DLL_FUNCTIONS` — every function pointer
`NULL`, honest about implementing nothing — and `GiveFnptrsToDll` accepts
what the engine hands it and does nothing else, same honesty.

The client side has no equivalent hard requirement at this phase:
`engine/client/dll_int/cl_game.c` looks up each `HUD_*`/`Initialize` name
individually via `COM_GetProcAddress`, and a name not found in the static
table just leaves that one function pointer `NULL` — not a load failure.
That lookup only happens inside `CL_LoadProgs`, which only runs during a
real client boot, and no Xbox `main()`/entry point/running binary exists in
this tree yet (`docs/r3d/divergences.md` #14). So `client/exports.txt` is
genuinely empty — zero exports, not a placeholder — matching the honest
current state rather than faking coverage nothing calls yet.

## Not runtime-safe, and that's fine for now

Nothing here is meant to survive an actual game frame — `GetEntityAPI2`'s
zeroed struct would crash the instant anything tried to call e.g.
`pfnSpawn`. That is out of scope for what X1 measures (linking, not
running); wiring up real behaviour, or reconsidering this module entirely
once `deps/hlsdk` gets its own Xbox port, is X7's job.
