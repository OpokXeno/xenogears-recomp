#!/usr/bin/env python3
"""
glitch_watch.py — AUTONOMOUS 1-frame-prim-dropout detector + forensic catcher.

Watches the GP0 ring continuously while you play. For each build frame with
both same-parity neighbors in the ring, it runs the same A&C-B diff as
gp0_diff.py (MISSING / ATTR-pure ALTERED, known-benign families suppressed).
On a non-benign anomaly it IMMEDIATELY captures the forensic bundle for that
frame (wtrace windows, RAM snapshots, GP0 stream, GTE latch) into
tools/captures/glitch_<frame>.json and logs one line to stdout.

YOU DO NOT NEED TO RUN ANYTHING WHEN YOU SEE THE GLITCH — the watcher has
already caught it. Just keep playing; check tools/glitch_watch.log later.

Traps (re-armed on start, idempotent):
  wtrace: 0x1DB400-0x1DB600 (terrain strip) + 0x000D6600-0x000D6800 (sliver OT)
  snapshots: slot0/1 strip, slot2/3 sliver region
"""

import json
import os
import sys
import time
from collections import Counter

sys.path.insert(0, "/home/pc/xenogears-port/XenogearsRecomp/psxrecomp/tools")
from debug_client import query  # noqa: E402

HOST, PORT = "127.0.0.1", 4370
OUT_DIR = "/home/pc/xenogears-port/XenogearsRecomp/tools/captures"
POLL_S = 2.0

SNAP = {0: 0x1DB500, 1: 0x1DB580, 2: 0x000D6700, 3: 0x000D6780}
RANGES = [(0x1DB400, 0x1DB600), (0x000D6600, 0x000D6800)]

TEX_POLY = {"0x24", "0x2C", "0x2D", "0x2E", "0x25", "0x27",
            "0x34", "0x3C", "0x35", "0x3D", "0x36", "0x3E"}


def _h(w): return (int(w, 16) >> 16) & 0xFFFF
def _l(w): return int(w, 16) & 0xFFFF
def _s16(v): return v - 0x10000 if v >= 0x8000 else v


def sig(e):
    return (e["op"], e["src"])


def is_mmio(e):
    return int(e["src"], 16) >= 0x1F800000


def benign_missing(e):
    op, src, w = e["op"], int(e["src"], 16), e["w"]
    if op == "0x24" and (0x1DB400 <= src <= 0x1DB5FF or 0x1EB400 <= src <= 0x1EB5FF):
        return "bottom-edge terrain strip"
    if op == "0x2E" and (int(w[0], 16) & 0xFFFFFF) in (0x262626, 0x707070):
        return "glint/shadow sprite"
    if op == "0x01" and src == 0x1F801810:
        return "GP0(01h) cache-clear NOP"
    return None


# self-tuning repeat suppression: a src that alarms RECURRINGLY is a
# systematic blinker by definition (the glitch is rare) — capture once,
# then silence it.
RECUR_SUPPRESS_AFTER = 1
_alarm_count = Counter()
_suppressed = set()


def word_diffs(op, a, b):
    out = []
    if len(a) != len(b):
        return [-1]
    tex = op in TEX_POLY
    for i, (x, y) in enumerate(zip(a, b)):
        if x == y:
            continue
        if i == 0:
            out.append(i)
            continue
        if tex and i % 2 == 0:
            hx, hy = _h(x), _h(y)
            if hx != hy and (hx == 0 or hy == 0 or abs(hx - hy) > 3):
                out.append(i)
                continue
            ux, uy = int(x, 16) & 0xFF, (int(x, 16) >> 8) & 0xFF
            vx, vy = int(y, 16) & 0xFF, (int(y, 16) >> 8) & 0xFF
            if abs(ux - vx) > 32 or abs(uy - vy) > 32:
                out.append(i)
            continue
        if (abs(_s16(_h(x)) - _s16(_h(y))) > 2 or
                abs(_s16(_l(x)) - _s16(_l(y))) > 2):
            out.append(i)
    return out


