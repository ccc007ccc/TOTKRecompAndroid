"""Stage one Gradle APK and its linkable runtime with explicit variant/profile metadata."""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path

PROFILES = ("dev", "performance", "snapdragon-8elite")


def apk_application_id(apk):
    """Use the SDK reader on the actual binary manifest, independently of adjacent build metadata."""
    sdk = Path(os.environ.get("ANDROID_HOME", "/home/deck/Code/Toolchains/android-sdk"))
    executable = "aapt.exe" if os.name == "nt" else "aapt"
    candidates = sorted((sdk / "build-tools").glob(f"*/{executable}"),
                        key=lambda path: tuple(map(int, re.findall(r"\d+", path.parent.name))))
    if not candidates:
        raise ValueError("Android SDK aapt is required; source android/dev_env.sh")
    result = subprocess.run([str(candidates[-1]), "dump", "badging", str(apk)], capture_output=True, text=True, check=True)
    package = re.search(r"^package: name='([^']+)'", result.stdout, re.M)
    if not package:
        raise ValueError("APK has no application package in its manifest")
    return package.group(1)


def runtime_identity(data):
    """Read exported identity strings from an arm64 ELF, including APK-stripped libraries."""
    def read(fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise ValueError("Truncated runtime ELF")
        return struct.unpack_from(fmt, data, offset)

    header = read("<16sHHIQQQIHHHHHH", 0)
    if header[0][:6] != b"\x7fELF\x02\x01" or header[1:3] != (3, 183):
        raise ValueError("Runtime must be a little-endian arm64 ELF shared library")
    phoff, shoff = header[5:7]
    phsize, phnum, shsize, shnum = header[9:13]
    if phsize != 56 or shsize != 64 or not phnum or not shnum:
        raise ValueError("Runtime ELF has no supported program/section tables")
    segments = [read("<IIQQQQQQ", phoff + i * phsize) for i in range(phnum)]
    sections = [read("<IIQQQQIIQQ", shoff + i * shsize) for i in range(shnum)]
    wanted = {"gamelib_runtime_id", "gamelib_runtime_profile"}
    values = {}
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        if section[9] != 24 or section[5] % 24 or section[6] >= shnum:
            raise ValueError("Invalid runtime dynamic symbol table")
        strings = sections[section[6]]
        start, size = strings[4:6]
        if start + size > len(data):
            raise ValueError("Truncated runtime symbol names")
        names = data[start:start + size]
        for offset in range(section[4], section[4] + section[5], 24):
            name_at, info, other, defined, address, size = read("<IBBHQQ", offset)
            end = names.find(b"\0", name_at)
            if end < 0:
                raise ValueError("Invalid runtime symbol name")
            name = names[name_at:end].decode("ascii")
            if name not in wanted:
                continue
            if info & 15 != 1 or not defined or other & 3 or not 1 < size <= 128:
                raise ValueError("Runtime identity is not an exported string")
            for segment in segments:
                if segment[0] == 1 and segment[3] <= address and address + size <= segment[3] + segment[5]:
                    at = segment[2] + address - segment[3]
                    if at + size > len(data):
                        raise ValueError("Truncated runtime identity")
                    value = data[at:at + size]
                    if value[-1:] != b"\0" or b"\0" in value[:-1] or name in values:
                        raise ValueError("Invalid runtime identity string")
                    values[name] = value[:-1].decode("ascii")
                    break
            else:
                raise ValueError("Runtime identity is outside file-backed ELF segments")
    if values.keys() != wanted:
        raise ValueError("Runtime lacks build identity exports; rebuild the APK")
    identity, profile = values["gamelib_runtime_id"], values["gamelib_runtime_profile"]
    if not re.fullmatch(r"[0-9a-f]{16}", identity) or profile not in PROFILES:
        raise ValueError("Invalid runtime build identity/profile")
    return identity, profile


def package(metadata, output, variant, profile, application_id=None):
    metadata, output = Path(metadata), Path(output)
    info = json.loads(metadata.read_text())
    expected = application_id or "com.totkrecomp.android" + (".debug" if variant == "debug" else "")
    if expected not in {"com.totkrecomp.android", "com.totkrecomp.android.debug", "com.totkrecomp.android.perftest"}:
        raise ValueError("Unexpected application package")
    if info["applicationId"] != expected or info.get("variantName") != variant or len(info["elements"]) != 1:
        raise ValueError("APK metadata does not match the requested variant")
    apk = metadata.parent / info["elements"][0]["outputFile"]
    if apk_application_id(apk) != expected:
        raise ValueError("Actual APK manifest does not match its package metadata")
    name = f"TOTKRecompAndroid-{variant}-{profile}-arm64-v8a.apk"
    with zipfile.ZipFile(apk) as archive:
        runtime = archive.read("lib/arm64-v8a/libmain.so")
    identity, actual_profile = runtime_identity(runtime)
    if actual_profile != profile:
        raise ValueError("Packaged runtime does not match the requested build profile")
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(apk, output / name)
    # Dynamic symbols survive APK stripping; this is the exact library packaged in this APK, never an mtime guess.
    (output / "libmain.so").write_bytes(runtime)
    with apk.open("rb") as stream:
        apk_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    record = dict(variant=variant, profile=actual_profile, gamelib_id=identity, application_id=expected, apk=name,
                  version_name=info["elements"][0]["versionName"],
                  apk_sha256=apk_hash,
                  runtime_sha256=hashlib.sha256(runtime).hexdigest())
    (output / "build.json").write_text(json.dumps(record, indent=2) + "\n")
    print(output / name)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("metadata", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--variant", choices=("debug", "release"), required=True)
    p.add_argument("--profile", choices=("dev", "performance", "snapdragon-8elite"), required=True)
    p.add_argument("--package", dest="application_id")
    args = p.parse_args()
    package(args.metadata, args.output, args.variant, args.profile, args.application_id)
