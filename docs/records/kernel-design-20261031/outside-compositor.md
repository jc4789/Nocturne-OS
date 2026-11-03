<!-- 2026-10-31 に持ち主が貼った、外の AI の合成の案（原文のまま）。取り込んだ所は choice.md の「合成の約束」。 -->

I’d design it around **deadlines and independently paced displays**, not around a global “render a frame, then wait for vblank” loop.

At 600 Hz, a refresh lasts **1.667 ms**. That is the interval between presentation opportunities, *not* a guaranteed 1.667 ms in which to receive an app’s frame, composite it, and deliver it to the display. The design has to avoid doing most of that work on the critical path.

## The basic model

Each display has its own **presentation engine**: its own clock, pending frames, deadline estimates, and decision about what to show next. A 600 Hz display, a 144 Hz display, and a VRR display do not share a compositor tick. A window spanning two displays may be sampled at different times on each.

Applications publish **surface streams**—immutable images with readiness signals, damage, color information, and optional timing intent. Separately, they publish **scene transactions**: atomic changes to placement, visibility, clipping, effects, and input regions. The compositor never waits for an application while holding up a display. At a display deadline, it takes the newest *ready, complete* state; otherwise it presents the last valid state.

That gives the central rule:

> **A missed application deadline costs that application a presentation opportunity, not the entire desktop a refresh.**

## Make the common case almost free

The presentation engine chooses the cheapest valid route for each opportunity:

1. **Direct scanout** when one surface can be displayed as-is.
2. **Hardware composition** for a small number of surfaces or a cursor, if the display hardware can do it correctly.
3. **GPU composition** when transforms, effects, color conversion, or too many layers require it.

These are outcomes of the same scene model, not separate application APIs. Moving from direct scanout to GPU composition must not require an app to recreate its buffers or renegotiate its presentation contract.

The GPU path should be incremental: track damage, reuse unchanged content, and avoid full-screen passes for a tiny blinking caret. Conversely, don’t let damage tracking become so elaborate that calculating damage costs more than rendering it.

## Schedule backward from scanout

For every presentation opportunity, the display engine estimates a **latest safe submission time** from measured display and GPU behavior. Work is prepared before that time:

- Ready surfaces are identified without blocking.
- Likely composition work is started early.
- Near the cutoff, the engine selects the newest state it can *prove* will be ready.
- If new work misses, it uses an older complete result.

A pointer should normally use an independently updated hardware cursor. For interactive surfaces that need composition, allow a narrowly defined **late update**—for example, replacing a cursor position or transform before the command submission cutoff—without reopening the whole scene.

Predictions are useful, but a prediction is never a reason to display a partially written image. Every presented image has an explicit lifetime: the producer cannot reuse it until all displays and GPU work referencing it have finished.

## Separate input latency from frame rate

Input delivery should not wait for the next display tick. Send events immediately to the relevant app, with accurate timestamps and a prediction of the next viable presentation time. Coalesce intermediate pointer motion when appropriate; never silently coalesce button presses, key transitions, or ordered gestures.

The compositor should also own the responsiveness of things it can render independently—pointer movement, perhaps some window-management interactions—so a stalled application cannot make the whole machine *feel* stalled.

## Treat 600 Hz as a constraint, not a mandate

An application does **not** owe the compositor 600 distinct frames per second. A video may update at its source cadence; a static page may not update at all; a game may target 600, 300, or an adaptive rate. The display engine can repeat a valid frame without asking the app to render it again.

VRR should be supported per display, but not used as an excuse for unpredictable latency. The engine should choose between presenting now, waiting briefly for a known imminent frame, and holding a stable cadence according to the surface’s stated intent and observed behavior.

## Build in admission and observability

I would expose an honest contract to applications: their submission time, when the image became ready, whether it was presented, repeated, or skipped, and the actual presentation timestamp. The compositor records why a deadline was missed—app late, GPU busy, composition too costly, display cutoff missed—rather than reporting generic “dropped frames.”

Under load, degrade deliberately: reuse an old frame, reduce optional effects, or decline an expensive overlay transition. Never make all displays miss their deadlines to rescue one late surface.

## Hardware and measurement reality

At 4K, 600 distinct 8-bit RGBA frames represent roughly **20 GB/s of active pixel data** before blanking, intermediate rendering, or multiple reads and writes. A careless full-screen composition pipeline can multiply the memory traffic. Scanout bandwidth, display link capability, GPU scheduling latency, and buffer availability are therefore architectural concerns, not final optimizations.

I’d validate the design with a trace from **input device → app receipt → image ready → composition/scanout selection → photons**, using external measurement for the last step. The key metrics are latency distributions and missed-deadline causes—not just average compositor frame time.

In short: **per-display deadlines, immutable publishable state, nonblocking selection, direct scanout where correct, incremental composition where necessary, and explicit fallback to the last complete frame.** That architecture works at ordinary refresh rates too; 600 Hz simply makes hidden waits and unnecessary work impossible to ignore.
