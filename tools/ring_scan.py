#!/usr/bin/env python3
"""
ring_scan.py — sweep the dev-mode rings for the rare 1-frame polygon glitch.

Maintainer hint: "see if it can identify it in the ring buffer while running
in dev mode". Three ring sources are swept in one pass:

  1. FRAME RING (36000 frames, ~10 min): display x/y/disabled (GP1(05h)
     flip), i_stat. Detects: display disabled, flip outside dominant set,
     rare y-transition bigrams (repeat / skipped flip / wrong buffer).
  2. GP1 TRACE (512K writes): every GP1 write with PC/RA provenance, frame
     filtered to the window. Detects: spurious reset (0x00), display off
     (0x03), odd display-area (0x05) values, rare GP1 commands mid-gameplay.
  3. GTE RINGS: gte_frame_stats (512 frames, nproj/nsat/nflat per frame),
     gte_rtp_ring (256K RTPS/RTPT with inputs+outputs, frame-filtered),
     gte_latch (8192 latched degenerate/saturated projections WITH FULL
     INPUTS + caller_ra). Detects: nsat/nflat spike frames, and shows the
     exact degenerate projections (verts in, screen out, FLAG, caller).

NOTE: pause/step were removed upstream; the game keeps running. The GTE
rings are SHALLOW (frame_stats ~8.5s, rtp ring ~seconds of projections):
run this as soon as possible after seeing the glitch.

Usage:
    python3 tools/ring_scan.py --ago 5
    python3 tools/ring_scan.py --ago 10 --dump 123456   # full frame record
"""

import argparse
import json
import statistics
import sys
from collections import Counter

sys.path.insert(0, "/home/pc/xenogears-port/XenogearsRecomp/psxrecomp/tools")
from debug_client import query  # noqa: E402

HOST, PORT = "127.0.0.1", 4370


def one(cmd):
    r = query(HOST, PORT, cmd)
    if not r.get("ok"):
        print(f"[warn] {cmd.get('cmd')} -> {r.get('error', r)}")
    return r


def mad_baseline(vals):
    med = statistics.median(vals)
    mad = statistics.median([abs(v - med) for v in vals])
    return med, mad


# --------------------------------------------------------------------------
def scan_frame_ring(lo, newest):
    print(f"\n=== FRAME RING {lo}..{newest} ===")
    rec = {}
    for f in range(lo, newest + 1):
        r = one({"cmd": "get_frame", "frame": f})
        if r.get("ok"):
            rec[f] = r
    frames = sorted(rec)
    if not frames:
        print("  no records")
        return

    xy_count = Counter((rec[f]["display"]["x"], rec[f]["display"]["y"]) for f in frames)
    dominant = {xy for xy, _ in xy_count.most_common(4)}
    ist_major = Counter(int(rec[f]["i_stat"], 16) & 1 for f in frames).most_common(1)[0][0]

    yseq = [rec[f]["display"]["y"] for f in frames]
    trans = Counter()
    trans_at = {}
    for i in range(1, len(frames)):
        t = (yseq[i - 1], yseq[i])
        trans[t] += 1
        trans_at.setdefault(t, []).append(frames[i])

    print(f"  dominant (x,y)={sorted(dominant)} | ist&1 majority={ist_major}")
    rare_trans = []
    for (a, b), c in sorted(trans.items(), key=lambda kv: -kv[1]):
        if c == 1:
            rare_trans.append((a, b))
    print(f"  transitions: {dict(trans)}")
    if rare_trans:
        print(f"  RARE transitions: {rare_trans} at frames "
              f"{[trans_at[t][0] for t in rare_trans]}")

    hits = 0
    for f in frames:
        r = rec[f]
        d = r["display"]
        reasons = []
        if d["disabled"]:
            reasons.append("DISPLAY DISABLED")
        if (d["x"], d["y"]) not in dominant:
            reasons.append(f"flip ({d['x']},{d['y']}) outside dominant")
        if (int(r["i_stat"], 16) & 1) != ist_major:
            reasons.append("i_stat&1 off-pattern")
        if reasons:
            hits += 1
            print(f"  frame {f}: {'; '.join(reasons)} disp={d} last_func={r['last_func']}")
    if not hits and not rare_trans:
        print("  clean: no flip/display/i_stat anomalies")


# --------------------------------------------------------------------------
def scan_gp1(lo, newest):
    print(f"\n=== GP1 TRACE {lo}..{newest} ===")
    r = one({"cmd": "gp1_dump", "frame_lo": lo, "frame_hi": newest, "count": 4096})
    entries = r.get("entries", [])
    if not entries:
        print("  no GP1 writes in window")
        return
    by_cmd = Counter((int(e["val"], 16) >> 24) & 0x3F for e in entries)
    print(f"  {len(entries)} writes; by cmd: {dict(by_cmd)}")
    # routine gameplay traffic: 0x05 (display area) every flip, 0x04 (dma dir)
    RARE_OK = {0x05, 0x04}
    for e in entries:
        v = int(e["val"], 16)
        cmd = (v >> 24) & 0x3F
        if cmd not in RARE_OK:
            print(f"  RARE GP1 cmd 0x{cmd:02X} val={e['val']} frame={e['frame']} "
                  f"pc={e['pc']} ra={e['ra']} cpu_pc={e['cpu_pc']}")
    # odd display-area values (x,y) outside what the frame ring saw
    for e in entries:
        v = int(e["val"], 16)
        if ((v >> 24) & 0x3F) == 0x05:
            x = v & 0x3FF
            y = (v >> 10) & 0x1FF
            if (x, y) not in {(0, 0), (0, 216), (0, 256), (0, 16), (0, 240)}:
                print(f"  odd GP1(05h) area ({x},{y}) frame={e['frame']} pc={e['pc']} ra={e['ra']}")


