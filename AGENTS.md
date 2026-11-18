# This fork

`master` is upstream's [Ring Racers](https://github.com/KartKrewDev/RingRacers)
`maintenance-2.4` branch plus our changes, one commit per change. The UWP (Xbox)
app is built from this repository too: `uwp/` holds its launcher, build script,
SDL patches, profile recordings, and tools, and `CHANGES.md` is the user-facing
changelog. They came from a separate repository,
[ringracers-uwp](https://github.com/ZebulanStanphill/ringracers-uwp) (archived),
whose history joins this one at the commit "Merge the UWP app's repository".

# Commit scope

Implement one change or fix per commit, with its documentation (its commit
message and the docs in `docs/` or `tests/README.md` that describe it) and its
`CHANGES.md` entry. Split unrelated changes into separate commits, even when
they're made in the same session.

# Writing style

Use the Oxford comma ("fades, wipes, and Encore inversion") in documentation,
`CHANGES.md` entries, in-game text, and commit messages.

# Staying rebaseable

Our commits are rebased onto newer upstream releases, so keep them easy to
replay. Rebase with `git rebase --rebase-merges`: the UWP app's earlier history
is a second line of commits that joins at one merge, and a plain rebase would
flatten it into this one and stop on its old conflicts. With `--rebase-merges`
those commits stay as they are and the merge is made again.

- Prefer new files and small, contained hooks over edits spread through upstream
  code. Don't reformat, rename, or reorder upstream code you aren't changing.
- Mark a change inside upstream code with a comment that starts `[ZE]:` (for
  Zebulan Edition, this fork), or `[UWP]:` when the code is only compiled for or
  only applies to the Xbox (UWP) build. Names and log lines follow the same rule:
  `UWP` and `uwp_` are for what only the Xbox build has.
- Gate platform-specific code so every platform keeps building: `_UWP` for the
  UWP build, and `HAVE_VIBERANT` for builds that contain the VibeRant driver
  (`src/hardware/r_viberant`). The driver has three backends. `HAVE_VIBERANT_D3D`
  (`SRB2_CONFIG_VIBERANT_D3D`) is Direct3D 12 itself, VibeRant D3D, which UWP builds and
  Windows desktop builds made with MSVC or clang-cl have by default.
  `HAVE_VIBERANT_VULKAN` (`SRB2_CONFIG_VIBERANT_VULKAN`) is the Vulkan backend, VibeRant Vulkan,
  which Linux builds and those Windows desktop builds have by default:
  `src/hardware/r_viberant/vulkan` implements the Direct3D 12 interfaces the driver
  uses, so the driver's own code is the same for both
  (`docs/vulkan-backend.md`). Only the driver's device, shader, and pipeline
  setup, presentation, and messages ask which backend it is on
  (`Core_Vulkan()`); engine code asks (`HWR_Vulkan()`) only to pick the
  renderer, handle its window, and name it. A UWP build has the driver as its
  only hardware driver; a build with `HAVE_VIBERANT` but not `_UWP` has both it and
  OpenGL, and the video code picks one per run. Driver-dependent behavior (what
  the hardware renderer draws, how it walks the level, and calls into the
  driver) therefore checks `HWR_VibeRant()` (`src/hardware/hw_driver.h`) at run
  time. It is the constant 1 with `_UWP`, the constant 0 without
  `HAVE_VIBERANT`, and `hwr_viberant` otherwise.
  `#ifdef HAVE_VIBERANT` is only for what has to be compiled out without the driver:
  includes, types, struct fields, enum values, prototypes, function definitions, a
  call of a VibeRant driver function (with the `HWR_VibeRant()` check around it), and
  bookkeeping that doesn't depend on the driver. Code under `HAVE_VIBERANT` must
  compile without `_UWP` (Xbox-only things inside it get their own `_UWP`
  guard) and without Windows (Linux builds have `HAVE_VIBERANT` with only the
  Vulkan backend; `vulkan/vk12_win.h` stands in for the few Win32 calls the
  driver makes). MinGW builds must build without any of them.
  `HAVE_VIBERANT_METAL` (`SRB2_CONFIG_VIBERANT_METAL`) is a third backend, VibeRant Metal, which
  macOS builds have by default, and as their only one: `src/hardware/r_viberant/metal`
  implements the same interfaces on Metal (`docs/metal-backend.md`),
  `Core_Metal()` is where the driver asks, and `HWR_Metal()` names the renderer.
  Its files are Objective-C++ and can't include the game's headers (Apple's and
  the game's don't go into one file), so what they need of the game goes
  through a small C++ file (`metal/mtl_present_game.cpp`). Metal code can only
  be compiled on macOS, which here means CI: the `metal` and `macos` jobs of
  `tests.yml`, and, for running the game itself on a macOS runner, the Metal
  frames workflow (`tests/README.md`, "Metal frames").
- A fix to a change that has its own commit can be made with
  `git commit --fixup=<commit>`, to be squashed into it at the next rebase.

# In-race behavior

Never change what happens in a race relative to upstream. The physics, collision,
items, bots, random number streams, timing, and everything else game logic
decides must give the results upstream's code gives, in every state the game can
reach: every map, every mode, mods (Lua and ACS scripts, custom maps), netgames,
and replays. A difference, however small, gives some players an advantage or a
disadvantage, and breaks replays, staff ghosts, and online play with the official
game. This fork changes how the game is drawn, where it runs, and how fast, never
how it plays.

A change that touches game logic, such as an optimization, is only acceptable
when identical behavior follows from how it is built, for every input, not just
the tested ones:

- The same upstream code runs on the same data in the same order, and the only
  work removed is work whose result is provably thrown away (for example, a call
  that would have returned at its first test with nothing changed). Keep side
  effects, their order, and every value a script or later code can read.
- Prefer a design in which a mismatch is impossible over one that must be kept in
  step. Keep a value in one place rather than a copy that has to be refreshed, and
  let the compiler find every user (remove or rename the field) rather than a
  search.
- Don't cache or skip anything that depends on state other code can change behind
  it (object fields that Lua can write, for example) unless the check covers that
  state exactly.
- Write the argument down in the commit message: what is skipped or moved, why
  the result is the same in every case, and what it assumes.

Tests support that argument and never replace it. Such a change comes with a
native test that compares the new code with a frozen copy of the earlier code on
random and hand-made inputs, with negative controls that show the comparison can
fail, and with a replay comparison: staff ghosts played with the old and the new
build must reach the same state (every thinking object, every racer, every random
seed) on every tic. Have an independent reviewer try to break it. Don't merge it
without the owner's explicit approval.

Changes that only draw, log, or measure are not game logic, but they must not
feed back into it: nothing the renderer, the frame rate, or a log line does may
change a value game logic reads.

## What the rule cannot cover

The rule can only promise upstream's behavior where upstream's code defines
one. Where upstream itself reads or writes memory it doesn't own, what happens
depends on the compiler and on where things lie in memory, and no change, not
even adding a field to a structure, can keep that the same. Known cases:

- Invalid map data. A map can ship its own `BLOCKMAP` lump, which the game
  loads without checking it (`P_ReadBlockMapLump`), and a Doom-format seg's
  linedef number isn't checked either (`P_LoadSegs`). An entry that names a
  line number at or past the map's number of lines makes the blockmap and sight
  walks read and write outside the level's arrays, in upstream and here, with
  different memory behind them.
- A value upstream reads before setting it. `P_SlideMove` builds a line on the
  stack without its `angle`, which `P_HitSlideLine` would read. What it finds
  depends on the stack's layout, which changes with the size of any structure
  nearby. (That path needs `g_tm.floorthing`, which only disabled code sets.)
- A script that leaves the game in a state upstream's code doesn't handle, so
  that it reads through a null or freed pointer. A `MobjLineCollide` hook, for
  example, can remove the moving object and leave `g_tm.thing` null in the
  middle of a blockmap line walk, and upstream then faults at the next line it
  visits. Where the result in practice is a crash at a known point, as there,
  keep that point. Where the result is whatever happens to be in memory, it
  can't be kept.

Don't lean on this. Before calling a case undefined, show the read or write
that leaves upstream's own objects, and say so in the commit message as an
assumption. A case where upstream's result is defined, however odd or
unintended, is covered by the rule. Don't "fix" an undefined case either (by
rejecting such a map at load, for example) without the owner's say-so: any
defined result is itself a difference from upstream.

