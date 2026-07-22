#!/usr/bin/env python3
"""Continuous GP0-ring sweeper for the transient polygon-corruption hunt.

Every sweep dumps the last SWEEP_FRAMES frames from the GP0 ring (step 4 =
same OT parity), scans for the three corruption signatures vs same-parity
neighbors (coord garbage / uv garbage / missing-extra prims), and on a hit
immediately dumps wtrace + snapshot forensics for the hit frame.

Usage: xg_sweep.py [port] [out_dir]
"""
import json
import os
import socket
import sys
import time

XY_IDX = {}
for op in ("0x24", "0x2C", "0x2D", "0x2E", "0x2F"):
    XY_IDX[op] = [1, 3, 5, 7]
for op in ("0x34", "0x35", "0x36", "0x37"):
    XY_IDX[op] = [1, 4, 7]
for op in ("0x3C", "0x3D", "0x3E", "0x3F"):
    XY_IDX[op] = [1, 4, 7, 10]

UV_IDX = {}
for op in ("0x24", "0x2C", "0x2D", "0x2E", "0x2F"):
    UV_IDX[op] = [2, 4, 6, 8]
for op in ("0x34", "0x35", "0x36", "0x37"):
    UV_IDX[op] = [2, 5, 8]
for op in ("0x3C", "0x3D", "0x3E", "0x3F"):
    UV_IDX[op] = [2, 5, 8, 11]

SWEEP_FRAMES = 96
SLEEP_S = 6.0
HIT_COOLDOWN_S = 20.0


