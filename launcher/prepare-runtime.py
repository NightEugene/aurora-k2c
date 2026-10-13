#!/usr/bin/env python3
"""Prepare the version-specific runtime from a user-supplied compatible APK."""

import argparse
import hashlib
from pathlib import Path
import shutil
import tempfile
import zipfile

UNITY_SHA256 = "2de032fd5d52b0a6cd684c1b13b5dc8739b0035bc9e07baeb36e16173a923292"
PATCHED_SHA256 = "e24a11c0451f3be60e8d033a89382db7f4d915f9135324c777b56dab50ceaaa8"
LIBRARIES = ("libmain.so", "libunity.so", "libil2cpp.so", "lib_burst_generated.so", "libsteam_api.so", "libPDALIFE.so", "libpdaprotect.so")


def prepare(apk: Path, destination: Path) -> None:
    if destination.exists():
        raise ValueError(f"Destination already exists: {destination}; choose a new --output directory")
    with zipfile.ZipFile(apk) as archive:
        unity = bytearray(archive.read("lib/arm64-v8a/libunity.so"))
        if hashlib.sha256(unity).hexdigest() != UNITY_SHA256:
            raise ValueError("Unsupported libunity.so; this runtime requires the tested Unity 6000.0.61f1 APK")
        # Existing port's Android frame-timing adaptation, unrelated to DLC access.
        offset = 0x80BE28
        if unity[offset:offset + 4] != bytes.fromhex("687640f9"):
            raise ValueError("Unexpected Unity frame-timing instruction")
        unity[offset:offset + 4] = bytes.fromhex("08000014")
        if hashlib.sha256(unity).hexdigest() != PATCHED_SHA256:
            raise ValueError("Patched Unity checksum mismatch")
        managed = [name for name in archive.namelist()
                   if name.startswith("assets/bin/Data/Managed/Metadata/")
                   or name.startswith("assets/bin/Data/Managed/Resources/")]
        if "assets/bin/Data/Managed/Metadata/global-metadata.dat" not in managed:
            raise ValueError("APK has no IL2CPP metadata")
        destination.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".k2c-runtime-", dir=destination.parent))
        try:
            libs = staging / "lib/arm64-v8a"
            libs.mkdir(parents=True)
            for name in LIBRARIES:
                (libs / name).write_bytes(unity if name == "libunity.so" else archive.read("lib/arm64-v8a/" + name))
            for base in ("external", "files"):
                for name in managed:
                    if name.endswith("/"):
                        continue
                    relative = Path(name.removeprefix("assets/bin/Data/Managed/"))
                    if ".." in relative.parts or relative.is_absolute():
                        raise ValueError("Unsafe metadata path in APK")
                    target = staging / base / "il2cpp" / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.read(name))
            staging.rename(destination)
        except BaseException:
            shutil.rmtree(staging)
            raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "rpm-source/runtime")
    args = parser.parse_args()
    try:
        prepare(args.apk, args.output)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Runtime preparation failed: {error}\n")
    print(f"Runtime prepared: {args.output}")


if __name__ == "__main__":
    main()
