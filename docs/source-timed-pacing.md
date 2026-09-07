# Source-timed Smooth experiment

Opt-in: Frame pacing = Smooth, then enable Source-timed Smooth (experimental).
Changes apply on the next stream. The setting defaults off. Switching it off
restores the prior Smooth implementation; Standard and Low latency are unchanged.

## Scope

- Linux Gamescope Vulkan/libplacebo with hardware decoding and presentation feedback.
- Reuse the existing render thread, source RTP timestamps, and three-frame queue cap.
- Select the newest due frame at a predicted display slot, with one reported panel
  refresh of reserve. Do not wait for a second source frame just to prime a buffer.
- Use vkGetRefreshCycleDurationGOOGLE, not the spacing of sparse source presents,
  to determine panel refresh. Never equate 30 FPS delivery to a 30 Hz panel.
- Corroborate the API period against the SDL display mode. Live testing found a
  stale 60 Hz API default on the 90 Hz Deck. On disagreement, validate 32 actual
  intervals normalized by display scans before activating source timing.
- Convert MONOTONIC presentation targets to the local RAW decode-clock domain using
  bracketed samples. Do not compare absolute values from the two clocks.
- Drop superseded candidates; wait without synthetic presents when nothing is due.
- Missing/stale feedback uses a latest-frame fallback. Missing source metadata
  disables the experiment for that decoder instance.
- Reset the source timeline after a long gap or timestamp discontinuity. Accept
  wrapping RTP counters, and calibrate resume bursts without a fixed frame count.

No protocol, Sunshine, encoder, audio, color, input, or hibernation-policy change.

## Validation

tests/sourcetimeline_test.cpp exercises the actual source-clock/selection policy.
It is not a GPU benchmark. CI runs it alongside the existing stream-policy tests.
Test stable 90 and 45 FPS with matching capture settings before variable rates.
Compare decoded queue age and actual-versus-requested present deadlines, not just
interval jitter. Intentional repeated scans at low FPS are not missed deadlines.

Pacing diagnostics now report target-late slots separately from presentation gaps.
Phase error modulo one refresh alone concealed full-refresh misses in the older
statistics. Source queue age is measured after decoding, not input-to-photon time.

Keep disabled unless live A/B results justify using it. One refresh of reserve
cannot hide missing source frames, long outages, or a compositor deadline miss.

## Deferred input issue

Settings Back sometimes returns the picture without working controls. Scripted
Escape checks passed on pacing.9, but the user reported the real interaction still
failed. Do not mark it fixed or restart that investigation as part of pacing work.
User requested keeping the current implementation and revisiting it later.
