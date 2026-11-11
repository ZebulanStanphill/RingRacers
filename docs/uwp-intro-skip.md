# UWP startup intro skip timing

The built-in intro runs at 35 tics/second. This change applies only under
`_UWP`; desktop keeps its original timing. Custom cutscenes, levels, netplay,
and replay processing are unchanged.

## Original behavior

`F_IntroResponder` accepts Escape, Enter, Space, Backspace, mouse buttons 1/2,
and Xbox A/X (Enter) or B (Backspace). Y maps to `n` and does not skip;
Start, bumpers and stick movement are not ordinary skip buttons. Trigger-axis
events use a separate threshold path. The mapping is hardcoded, independent of
player control bindings or assignment. SDL controller buttons become
`ev_keydown`/`ev_keyup` with `KEY_JOY1 + button` and their device instance ID.
`G_Responder` calls the intro responder for `GS_INTRO`.

The responder has no minimum-time gate and no requirement for a key-up before
another press. It latches one press until `F_IntroTicker`, which clears the
latch every tic, including when the skip is refused. An early tap is lost;
holding a controller button does not synthesize another button-down event.

`F_IntroTicker` originally requires `intro_curtime > TICRATE/2`, i.e. an elapsed
counter of at least 18 tics (0.514 s), for every ordinary skip. It checks the
previous counter before updating it; a scene starting at counter zero therefore
first acts on such a press on its 19th ticker call (0.543 s).

- Scene 0, associates (`STARTUP`): 70 tics / 2 s. The scene eligibility test
  excludes it entirely, so no ordinary press skips it. On UWP it is also shown
  during loading. Desktop's first six elapsed counter values draw blue instead.
- Scene 1, SEGA/disclaimer: nominally 315 tics / 9 s. Before `DISCLAIMER_SLIDE`,
  a qualifying press fast-forwards to the slide, not past the warning. The slide
  cannot be skipped. In `DISCLAIMER_FINAL`, a press is accepted only after
  `dc_textfade == 0`. With one drawing state update per tic, the normal animation
  takes 17 shine + 15 white hold + 5 fade + 65 sound hold + 28 slide = 130 updates
  to reach FINAL, then 25 updates to fade the text fully in (decrements at local
  tics 0,3,...24). Thus dismissal becomes possible after about 155 updates /
  4.43 s without an earlier fast-forward. A fast-forward starts the 28-update
  slide immediately, followed by the 25-update warning fade (about 1.51 s).
  Before the game has truly started, it begins in FINAL and needs those 25
  updates / 0.714 s. These are animation-update counts, not guaranteed wall
  times: the drawer advances at most once per distinct intro counter it sees,
  and initial setup at counter zero returns without an update. Automatic exit
  begins when the remaining counter is below 17 (elapsed 299 / 8.54 s), plus
  blocking wipes. First-start flow goes to the title after the disclaimer.
- Scene 2, Kart Krew: 105 tics / 3 s. A press works at elapsed 18 / 0.514 s,
  then goes to the title. Its logo sound at elapsed 17 is not an input guard.
- Scene 3, S&K: 23 tics / 0.657 s. Only the last five elapsed counter values
  (18 through 22) admit a skip, then go to the title.
- Scene 4, Get ready: 46 tics / 1.314 s. Skipping likewise starts at elapsed 18,
  then goes to the title.

All ordinary skipping also requires `skippableallowed`, computed by
`F_StartIntro` from the progression challenge/unlock state. This lock is kept.
Quick-skip sequences are separate development/testing paths and are unchanged.

## Changes and retained guards

UWP accepts a fresh press on any logo card from elapsed zero, including scene 0
(which advances to the disclaimer). The half-second gate is removed on UWP,
including the optional opening SEGA animation. The disclaimer still cannot be
dismissed until its text has faded fully in; skipping its opening animation
still goes through the slide and warning. This preserves upstream's deliberate
warning visibility requirement. Natural durations and wipes are unchanged.
The press latch and elapsed counter are reset at intro start to avoid carrying
state from a previous UI now that no time gate masks it. Existing event-based
controller behavior is retained; this does not add hold-to-skip or input buffering.

`I_StartupInput` initializes SDL after graphics startup, well before
`F_StartIntro`. SDL queues controller-added events for attached pads, but the
engine previously opened them only in `I_GetEvent` during polling. UWP now
drains just those initial added events at input startup through the existing
open handler. This begins button event delivery earlier, consumes each added
event once (avoiding duplicate opens), and leaves button events and later
hotplug processing to the normal event loop. `-nojoy` and initialization failure
still return before this hook. Device registration/logging still happens later,
when the engine processes its event queue.

The reported log's connection notice about 1.1 s after entering `GS_INTRO`
therefore does not prove that SDL enumeration itself took 1.1 s: it timestamps
engine event processing after opening. The new hook removes the avoidable
wait to open already-enumerated pads; it cannot accelerate actual asynchronous
OS/SDL discovery or supply input before a controller exists.

Likewise, the reported roughly one-second palette/first-frame stalls prevent
normal polling and drawing while the main thread is blocked. `V_SetPalette`
synchronously invokes the renderer's palette path; it contains no intro input
gate. The log timings supplied with the complaint cannot isolate enumeration,
palette work and presentation costs. Those stalls and blocking wipes remain;
Xbox profiling and a device test are required to measure the remaining latency.

## Validation

`intro_uwp` and `intro_desktop` extract the full production ticker, responder,
scene durations and disclaimer enum. They check immediate and 17/18-tic
boundaries, disclaimer visibility and lost early taps, the progression lock,
Xbox/keyboard mapping, key-up rejection, the one-press latch, and unchanged
natural durations. The UWP test fails against the pre-fix ticker on its first
scene-zero skip assertion. `startup_gamepads` executes the production initial
event drain and controller-open handler against SDL mocks, checking multiple
pads, failed opens, instance IDs, preserved button events and no duplicate drain.

Live SDL integration, Windows/UWP linking and Xbox input/visual latency require
platform testing.