# --------------------------------------------------------------------------
def scan_gte(lo, newest):
    print(f"\n=== GTE frame_stats (window {lo}..{newest}) ===")
    r = one({"cmd": "gte_frame_stats", "frames": 512})
    fs = [e for e in r.get("frames", []) if lo <= e["frame"] <= newest and e["nproj"] > 0]
    if len(fs) < 10:
        print("  insufficient frame_stats in window (rings are shallow — run sooner after the glitch)")
        fs = [e for e in r.get("frames", []) if e["nproj"] > 0]
        if not fs:
            return
        print(f"  (falling back to {len(fs)} frames outside window for baseline)")
    med_sat, mad_sat = mad_baseline([e["nsat"] for e in fs])
    med_flat, mad_flat = mad_baseline([e["nflat"] for e in fs])
    med_proj, _ = mad_baseline([e["nproj"] for e in fs])
    print(f"  baseline over {len(fs)} frames: nproj~{med_proj:.0f} "
          f"nsat~{med_sat:.0f}±{mad_sat:.0f} nflat~{med_flat:.0f}±{mad_flat:.0f}")

    spikes = []
    for e in fs:
        sat_hi = e["nsat"] > med_sat + max(3 * mad_sat, 2)
        flat_hi = e["nflat"] > med_flat + max(3 * mad_flat, 2)
        proj_lo = e["nproj"] < med_proj * 0.5
        if sat_hi or flat_hi or proj_lo:
            spikes.append((e, sat_hi, flat_hi, proj_lo))
    if not spikes:
        print("  no nsat/nflat/nproj outlier frames")
    for e, sat_hi, flat_hi, proj_lo in spikes:
        why = []
        if sat_hi: why.append(f"nsat={e['nsat']}")
        if flat_hi: why.append(f"nflat={e['nflat']}")
        if proj_lo: why.append(f"nproj={e['nproj']} (half of baseline!)")
        print(f"  SPIKE frame {e['frame']}: {'; '.join(why)}")

    # latch ring: degenerate projections with full inputs, frame-tagged
    print("\n=== GTE latch (degenerate projections in window) ===")
    r = one({"cmd": "gte_latch_dump", "count": 256})
    latched = [e for e in r.get("entries", []) if lo <= e["frame"] <= newest]
    if not latched:
        print("  none in window")
    else:
        by_frame = Counter(e["frame"] for e in latched)
        by_ra = Counter(e["ra"] for e in latched)
        print(f"  {len(latched)} latched in window; frames={dict(by_frame)}")
        print(f"  caller_ra histogram: {dict(by_ra)}")
        # latched entries on spike frames are the money shots
        spike_frames = {e[0]["frame"] for e in spikes}
        show = [e for e in latched if e["frame"] in spike_frames] or latched[:5]
        for e in show[:8]:
            print(f"  frame {e['frame']} ra={e['ra']} cmd={e['cmd']}")
            print(f"    V0={e['V0']} V1={e['V1']} V2={e['V2']}")
            print(f"    S0={e['S0']} S1={e['S1']} S2={e['S2']} SZ={e['SZ']} FLAG={e['FLAG']}")
            print(f"    TR={e['TR']} H={e['H']}")

    # per-spike-frame full RTP dump
    for e, *_ in spikes[:3]:
        f = e["frame"]
        print(f"\n=== GTE rtp ring, frame {f} ===")
        rr = one({"cmd": "gte_ring_dump", "count": 512, "newest": 0, "frame": f})
        ents = rr.get("entries", [])
        if not ents:
            print("  (ring rolled past — too late; run sooner)")
            continue
        degen = [x for x in ents
                 if any(c >= 1023 or c <= -1023 for c in x["S2"]) or x["SZ"][2] == 0]
        print(f"  {len(ents)} projections, {len(degen)} degenerate(S2 sat or SZ3=0)")
        for x in degen[:6]:
            print(f"  ra={x['ra']} V0={x['V0']} V1={x['V1']} V2={x['V2']}")
            print(f"    S0={x['S0']} S1={x['S1']} S2={x['S2']} SZ={x['SZ']} FLAG={x['FLAG']}")


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ago", type=float, default=8.0,
                    help="seconds between seeing the glitch and running this")
    ap.add_argument("--window", type=int, default=0,
                    help="explicit window size in frames (overrides --ago)")
    ap.add_argument("--dump", type=int, default=0,
                    help="print full get_frame JSON for this frame number")
    ap.add_argument("--skip-frame-ring", action="store_true",
                    help="skip the slow per-frame get_frame sweep (GTE/GP1 only)")
    args = ap.parse_args()

    window = args.window or int(args.ago * 60) + 120

    hist = one({"cmd": "history"})
    if not hist.get("ok"):
        print("[err] history failed; is build-dbg running with the debug server up?")
        return
    newest, oldest = int(hist["newest"]), int(hist["oldest"])
    lo = max(oldest, newest - window + 1)
    print(f"[ring] frames {lo}..{newest} (window {newest-lo+1}, ring oldest {oldest})")

    if not args.skip_frame_ring:
        scan_frame_ring(lo, newest)
    scan_gp1(lo, newest)
    scan_gte(lo, newest)

    if args.dump:
        print(f"\n=== get_frame {args.dump} ===")
        print(json.dumps(one({"cmd": "get_frame", "frame": args.dump}), indent=2))


if __name__ == "__main__":
    main()
