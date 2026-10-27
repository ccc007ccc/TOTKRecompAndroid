#!/usr/bin/env python3
"""Rank stage changes in one selected lightweight event; optionally make a small preview.

Run inside dev with the project's Python (numpy/Pillow). This reads only local,
selected event files. It neither contacts a phone nor changes a capture.
"""
import argparse
import gzip
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def load_frame(directory):
    meta = json.loads((directory / "diagnostic.json").read_text())
    if meta.get("format") != "light-diagnostic-v1":
        raise ValueError("not a lightweight diagnostic frame")
    with gzip.open(directory / "stages.bin.gz", "rb") as stream:
        raw = stream.read((24 << 20) + 1)
    if len(raw) > 24 << 20:
        raise ValueError("stage data exceeds the format budget")
    images, occurrences = {}, defaultdict(int)
    for item in meta["images"]:
        if item["status"] != "ok":
            continue
        aspect, fmt = item["aspect"], item["stored_format"]
        if aspect == 1 and fmt == 97:  # RGBA16F
            dtype, channels = "<f2", 4
        elif aspect == 2 and fmt in (126, 130):  # D32F / D32F+S8
            dtype, channels = "<f4", 1
        elif aspect == 2 and fmt == 124:  # D16
            dtype, channels = "<u2", 1
        elif aspect == 2 and fmt == 129:  # D24+S8, depth packed in 32 bits
            dtype, channels = "<u4", 1
        elif aspect == 4:
            dtype, channels = "u1", 1
        else:
            continue
        expected = item["width"] * item["height"] * channels * np.dtype(dtype).itemsize
        start, count = item["offset"], item["bytes"]
        if count != expected or start < 0 or start + count > len(raw):
            raise ValueError("invalid stage byte range")
        pixels = np.frombuffer(raw, dtype=dtype, count=count // np.dtype(dtype).itemsize,
                               offset=start).reshape(item["height"], item["width"], channels)
        if aspect == 2 and fmt == 129:
            pixels = (pixels & 0xffffff).astype(np.float32) / 0xffffff
        elif dtype == "<u2":
            pixels = pixels.astype(np.float32) / 65535
        else:
            pixels = pixels.astype(np.float32)
        identity = (item["address"], item["slot"], item["level"], item["layer"], fmt)
        ordinal = occurrences[identity]
        occurrences[identity] += 1
        images[identity + (ordinal,)] = (item, pixels)
    return meta, images


def preview(pixels, aspect, limits=None):
    pixels = np.nan_to_num(pixels, nan=0, posinf=1, neginf=0)
    if aspect == 1:
        rgb = pixels[:, :, :3]
    else:
        low, high = limits if limits is not None else (float(pixels.min()), float(pixels.max()))
        grey = (pixels[:, :, 0] - low) / max(high - low, 1e-6)
        rgb = np.repeat(grey[:, :, None], 3, axis=2)
    return Image.fromarray((np.clip(rgb, 0, 1) * 255).astype("uint8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("event", type=Path)
    parser.add_argument("--preview", type=Path, help="save a small contact sheet")
    parser.add_argument("--limit", type=int, default=8)
    args = parser.parse_args()
    event = json.loads((args.event / "event.json").read_text())
    if event.get("format") != "light-diagnostic-v1":
        parser.error("event is not a lightweight diagnostic bundle")
    frames = [load_frame(args.event / f["capture"]) for f in event["frames"]]
    for f, (meta, images) in zip(event["frames"], frames):
        print(f"frame {f['frame']}: {len(images)} stage images; "
              f"omitted images/inputs/log lines: {meta['images_skipped']}/"
              f"{meta['inputs_skipped']}/{meta['log_lines_skipped']}")
    rows = []
    if len(frames) == 2:
        before, after = frames[0][1], frames[1][1]
        print(f"unmatched stages: before {len(before.keys()-after.keys())}, after {len(after.keys()-before.keys())}")
        for key in before.keys() & after.keys():
            a, ap = before[key]
            b, bp = after[key]
            if ap.shape != bp.shape:
                continue
            finite = np.isfinite(ap) & np.isfinite(bp)
            delta = np.abs(np.nan_to_num(ap) - np.nan_to_num(bp))
            score = float(delta[finite].mean()) if finite.any() else float("inf")
            rows.append((score, a, ap, b, bp, int((~finite).sum())))
        rows.sort(key=lambda r: (-r[0], r[1]["group"], r[1]["slot"]))
    else:
        rows = [(0, item, px, item, px, int((~np.isfinite(px)).sum()))
                for item, px in frames[0][1].values()]
    rows = rows[:max(1, min(args.limit, 32))]
    for score, item, _, other, _, invalid in rows:
        print(f"group {item['group']} -> {other['group']} slot {item['slot']} "
              f"@{item['address']} aspect {item['aspect']}: mean difference {score:.6g}, nonfinite {invalid}")
    if args.preview and rows:
        columns = len(frames)
        sheet = Image.new("RGB", (320 * columns, 206 * len(rows)), "#16191d")
        draw = ImageDraw.Draw(sheet)
        for y, (_, item, ap, other, bp, _) in enumerate(rows):
            finite = np.concatenate((ap[np.isfinite(ap)], bp[np.isfinite(bp)]))
            limits = (float(finite.min()), float(finite.max())) if finite.size else (0, 1)
            for x, (i, pixels) in enumerate([(item, ap), (other, bp)][:columns]):
                thumb = preview(pixels, i["aspect"], limits).resize((320, 180), Image.Resampling.NEAREST)
                sheet.paste(thumb, (x * 320, y * 206 + 26))
                draw.text((x * 320 + 4, y * 206 + 5), f"group {i['group']} slot {i['slot']} @{i['address']}", fill="white")
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(args.preview)
        print(args.preview)


if __name__ == "__main__":
    main()
