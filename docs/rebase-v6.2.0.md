# Steam Deck rebase onto Moonlight 6.2.0

Date: 2026-10-09. Branch: `deck/rebase-v6.2.0-20261009`.

## Provenance and rollback

- Application upstream: `v6.2.0` (`de2467e4`).
- Previous application: `7c1c562e`, retained on
  `deck/speaker-controls-20260912`; its installed AppDir is not replaced.
- Rebased application: `99f187e2` (52 custom commits).
- Common-c upstream: `f900dd4767759c7b9d0e93bcea666b55c69ea62f`.
- Rebased common-c: `f797aafac0236a14b7333f837e9887baed6074d5`.
- Previous common-c: `e83d1fb5`, retained on `deck/rebuild`.

Both repositories use a new branch, with no force-push or history replacement.
Common-c's encrypted extension channel and video recovery signals rebase with
unchanged patches. The submodule points to the rebased fork, not stock common-c.

## Integration decisions

- Keep the upstream keyboard/mouse function moves and extended-key state
  tracking. Input-generation gates call the updated keyboard-release function;
  do not restore its old implementation in `input.cpp`.
- Preserve this fork's existing 1280x800/90 defaults. Saved settings retain
  precedence; this rebase does not change the user's bitrate or pacing mode.
- Preserve upstream's delayed startup spinner to avoid decoder-probe reentrancy.
- Preserve resume recovery, input focus gates, live bitrate/display settings,
  Deck microphone, AV1 recovery, all three pacing modes and speaker controls.
- Take upstream SDL 3.4.18, sdl2-compat 2.32.74, FFmpeg 9.0.2, libplacebo and
  system-libva AppImage selection. Keep the existing libplacebo present hook.
- Keep the X11 AppImage path. No forced Wayland, Wi-Fi, power, Sunshine, launcher,
  hibernation or installed-device configuration changes are part of this rebase.

## Validation

Local WSL validation passed: 11 speaker-routing Python tests, speaker safety,
recovery policy/settings, adaptive audio buffer, low-latency selection, Deck
protocol and source-timeline tests. Common-c built in Release with OpenSSL.
The AppImage shell script parses, and the build workflows pass actionlint.

Git range-diff accounts for all 52 application commits and both common-c
commits. Pacing, audio, microphone and lifecycle implementations otherwise
match the previous fork. Patch-file context whitespace is pre-existing.

Full AppImage and SDL callback verification runs in
[GitHub Actions](https://github.com/N0nko/moonlight-qt/actions/runs/37847630031).
Do not infer live hardware success from build/test success.

## Device acceptance before promotion

The Deck was unreachable during preparation. Keep the previous AppDir and
settings until a candidate passes AV1/HDR and HEVC video, native touch,
trackpad/controller and Steam overlay, live bitrate, microphone/speaker audio,
and one sleep/hibernate resume cycle. Confirm VAAPI hardware decoding and the
selected libva with the actual launcher, especially if it bypasses AppRun.
Compare pacing/power only at matched settings and network conditions; no
latency or power improvement is claimed from this rebase alone.