def analyze(F, P, N):
    """MISSING in F vs both neighbors + ATTR-pure ALTERED. Returns
    (missing_list, altered_list) with benign MISSING already filtered out."""
    def msig(e):
        return (e["op"], e["src"], tuple(e["w"])) if is_mmio(e) else sig(e)

    sigF = Counter(msig(e) for e in F)
    sigP = Counter(msig(e) for e in P)
    sigN = Counter(msig(e) for e in N)
    missing = []
    for s, c in ((sigP & sigN) - sigF).items():
        ref = next(e for e in P if msig(e) == s)
        if not benign_missing(ref):
            missing.append((c, ref))

    idxP, idxN = {}, {}
    for e in P:
        if not is_mmio(e):
            idxP.setdefault(sig(e), e)
    for e in N:
        if not is_mmio(e):
            idxN.setdefault(sig(e), e)
    altered = []
    for e in F:
        if is_mmio(e):
            continue
        s = sig(e)
        if s not in idxP or s not in idxN:
            continue
        ep, en = idxP[s], idxN[s]
        dp = set(word_diffs(e["op"], e["w"], ep["w"]))
        dn = set(word_diffs(e["op"], e["w"], en["w"]))
        dpn = set(word_diffs(e["op"], ep["w"], en["w"]))
        common = sorted((dp & dn) - dpn)
        if not common:
            continue
        tex = e["op"] in TEX_POLY
        attr_hit, coord_hit = False, False
        for wi in common:
            if wi == 0:
                attr_hit = True
            elif tex and wi % 2 == 0 and _h(e["w"][wi]) != _h(ep["w"][wi]):
                attr_hit = True
        if attr_hit:
            for wi in dp:
                if wi == 0:
                    continue
                if tex and wi % 2 == 0:
                    if _h(e["w"][wi]) == _h(ep["w"][wi]):
                        coord_hit = True
                        break
                else:
                    coord_hit = True
                    break
        if attr_hit and not coord_hit:
            altered.append((common, e, ep))
    return missing, altered


# Rolling per-frame cache of latched degenerate GTE projections: the latch
# ring is SHALLOW (~2s of sat projections), so it must be harvested every
# poll, not at alarm time.
_latch_by_frame = {}


def harvest_latch():
    r = query(HOST, PORT, {"cmd": "gte_latch_dump", "count": 256})
    for e in r.get("entries", []):
        _latch_by_frame.setdefault(e["frame"], {})[e.get("seq", id(e))] = e
    # trim to recent
    if len(_latch_by_frame) > 400:
        for f in sorted(_latch_by_frame)[:-400]:
            del _latch_by_frame[f]


def capture(frame, missing, altered):
    bundle = {"frame": frame, "missing": [], "altered": [], "wtrace": {}, "forensics": {}}
    for c, ref in missing:
        bundle["missing"].append({"count": c, "op": ref["op"], "src": ref["src"],
                                  "ra": ref["ra"], "func": ref["func"], "w": ref["w"]})
    for common, e, ep in altered:
        bundle["altered"].append({"op": e["op"], "src": e["src"], "words": common,
                                  "F": e["w"], "nb": ep["w"], "ra": e["ra"]})
    for lo, hi in RANGES:
        r = query(HOST, PORT, {"cmd": "wtrace_dump",
                               "addr_lo": f"0x{lo:08X}", "addr_hi": f"0x{hi:08X}",
                               "frame_lo": frame - 2, "frame_hi": frame + 2,
                               "count": 2048})
        bundle["wtrace"][f"0x{lo:08X}"] = r.get("entries", [])
    for slot, addr in SNAP.items():
        r = query(HOST, PORT, {"cmd": "read_frame_ram", "frame": frame,
                               "addr": f"0x{addr:08X}", "len": 128})
        bundle["forensics"][f"snap{slot}_0x{addr:08X}"] = r.get("hex", r.get("error"))
    r = query(HOST, PORT, {"cmd": "gpu_frame_dump", "frame": frame, "count": 8192})
    bundle["gp0_frame"] = r.get("entries", [])
    # GTE evidence harvested in advance (latch is shallow)
    bundle["gte_latch_on_frame"] = [e for f in range(frame - 2, frame + 3)
                                    for e in _latch_by_frame.get(f, {}).values()]
    r = query(HOST, PORT, {"cmd": "gte_ring_dump", "count": 512, "newest": 0, "frame": frame})
    bundle["gte_rtp_frame"] = r.get("entries", [])
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, f"glitch_{frame}.json")
    with open(path, "w") as fh:
        json.dump(bundle, fh, indent=1)
    return path


