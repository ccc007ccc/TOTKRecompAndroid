"""Validate the supported TotK ExeFS before generating or deploying game code."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

TITLE_ID = 0x0100F2C0115B6000
VERSION = "1.4.3"
# Canonical addresses are runtime/rt.h and game_profile.h's reserved regions.
MODULES = {
    "main": (0x7000000000, "277178b7dba1b6d4949a84778d4abc58b31b34f5"),
    "sdk": (0x7004000000, "574284b40c06cf1821a44857939083bee5716820"),
    "subsdk0": (0x7006000000, "f39f4c431234c72eb520cfdd1001bbcf02b3e885"),
    "rtld": (0x7007000000, "263c3426d28af8d8cce29a9d2da8ff0ff9a76456"),
}


def inspect(exefs):
    exefs = Path(exefs)
    records = []
    for index, (name, (base, expected)) in enumerate(MODULES.items()):
        data = (exefs / name).read_bytes()
        if len(data) < 0x100 or data[:4] != b"NSO0":
            raise ValueError(f"{name}: invalid NSO")
        build_id = data[0x40:0x60].hex()
        if build_id != expected.ljust(64, "0"):
            raise ValueError(f"{name}: unsupported or mixed build {build_id}; expected TotK {VERSION}")
        end = struct.unpack_from("<I", data, 0x34)[0] + struct.unpack_from("<I", data, 0x38)[0]
        end += struct.unpack_from("<I", data, 0x3C)[0]
        size = (end + 0x3FFF) & ~0x3FFF
        limit = list(MODULES.values())[index + 1][0] if index + 1 < len(MODULES) else 0x7080000000
        if not size or base + size > limit:
            raise ValueError(f"{name}: module overlaps the next region")
        records.append(dict(name=name, base=base, size=size, build_id=build_id,
                            sha256=hashlib.sha256(data).hexdigest()))
    npdm = (exefs / "main.npdm").read_bytes()
    if len(npdm) < 0x80 or npdm[:4] != b"META":
        raise ValueError("invalid main.npdm")
    aci = struct.unpack_from("<I", npdm, 0x70)[0]
    if npdm[aci:aci + 4] != b"ACI0" or struct.unpack_from("<Q", npdm, aci + 0x10)[0] != TITLE_ID:
        raise ValueError("main.npdm is not Tears of the Kingdom")
    return dict(title_id=f"{TITLE_ID:016X}", version=VERSION, modules=records)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exefs", type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.exefs), indent=2))
