"""The default set of replays: staff ghosts out of data/staffghosts.pk3, picked for different looks.

A replay is a dict:
  name   the directory name in the results
  ghost  a staff ghost, "<MAP>-<skin>-<player>" (the lump name in staffghosts.pk3 without ".lmp"), played with
         -shotghost; or "args": a list of engine arguments that start some other replay (for example
         ["+playdemo", "replay/mine.lmp"], a file relative to the game's home directory)
  tics   level times to take a screenshot at (the engine's -shottics)
  why    what the replay is there to cover

Level time 0 is the start of the level: the title card shows for the first seconds and the race clock starts at about
tic 37, so the early tics catch the start line with the title card and the first moments of the race. The staff
ghosts are Time Attack runs, which have one racer on the track; there are no race replays with more in the game's
data (record some and add them with "args" for the start-grid case).
"""

# Each replay is covered at the start (tics 5, 35, 70: title card, the line, the first seconds), then three tics
# through the run. A replay shorter than its last tic just stops there; the comparer reports the missing frames.
_EARLY = [5, 35, 70]


def _r(name, ghost, later, why):
    return {"name": name, "ghost": ghost, "tics": _EARLY + later, "why": why}


DEFAULT_REPLAYS = [
    _r("aerial-highlands", "RR_AERIALHIGHLANDS-blaze-MK", [400, 1500, 3500],
       "sky, clouds and a skybox, brick walls, Software's sky drawing"),
    _r("marble-garden", "RR_MARBLEGARDEN-aigis-eebrozgi", [500, 1800, 4000],
       "Marble Garden Zone: portals, long draw distance, many sector tricks"),
    _r("aquatic-cathedral", "RR_AQUATICCATHEDRAL1-eggman-Freyaday", [300, 700, 1100],
       "water you drive in and under, water surface and under-water tint"),
    _r("hydro-plant", "RR_HYDROPLANT-eggman-Freyaday", [500, 1800, 3800],
       "water, translucent water planes, light shafts"),
    _r("deluged-metroplex", "RR_DELUGEDMETROPLEX-eggman-Freyaday", [400, 900, 1700],
       "flooded streets: water and city 3D floors"),
    _r("dark-chao-garden", "RR_DARKCHAOGARDEN-eggman-Freyaday", [400, 900, 1500],
       "dark level, point-like lights, sector lighting"),
    _r("vanta-black-violet", "RR_VANTABLACKVIOLET-heavymagician-Freyaday", [500, 1800, 4000],
       "dark level, lit sprites and bright effects against black"),
    _r("mystic-cave", "RR_MYSTICCAVE-eggman-Freyaday", [400, 1500, 3000],
       "dark cave, coloured sector lights, 3D floors (bridges)"),
    _r("haunted-ship", "RR_HAUNTEDSHIP-blaze-MK", [400, 1200, 2500],
       "fog and a dim, hazy ship interior"),
    _r("pestilence", "RR_PESTILENCE-heavymagician-Freyaday", [500, 1500, 3000],
       "fog and coloured haze, sector colourmaps"),
    _r("metropolis", "RR_METROPOLIS-eggman-Freyaday", [400, 1200, 2700],
       "3D floors and a city, the map the spring clip overflow was found on"),
    _r("sky-babylon", "RR_SKYBABYLON-heavymagician-Freyaday", [500, 1800, 4000],
       "a large open sky level with a skybox and distant geometry"),
    _r("robotnik-coaster", "RR_ROBOTNIKCOASTER-eggman-Freyaday", [500, 1800, 3500],
       "an amusement park: many sprites, scrolling textures, 3D floors"),
    _r("darkvile-castle", "RR_DARKVILECASTLE1-eggman-Freyaday", [500, 1800, 3600],
       "a dark castle: FOFs, flickering lights, fog"),
    _r("whirl-waters", "RR_WHIRLWATERS-eggman-Freyaday", [300, 1000, 1900],
       "water with currents, flowing water textures"),
]

# A short set for trying a setup out
QUICK_REPLAYS = [r for r in DEFAULT_REPLAYS if r["name"] in ("aerial-highlands", "aquatic-cathedral")]


def ghost_args(replay):
    """The engine arguments that start a replay."""
    if replay.get("args"):
        return list(replay["args"])
    return ["-shotghost", replay["ghost"]]
