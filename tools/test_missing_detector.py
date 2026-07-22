#!/usr/bin/env python3
"""Test matrix for MissingPatchDetector.

Synthetic 6-case matrix using numpy/PIL only. Each case feeds a deterministic
sequence of small (80x60) grayscale frames into a fresh detector and asserts
on the per-frame flag decisions.

  a. 1-frame dark hole (localized dark deviation for 1 frame)         => MUST flag
  b. 1-frame bright hole (localized bright deviation for 1 frame)     => MUST flag
  c. 4-frame hole (localized deviation for 4 frames)                  => at least one
                                                                        frame in the
                                                                        event flags
  d. Periodic blinker (same region, fixed period, same polarity)     => periodicity
                                                                        suppression
                                                                        kicks in
                                                                        (total flags
                                                                        far below the
                                                                        would-be count)
  e. Full-screen fade (uniform global brightness ramp)                => MUST NOT flag
  f. Camera pan / large-scale content shift (global change)           => MUST NOT flag
  g. Necessity proof: the bright 1-frame hole from (b) must NOT be
     flagged by BlackPatchDetector but MUST be flagged by
     MissingPatchDetector.

Each MissingPatchDetector instance is given an injectable clock via
`score(pil_img, now=<t>)` so the trailing 5s periodicity window is
deterministic. The production code path falls back to time.time() when
`now` is not supplied.
"""

import os
import sys
import traceback

import numpy as np
from PIL import Image

# Make `glitch_capture` importable regardless of how the test is invoked.
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import glitch_capture as gc  # noqa: E402

# Internal downsample scale used by both detectors.
SMALL = (80, 60)


# ---------------------------------------------------------------------------
# Synthetic frame builders
# ---------------------------------------------------------------------------

def synth_frame(value, small=SMALL, hole=None):
    """Return a PIL Image (mode L) of size `small` filled with `value`. If
    `hole` is (x0, y0, x1, y1, hole_value) the rectangular region is
    overwritten."""
    a = np.full((small[1], small[0]), value, dtype=np.uint8)
    if hole is not None:
        x0, y0, x1, y1, hv = hole
        a[y0:y1, x0:x1] = hv
    return Image.fromarray(a, mode="L")


def synth_fade_frames(n=30, base=50, top=230, small=SMALL, shot_dt=0.08):
    """Smooth full-screen fade: each frame is uniform but value ramps
    linearly.  All pixels change together => frac must exceed frac_hi."""
    frames = []
    for i in range(n):
        v = int(round(base + (top - base) * i / max(1, n - 1)))
        frames.append(synth_frame(v, small=small))
    return frames


def synth_pan_frames(n=30, small=SMALL):
    """Simulate a large-scale content shift by alternating uniform-color
    "scenes" (a coarser, blockier cousin of a true camera pan, but the
    distinguishing signal is the same: the deviation is GLOBAL, not
    localized, so frac must exceed frac_hi)."""
    h, w = small[1], small[0]
    frames = []
    palette = [30, 220, 80, 200, 40, 180, 60, 240, 20, 160]
    for i in range(n):
        v = palette[i % len(palette)]
        frames.append(synth_frame(v, small=small))
    return frames


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def feed_missing(det, frames, shot_dt=0.08, start_t=0.0):
    """Feed a sequence of PIL frames to a MissingPatchDetector using the
    injectable clock.  Returns a list of dicts with per-frame results."""
    out = []
    for i, f in enumerate(frames):
        t = start_t + i * shot_dt
        flagged, frac, info = det.score(f, now=t)
        out.append({
            "i": i,
            "t": t,
            "flagged": bool(flagged),
            "frac": float(frac),
            "info": dict(info) if isinstance(info, dict) else {},
        })
    return out


def feed_black(det, frames):
    """Feed a sequence of PIL frames to a BlackPatchDetector (legacy
    2-tuple return)."""
    out = []
    for f in frames:
        flagged, frac = det.score(f)
        out.append({"flagged": bool(flagged), "frac": float(frac)})
    return out


def assert_flagged_in_window(out, lo, hi, case):
    flags = [r["i"] for r in out if r["flagged"]]
    assert any(lo <= f <= hi for f in flags), (
        "%s: expected at least one flag in window [%d,%d], got flags %s"
        % (case, lo, hi, flags)
    )


def assert_not_flagged(out, case):
    flags = [r["i"] for r in out if r["flagged"]]
    assert not flags, (
        "%s: expected NO flags, got %d flags at indices %s"
        % (case, len(flags), flags)
    )


# ---------------------------------------------------------------------------
# Case (a) — 1-frame DARK hole
# ---------------------------------------------------------------------------

def case_a_one_frame_dark_hole():
    det = gc.MissingPatchDetector(shot_interval_s=0.08, blink_window_s=5.0)
    # 20 background frames, then 1 dark-hole frame, then 10 background.
    frames = [synth_frame(128) for _ in range(20)]
    hole = (10, 10, 18, 18, 0)        # 8x8 = 64 px, ~1.33% of 4800
    frames.append(synth_frame(128, hole=hole))
    frames.extend(synth_frame(128) for _ in range(10))
    out = feed_missing(det, frames, shot_dt=0.08)
    assert_flagged_in_window(out, 20, 21, "case_a_one_frame_dark_hole")


# ---------------------------------------------------------------------------
# Case (b) — 1-frame BRIGHT hole
# ---------------------------------------------------------------------------

def case_b_one_frame_bright_hole():
    det = gc.MissingPatchDetector(shot_interval_s=0.08, blink_window_s=5.0)
    frames = [synth_frame(128) for _ in range(20)]
    hole = (10, 10, 18, 18, 240)      # bright deviation
    frames.append(synth_frame(128, hole=hole))
    frames.extend(synth_frame(128) for _ in range(10))
    out = feed_missing(det, frames, shot_dt=0.08)
    assert_flagged_in_window(out, 20, 21, "case_b_one_frame_bright_hole")


