"""Read a RomFS index and export the loose files consumed by the FFmpeg HLE."""
import argparse
import struct
from pathlib import Path

EMPTY = 0xFFFFFFFF


def entries(image):
    image = Path(image)
    size = image.stat().st_size
    with image.open("rb") as f:
        h = struct.unpack("<10Q", f.read(0x50))
        if h[0] != 0x50 or any(h[i] + h[i + 1] > size for i in (1, 3, 5, 7)):
            raise ValueError("invalid RomFS header")
        if h[4] > 256 << 20 or h[8] > 256 << 20:
            raise ValueError("RomFS metadata exceeds supported size")
        f.seek(h[3]); dirs = f.read(h[4])
        f.seek(h[7]); files = f.read(h[8])
    seen_dirs, seen_files = set(), set()

    def name(data, off, fixed, count):
        if off + fixed + count > len(data):
            raise ValueError("truncated RomFS name")
        value = data[off + fixed:off + fixed + count].decode("utf-8")
        if value in (".", "..") or "/" in value or "\\" in value:
            raise ValueError("unsafe RomFS name")
        return value

    pending = [(0, "")]
    while pending:
        off, parent = pending.pop()
        if off in seen_dirs or off + 24 > len(dirs):
            raise ValueError("invalid/cyclic RomFS directory")
        seen_dirs.add(off)
        _, sibling, child, file, _, length = struct.unpack_from("<6I", dirs, off)
        part = name(dirs, off, 24, length)
        if not part and off != 0:
            raise ValueError("empty RomFS directory name")
        rel = parent + part + "/" if part else parent
        if sibling != EMPTY: pending.append((sibling, parent))
        if child != EMPTY: pending.append((child, rel))
        while file != EMPTY:
            if file in seen_files or file + 32 > len(files):
                raise ValueError("invalid/cyclic RomFS file")
            seen_files.add(file)
            _, sibling, offset, length, _, n = struct.unpack_from("<IIQQII", files, file)
            part = name(files, file, 32, n)
            if not part or offset + h[9] + length > size:
                raise ValueError("invalid/truncated RomFS file")
            yield rel + part, h[9] + offset, length
            file = sibling


def extract_movies(image, output):
    output = Path(output)
    count, total = 0, 0
    with Path(image).open("rb") as source:
        for rel, offset, size in entries(image):
            if not (rel.startswith("Movie/") or rel.endswith(".webm")):
                continue
            dest = output / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            partial = dest.with_name(dest.name + ".part")
            source.seek(offset)
            remaining = size
            with partial.open("wb") as f:
                while remaining:
                    data = source.read(min(remaining, 4 << 20))
                    if not data: raise ValueError("truncated movie")
                    f.write(data)
                    remaining -= len(data)
            partial.replace(dest)
            count += 1
            total += size
    print(f"Exported {count} loose movie files ({total} bytes)")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image", type=Path)
    p.add_argument("output", type=Path)
    a = p.parse_args()
    extract_movies(a.image, a.output)
