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