# ---------------------------------------------------------------------------
# Case (c) — 4-frame hole
# ---------------------------------------------------------------------------

def case_c_four_frame_hole():
    det = gc.MissingPatchDetector(shot_interval_s=0.08, blink_window_s=5.0)
    frames = [synth_frame(128) for _ in range(20)]
    hole = (10, 10, 18, 18, 0)
    for _ in range(4):
        frames.append(synth_frame(128, hole=hole))
    frames.extend(synth_frame(128) for _ in range(10))
    out = feed_missing(det, frames, shot_dt=0.08)
    # The 4-frame event lives at indices 20..23.  At least one of those
    # frames must be flagged (the early ones — the median still matches
    # the background so the deviation is large).
    assert_flagged_in_window(out, 20, 23, "case_c_four_frame_hole")


# ---------------------------------------------------------------------------
# Case (d) — periodic blinker
# ---------------------------------------------------------------------------

def case_d_blinker():
    det = gc.MissingPatchDetector(
        shot_interval_s=0.1, blink_window_s=2.0, blink_k=3,
    )
    n = 50
    period = 2
    frames = []
    for i in range(n):
        if i % period == 0:
            frames.append(synth_frame(128, hole=(10, 10, 18, 18, 0)))
        else:
            frames.append(synth_frame(128))
    out = feed_missing(det, frames, shot_dt=0.1)
    flags = [r["i"] for r in out if r["flagged"]]
    would_be = sum(1 for i in range(n) if i % period == 0)
    assert len(flags) < would_be, (
        "case_d_blinker: every hole flagged (no suppression at all): "
        "flags=%d would_be=%d indices=%s" % (len(flags), would_be, flags)
    )
    later_holes = [i for i in range(n // 2, n) if i % period == 0]
    later_flagged = set(i for i in flags if i >= n // 2)
    later_suppressed = [i for i in later_holes if i not in later_flagged]
    assert later_suppressed, (
        "case_d_blinker: no suppression visible in the latter half. "
        "later_holes=%s later_flagged=%s" % (later_holes, sorted(later_flagged))
    )


# ---------------------------------------------------------------------------
# Case (e) — full-screen fade
# ---------------------------------------------------------------------------

def case_e_full_screen_fade():
    det = gc.MissingPatchDetector(shot_interval_s=0.08, blink_window_s=5.0)
    frames = synth_fade_frames(n=30, base=50, top=230, shot_dt=0.08)
    out = feed_missing(det, frames, shot_dt=0.08)
    assert_not_flagged(out, "case_e_full_screen_fade")


# ---------------------------------------------------------------------------
# Case (f) — camera pan / large-scale content shift
# ---------------------------------------------------------------------------

def case_f_camera_pan():
    det = gc.MissingPatchDetector(shot_interval_s=0.08, blink_window_s=5.0)
    frames = synth_pan_frames(n=30)
    out = feed_missing(det, frames, shot_dt=0.08)
    # The deviation is global, so frac > frac_hi, so the detector must
    # not flag.  Allow a small tolerance for the very first 1-2 frames
    # before history fills, but no flag should ever survive.
    assert_not_flagged(out, "case_f_camera_pan")


# ---------------------------------------------------------------------------
# Case (g) — necessity proof: bright hole flags on Missing, NOT on Black
# ---------------------------------------------------------------------------

def case_g_necessity_bright_hole():
    frames = [synth_frame(128) for _ in range(20)]
    frames.append(synth_frame(128, hole=(10, 10, 18, 18, 240)))
    frames.extend(synth_frame(128) for _ in range(10))

    black = gc.BlackPatchDetector()
    out_b = feed_black(black, frames)
    flags_b = [i for i, r in enumerate(out_b) if r["flagged"]]
    assert not any(20 <= f <= 21 for f in flags_b), (
        "case_g_necessity: BlackPatchDetector should NOT flag a brightening "
        "hole (it's polarity-locked to DARK), but flagged at %s" % flags_b
    )

    miss = gc.MissingPatchDetector(shot_interval_s=0.08, blink_window_s=5.0)
    out_m = feed_missing(miss, frames, shot_dt=0.08)
    flags_m = [r["i"] for r in out_m if r["flagged"]]
    assert any(20 <= f <= 21 for f in flags_m), (
        "case_g_necessity: MissingPatchDetector MUST flag the brightening "
        "hole that BlackPatchDetector ignores, but flagged at %s" % flags_m
    )


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

CASES = [
    ("a_one_frame_dark_hole",     case_a_one_frame_dark_hole),
    ("b_one_frame_bright_hole",   case_b_one_frame_bright_hole),
    ("c_four_frame_hole",         case_c_four_frame_hole),
    ("d_blinker",                 case_d_blinker),
    ("e_full_screen_fade",        case_e_full_screen_fade),
    ("f_camera_pan",              case_f_camera_pan),
    ("g_necessity_bright_hole",   case_g_necessity_bright_hole),
]


def main():
    fails = 0
    for name, fn in CASES:
        try:
            fn()
            print("PASS  %s" % name)
        except Exception as e:                      # noqa: BLE001
            fails += 1
            print("FAIL  %s: %s" % (name, e))
            traceback.print_exc()
    if fails:
        print("%d/%d FAILED" % (fails, len(CASES)))
        sys.exit(1)
    print("ALL %d CASES GREEN" % len(CASES))
    sys.exit(0)


if __name__ == "__main__":
    main()
