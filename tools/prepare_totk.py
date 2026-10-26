"""Apply the user's TotK update to its base NCA and prepare this runtime's inputs.

Input directories contain the NCA/ticket files extracted from each NSP. No keys
or game data are fetched. Run in dev; scratch and logs stay under --output.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path

from totk import inspect
from totk_romfs import extract_movies


def prepare(base, update, keys, hactool, output):
    base, update, keys, hactool, output = (Path(p).resolve() for p in (base, update, keys, hactool, output))
    output.mkdir(parents=True, exist_ok=True)
    # TotK has a single program NCA, larger than control/manual/metadata NCAs.
    base_nca = max(base.glob("*.nca"), key=lambda p: p.stat().st_size)
    update_nca = max(update.glob("*.nca"), key=lambda p: p.stat().st_size)
    with tempfile.TemporaryDirectory(prefix="prepare-", dir=output) as work:
        work = Path(work)
        config = work / "config/switch"
        config.mkdir(parents=True)
        titles = []
        for folder in (base, update):
            for ticket in folder.glob("*.tik"):
                t = ticket.read_bytes()
                if len(t) < 0x2B0 or t[:4] != bytes.fromhex("04000100") or t[0x281] != 0:
                    raise ValueError("expected an RSA-2048 common ticket")
                titles.append(t[0x2A0:0x2B0].hex() + " = " + t[0x180:0x190].hex())
        (config / "title.keys").write_text("\n".join(titles) + "\n")
        # hactool's parser rejects some unrelated newer key records. Retain only
        # the NCA keys it needs, without changing or printing the user's keyset.
        keep = []
        for line in keys.read_text().splitlines():
            m = re.fullmatch(r"\s*(\w+)\s*=\s*([0-9a-fA-F]+)\s*", line)
            if m and len(m[2]) in (32, 64) and (
                m[1] == "header_key" or m[1].startswith(("master_key_", "titlekek_", "key_area_key_application_"))
                or m[1] in ("aes_kek_generation_source", "aes_key_generation_source")
            ):
                keep.append(line)
        keyset = work / "nca.keys"
        keyset.write_text("\n".join(keep) + "\n")
        keyset.chmod(0o600)
        env = dict(os.environ, XDG_CONFIG_HOME=str(work / "config"), TMPDIR=str(work), TMP=str(work), TEMP=str(work))
        partial = output / "romfs.bin.part"
        cmd = [str(hactool), "--disablekeywarns", "--suppresskeys", "-k", str(keyset),
               "--basenca=" + str(base_nca), "--exefsdir=" + str(output / "exefs"),
               "--section1=" + str(partial), str(update_nca)]
        with (output / "prepare.log").open("w") as log:
            result = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode != 0:
            raise RuntimeError("hactool failed; see prepare.log")
        report = inspect(output / "exefs")
        # hactool may return 0 on a corrupt section: check the actual output.
        with partial.open("rb") as f:
            header = f.read(0x50)
        if len(header) != 0x50 or struct.unpack_from("<Q", header)[0] != 0x50:
            raise ValueError("merged section is not a RomFS image")
        size = partial.stat().st_size
        fields = struct.unpack("<10Q", header)
        if any(fields[i] + fields[i + 1] > size for i in (1, 3, 5, 7)) or fields[9] >= size:
            raise ValueError("merged RomFS is truncated")
        partial.replace(output / "romfs.bin")
        extract_movies(output / "romfs.bin", output / "romfs")
        report["romfs_bytes"] = size
        (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"Prepared TotK {report['version']}: four modules and {size} RomFS bytes")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ("base", "update", "keys", "hactool", "output"):
        p.add_argument("--" + arg, type=Path, required=True)
    a = p.parse_args()
    prepare(a.base, a.update, a.keys, a.hactool, a.output)
