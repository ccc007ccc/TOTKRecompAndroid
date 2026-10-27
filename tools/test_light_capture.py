#!/usr/bin/env python3
"""Verify the selected results produced by test_light_capture.c on a real GPU.

Usage: python tools/test_light_capture.py <retrieved synthetic output/flicker>
"""
import gzip
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

root = Path(sys.argv[1])
reasons = set()
for path in sorted(root.glob("*/event-0001/event.json")):
    event = json.loads(path.read_text())
    assert event["format"] == "light-diagnostic-v1"
    reasons.add(event["reason"])
    values = [128, 0] if event["reason"] == "threshold" else [64]
    assert [f["frame"] for f in event["frames"]] == ([2, 3] if len(values) == 2 else [7])
    for frame, expected in zip(event["frames"], values):
        image = Image.open(path.parent / frame["file"]).convert("RGB")
        assert image.size == (960, 540)
        assert image.getextrema() == ((expected, expected),) * 3
        directory = path.parent / frame["capture"]
        meta = json.loads((directory / "diagnostic.json").read_text())
        data = gzip.decompress((directory / "stages.bin.gz").read_bytes())
        inputs = gzip.decompress((directory / "inputs.bin.gz").read_bytes())
        log = gzip.decompress((directory / "draws.txt.gz").read_bytes())
        assert len(data) <= 24 << 20 and len(inputs) <= 8 << 20 and len(log) <= 4 << 20
        assert not meta["replayable"]
        assert meta["inputs"][0]["original_bytes"] == 1 << 32
        assert meta["inputs"][0]["bytes"] == 4096
        # The fixture uses unquantised .5 / .25 values when supplying constants.
        expected_input = {128: 500, 0: 0, 64: 250}[expected]
        assert np.array_equal(np.frombuffer(inputs[:4096], dtype="<u4"), np.arange(1024) + expected_input)
        images = [i for i in meta["images"] if i["status"] == "ok"]
        integer = next(i for i in meta["images"] if i["address"] == "112233")
        assert integer["format"] == 98 and integer["status"] == "unsupported" and integer["bytes"] == 0
        assert images
        for item in images:
            pixels = data[item["offset"] : item["offset"] + item["bytes"]]
            assert len(pixels) == item["bytes"] and max(item["width"], item["height"]) <= 128
            if item["aspect"] == 1:
                rgba = np.frombuffer(pixels, dtype="<f2").reshape(-1, 4)
                assert np.all(abs(rgba[:, :3].astype(np.float32) - expected / 255) < .001)
            elif item["aspect"] == 2:
                assert np.all(np.frombuffer(pixels, dtype="<f4") == .75)
        if expected == 64:
            assert meta["images_skipped"] and meta["inputs_skipped"] and meta["log_lines_skipped"]
            assert b"critical-late-draw shader_state=0" in log
        else:
            assert len(images) == 2 and meta["images_skipped"] == 1
        total = sum(p.stat().st_size for p in directory.iterdir())
        assert total == frame["capture_bytes"]
        print(f"frame {frame['frame']}: {total} bytes; GPU/input/log raw {len(data)}/{len(inputs)}/{len(log)}")
    assert Image.open(path.parent / "preview.png").size == (320 * len(values), 188)
assert reasons == {"threshold", "manual"}
assert not list(root.rglob("*.partial"))
print("PASS: matching previews, HDR color/depth, bounded constants, graceful overflow, and late draw log")
