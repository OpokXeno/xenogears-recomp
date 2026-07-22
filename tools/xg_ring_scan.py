#!/usr/bin/env python3
"""Scan the glitch_capture screenshot ring for transient localized 'hole' frames.

Numeric corroboration for the missing-poly hunt: for each consecutive shot
triplet (A,B,C) compute dev(B) = |B - (A+C)/2| on a downscaled grayscale,
find the strongest concentrated 12x12-block blob, and report triplets whose
blob stands out from scene noise. Saves full-res crops of top candidates.

Usage: xg_ring_scan.py [ring_dir] [n_shots] [out_dir]
"""
import os
import sys

import numpy as np
from PIL import Image


def main() -> int:
    ring = sys.argv[1] if len(sys.argv) > 1 else "/tmp/xg_cap/ring"
    n_shots = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    out_dir = sys.argv[3] if len(sys.argv) > 3 else "/tmp/xg_cap/crops"
    os.makedirs(out_dir, exist_ok=True)

    files = sorted(
        (f for f in os.listdir(ring) if f.startswith("shot_f") and f.endswith(".png")),
        key=lambda f: int(f[6:-4]),
    )[-n_shots:]
    if len(files) < 3:
        print("not enough shots:", len(files))
        return 1

    frames = [int(f[6:-4]) for f in files]
    imgs = []
    for f in files:
        im = Image.open(os.path.join(ring, f)).convert("L")
        im = im.resize((240, 135), Image.BILINEAR)
        imgs.append(np.asarray(im, dtype=np.float32))

    bs = 12
    results = []
    for i in range(1, len(imgs) - 1):
        dev = np.abs(imgs[i] - (imgs[i - 1] + imgs[i + 1]) / 2.0)
        ref = np.abs(imgs[i - 1] - imgs[i + 1])
        bh = dev.shape[0] // bs
        bw = dev.shape[1] // bs
        blocks = dev[: bh * bs, : bw * bs].reshape(bh, bs, bw, bs).mean(axis=(1, 3))
        noise = float(np.median(blocks))
        by, bx = np.unravel_index(int(np.argmax(blocks)), blocks.shape)
        blob = float(blocks[by, bx])
        results.append(
            {
                "frame": frames[i],
                "blob": blob,
                "noise": noise,
                "ref_mean": float(ref.mean()),
                "bx": int(bx),
                "by": int(by),
            }
        )

    def score(r):
        return r["blob"] - max(12.0, 5.0 * r["noise"])

    results.sort(key=score, reverse=True)
    print("frame   blob  noise  refAC  block(240x135)")
    for r in results[:8]:
        print(
            f"{r['frame']:>7} {r['blob']:6.1f} {r['noise']:5.2f} {r['ref_mean']:5.2f}"
            f"  ({r['bx']},{r['by']}) score={score(r):.1f}"
        )

    scale_x = Image.open(os.path.join(ring, files[0])).width / 240.0
    scale_y = Image.open(os.path.join(ring, files[0])).height / 135.0
    for r in results[:3]:
        f = r["frame"]
        src = os.path.join(ring, f"shot_f{f:08d}.png")
        im = Image.open(src)
        cx = int((r["bx"] + 0.5) * bs * scale_x)
        cy = int((r["by"] + 0.5) * bs * scale_y)
        half = 160
        crop = im.crop((max(0, cx - half), max(0, cy - half), cx + half, cy + half))
        crop.save(os.path.join(out_dir, f"crop_f{f:08d}.png"))
    print("crops ->", out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
