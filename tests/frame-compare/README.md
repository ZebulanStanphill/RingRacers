# Frame comparison

Plays the same replays in two builds or renderers of the game, takes a screenshot at the
same game tics, and compares the screenshots pixel by pixel. It is the check that a
renderer (VibeRant D3D, a Vulkan one, Legacy GL) draws what Software draws, or what another
renderer draws, and that a change did not alter a frame it shouldn't have.

Needs [uv](https://docs.astral.sh/uv/) (it fetches `numpy` and `pillow` itself), and a
game data directory (`bios.pk3`, `data/`, `models/`; `$RR_GAME_DIR`, by default
`~/.cache/rr-wine/game`). The staff ghosts it plays are in `data/staffghosts.pk3`.

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
| `native` | path of a game binary of this machine | the binary, in the game data directory |
| `wine` | an `.exe` in the game data directory (or a full path) | the Windows build under Wine (`~/.cache/rr-wine/prefix`, `$RR_WINE_PREFIX`) |

The arguments are the renderer's: `-software`, `-opengl`, `-d3d12` (Windows builds), and whatever
the Vulkan renderer gets. Comparing a Vulkan build with VibeRant D3D is a third side beside the
other two, with no change to the tool:
`--side "vk=native:/other/build/bin/ringracers -vulkan"`. A new way of starting a game (another
emulator, a remote machine) is a class and one line in `KINDS` in `sides.py`.

A Windows build comes from CI: push the branch, and download the `windows-desktop` artifact of its
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
`rr-three` (a native binary called `ringracers_<branch>` does). The runner also checks once a second
that no game window is anywhere but on `special:rrtest`, and kills the game and stops if one is.
The game is always started with `+showfocuslost off +pauseifunfocused off`, a fixed window
(`--width`, `--height`, 1280x720), `-noaudio`, and a fresh home directory per run.

## Capture mode (the engine's part)

The engine takes the screenshots. `-shottics 70,210,350` with a replay plays the replay with
exactly one frame drawn per tic and no interpolation, takes a screenshot of the finished frame
(HUD included) when the level time reaches each listed tic, names it `<MAP>_<tic>.png` in
`<home>/.ringracers/framecapture` (`-shotdir <dir>` picks another directory), and quits after the
last one or when the replay ends. The level time is the one the game counts from the start of
the level (the title card shows at the start, and the race clock of a Time Attack ghost starts at
about tic 37).

The replay is started with `-shotghost RR_AERIALHIGHLANDS-blaze-MK` (a staff ghost out of
`staffghosts.pk3`, by the name of its file without `.lmp`), or with `-playdemo file.lmp` /
`+playdemo file.lmp` for a replay file (relative to the game's home directory). The command line
can't start a staff ghost itself: `-playdemo` and the `playdemo` command only read replay files, and
refuse the staff ghosts' version 0. See `src/d_framecapture.cpp`.

Capture mode keeps the game at one tic per frame, always draws the frame (no frame skipping),
and turns frame interpolation off. It also pins what would make two runs of the same replay differ:

- the unsynced random numbers (`M_Random`, which is `rand()` seeded with the time and differs between
  C libraries; the HUD's `PR_INTERPHUDRANDOM` and `PR_NUISANCE`) restart every frame from a state that
  depends on the level time alone;
- the animations the machine's clock drives (a dancing string such as the EXP counter, a blinking button
  prompt) read the level time instead of `I_GetTime()`;
- the "Screen shot saved" message and the capture's own messages stay out of the console (they would be
  in the next frame).

Without `-shottics` none of it applies. Not pinned: Software's threaded rendering (`parallelsoftware`), which is left on; no two
runs of the same Software build differed in the replays checked with it. What stays different
between renderers on purpose is what a renderer draws itself, such as Legacy GL's coronas (their alpha
is an unsynced random number too).

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

A frame is identical when its decoded RGB pixels are equal (not its PNG bytes).

## The default replays

Fifteen staff ghosts picked for different looks: water (Aquatic Cathedral, Hydro Plant, Deluged
Metroplex, Whirl Waters), fog and haze (Haunted Ship, Pestilence), dark levels (Dark Chao Garden, Vanta
Black Violet, Mystic Cave, Darkvile Castle), skies and skyboxes (Aerial Highlands, Sky Babylon), 3D
floors and cities (Metropolis, Robotnik Coaster), and Marble Garden Zone for its portals. Each has six tics: three
in the first seconds (title card, the start line, the first moments of the race) and three through the run.
`replays.py` has the list with the reason for each.

Limits of what the game's data offers: the staff ghosts are Time Attack runs, so there is one racer on the
track and no start grid with many racers; and none of them is an Encore replay. A race replay recorded
with bots, or an Encore one, can be added to `replays.py` with `"args": ["+playdemo", "replay/mine.lmp"]`.
