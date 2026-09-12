# Steam Deck speaker sound

Optional OLED speaker processing, selected in Moonlight Audio Settings. Off is
the default. This is generic transaural processing, not Dolby Atmos decoding,
personalized calibration, or guaranteed surround at every head position.

## Modes

- **Off:** original audio, no filter process.
- **Spacious stereo:** gentle frequency-dependent crosstalk reduction. The mono
  sum is preserved, apart from 4.5 dB headroom and a 1 ms FIR design delay.
- **Virtual surround:** separate 5.1/7.1 channel directions using generic MIT
  KEMAR HRTFs, followed by the speaker filter. 9 dB headroom, 2 ms FIR design
  delay. Select 5.1/7.1 in Moonlight and reconnect if currently streaming stereo.
  Stereo sources still work but cannot provide independent rear information.
  Rear-left/right include two quiet, filtered lateral reflections at 12/20 ms
  to help externalize the rear image. Front, centre and side channels remain dry.
  There is no feedback/reverb tail or added delay to the direct sound. These
  reflection parameters are a listening-tuned preview, not a measured room.

These delays describe the filters, NOT total audio/stream latency. PipeWire
scheduling, device buffers and transport still contribute. No global quantum,
sample rate, audio queue or microphone policy is changed.

Normal volume may be lower. Compare at similar perceived loudness, without
excessive volume. A stereo-linked, zero-lookahead sample-peak guard limits both
outputs to 0.95, preserving their relative levels. It is an emergency overload
guard, not a true-peak mastering limiter; heavy overload can pump/distort.

## Live controls

- **Width (50-150%):** scales the stereo difference after spatial processing,
  keeping the centre unchanged. 100% is the original width.
- **Distance (0-200%):** varies quiet, filtered room-reflection strength. It is
  a perceptual cue, not a physical distance in metres or a room calibration.
  Virtual surround applies this only to the rear-left/right paths. Spacious
  stereo adds side-only ambience, leaving centred/mono dialogue dry.
- **Reset:** restores Width 100%, Distance 0% for Spacious stereo; Width 100%,
  Distance 100% for Virtual surround. Both reproduce the previously shipped
  defaults (surround FIR reconstruction differs by at most one PCM32 LSB).

Profiles are saved separately for both modes, including across Off/reboots.
Sliders update native PipeWire control ports without rebuilding the graph,
reconnecting Moonlight, or adding direct-path delay. Changes are coalesced over
120 ms and smoothed in the native plugin over 20 ms to avoid abrupt steps.
Moving a slider does not regenerate FIRs. Strong settings can sound less natural;
Reset returns to the known baseline. There is no background control polling.

## Placement and limits

The model assumes approximately 22 cm speaker spacing and a centred listener
35-75 cm away. Its regularized inversion is limited to the useful mid band,
with bounded gain, rather than aggressive full-band cancellation. There is
no camera, head tracking, room measurement or calibration daemon. Ear shape,
head position, hands and reflections affect the result. Distance is not a
non-issue, but exact distance entry is not necessary for this conservative
preview. Large head movement can reduce or reverse the benefit.

Virtual surround is for discrete speaker channels. Disable the game's own
headphone/HRTF/3D-audio mode to avoid double processing. Use Off if the game's
existing mix sounds better. This does not invent height/object channels.

## Routing and recovery

A per-user PipeWire filter-chain uses WirePlumber smart filters, targeting only
the exact built-in OLED Speaker sink. Headphone and Bluetooth targets are not
matched. This affects all audio sent to those speakers while enabled, not just
Moonlight. Default sink/source choices remain unchanged. The output goes into
the normal ALSA Speaker sink, retaining Valve's installed DSP/protection.

The helper runs only on a settings query/change. No resident Python process,
timer, network listener or shell launcher hook is added. The native filter is
passive when idle. Changing mode briefly relinks audio; it does not reconnect
the video stream. Mode changes check actual playback links for existing active
speaker streams, not just published node names. Failure restores the previous
owned unit/config. If its links also fail, processing is disabled to let
WirePlumber restore direct audio instead of retaining a silent filter.
Removing/stopping the filter allows WirePlumber to restore direct audio.
This bounded mode-change check is not a resident watchdog for unrelated later
PipeWire, device, or suspend failures.

All persistent files are under the user's home:

```
~/.config/systemd/user/moonlight-deck-spatial.service
~/.local/state/moonlight/deck-spatial/{state.json,filter.conf,lock}
~/.local/share/moonlight/deck-spatial/<payload-hash>/
```

Payloads are independent of temporary AppImage mounts. Home files survive a
SteamOS update, but future PipeWire/WirePlumber compatibility still needs
checking. No rootfs, factory DSP or existing persistence-helper changes.

Emergency bypass, without restarting PipeWire or Moonlight:

```sh
python3 ~/Applications/Moonlight-SteamDeck-current.AppDir/usr/share/moonlight/deck-spatial/control.py mode 0
```

The dedicated unit can also be disabled directly with
`systemctl --user disable --now moonlight-deck-spatial.service`.

## Rebuild and test

Runtime assets are checked in; no NumPy or SOFA files are required on the Deck.
To regenerate with the same installed SOFA hash and libmysofa implementation:

```sh
python3 contrib/deck-spatial/export_hrtf.py > hrtf.json
python3 contrib/deck-spatial/design.py hrtf.json contrib/deck-spatial/assets
python3 tests/deckspatial_test.py
cc -std=c11 -O2 -Wall -Wextra -Werror tests/speakersafety_test.c -lm -o /tmp/speaker-test
/tmp/speaker-test
```

The offline designer needs NumPy. `assets/design.json` records source/asset
hashes, headroom and model-only results; those are not acoustic measurements of
this Deck/user. AppImage CI builds the tiny LV2 guard and bundles the helper,
filters and attribution. C/C++ audio decode, buffering and recovery are untouched.

## Sources

- [MIT KEMAR measurements and attribution](https://sound.media.mit.edu/resources/KEMAR.html)
- [Regularized loudspeaker crosstalk cancellation research](https://3d3a.princeton.edu/document/121)
- [WirePlumber smart filter routing](https://pipewire.pages.freedesktop.org/wireplumber/policies/smart_filters.html)
- [PipeWire convolution and LV2 graph support](https://docs.pipewire.org/page_module_filter_chain.html)
- [Research on environment and binaural externalization](https://mediatum.ub.tum.de/doc/980144/document.pdf)
- Vendored LV2 core header/license: https://github.com/lv2/lv2 (ISC). No third-party
  crosstalk-cancellation implementation was copied.
