# Training the Xbox build with PGO

The instrumented build records which engine code runs most often. It runs more
slowly than the normal build; use the normal package again after collecting data.

1. Run the **Build** GitHub Actions workflow manually with **pgo_generate**
   enabled. Download **ringracers-uwp-pgo-instrumented**, and install its
   certificate and package using the usual Dev Mode installation instructions.
2. Leave the prepared USB drive connected, with writable `E:\ringracers`.
   Play a varied session: Grand Prix with bots, Time Attack, menus, and online
   play if you like. Use the renderer and settings you normally play with.
3. Quit from the game's menu to save the recording, or enter `pgo_dump` in the
   console to save during play. Suspension also attempts a save, but abrupt
   termination or power loss can lose the unsaved part of a session.
4. Copy **all** `.profraw` files from `E:\ringracers\pgo\` on the USB drive
   and send them to the maintainer, along with the instrumented build's commit
   and your console model/settings. Each successful dump starts a fresh segment;
   include every segment once. The game logs paths and save results in
   `E:\ringracers\latest-log.txt`; there are no in-game PGO notifications.

The folder is created automatically. If saves fail, check the USB permissions
from the installation instructions and send the log. Failed writes retain the
counters for a later retry. Capture timing and counter resets can lose a few
samples from active worker threads; these are optimisation hints, not game data.

## Maintainers

Commit collected files under the app repository's `pgo/` directory. Keep filenames
unique, avoid duplicate submissions (which would overweight a session), and record
the capture build/toolchain and workload in the commit message. Do not commit the
merged `build/ringracers.profdata`. Normal push/PR builds automatically use tracked
`pgo/*.profraw` files when present, otherwise PGO stays off.

From the x64 VS developer shell, `./build.ps1 -Pgo generate`, `-Pgo use`,
`-Pgo off`, or `-Pgo auto` selects the mode; `RR_PGO` supplies the default.
Explicit `use` fails when there are no tracked captures. Every configure clears
unused PGO cache settings, so switching modes does not leave instrumentation on.
PGO affects only the engine target, not vcpkg dependencies; the existing vcpkg
cache is shared between modes.

The build uses the selected `clang-cl` for both engine languages. For generation,
the launcher CMake queries that compiler with `/clang:-print-resource-dir` and
requires exactly one `clang_rt.profile-x86_64.lib` below its `lib/` directory.
It links that archive with MSVC's linker. Missing runtime files are a configure
error; install compiler-rt for that compiler rather than borrowing another version.

For profile use, the build first looks for `llvm-profdata.exe` beside `clang-cl`.
If VS omitted it, the script downloads the **same full LLVM release version**
from the LLVM project's GitHub release and uses 7-Zip to extract just
`bin/llvm-profdata.exe` into `deps/llvm-<version>/`. CI's Windows image supplies
7-Zip; local builders need `7z` on PATH for this fallback. The tool's reported
version must exactly match clang-cl's version. No global LLVM installation or
compiler replacement is performed. A VS-only version without a matching upstream
release requires manually supplying its matching `llvm-profdata.exe` beside the
compiler. All tracked captures are merged in one invocation before configuration;
unreadable or incompatible captures fail the build rather than silently disabling
PGO. A compiler upgrade may require new captures.

Stale, structurally compatible profiles only lose optimisation, never correctness:
Clang ignores mismatched function data. Expected unprofiled/out-of-date diagnostics
are suppressed; other warnings/errors remain visible. PGO does not enable fast
math or change game logic, netplay, or replay semantics.

## UWP runtime audit and device validation

LLVM's Windows compiler-rt profiling code uses CRT file operations (`fopen`,
`_mkdir`, `_getpid`) and includes desktop API references such as
`GetComputerNameExW` (hostname substitution), `CreateFileMapping`/`MapViewOfFile`
(mapping/continuous mode), and `CreateEvent`, `LockFileEx`, `UnlockFile` (merging).
See LLVM's [file writer](https://github.com/llvm/llvm-project/blob/release/19.x/compiler-rt/lib/profile/InstrProfilingFile.c),
[utilities](https://github.com/llvm/llvm-project/blob/release/19.x/compiler-rt/lib/profile/InstrProfilingUtil.c),
and [Windows mapping/locking implementation](https://github.com/llvm/llvm-project/blob/release/19.x/compiler-rt/lib/profile/WindowsMMap.c).
This is a source audit of LLVM 19, not proof for every VS LLVM version.

Our capture uses literal timestamp/sequence filenames, ordinary writes, and no
hostname/PID/merge/continuous patterns. It supplies `__llvm_profile_runtime` to
suppress the runtime's automatic initialization and exit writer; startup selects
the writable filename explicitly. Suspension is handled by an SDL event watch:
the pinned SDL WinRT implementation delivers `SDL_APP_WILLENTERBACKGROUND`
synchronously before finishing its suspension callback. No launcher changes are
needed. Successful explicit writes reset counters and rotate the filename, so
previous gameplay is not counted again by subsequent dumps or an exit handler.

The desktop API references can still be pulled in at link time even when their
branches are unused. WindowsApp/OneCore and Xbox Dev Mode may provide them, but
link/package validation and an Xbox test remain required. If imports are rejected,
a UWP-compatible compiler-rt build may be necessary; do not assume this runtime
is Store-certifiable. Verify dump success, nonempty captures, merging, suspension,
and the optimised package on the console before treating PGO as validated.
