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
  UWP build, and `HAVE_D3D12` for builds that contain the Direct3D 12 driver
  (`src/hardware/r_d3d12`, `SRB2_CONFIG_D3D12`), which UWP builds and Windows desktop builds
  made with MSVC or clang-cl have by default. A UWP build has it as its only hardware driver; a build with `HAVE_D3D12`
  but not `_UWP` has both it and OpenGL, and the video code picks one per run
  of the game. Behaviour that depends on the driver (what the hardware renderer
  draws or how it walks the level, calls into the driver) therefore checks
  `HWR_D3D12()` (`src/hardware/hw_driver.h`) at run time: it is the constant 1 with
  `_UWP`, the constant 0 without `HAVE_D3D12`, and `hwr_d3d12` between them.
  `#ifdef HAVE_D3D12` is only for what has to be compiled out without the driver:
  includes, types, struct fields, enum values, prototypes, function definitions, a
  call of a Direct3D 12 function (with the `HWR_D3D12()` check around it), and
  bookkeeping that doesn't depend on the driver. Code under `HAVE_D3D12` must
  compile without `_UWP` (Xbox-only things inside it get their own `_UWP`
  guard). Linux, macOS and other Windows builds must build without either.
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
`.github/workflows/tests.yml` runs both, and checks that Linux and macOS still build
without the Direct3D 12 driver, and that Windows desktop (not UWP) builds with it
by default (beside OpenGL, using the desktop presenter).
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
  updates): prefer Haiku 5.5, falling back to the latest GPT Luna model.
- Medium tasks (implementing a settled design, focused fixes, writing tests):
  prefer GPT-6.1-Sol, falling back to Sonnet 5.5.

Review a subagent's changes before committing or pushing them.
