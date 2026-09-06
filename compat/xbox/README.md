# `compat/xbox` — POSIX shims for nxdk

nxdk provides C99 through pdclib and a good slice of Win32, but **no low-level
POSIX file I/O at all**. `filesystem/` is built on raw file descriptors, so
something has to bridge them.

Measured, not assumed — `grep` over `filesystem/*.c`:

| Call | Uses | nxdk provides |
|---|---|---|
| `lseek` | 14 | no |
| `close` | 5 | no |
| `stat` | 4 | **no** — declared inside `#if 0` in `xboxrt/libc_extensions/stat.h` |
| `open` | 4 | no |
| `read` | 3 | no |
| `write` | 2 | no |
| `opendir`/`readdir`/`closedir` | 3 | no — there is no `dirent.h` anywhere |
| `mkdir`, `remove`, `chdir`, `dup`, `fsync`, `_commit` | 1 each | no |

The headers those calls need are equally absent: no `fcntl.h`, no `sys/stat.h`,
no `sys/types.h`, no `dirent.h`, no `io.h`. nxdk's `unistd.h` is a licence stub
containing nothing.

## Why a compat directory instead of `#ifdef`s

`AGENTS.md` ranks divergence forms by maintenance cost, and "new file in one of
our directories" is the cheapest rung. The alternative is dozens of guards
scattered through a module upstream actively maintains, each one a future merge
conflict.

Nothing here is on the Xbox include path by accident: the build adds
`-I compat/xbox` only for that target, so these headers are invisible to every
other platform.

## What is underneath

Win32, which nxdk does implement: `CreateFileA`, `ReadFile`, `WriteFile`,
`SetFilePointer`, `CloseHandle`, `GetFileSizeEx`, `GetFileInformationByHandle`,
`FindFirstFileA`/`FindNextFileA`/`FindClose`, `CreateDirectoryA`,
`DeleteFileA`, `SetEndOfFile`. `fsync` goes to `NtFlushBuffersFile` from
`xboxkrnl`, since `FlushFileBuffers` is not exposed.

Two are genuinely missing and are handled in `xbox_posix_io.c`:
`SetCurrentDirectoryA` and `DuplicateHandle`. See the notes there.

## Files

| File | Purpose |
|---|---|
| `fcntl.h` | `O_*` constants and `open` |
| `unistd.h` | `read`, `write`, `lseek`, `close` and friends |
| `sys/types.h` | `off_t`, `ssize_t`, `mode_t` |
| `sys/stat.h` | `struct stat`, `stat`, `fstat`, `mkdir`, `S_IS*` |
| `dirent.h` | `DIR`, `struct dirent`, `opendir`/`readdir`/`closedir` |
| `io.h` | the underscore-prefixed CRT names `io.c` reaches for under `XASH_WIN32` |
| `xbox_posix_io.c` | the implementation |
