# Frame comparison

Compare builds or renderers by playing the same replays and checking screenshots pixel by
pixel at matching game tics. Use it to check VibeRant D3D, Vulkan, or Legacy GL against
Software or another renderer, or to verify that a change preserves frames it should leave unchanged.

You need [uv](https://docs.astral.sh/uv/), which fetches `numpy` and `pillow`, and a game
data directory with `bios.pk3`, `data/`, and `models/`. Set `$RR_GAME_DIR` to use a path
other than `~/.cache/rr-wine/game`. Staff ghosts are in `data/staffghosts.pk3`.

## Running

```sh
F=tests/frame-compare/framecompare.py

# Play the replays on each side (one game at a time, sides one after the other per replay) ...
uv run $F run results \
  --side "sw=native:/path/to/build/bin/ringracers -software" \
  --side "d3d=wine:rr-d3d12-fc.exe -d3d12"

# ... and compare two of the sides
uv run $F compare results sw d3d          # writes results/compare-sw-vs-d3d/index.html and report.json
```

`uv run $F list` shows the default replays, `--quick` runs two of them, `--replays a,b` picks some,
`--tics 70,210` replaces their tics, and `--resume` skips runs that finished already.
`compare ... --fail-on-diff` exits 1 if any frame differs, for scripts; `--threshold N` counts a pixel
only when a channel differs by more than N.

A **side** is `NAME=KIND:TARGET [renderer arguments]`:

| kind | TARGET | runs |
| --- | --- | --- |
| `native` | path to a local game binary | the binary, in the game data directory |
| `wine` | an `.exe` in the game data directory (or a full path) | the Windows build under Wine (`~/.cache/rr-wine/prefix`, `$RR_WINE_PREFIX`) |

The arguments are the renderer's: `-software`, `-opengl`, `-d3d12` (Windows builds), and whatever
the Vulkan renderer takes. To compare Vulkan with VibeRant D3D, add a third side
without changing the tool.
`--side "vk=native:/other/build/bin/ringracers -vulkan"`. A new way of starting a game (another
emulator or a remote machine) needs a class and one line in `KINDS` in `sides.py`.

For a Windows build, push the branch and download the `windows-desktop` artifact of its
Tests run (`gh run download -R ZebulanStanphill/RingRacers -n windows-desktop <run id>`). Copy it
into the game directory with a name that has `rr-d3d12` in it (the window rule below matches on
that), for example `rr-d3d12-fc.exe`.

### Windows must stay hidden

A game window must never cover the desktop or take focus (a game that loses focus pauses, and the
results are invalid). On Hyprland, a window rule sends the games to a hidden workspace; install it
once per Hyprland session, with `--install-hypr-rule` or by hand:

```sh
hyprctl eval 'hl.window_rule({ name = "rr-wine-test", match = { class = "(?i).*(ringracers|rr-d3d12|rr-three).*" }, workspace = "special:rrtest silent", no_initial_focus = true, no_focus = true })'
```

The class is the binary's name without `.exe`: it must contain `ringracers`, `rr-d3d12`, or
`rr-three` (a native binary called `ringracers_<branch>` does). Once a second, the runner checks that all game windows are on `special:rrtest`.
If it finds a window elsewhere, it kills the game and stops.
The game is always started with `+showfocuslost off +pauseifunfocused off`, a fixed window
(`--width`, `--height`, 1280x720), `-noaudio`, and a fresh home directory per run.

## Capture mode (the engine's part)

Use `-shottics 70,210,350` with a replay to capture the finished frame, including the HUD, at
each listed level tic. The engine draws exactly one frame per tic without interpolation, saves
each shot as `<MAP>_<tic>.png` in `<home>/.ringracers/framecapture` (`-shotdir <dir>` overrides
it), and quits after the last shot or when the replay ends. Level time starts at the title
card. A Time Attack ghost's race clock starts at about tic 37.

The replay is started with `-shotghost RR_AERIALHIGHLANDS-blaze-MK` (a staff ghost out of
`staffghosts.pk3`, by the name of its file without `.lmp`), or with `-playdemo file.lmp` /
`+playdemo file.lmp` for a replay file (relative to the game's home directory). The command line
can't start a staff ghost itself: `-playdemo` and the `playdemo` command only read replay files, and
refuse the staff ghosts' version 0. See `src/d_framecapture.cpp`.

Capture mode also controls other sources of variation between runs of the same replay.

- the unsynced random numbers (`M_Random`, which is `rand()` seeded with the time and differs between
  C libraries; the HUD's `PR_INTERPHUDRANDOM` and `PR_NUISANCE`) restart every frame from a state that
  depends on the level time alone;
- the animations the machine's clock drives (a dancing string such as the EXP counter, a blinking button
  prompt) read the level time instead of `I_GetTime()`;
- the time between two frames, which some animations go by instead of the level time (Legacy GL's underwater and
  heat waves, scrolling parts of the HUD), is one tic, whatever the clock measured;
- the "Screen shot saved" message and the capture's own messages stay out of the console (they would be
  in the next frame).

These controls require `-shottics`. Software's threaded rendering (`parallelsoftware`) stays
on. In the replays checked with it, no two runs of the same Software build differed.
Renderer-specific drawing still varies by design, such as Legacy GL's coronas, whose alpha
also uses an unsynced random number.

## What the tool produces

`results/<side>/<replay>/` has the screenshots (`<MAP>_<tic>.png`) and `run.log` (the game's own output) of each run, and
`results/manifest.json` records the commands. `compare` writes `results/compare-<a>-vs-<b>/`:

- `index.html`: a page with the totals, each frame that differs (sorted by how much, with its numbers and
  a side-by-side image: the first side's frame, the second side's, and a difference map from red to yellow)
  and a table of every frame. Images are referenced by relative path, so the results directory can be moved.
- `report.json`: per frame, `status` (`identical`, `different`, `missing in <side>`, `size differs`),
  `differing_pixels`, `differing_share`, `mean_channel_diff` (over all pixels and channels), `max_channel_diff`,
  `bbox` of the differences, and the paths of both frames and of the side-by-side image.
- `sheets/`: the side-by-side images.

Frame identity compares decoded RGB pixels and ignores PNG encoding.

## The default replays

Fifteen staff ghosts picked for different looks: water (Aquatic Cathedral, Hydro Plant, Deluged
Metroplex, Whirl Waters), fog and haze (Haunted Ship, Pestilence), dark levels (Dark Chao Garden, Vanta
Black Violet, Mystic Cave, Darkvile Castle), skies and skyboxes (Aerial Highlands, Sky Babylon), 3D
floors and cities (Metropolis, Robotnik Coaster), and Marble Garden Zone for its portals. Each has six tics: three
in the first seconds (title card, the start line, the first moments of the race) and three through the run.
`replays.py` has the list with the reason for each.

The staff ghosts are single-racer Time Attack runs, with no crowded start grid or Encore replays.
To use a race replay recorded with bots or an Encore replay, add it to `replays.py` with `"args": ["+playdemo", "replay/mine.lmp"]`.
