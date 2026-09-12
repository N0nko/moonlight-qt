# Low Latency: newest decoded frame

Linux Low Latency (PM_CURRENT) selects the newest decoded frame after the
renderer is ready. Superseded decoded frames are freed before GPU submission,
not after displaying an older queued frame. This adds no reserve, thread,
polling timer or protocol change. Standard, Smooth, non-Linux and renderers
without internal pacing retain their previous selection policy.

Set MOONLIGHT_LATEST_FRAME=0 before launch for the old selection policy in the
same binary. An explicit frame reserve also disables newest-only selection.

## Measurement

Pacing diagnostics pair each Vulkan presentation ID with its decoded-ready
timestamp and CPU present-hook time. A bounded 256-entry history reports
ready-to-submit, ready-to-present and submit-to-present percentiles every five
seconds. The decode clock is bridged to MONOTONIC with a bracketed sample;
invalid, overwritten, duplicate and greater-than-five-second samples are ignored.
Swapchain replacement clears the history. No per-frame logging is added.

These are displayed-frame pipeline ages from compositor feedback, NOT physical
input-to-photon latency. Dropped decoded frames do not enter the displayed-age
distribution. Compare presentation gaps and drop counts alongside age, with the
same scene, host display/refresh, codec, bitrate and pacing mode in all runs.

## Optional submission-timing experiment

MOONLIGHT_ADAPTIVE_PRESENT_LEAD=1 opts Low Latency into bounded margin feedback.
It defaults off. Every 32 usable reports, processing-time p95 plus 0.25 ms
headroom adjusts the guard by at most 0.125 ms, within 0.5-2 ms. Zero/invalid
driver margins or long queue delays are not treated as useful work estimates.
Stale feedback or a changed refresh period uses the existing fixed guard.
MOONLIGHT_PRESENT_LEAD_US overrides the experiment. No extra frame is reserved.

Do not promote adaptive timing without a repeated live age/gap comparison.
Keep the prior AppDir and settings for rollback. No Sunshine, Wi-Fi, audio,
input, HDR or hibernation policy is changed by this patch.

## Deck comparison, 2026-09-12

Same-binary old/new/old/new runs at 1280x800, 90 FPS AV1 HDR, 200 Mbps,
45 seconds each after warmup, using the pendulum. New selection reduced the
worst five-second-window ready-to-present p99 from 47.97 to 37.85 ms across
the two repeats. Mean window medians were 33.87 versus 33.41 ms, but the
second new-policy run had a higher median than its baseline: a consistent
typical-latency reduction is NOT established. Presentation gaps were 0/8112
old versus 1/8112 new; the short test cannot establish a smoothness change.
These are window aggregates, not global percentiles or input-to-photon times.

Keep newest selection enabled and adaptive lead disabled. The exploratory
adaptive run retained a reported 1.389 ms lead and did not demonstrate benefit.
The remaining roughly 24-27 ms present-hook-to-presentation queue is not fixed
by newest decoded selection. Keep phase/queue measurements for future work;
do not add a frame reserve or claim zero jitter on the strength of this test.
