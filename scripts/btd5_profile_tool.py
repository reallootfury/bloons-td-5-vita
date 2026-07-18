#!/usr/bin/env python3
"""Validate DGDATA saves and create a schema-safe BTD5 4.7 test profile."""

import argparse
import copy
import json
from pathlib import Path

MAGIC = b"DGDATA"
CHECKSUM_POLYNOMIAL = 0xDB710641


def arithmetic_shift_32(value, bits):
    value &= 0xFFFFFFFF
    value = (value ^ 0x80000000) - 0x80000000
    return (value >> bits) & 0xFFFFFFFF


def dgdata_checksum(data):
    result = 0
    for byte in data:
        temporary = (result ^ byte) & 0xFF
        if temporary & 1:
            temporary = arithmetic_shift_32(
                temporary ^ CHECKSUM_POLYNOMIAL, 1)
        else:
            temporary = arithmetic_shift_32(temporary, 1)
        for index in range(7):
            if index:
                temporary = arithmetic_shift_32(temporary, 1)
            if temporary & 1:
                temporary ^= CHECKSUM_POLYNOMIAL
        result = arithmetic_shift_32(temporary, 1) ^ (
            arithmetic_shift_32(result, 8) & 0xFFFFFF)
    return result


def decode_save(path):
    raw = Path(path).read_bytes()
    if len(raw) < 15 or raw[:6] != MAGIC:
        raise ValueError(f"{path}: not a DGDATA save")
    try:
        stored_checksum = int(raw[6:14], 16)
    except ValueError as error:
        raise ValueError(f"{path}: invalid checksum header") from error
    decoded = bytes(
        (value - (index % 6 + 21)) & 0xFF
        for index, value in enumerate(raw[14:])
    )
    calculated_checksum = dgdata_checksum(decoded)
    if stored_checksum != calculated_checksum:
        raise ValueError(
            f"{path}: checksum mismatch: stored {stored_checksum:08x}, "
            f"calculated {calculated_checksum:08x}")
    return json.loads(decoded.decode("utf-8"))


def encode_save(profile):
    decoded = json.dumps(
        profile, ensure_ascii=True, separators=(",", ":")
    ).encode("utf-8")
    checksum = dgdata_checksum(decoded)
    encoded = bytes(
        (value + (index % 6 + 21)) & 0xFF
        for index, value in enumerate(decoded)
    )
    return MAGIC + f"{checksum:08x}".encode("ascii") + encoded


def make_v47_test_profile(template):
    profile = copy.deepcopy(template)
    if profile.get("Version", {}).get("LastGame") != [4, 7, 0]:
        raise ValueError("template is not a native BTD5 4.7 profile")

    levels = profile["Levels"]
    # A profile written by the Vita-running 4.7 executable has exactly 23
    # tower/power records.  Do not infer this count from newer desktop data:
    # appending records that 4.7 does not expect can leave its startup loader
    # waiting forever while nativeSurfaceCreated is assembling game state.
    if len(levels.get("Towers", [])) != 23:
        raise ValueError(
            "template is not a native BTD5 4.7 profile: expected 23 "
            f"tower/agent records, found {len(levels.get('Towers', []))}")
    levels["Rank"] = 100
    levels["RankXP"] = 0
    for tower in levels["Towers"]:
        tower["XP"] = 999999

    items = profile["Items"]
    items["MonkeyMoney"] = 999999
    items["Tokens"] = 999999
    items["MusicOn"] = True
    items["SFXOn"] = True
    items["UsedQuickPlay"] = True

    profile["UnlockedTowers"] = [False] + [True] * 23
    profile["UnlockedLevel4Upgrades"] = [False] + [True] * 23
    for tower_name, paths in profile["UnlockedTowerLevelUps"].items():
        unlocked = tower_name != "TestTower"
        paths["PathA"] = [unlocked] * len(paths["PathA"])
        paths["PathB"] = [unlocked] * len(paths["PathB"])
    for tower_name, seen in profile["UnlockedTowerLevelSeen"].items():
        value = 0 if tower_name == "TestTower" else 4
        profile["UnlockedTowerLevelSeen"][tower_name] = [value] * len(seen)

    profile["SandboxUnlocked"] = True
    profile["FastTrackUnlocked"] = True
    # Starting Sandbox at a later round complicates loading and stress-test
    # comparisons, so make Fast Track available but leave it disabled.
    profile["FastTrackActive"] = False
    profile["BypassRanks"] = False
    profile["UnlockAll"] = False
    return profile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("template", type=Path,
                        help="native BTD5 4.7 Profile.save")
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()

    template = decode_save(args.template)
    profile = make_v47_test_profile(template)
    encoded = encode_save(profile)
    args.output_directory.mkdir(parents=True, exist_ok=True)
    for name in ("Profile.save", "OldProfile.save"):
        (args.output_directory / name).write_bytes(encoded)
    (args.output_directory / "Profile.decoded.json").write_text(
        json.dumps(profile, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8")

    verified = decode_save(args.output_directory / "Profile.save")
    print(
        f"Created BTD5 {verified['Version']['LastGame']} test profile: "
        f"{len(verified['Levels']['Towers'])} tower/power records, "
        f"rank {verified['Levels']['Rank']}, "
        f"SandboxUnlocked={verified['SandboxUnlocked']}")


if __name__ == "__main__":
    main()
