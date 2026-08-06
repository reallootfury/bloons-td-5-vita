# Changelog

## v1.01

- Reworked front-touch input delivery to preserve rapid taps, drags, releases, and tower or special-placement gestures.
- Improved placement-drag handling so touch release events are delivered in order instead of closing the selected tower or special prematurely.
- Reduced non-logging build overhead by disabling persistent diagnostics, profiler hooks, watchdog polling, and per-frame timing work.
- Added conservative OpenGL ES vertex-attribute state caching to avoid redundant glVertexAttribPointer setup.
- Added fingerprint-gated frame-debt protection for unusually large native update deltas.
- Removed unnecessary glFinish and glFlush diagnostic overhead from the standard VPK.
- Kept detailed runtime diagnostics limited to the separate logging VPK.


## v1.00 — 2026-07-18

First public release.

### Added

- Dual boot for the pinned BTD5 3.37 and 4.7 ARMv7 Android releases.
- Android lifecycle, JNI, Bionic/POSIX, asset, EGL, audio, input, time, and
  filesystem compatibility layers for PlayStation Vita.
- Vita analog cursor, touchscreen controls, placement-drag safety, contextual
  back/pause handling, and clean save-and-exit.
- Music, sound effects, durable profile-save handling, bounded per-session
  logging, and host-side log analysis.
- Settings-only Low Graphics mode and persistent five-second cursor timeout.
- Native executable fingerprint validation and documented phone-compatible
  data installation.
- Custom LiveArea artwork and release packaging.
- Separate standard and troubleshooting VPKs with packaged logging modes.

### Known limitations

- Very late sandbox rounds can remain limited by the game's CPU simulation.
- Online and Android service integrations are not supported.
- Runtime testing is limited to the exact native executable fingerprints
  recognized by the loader.