def arm():
    query(HOST, PORT, {"cmd": "wtrace_disarm_all"})
    for lo, hi in RANGES:
        r = query(HOST, PORT, {"cmd": "wtrace_arm",
                               "lo": f"0x{lo:08X}", "hi": f"0x{hi:08X}"})
        print(f"[arm] wtrace 0x{lo:08X}-0x{hi:08X}: ok={r.get('ok')}", flush=True)
    for slot, addr in SNAP.items():
        query(HOST, PORT, {"cmd": "set_snapshot", "slot": slot, "addr": f"0x{addr:08X}"})
    print(f"[arm] snapshots: {SNAP}", flush=True)


def main():
    arm()
    frames = {}          # frame -> entries (build frames only)
    done = set()         # frames already analyzed (as interior)
    last = None
    print("[watch] running. Play normally — anomalies auto-captured.", flush=True)
    while True:
        try:
            h = query(HOST, PORT, {"cmd": "history"})
            newest = int(h["newest"])
            if last is not None and newest < last - 1000:
                # game rebooted: traps are gone, ring rolled — re-arm cleanly
                print("[watch] frame counter rolled back (reboot?) — re-arming", flush=True)
                arm()
                frames.clear()
                done.clear()
                _latch_by_frame.clear()
            start = newest - 4 if last is None else last + 1
            harvest_latch()
            for f in range(start, newest + 1):
                r = query(HOST, PORT, {"cmd": "gpu_frame_dump", "frame": f, "count": 8192})
                ents = r.get("entries", [])
                if ents:
                    frames[f] = ents
            last = newest
            # analyze frames that just became interior (need same-parity N at F+4)
            for F in sorted(frames):
                if F in done or (F - 4) not in frames or (F + 4) not in frames:
                    continue
                done.add(F)
                missing, altered = analyze(frames[F], frames[F - 4], frames[F + 4])
                # self-tuning repeat suppression (per op+src)
                missing = [m for m in missing
                           if (m[1]["op"], m[1]["src"]) not in _suppressed]
                altered = [a for a in altered
                           if (a[1]["op"], a[1]["src"]) not in _suppressed]
                if not (missing or altered):
                    continue
                path = capture(F, missing, altered)
                desc = [f"MISSING {c}x {ref['op']}@{ref['src']}" for c, ref in missing]
                desc += [f"ALTERED {e['op']}@{e['src']}w{common}" for common, e, ep in altered]
                print(f"[GLITCH?] frame {F}: {'; '.join(desc)} -> {path}", flush=True)
                for c, ref in missing:
                    k = (ref["op"], ref["src"])
                    _alarm_count[k] += 1
                    if _alarm_count[k] > RECUR_SUPPRESS_AFTER:
                        _suppressed.add(k)
                        print(f"[suppress] recurring {k} = systematic blinker, silenced", flush=True)
                for common, e, ep in altered:
                    k = (e["op"], e["src"])
                    _alarm_count[k] += 1
                    if _alarm_count[k] > RECUR_SUPPRESS_AFTER:
                        _suppressed.add(k)
                        print(f"[suppress] recurring {k} = systematic, silenced", flush=True)
            # trim
            for f in list(frames):
                if f < newest - 40:
                    del frames[f]
        except Exception as ex:
            print(f"[watch] poll error (game closed?): {ex}", flush=True)
            time.sleep(5)
        time.sleep(POLL_S)


if __name__ == "__main__":
    main()
