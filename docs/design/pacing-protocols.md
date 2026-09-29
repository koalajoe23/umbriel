# Pacing protocols: fifo-v1 and commit-timing-v1

SDL 3.4 opens its window over Wayland only when the compositor advertises
`wp_fifo_manager_v1`; otherwise it falls back to Xwayland
(`SDL_waylandvideo.c`: "This compositor lacks support for the fifo-v1
protocol; falling back to XWayland for GPU performance reasons"). Minecraft
26.3 is one such client. `src/pacing/` gives Umbriel real implementations of
`wp_fifo_v1` and `wp_commit_timer_v1` so SDL3 clients run natively without
`SDL_VIDEO_DRIVER=wayland`.

Both protocols answer one question: when may this surface's next content
update become current? `Pacing` (`pacing.h`) is the only type `Server` sees;
it owns a `FifoManager` and a `CommitTimingManager`, each a manager global
plus per-surface protocol objects, both built on one `SurfacePacer`
(`surface_pacer.h`) per `wlr_surface`. The pacer tracks a surface's pacing
output, records that output's latest present event for prediction, and emits
one refresh signal — after a frame done for that output, on a timer while
hidden, or from a stall watchdog — but only while a manager with work to do
retains it. `PacerSubscription` (`pacer_subscription.h`) is the shared
retain/subscribe/unlink glue the two managers use to listen to a pacer and
retain it while they have work; `ProtocolGlobal` (`protocol_global.h`) is the shared "bind creates a
stateless resource" glue for both manager globals. The release rules
themselves are pure functions with no wlroots types: `fifo_queue.h`
(hold/release ordering, in-flight barrier setters) and
`commit_timing_policy.h` (presentation prediction, the release decision,
hidden-tick and stall-watchdog intervals). `pacing_clock.h`
holds the millisecond-rounding helpers `wl_event_source` timers need.

## Scope decision

wlroots 0.20.2 ships neither protocol. Upstream merge requests exist but are
unmerged: fifo-v1 in
[!4463](https://gitlab.freedesktop.org/wlroots/wlroots/-/merge_requests/4463)
and commit-timing-v1 in
[!4617](https://gitlab.freedesktop.org/wlroots/wlroots/-/merge_requests/4617).
`SCOPE.md` accepts "Wayland protocol support that real applications need",
and the SDL fallback above is that need. This is a temporary, removable
in-tree exception — the same shape as `src/server/wine_color_manager.{h,cpp}`
— not a permanent addition to Umbriel's protocol surface.

Advertising either global without real barrier semantics was rejected outright:
clients using fifo would render unthrottled, the exact starvation SDL's
fallback exists to avoid.

Out of scope: configuration keys (nothing here a user would sensibly
disable), protocol versions above 1, and VRR- or tearing-aware prediction.

## The umbrielfx hook is a per-surface addon

The spec's original plan put `output_change` and `frame_done` signals on
`struct wlr_scene`. That does not work: capture scenes are one per window
(`src/view/view.cpp:212`), so signals on `wlr_scene` would need every `View`
to register its own scene with the pacing code. Instead,
`umbrielfx_surface_pacing_get(surface)` (`umbrielfx/include/umbrielfx/types/
surface_pacing.h`) finds or creates a `wlr_addon` on `wlr_surface.addons`.
It receives events from every scene a surface appears in — main and capture
alike — with no per-view wiring, and leaves `struct wlr_scene` unchanged.

In `umbrielfx/types/scene/surface.c`, only when a surface already has the
addon: `handle_scene_buffer_outputs_update` recomputes
`umbrielfx_surface_frame_pacing_output(surface)` after updating scene
memberships, including on the suspend path, and emits `output_change` when
the value differs; `handle_scene_buffer_frame_done` emits `frame_done`
(output, `when`) next to `wlr_surface_send_frame_done`, behind the same
pacing-output filter.

The addon also listens to its current output's `destroy` signal directly
(`umbrielfx_surface_pacing::output_destroy`). Destroying a `wlr_scene_output`
does not re-run `outputs_update` for the surfaces that were on it, so without
this listener a destroyed pacing output would stay recorded as current and
the pacer would wait on a dead output forever. On the signal, the addon's
output is cleared and `output_change` fires as normal; `SurfacePacer` treats
that surface as hidden until umbrielfx reports a new one.

Frame done, not `present`, drives the visible refresh: `Output::handleFrame`
renders and commits only when the scene changed (`src/output/output.cpp:
1337`), so `present` does not fire on an idle frame, while
`wlr_scene_output_send_frame_done` runs unconditionally at the end of every
frame (`src/output/output.cpp:1478`). Clearing a fifo barrier on `present`,
as !4463 does, could stall a visible surface whose content update caused no
redraw.

## Refreshes are emitted after the scene walk, not inside it

A frame done reaches the pacer from inside the scene's frame-done tree walk
(`Output::handleFrame` → `wlr_scene_output_send_frame_done` →
`scene_node_send_frame_done`, a plain `wl_list_for_each`). A refresh listener
unlocks cached commits, and `wlr_surface_unlock_cached` runs the full role
commit path (`View::handleCommit` reparents, restacks, snapshots, and tears
down effect nodes), which could free list links the walk still holds. So
`SurfacePacer` never emits from the frame-done callback: it records the
refresh and arms an idle source (once; a second frame done before it runs
only replaces the recorded refresh), and the idle callback emits it. The
source is removed with the pacer. `Output` knows nothing of this. Timing is
unchanged, since released content renders on the next frame either way.

Two details keep it that way. The release-frame prediction is taken at the
frame done, from the presents recorded before it: headless sends a frame's
own present from an idle source queued ahead of the deferred refresh, and
predicting from that present would release a timed commit one period early.
And each refresh instant gets a serial when it happens
(`SurfacePacer::refreshSerial`), so the fifo can tell that a barrier applied
between a frame done and its deferred refresh was not used by that frame
(next section).

## In-flight barrier setters

fifo-v1 says a `set_barrier` update "will be active for at least one refresh
cycle". Deciding readiness at `client_commit` from the applied barrier alone
breaks that when another lock holds the setter: commit A sets the barrier but
waits on commit-timing (or a syncobj), commit B waits for the barrier and
finds none set and an empty queue, so it is not held, and when A's other lock
releases, wlroots applies A and B in the same flush. `FifoQueue` therefore
records every client commit (`committed`) and tracks setters from commit
until they apply (`applied`): a wait_barrier commit behind an unapplied setter
is held, and `refresh()` releases no commit that an unapplied setter is ahead
of, so a setter a refresh has just released still blocks the commits behind
it until it applies.

A barrier that applies at or after a refresh's instant was not used by that
refresh, so `Fifo` does not let that refresh clear it: it records the pacer's
refresh serial when a barrier is set and skips any refresh whose serial is
not newer. This covers the deferred-refresh window above and the case where
another listener of the same refresh applies the setter — commit-timing's
listener releasing A before the fifo's listener runs. The
`timing-fifo-pair` case of `protocol/commit_timing` asserts both: without
either piece, the setter is discarded.

## Retained only while working; the stall watchdog

A pacer emits refreshes and runs timers only while retained, and a manager
retains it only while it has work: a fifo while its barrier is set, commits
are held, or a setter is in flight (`FifoQueue::idle`); a commit timer while
commits are held. `PacerSubscription` holds at most one retain and drops it
on every way out (release, the owner's detach, the pacer going first).
Mesa creates a `wp_fifo_v1` for every Vulkan FIFO swapchain, so retaining for
the object's whole lifetime kept a 40–144 Hz hidden tick running for every
hidden, idle game or app; now a hidden surface ticks only until its queue
drains and its last barrier has had a refresh.

While retained and visible, a stall watchdog emits a refresh once
`kStallRefreshPeriods` (4) periods pass with none (`stallRefreshDelay`).
fifo-v1 explicitly allows clearing the condition early "to ensure client
forward progress". It covers the spec's session-inactive case: when the
session has lost the DRM device, `Output::handleFrame` returns before
rendering and sends no frame done, while the surface keeps its pacing output,
so held commits would otherwise stall for a whole VT switch. The spec asked
for that surface to lose its pacing output and fall back to the hidden tick;
the watchdog reaches the same end (queues keep draining, at a quarter of the
rate) without `Output` knowing about pacing, and also covers any other way an
output stops producing frames. A watchdog refresh counts as clock-only, like
a hidden tick: commit-timing releases only commits whose target has passed.
It is re-armed lazily (when it fires it re-checks and re-arms for what is
left), so steady frames cost no timer updates. A headless harness has no
session to lose, so the watchdog's interval is pinned by a unit test
(`tests/unit/commit_timing.cpp`) rather than a check.

## Predicting the release frame

`PacerRefreshEvent::releasePresent`, which `SurfacePacer` fills at each
frame done from `commit_timing_policy.h`'s `predictReleaseFramePresent`, is
the answer to
"what present will a commit released at this refresh reach?" It extrapolates
from the pacing output's last recorded `present` event, advancing by whole
periods, and depends on whether the frame behind the refresh committed.
`Output::handleFrame` sends frame done after every frame, committed or not,
and wlroots keeps `wlr_output.frame_pending` set from a successful commit
(`output_apply_commit`) until the next frame event (`wlr_output_send_frame`),
so `SurfacePacer` snapshots it at the frame done:

- **A flip is pending** (`frame_pending`): the released commit's damage waits
  for that flip's frame event, renders then, and presents a period later —
  `predictFollowingPresent`, the present after next.
- **No flip is pending**: the frame followed a flip with nothing new to draw.
  The released commit's damage gets a frame straight away
  (`wlr_output_schedule_frame` queues it on an idle source), which flips at
  the very next vblank — `predictNextPresent`. Assuming a pending flip here
  would predict a period too late and release the commit a period early.

A frame the commit timer asks for (`SurfacePacer::requestFrame`) always
commits: `wlr_output_schedule_frame` sets `output->needs_frame`, which makes
`wlr_scene_output_needs_frame` true, and `wlr_scene_output_build_state`
renders a buffer even with no damage. So the refresh of a requested frame has
a flip pending, and the next frame waits for that flip: asking for a frame
while a head is not yet due never turns into back-to-back frames.

This DRM behaviour is argued from the wlroots 0.20.2 source (`output.c`, the
DRM page-flip handler), not measured: the harness runs only headless outputs.

### Stale prediction: never early, up to one period late after idle

The extrapolation is only trustworthy while the last present is recent:
`kPredictionFreshPeriods` (2 periods). Once the surface's last present is
older than that, or it has never presented, the release frame is predicted
at `now` instead of by extrapolating a possibly-stale phase, whatever
`frame_pending` says.

This was found empirically: a headless output that has been idle resumes
rendering at an arbitrary phase relative to its last recorded present, so
extrapolating from a stale present could place the predicted release a whole
period in the wrong direction. Falling back to `now` after idle keeps the
rule one-sided — a timed commit is never released early, but it can be
released up to one period late right after an idle stretch, which is the
same bound normal (non-predictive) pacing gives a surface with no history at
all.

### Headless

A headless output has no vblank grid. It "presents" a commit at commit time
(from an idle source) and restarts its frame timer there, so its next frame
comes one frame delay after that commit. When a frame is requested while the
output idles, off the phase of its previous presents, that commit moves the
output's whole frame phase, while the prediction — still fresh by age — steps
along the old grid. A commit released at that refresh renders at the moved
frame and can present up to a period before its prediction. A stream that
targets three periods past each present hits this on every other commit
(about −16.7 ms at 60 Hz), and two periods past does occasionally under load
(down to about −12.7 ms), so `protocol/commit_timing_stream` streams only up to
a lead of one period. A DRM output's requested frame flips at the next vblank
of the same grid, so the prediction holds there; no headless-specific logic
exists in `src/pacing/`.

## Destroy semantics: two different protocol answers, taken as written

`wp_commit_timer_v1.destroy` follows its XML text: "Existing timing
constraints are not affected by the destruction."
`CommitTimingManager`'s `handleResourceDestroy` only clears the resource
pointer; held commits stay in the queue and keep waiting for their targets,
released by the normal refresh or hidden-tick path. A `get_timer` on the same
surface afterwards succeeds — nothing about the surface's timer state is
considered "still in use" once the resource is gone and the queue drains.

`wp_fifo_v1.destroy` releases every commit the fifo is holding
(`FifoQueue::drain`, called from `Fifo::handleResourceDestroy`), matching the
spec's stated intent. This is recorded here as an open interpretation rather
than a settled reading: `fifo-v1.xml` says "Surface state changes previously
made by this protocol are unaffected by this object's destruction," which
could equally be read as "queued barriers stay in force after the object
goes away" rather than "release everything now." Draining was chosen because
an orphaned queue with no `wp_fifo_v1` left to ever clear its barrier would
otherwise wedge the surface's pending commits indefinitely. Revisit this if
a future protocol clarification or a wlroots reference implementation picks
the other reading.

## Synchronized subsurfaces: honoured differently per protocol

A synchronized subsurface's own commit is never independently timed by its
parent's frame callbacks — its cached state only becomes current when the
parent's commit releases it. The two protocols still treat it differently,
because their XML says different things:

- `wp_fifo_v1`: `FifoQueue::shouldHold` never holds a synchronized
  subsurface's commit for a `wait_barrier` (`fifo-v1.xml` exempts them
  explicitly; the parent's commit already carries them).
- `wp_commit_timer_v1`: a synchronized subsurface's timestamped commit is
  held exactly like any other — the protocol makes no such exception — so
  wlroots applies the cached state only once both the parent's commit and
  this lock have released it.

## Deliberate differences from the upstream MRs

Three choices in `src/pacing/` differ from !4463 (fifo-v1) and !4617
(commit-timing-v1) on purpose, not by oversight:

1. **Barriers clear on frame done, not on `present`.** See "Frame done, not
   `present`" above — clearing on `present` would stall a visible surface
   whose content caused no redraw.
2. **A pacing-output change does not flush the fifo (or commit-timing)
   queue.** Both `handlePacerRefresh` implementations note this: switching
   outputs simply means the queue starts following the new output's
   refreshes. Flushing on output change would drop in-flight barriers for a
   surface that, say, migrated between two mapped outputs mid-burst.
3. **Timed commits release at frame boundaries, not by a per-commit timer.**
   A commit due for release waits for the next real refresh (or hidden tick)
   rather than firing its own timer at exactly its target, so a released
   commit reliably makes the frame it targeted instead of applying between
   frames and waiting for the next one anyway.

## The early-tolerance layer

The strict release rule (`timedCommitDueStrict`) says a timed commit is due
once its target has actually arrived: `target <= framePresent`. On top of
that, `timedCommitDue` — the function every caller actually uses — subtracts
`commitTimingEarlyTolerance(period)` from the target before the comparison,
where the tolerance is `period / kCommitTimingEarlyToleranceDivisor` (a
quarter of the refresh period). This absorbs a frame that lands a handful of
microseconds before its exact predicted instant, which would otherwise
gratuitously delay the commit by a whole extra period for no perceptible
gain.

The tolerance is deliberately one named constant used from one call site
inside `timedCommitDue`, so it can be removed without touching the strict
rule or its tests:

1. Delete `kCommitTimingEarlyToleranceDivisor` and `commitTimingEarlyTolerance`
   from `commit_timing_policy.h`/`.cpp`.
2. Change `timedCommitDue`'s body to call `timedCommitDueStrict(target,
   framePresent)` directly (or delete `timedCommitDue` and call
   `timedCommitDueStrict` from `commit_timing_manager.cpp`).
3. Delete the tolerance-specific cases in `tests/unit/commit_timing.cpp`
   (kept in their own test group precisely so this is a clean removal); the
   strict-boundary tests are unaffected.

## Known limitation: VRR and tearing

Prediction assumes presentation is periodic: it extrapolates the next
present from the last one plus whole multiples of a period. Under variable
refresh rate or tearing, presentation is not periodic, so both the release
prediction and the hidden-tick interval become approximate — closer to a
reasonable guess than a guarantee. This is accepted scope, not a bug to fix
here (see "Out of scope" above); revisit only if the removal in the next
section is not imminent and a real client's behavior degrades under VRR.

## Removal when wlroots ships both protocols

1. Delete `src/pacing/` and its unit tests (`tests/unit/fifo_queue.cpp`,
   `tests/unit/commit_timing.cpp`, removed from the `unit_tests` table); add
   `wlr_fifo_manager_v1_create` and `wlr_commit_timing_manager_v1_create` in
   `Server` where `m_pacing` is constructed and reset today
   (`src/server/server.cpp`).
2. Replace the umbrielfx addon with whatever API the merged MRs expose for
   telling a fifo or commit timer which output paces it (the open MRs'
   `set_output`-style calls are not a merged API), called from
   `umbrielfx/types/scene/surface.c` with the same capture-scene filter (only
   surfaces with pacing state attached, gated on the pacing output).
   umbrielfx forks wlroots' scene graph, so the MRs' own changes to wlroots'
   `types/scene/surface.c` do not reach Umbriel: port them into umbrielfx's
   copy. Delete `umbrielfx/include/umbrielfx/types/surface_pacing.h` and
   `umbrielfx/types/scene/surface_pacing.c`, and `SurfacePacer` and
   `PacerSubscription` with them. Keep `wp_fifo_manager_v1` and
   `wp_commit_timing_manager_v1` in `kAllowedSecurityContextGlobals`
   (`src/server/server.cpp`); the interface names do not change.
3. Keep the harness checks: `tests/harness/checks/protocol/{fifo,
   commit_timing,commit_timing_stream}.sh` and
   `tests/harness/clients/pacing_client.cpp` assert protocol behavior, not
   this implementation, and should keep passing unchanged against the
   wlroots-native globals. `protocol/fifo_hidden` is different: it asserts
   Umbriel's hidden-tick policy (a hidden surface drains one commit per
   tick), which the protocol does not require — it lets a compositor ignore
   the constraint while hidden. Keep it only if the native implementation
   adopts the same policy; otherwise reduce it to "the burst drains" or drop
   it.
