# This fork

`master` is upstream's [Ring Racers](https://github.com/KartKrewDev/RingRacers)
`maintenance-2.4` branch plus our changes, one commit per change. The UWP (Xbox)
app is built from this repository too: `uwp/` holds its launcher, build script,
SDL patches, profile recordings and tools, and `CHANGES.md` is the user-facing
changelog. They came from a separate repository,
[ringracers-uwp](https://github.com/ZebulanStanphill/ringracers-uwp) (archived),
whose history joins this one at the commit "Merge the UWP app's repository".

# Commit scope

Keep each commit focused: implement one change or fix per commit, with the
documentation for that change (its commit message, and the docs in `docs/` or
`tests/README.md` that describe it) and its `CHANGES.md` entry. Split unrelated
changes into separate commits, even when they're made in the same session.

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
  code. Don't reformat, rename or reorder upstream code you aren't changing.
- Gate platform-specific code so every platform keeps building: `_UWP` for the
  UWP build, and `HAVE_VIBERANT` for builds that contain the VibeRant driver
  (`src/hardware/r_viberant`). The driver has three backends. `HAVE_VIBERANT_D3D`
  (`SRB2_CONFIG_VIBERANT_D3D`) is Direct3D 12 itself, VibeRant D3D, which UWP builds and
  Windows desktop builds made with MSVC or clang-cl have by default.
  `HAVE_VIBERANT_VULKAN` (`SRB2_CONFIG_VIBERANT_VULKAN`) is the Vulkan backend, VibeRant Vulkan,
  which Linux builds and those Windows desktop builds have by default:
  `src/hardware/r_viberant/vulkan` implements the Direct3D 12 interfaces the driver
  uses, so the driver's own code is the same for both
  (`docs/vulkan-backend.md`). Only code that makes the device, compiles shaders
  or presents asks which backend it is on (`Core_Vulkan()`); engine code doesn't,
  except to name the renderer (`HWR_Vulkan()`). A UWP build has the driver as its
  only hardware driver; a build with `HAVE_VIBERANT` but not `_UWP` has both it and
  OpenGL, and the video code picks one per run of the game. Behaviour that depends on the driver (what the hardware renderer
  draws or how it walks the level, calls into the driver) therefore checks
  `HWR_VibeRant()` (`src/hardware/hw_driver.h`) at run time: it is the constant 1 with
  `_UWP`, the constant 0 without `HAVE_VIBERANT`, and `hwr_viberant` between them.
  `#ifdef HAVE_VIBERANT` is only for what has to be compiled out without the driver:
  includes, types, struct fields, enum values, prototypes, function definitions, a
  call of a Direct3D 12 function (with the `HWR_VibeRant()` check around it), and
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

# Versioning

The fork's version is on top of the Ring Racers version, as
`<Ring Racers version>_z<fork version>`: `2.4_z1` is the first release,
`2.4_z1.1` a bugfix release after it, `2.4.1_z1` the first release based on
Ring Racers 2.4.1.

To release, make a commit that raises `FORKVERSION` in `src/fork_version.h`, tag
it `<Ring Racers version>_z<fork version>` (`v` before it also works) and push the
tag. A build is a release only from a clean checkout of a commit with that tag
(`cmake/Modules/ForkVersion.cmake`). Builds from any other commit are
development builds, and the title screen, `version` and the startup log say
`-dev` and the commit, e.g. `v2.4_z1-dev (1a2b3c4d)`.

`FORKVERSION` goes back to `"0"` (no release yet on that Ring Racers version) when
the fork is rebased onto a new Ring Racers version.

The text is display and log only. Nothing sent over the network or written to
replays or saves carries it; `SRB2VERSION`, `VERSIONSTRING` and `MODVERSION` stay
upstream's, and `DEVELOP` stays off, so netplay and replays remain compatible with
the unmodified game.

# Testing

`tests/README.md` describes the native regression tests (Linux or macOS, with
sanitizers) and the Direct3D 12 WARP harness (Windows).
`.github/workflows/tests.yml` runs both, and checks that Linux builds the driver with
only its Vulkan backend, that macOS builds it with only its Metal backend, and that
Windows desktop (not UWP) builds with it by default (beside OpenGL, using the desktop presenter).
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
and replies to the user. Give each subagent a self-contained brief: the
settled design, constraints, and the commit attribution to use.

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
