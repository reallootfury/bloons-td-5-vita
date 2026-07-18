# Third-party software

This repository builds on existing open-source Vita and Android compatibility
work. Original license files and attribution must be retained when
redistributing the source.

## Vendored components

### FalsoJNI

- Upstream: <https://github.com/v-atamanenko/FalsoJNI>
- Vendored baseline: v1.4 (`083d5a07e025c6dfbb54ac68fd1acf696c23a86c`)
- License: MIT, preserved in `lib/falso_jni/LICENSE`
- Local changes: protect BTD5 music handles from generic JNI reference
  deletion and mirror FalsoJNI messages into the persistent loader log.

### SoLoader support code

The loader, `so_util`, filesystem bridge, kubridge interfaces, and related
support code originate from or were adapted through
[SoLoader Boilerplate](https://github.com/v-atamanenko/soloader-boilerplate)
and its credited upstream projects. Their copyright notices are preserved in
the repository.

## Downloaded build dependencies

`scripts/build_softfp_deps.py` fetches pinned revisions of OpenSL ES, zlib,
libsndfile, and minimp3 into ignored `.deps-src/` and `.deps-softfp/`
directories. These generated dependency trees and archives are not part of the
Git repository. Their upstream licenses apply.
