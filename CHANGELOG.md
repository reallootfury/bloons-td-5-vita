# Changelog

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
