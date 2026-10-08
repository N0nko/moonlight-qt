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

Full AppImage and SDL callback verification passed in
[GitHub Actions](https://github.com/N0nko/moonlight-qt/actions/runs/37847630031)
at `99f187e2f0dd257a9a8f30b99344b66e3ebcf5b2`. Both setup and AppImage jobs
succeeded; every dependency build, stream-policy test, binary build and upload
step succeeded. Windows/macOS and Steam Link jobs were intentionally skipped.
SDL tests passed legacy/adaptive buffering, stereo/surround, producer gaps and
repeated teardown. CI completed at 2026-10-08 21:42:11 UTC.
Do not infer live hardware success from build/test success.

## Candidate delivery

[Durable prerelease](https://github.com/N0nko/moonlight-qt/releases/tag/deck-v6.2.0-candidate-20261009)
is labelled candidate/not live tested, is not latest, and tags the exact CI
source commit above rather than subsequent documentation commits.
Original CI artifact ID: `11580926283` (one-day retention).

Downloaded ZIP paths were checked before extraction. The extracted AppImage
contains SDL3, sdl2-compat, FFmpeg, libplacebo, Qt xcb and the spatial-audio
controller, WAV assets and LV2 safety plugin. CI applies the retained libplacebo
present-hook patch before building. AppRun probes host libva and selects the
bundled fallback only when needed. WSL `ldd` found no missing Moonlight libraries.
The optional offscreen `--version` smoke test could not start because only the
xcb Qt platform plugin is packaged; no GUI/hardware runtime pass is claimed.

Release assets include the unmodified CI AppImage, an AppDir tar.gz rooted at
`squashfs-root` preserving executable modes and relative symlinks, and
`SHA256SUMS`. Extract only into a new candidate directory and launch via AppRun;
do not overwrite the old build or bypass libva selection. No installer is run.

SHA-256:

```text
2c22316a95830205a6026cda9a6fac6c6abcaeffc14d498cb1a56e6304ea02dc  Moonlight-99f187-x86_64.AppImage
bd18612d22128685580dc06d96d7cf522cb76c8be5b1b7005feee6983959edda  Moonlight-99f187-x86_64.AppDir.tar.gz
```

## Device acceptance before promotion

The Deck was unreachable during preparation. Keep the previous AppDir and
settings until a candidate passes AV1/HDR and HEVC video, native touch,
trackpad/controller and Steam overlay, live bitrate, microphone/speaker audio,
and one sleep/hibernate resume cycle. Confirm VAAPI hardware decoding and the
selected libva with the actual launcher, especially if it bypasses AppRun.
Compare pacing/power only at matched settings and network conditions; no
latency or power improvement is claimed from this rebase alone.