# Versioning

The fork's version is on top of the Ring Racers version, as
`<Ring Racers version>_z<fork version>`: `2.4_z1` is the first release,
`2.4_z1.1` a bugfix release after it, `2.4.1_z1` the first release based on
Ring Racers 2.4.1.

To release, make a commit that raises `FORKVERSION` in `src/fork_version.h`, tag
it `<Ring Racers version>_z<fork version>` (`v` before it also works), and push the
tag. A build is a release only from a clean checkout of a commit with that tag
(`cmake/Modules/ForkVersion.cmake`). Builds from any other commit are
development builds, and the title screen, `version`, and the startup log say
`-dev` and the commit, e.g. `v2.4_z1-dev (1a2b3c4d)`.

`FORKVERSION` goes back to `"0"` (no release yet on that Ring Racers version) when
the fork is rebased onto a new Ring Racers version.

The text is display and log only. Nothing sent over the network or written to
replays or saves carries it; `SRB2VERSION`, `VERSIONSTRING`, and `MODVERSION` stay
upstream's, and `DEVELOP` stays off, so netplay and replays remain compatible with
the unmodified game.

# Testing

`tests/README.md` describes the native regression tests (Linux or macOS, with
sanitizers) and the driver's pixel tests: the Direct3D 12 WARP harness (Windows) for VibeRant D3D,
and the same scenes on VibeRant Vulkan (Linux) and VibeRant Metal (macOS).
`.github/workflows/tests.yml` runs all of them, and checks that Linux builds the driver with
only its Vulkan backend, that macOS builds it with only its Metal backend, and that
Windows desktop (not UWP) builds with it by default (beside OpenGL, using the desktop presenter).
It also runs `tests/check-retired-names.py`, which fails on a name the driver had before it
was called VibeRant; the script lists the old and new names.
`.github/workflows/build.yml` builds the UWP app with `uwp/build.ps1` and uploads
the package that is tested on Xbox; there is no local UWP build outside Windows.

