"""Stream the user's NSP from adb to project-local NCA files, without a second NSP copy.

Run in dev. This only reads the phone; it never changes the user's originals.
"""
import argparse
import os
import shlex
import struct
import subprocess
from pathlib import Path


def read_exact(stream, size):
    data = bytearray()
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            raise ValueError("truncated NSP")
        data.extend(chunk)
    return bytes(data)


def extract(stream, output):
    header = read_exact(stream, 16)
    magic, count, strings_size, _ = struct.unpack("<4sIII", header)
    if magic != b"PFS0" or not 0 < count < 10000 or strings_size > 1 << 20:
        raise ValueError("invalid NSP/PFS0 header")
    entries = read_exact(stream, count * 24)
    strings = read_exact(stream, strings_size)
    files = []
    for i in range(count):
        off, size, name_off, _ = struct.unpack_from("<QQII", entries, i * 24)
        end = strings.find(b"\0", name_off)
        if end < 0:
            raise ValueError("invalid PFS0 filename")
        name = strings[name_off:end].decode("utf-8")
        if not name or name in (".", "..") or "/" in name or "\\" in name:
            raise ValueError("unsafe PFS0 filename")
        files.append((off, size, name))
    output.mkdir(parents=True, exist_ok=True)
    position = 0
    for offset, size, name in sorted(files):
        if offset < position:
            raise ValueError("overlapping PFS0 entries")
        while position < offset:
            position += len(read_exact(stream, min(1 << 20, offset - position)))
        dest = output / name
        partial = output / (name + ".part")
        skip = dest.exists() and dest.stat().st_size == size
        print(f"{'Checking stream' if skip else 'Extracting'} {name}: {size} bytes", flush=True)
        out = None if skip else partial.open("wb")
        try:
            remaining = size
            while remaining:
                chunk = read_exact(stream, min(4 << 20, remaining))
                if out:
                    out.write(chunk)
                remaining -= len(chunk)
                position += len(chunk)
        finally:
            if out:
                out.close()
        if not skip:
            partial.replace(dest)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("package", help="Absolute NSP path on the phone")
    p.add_argument("output", type=Path)
    p.add_argument("--serial", required=True)
    p.add_argument("--adb", default="/home/deck/Code/Toolchains/android-sdk/platform-tools/adb")
    args = p.parse_args()
    # adb executes its final argument through a device shell.
    with subprocess.Popen([args.adb, "-s", args.serial, "exec-out", "cat " + shlex.quote(args.package)],
                          stdout=subprocess.PIPE) as proc:
        try:
            extract(proc.stdout, args.output)
        except BaseException:
            proc.terminate()
            raise
        proc.stdout.close()
        if proc.wait() != 0:
            raise RuntimeError("adb transfer failed")


if __name__ == "__main__":
    main()
