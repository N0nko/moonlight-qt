# Adaptive audio candidate

Baseline: 6fcb499c831c385e69bbb23577e11da202edf394.

Enable **Adaptive audio (experimental)** in the stream settings. It is off by
default and takes effect on the next stream. Unsupported device formats or
quanta fall back to the legacy SDL renderer. Turn on Pacing diagnostics for a
compact audio summary every five seconds.

The new renderer uses the existing SDL backend with a preallocated SPSC ring.
Its device callback does not allocate, lock, poll, log or wait for packets.
The target starts at 15 ms or one device quantum plus one packet, whichever is
larger. Repeated starvation grows it up to 60 ms; quiet playback relaxes it.
It trims sustained excess depth with a short fade instead of allowing drift
to accumulate without bound. This is adaptive playback, not a lossless DSP
claim: trimming/fading trades a short audio correction for bounded latency.

Explicit sleep/wake events and network or callback gaps above 250 ms invalidate
old queued samples. The event path does not depend on Linux's monotonic clock
advancing during suspend. Each new connection resets decoder bookkeeping.
A stopped
device or a callback that has stopped running triggers the existing device
reinitialization path. Video and Deck microphone transport are unchanged.
All preferences used by the audio worker are snapshotted at session creation.

`tests/adaptivebuffer_test.cpp` covers invalid formats, channel layouts,
priming, steady playback, overflow, starvation growth/reset, positive clock
drift and concurrent wrap/reset. These are synthetic policy tests, not proof
of speaker latency, subjective quality or power savings. The full SDL backend
and actual Deck endpoint still need A/B measurement and resume/unplug tests.

No cross-host A/V synchronization or new capture driver is included here.
Video is never delayed to satisfy audio. The existing source/presentation
pacing implementation is unchanged.

Rollback: turn the setting off and reconnect, or run the baseline binary.
