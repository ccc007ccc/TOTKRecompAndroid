"""Fail closed on changed compiler inputs, while renderer edits keep the cache."""
import json
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RELATIVE = Path("runtime/nvn/vulkan")
review = json.loads((ROOT / RELATIVE / "shader_cache_compat.json").read_text())

with tempfile.TemporaryDirectory(prefix="shader-contract-") as temporary:
    fixture = Path(temporary)
    for name in (*review["compatible_inputs"], str(RELATIVE / "shader_cache_contract.cmake"),
                 str(RELATIVE / "shader_cache_contract.h.in"), str(RELATIVE / "shader_cache_compat.json")):
        target = fixture / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, target)

    def generate(success=True):
        output = fixture / "generated.h"
        output.unlink(missing_ok=True)
        run = subprocess.run([
            "cmake", f"-DSHADER_CACHE_ROOT={fixture}", f"-DSHADER_CACHE_HEADER={output}",
            "-P", str(ROOT / "tools/generate_shader_cache_contract.cmake")],
            text=True, capture_output=True)
        assert (run.returncode == 0) == success, run.stdout + run.stderr
        if not success:
            assert not output.exists()
            return
        text = output.read_text()
        identity = re.search(r'#define SHADER_CACHE_ID "([0-9a-f]{64})"', text).group(1)
        legacy = re.findall(r'"([0-9a-f]{64})"', text.split("#define SHADER_COMPAT_CACHE_IDS", 1)[1])
        return identity, legacy

    actual = generate()
    matches_review = all(hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest
                         for name, digest in review["compatible_inputs"].items())
    assert actual[1] == (review["legacy_builds"] if matches_review else [])
    # Exercise the compatible branch using a private fixture even after a future
    # compiler update intentionally disables the checked-in legacy allowlist.
    review["compatible_inputs"] = {name: hashlib.sha256((fixture / name).read_bytes()).hexdigest()
                                  for name in review["compatible_inputs"]}
    (fixture / RELATIVE / "shader_cache_compat.json").write_text(json.dumps(review))
    baseline = generate()
    assert baseline[1] == review["legacy_builds"] and len(baseline[1]) == 2
    (fixture / RELATIVE / "exec.c").write_text("unrelated renderer change")
    assert generate() == baseline
    for name in review["compatible_inputs"]:
        target = fixture / name
        original = target.read_bytes()
        target.write_bytes(original + b"changed compiler input")
        changed = generate()
        assert changed[0] != baseline[0] and not changed[1], name
        target.write_bytes(original)
    assert generate() == baseline
    manifest = fixture / RELATIVE / "shader_cache_compat.json"
    invalid = dict(review, schema=2)
    manifest.write_text(json.dumps(invalid))
    generate(False)
    for old in ("../unreviewed", "a" * 63, "A" * 64):
        manifest.write_text(json.dumps(dict(review, legacy_builds=[old])))
        generate(False)
    missing = json.loads(json.dumps(review))
    del missing["compatible_inputs"][next(iter(missing["compatible_inputs"]))]
    manifest.write_text(json.dumps(missing))
    assert generate() == (baseline[0], [])

print("PASS: stable renderer-independent identity, all six compiler inputs invalidate compatibility, invalid schema/namespaces rejected, missing review fails closed")
