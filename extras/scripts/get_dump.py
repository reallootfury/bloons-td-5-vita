#!/usr/bin/env python3
"""Download the newest Vita core dump through VitaShell's FTP server."""

from __future__ import annotations

import argparse
from ftplib import FTP
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("host", help="PS Vita IP address")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("coredump"),
        help="destination file (default: coredump)",
    )
    args = parser.parse_args()

    with FTP() as ftp:
        ftp.connect(args.host, 1337)
        ftp.login()
        ftp.cwd("ux0:/data")
        dumps = sorted(
            Path(name).name
            for name in ftp.nlst()
            if Path(name).name.startswith("psp2core-")
        )
        if not dumps:
            parser.error("no psp2core dump found in ux0:data/")
        newest = dumps[-1]
        with args.output.open("wb") as output:
            ftp.retrbinary(f"RETR {newest}", output.write)

    print(f"Downloaded {newest} to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
