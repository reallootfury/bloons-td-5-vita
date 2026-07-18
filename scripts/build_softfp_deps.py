#!/usr/bin/env python3
"""Build the pinned soft-float dependencies required by BTD5 Vita."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys


OPENSL_ES_COMMIT = "e35b0630ac4c9091db63374d5c111d719887dde9"
ZLIB_COMMIT = "da607da739fa6047df13e66a2af6b8bec7c2a498"
LIBSNDFILE_COMMIT = "72f6af15e8f85157bd622ed45b979025828b7001"
MINIMP3_COMMIT = "7b590fdcfa5a79c033e76eacc05d0c3e4c79f536"


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command))
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=check,
        text=True,
    )


def fetch_pinned_repo(url: str, path: Path, commit: str) -> None:
    if not (path / ".git").is_dir():
        path.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "init", "-q", str(path)])
        run(["git", "-C", str(path), "remote", "add", "origin", url])
    run([
        "git", "-C", str(path), "fetch", "-q", "--depth", "1",
        "origin", commit,
    ])
    run([
        "git", "-C", str(path), "checkout", "-q", "--detach", "FETCH_HEAD",
    ])


def require_tools(names: tuple[str, ...]) -> None:
    missing = [name for name in names if shutil.which(name) is None]
    if missing:
        raise RuntimeError(f"Missing required tools: {', '.join(missing)}")


def main() -> int:
    vitasdk = os.environ.get("VITASDK")
    if not vitasdk:
        raise RuntimeError("VITASDK must point to VitaSDK-softfp.")

    require_tools((
        "git",
        "make",
        "arm-vita-eabi-gcc",
        "arm-vita-eabi-ar",
        "arm-vita-eabi-ranlib",
        "arm-vita-eabi-readelf",
    ))

    root = Path(__file__).resolve().parent.parent
    source = root / ".deps-src"
    output = root / ".deps-softfp"
    source.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)

    opensles = source / "opensles"
    zlib = source / "zlib"
    libsndfile = source / "libsndfile"
    minimp3 = source / "minimp3"

    fetch_pinned_repo(
        "https://github.com/frangarcj/opensles.git",
        opensles,
        OPENSL_ES_COMMIT,
    )
    fetch_pinned_repo(
        "https://github.com/madler/zlib.git",
        zlib,
        ZLIB_COMMIT,
    )
    fetch_pinned_repo(
        "https://github.com/libsndfile/libsndfile.git",
        libsndfile,
        LIBSNDFILE_COMMIT,
    )
    fetch_pinned_repo(
        "https://github.com/lieff/minimp3.git",
        minimp3,
        MINIMP3_COMMIT,
    )

    run(["git", "-C", str(opensles), "reset", "-q", "--hard",
         OPENSL_ES_COMMIT])
    run([
        "git", "-C", str(opensles), "apply",
        str(root / "scripts" / "opensles-vita.patch"),
    ])
    run([
        "git", "-C", str(opensles), "apply",
        str(root / "scripts" / "opensles-vita-buffer-cache.patch"),
    ])

    jobs = str(os.cpu_count() or 1)
    common_flags = "-O3 -mfloat-abi=softfp"
    opensles_flags = (
        f'{common_flags} -std=gnu17 -Wl,-q -Wall -I. -I../include '
        f'-I"{libsndfile / "include"}" -DUSE_OUTPUTMIXEXT -DUSE_SDL '
        "-DUSE_SNDFILE -DHAVE_PTHREAD"
    )
    opensles_make = opensles / "libopensles"
    run(["make", "clean"], cwd=opensles_make)
    run([
        "make", f"-j{jobs}", f"CFLAGS={opensles_flags}",
        f"CXXFLAGS={opensles_flags}",
    ], cwd=opensles_make)
    shutil.copyfile(
        opensles_make / "libOpenSLES.a",
        output / "libOpenSLES.a",
    )
    (output / "libOpenSLES.a").chmod(0o644)

    run(["make", "distclean"], cwd=zlib, check=False)
    configure = zlib / "configure"
    configure_text = configure.read_text()
    configure_text = configure_text.replace(
        'CFLAGS="${CFLAGS--O3} -fPIC"',
        'CFLAGS="${CFLAGS--O3}"',
    )
    configure.write_text(configure_text)

    zlib_env = os.environ.copy()
    zlib_env.update({
        "CHOST": "arm-vita-eabi",
        "CC": "arm-vita-eabi-gcc",
        "AR": "arm-vita-eabi-ar",
        "RANLIB": "arm-vita-eabi-ranlib",
        "CFLAGS": common_flags,
    })
    run(["./configure", "--static"], cwd=zlib, env=zlib_env)
    run(["make", f"-j{jobs}"], cwd=zlib)
    shutil.copyfile(zlib / "libz.a", output / "libz.a")
    (output / "libz.a").chmod(0o644)

    for archive in (output / "libOpenSLES.a", output / "libz.a"):
        attributes = subprocess.run(
            ["arm-vita-eabi-readelf", "-A", str(archive)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        if "Tag_ABI_VFP_args: VFP registers" in attributes:
            raise RuntimeError(f"Hard-float archive produced: {archive}")

    print(f"Soft-float archives ready in {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