# Commit attribution

Every commit created or amended with agent assistance must name the agents and
models that contributed in the commit message. Use the body or trailers; keep the
subject focused on the change. List each participating agent and its actual model,
and preserve existing attribution when amending or squashing a commit.

For example:

```text
Agent: Codex (Sol)
Model: gpt-6.1-sol
```

Preserve the human Git author identity. Do not invent model names or attribute
earlier work to the current agent.

# Delegating work

To reduce token usage, a more capable agent should hand routine work to
subagents rather than doing it itself, and keep the design decisions, reviews
and replies to the user. Give each subagent a self-contained brief with the
settled design, constraints, and commit attribution to use.

- Menial tasks (mechanical edits, searches, log extraction, documentation
  updates): prefer Claude Haiku, falling back to GPT Luna.
- Medium tasks (implementing a settled design, focused fixes, writing tests):
  prefer GPT Sol, falling back to Claude Sonnet.

These are model families, not versions: use the latest version of the family that
is available, which is assumed to be better than the ones before it.

Review a subagent's changes before committing or pushing them.

# Rendering sweeps

For a branch that changes rendering, run the Map sweep workflow on demand before
merging it (`gh workflow run map-sweep.yml --ref master -f ref=<branch>`), read
its job summary and the `map-sweep-reports` artifact, and resolve or explain every
red result: a health failure is a bug to fix or report, and a changed frame is a
prompt to look at its before, after, and difference crops. Also run it at
`size=1280x720` for a change to how geometry is clipped or filtered. See
`tests/README.md`, "Automated map sweeps", for the inputs, the baseline rules, and
what a sweep does not cover.