def q(port, d, timeout=8.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall((json.dumps(d) + "\n").encode())
    f = s.makefile()
    r = json.loads(f.readline())
    s.close()
    return r


def s16(v):
    return v - 0x10000 if v >= 0x8000 else v


def parse_frame(entries):
    prims = {}
    for e in entries:
        op = e["op"]
        idxs = XY_IDX.get(op)
        if not idxs or len(e["w"]) <= max(idxs):
            continue
        vs = []
        for i in idxs:
            v = int(e["w"][i], 16)
            vs.append((s16(v & 0xFFFF), s16((v >> 16) & 0xFFFF)))
        xs = [p[0] for p in vs]
        ys = [p[1] for p in vs]
        uvs = []
        uidxs = UV_IDX.get(op, [])
        for i in uidxs:
            if i < len(e["w"]):
                uvs.append(int(e["w"][i], 16))
        prims.setdefault((op, e["src"]), []).append(
            {"bbox": (min(xs), max(xs), min(ys), max(ys)), "uv": uvs, "e": e}
        )
    return prims


def anomalies(F, bad, refP, refN):
    out = []
    for k, bl in bad.items():
        if k not in refP or k not in refN:
            continue
        for b in bl:
            for p in refP[k]:
                for n in refN[k]:
                    pb = p["bbox"]
                    nb = n["bbox"]
                    bb = b["bbox"]
                    # coherence: refs must agree with each other (else it's animation)
                    coh = max(abs(pb[0] - nb[0]), abs(pb[1] - nb[1]),
                              abs(pb[2] - nb[2]), abs(pb[3] - nb[3]))
                    if coh > 24:
                        continue
                    rp = [(pb[i] + nb[i]) / 2 for i in range(4)]
                    dx = max(abs(bb[0] - rp[0]), abs(bb[1] - rp[1]))
                    dy = max(abs(bb[2] - rp[2]), abs(bb[3] - rp[3]))
                    wR = rp[1] - rp[0]
                    hR = rp[3] - rp[2]
                    wB = bb[1] - bb[0]
                    hB = bb[3] - bb[2]
                    degen = (wB <= 2 or hB <= 2) and wR > 4 and hR > 4
                    if degen or dx > 48 or dy > 48:
                        out.append(("COORD", k, bb, rp))
                    # uv garbage: refs coherent AND bad uv high-half differs or jumps
                    if b["uv"] and p["uv"] and n["uv"]:
                        for j, (ub, up, un) in enumerate(zip(b["uv"], p["uv"], n["uv"])):
                            if up == un and ub != up:
                                hi_b = ub >> 16
                                hi_r = up >> 16
                                du = abs((ub & 0xFF) - (up & 0xFF)) + abs(
                                    ((ub >> 8) & 0xFF) - ((up >> 8) & 0xFF)
                                )
                                if hi_b != hi_r or du > 64:
                                    out.append(("UV", k, hex(ub), hex(up)))
    return out


def missing_extra(F, bad, refP, refN):
    miss = []
    for k in refP:
        if k in refN and k not in bad:
            miss.append(("MISSING", k))
    for k in bad:
        if k not in refP and k not in refN:
            miss.append(("EXTRA", k))
    return miss


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4370
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/xg_cap/sweep"
    os.makedirs(out, exist_ok=True)
    last_hi = -1
    last_hit = 0.0
    print("[sweep] starting, out=%s" % out, flush=True)
    while True:
        try:
            fr = q(port, {"cmd": "frame"}, timeout=4.0).get("frame", 0)
        except Exception as e:
            print("[sweep] no server: %s" % e, flush=True)
            time.sleep(5.0)
            continue
        lo = max(0, fr - SWEEP_FRAMES)
        frames = {}
        for F in range(lo, fr + 1, 4):
            if F <= last_hi:
                continue
            try:
                r = q(port, {"cmd": "gpu_frame_dump", "frame": F, "count": 65536})
                ent = r.get("entries", [])
                if ent:
                    frames[F] = parse_frame(ent)
            except Exception:
                pass
        last_hi = fr
        hits = []
        Fs = sorted(frames)
        for i in range(1, len(Fs) - 1):
            F = Fs[i]
            an = anomalies(F, frames[F], frames[Fs[i - 1]], frames[Fs[i + 1]])
            me = missing_extra(F, frames[F], frames[Fs[i - 1]], frames[Fs[i + 1]])
            me_big = [m for m in me if True] if len(me) >= 4 else []
            distinct_prims = {tuple(a[1]) for a in an}
            if len(distinct_prims) >= 3 or me_big:
                hits.append((F, an, me))
        if hits and time.time() - last_hit > HIT_COOLDOWN_S:
            last_hit = time.time()
            tag = time.strftime("%H%M%S")
            hdir = os.path.join(out, "hit_%s" % tag)
            os.makedirs(hdir, exist_ok=True)
            hit_frames = sorted({F for F, _, _ in hits})
            raw = {}
            for F in hit_frames:
                for F2 in (F - 4, F, F + 4):
                    try:
                        r = q(port, {"cmd": "gpu_frame_dump", "frame": F2,
                                     "count": 65536}, timeout=8.0)
                        raw[F2] = r.get("entries", [])
                        with open(os.path.join(hdir, "gp0_f%06d.jsonl" % F2), "w") as fh:
                            for e in raw[F2]:
                                fh.write(json.dumps(e) + "\n")
                    except Exception:
                        pass
            with open(os.path.join(hdir, "hits.json"), "w") as fh:
                json.dump(
                    [
                        {
                            "frame": F,
                            "anomalies": [
                                [t, list(k) if isinstance(k, tuple) else k, str(a), str(b)]
                                for t, k, a, b in an
                            ],
                            "missing": [[t, list(k)] for t, k in me],
                        }
                        for F, an, me in hits
                    ],
                    fh,
                    indent=1,
                )
            try:
                flo = min(F for F, _, _ in hits) - 2
                fhi = max(F for F, _, _ in hits) + 2
                r = q(port, {"cmd": "wtrace_dump", "count": 2048,
                             "frame_lo": flo, "frame_hi": fhi}, timeout=20.0)
                with open(os.path.join(hdir, "wtrace.json"), "w") as fh:
                    json.dump(r, fh)
            except Exception as e:
                print("[sweep] wtrace fail: %s" % e, flush=True)
            try:
                r = q(port, {"cmd": "thread_trace", "count": 2048,
                             "frame_lo": str(flo), "frame_hi": str(fhi)},
                      timeout=20.0)
                with open(os.path.join(hdir, "thread_trace.json"), "w") as fh:
                    json.dump(r, fh)
            except Exception as e:
                print("[sweep] thread_trace fail: %s" % e, flush=True)
            try:
                r = q(port, {"cmd": "fn_entry_tail", "count": 256}, timeout=20.0)
                with open(os.path.join(hdir, "fn_entry_tail.json"), "w") as fh:
                    json.dump(r, fh)
            except Exception as e:
                print("[sweep] fn_entry_tail fail: %s" % e, flush=True)
            for F in hit_frames:
                try:
                    r = q(port, {"cmd": "gte_ring_dump", "count": 512,
                                 "frame": F, "newest": 0}, timeout=20.0)
                    with open(os.path.join(hdir, "gte_f%d.json" % F), "w") as fh:
                        json.dump(r, fh)
                except Exception as e:
                    print("[sweep] gte_ring fail: %s" % e, flush=True)
            print(
                "[sweep] HIT x%d -> %s (frames %s)"
                % (len(hits), hdir, [F for F, _, _ in hits][:6]),
                flush=True,
            )
        time.sleep(SLEEP_S)


if __name__ == "__main__":
    main()
