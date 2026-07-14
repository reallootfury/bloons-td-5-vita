# Bloons TD 5 Vita Loader

An in-progress PlayStation Vita loader for the Android ARMv7 build of Bloons
TD 5. It uses the Android-to-Vita `.so` loader approach: the game executable
and assets are supplied by each user and are never included in this repository
or its VPK.

## Current scope

The loader has a pinned target build, a Vita lifecycle bridge for BTD5's
exported `MainActivity_native*` interface, Android asset-manager replacement,
VitaGL/EGL support, and touch/key forwarding. Android-only storefront,
advertising, cloud/social, and live-event services still need runtime stubs or

This is a beta version of btd5, bugs are expected. The release of the beta is solely meant to help me find bugs that i cannot do on my own free time.

It is expected that v4.7 of Bloons TD 5 will be working soon. I am using a older version to see if it would simply run.



## Preparing user-owned data


You are looking to grab the v3.37 of Bloons TD 5

Do not mix this executable with another APK's assets or versions of Bloons TD 5

Copy the resulting directory to `ux0:data/btd5/` on the Vita. It must contain:

```
ux0:data/btd5/
├── assets/
├── base.apk
└── libnative.so
```

## Build

Requires VitaSDK-softfp, VitaGL, and kubridge. The target Vita also needs
`kubridge.skprx` and `libshacccg.suprx`.

```sh
export VITASDK=/path/to/vitasdk-softfp
export PATH="$VITASDK/bin:$PATH"
./scripts/build-softfp-deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The resulting `build/btd5-vita.vpk` contains only the loader.

For the first console test, build with `-DCMAKE_BUILD_TYPE=Debug`. The loader
will write its startup and Android-log bridge output to
`ux0:data/btd5/loader.log`; keep that file if it returns to the LiveArea or
crashes.

## Credits

Built from [SoLoader Boilerplate](https://github.com/v-atamanenko/soloader-boilerplate).
The architecture is informed by [Hill Climb Racing Vita](https://github.com/memory-hunter/hill-climb-racing-vita).
