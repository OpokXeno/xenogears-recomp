#!/usr/bin/env python3
"""
gp0_diff.py — find the 1-frame polygon glitch by diffing GP0 command streams
across same-parity build frames (A-B-A) from the always-on GP0 ring.

The GP0 ring (1M entries, ~30 s of gameplay) records every GP0 command with:
  frame, src (prim address in the ordering table), pc/func/ra (provenance),
  and the full command words (coords, clut, tpage, color).

Detector, per build frame F with same-parity neighbors F-2 / F+2:
  MISSING  : (op,src) present in BOTH neighbors but absent in F
             -> the game dropped the prim that frame (disappearing polygon)
  ALTERED  : (op,src) present in all three, but command words differ in F
             vs BOTH neighbors -> wrong tpage/clut/color (black/white poly)
             or garbage coords. Reports WHICH word index differs:
             POLY_FT3 (0x24): w[2]hi=clut, w[4]hi=tpage, w[1,3,5]=coords.
  Also reports per-opcode count dips (min(neighbors) - F) as a fallback
  when src addresses are unstable across frames.

Legit one-frame transients (particles) exist in only ONE frame, so they can
never be "in both neighbors" -> no false positives from them.

Run IMMEDIATELY after seeing the glitch (ring covers ~30 s):
    python3 tools/gp0_diff.py                 # scan last ~120 build frames
    python3 tools/gp0_diff.py --builds 200    # deeper scan (you were slow)
    python3 tools/gp0_diff.py --around 15036  # full prim dump of one frame
"""

import argparse
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, "/home/pc/xenogears-port/XenogearsRecomp/psxrecomp/tools")
from debug_client import query  # noqa: E402

HOST, PORT = "127.0.0.1", 4370


def dump_frame(f):
    r = query(HOST, PORT, {"cmd": "gpu_frame_dump", "frame": f, "count": 8192})
    if not r.get("ok"):
        return []
    return r.get("entries", [])


def sig(e):
    return (e["op"], e["src"])


def is_mmio(e):
    return int(e["src"], 16) >= 0x1F800000


def _h(w):
    return (int(w, 16) >> 16) & 0xFFFF


def _l(w):
    return int(w, 16) & 0xFFFF


def _s16(v):
    return v - 0x10000 if v >= 0x8000 else v


# textured-poly opcodes: words 2,4(,6) are uv<<0 | attr<<16 (clut/tpage),
# odd words are y<<16 | x vertex coords, w[0] is cmd|color
TEX_POLY = {"0x24", "0x2C", "0x2D", "0x2E", "0x25", "0x27",
            "0x34", "0x3C", "0x35", "0x3D", "0x36", "0x3E"}
COORD_TOL = 2   # sub-pixel camera wobble between same-buffer frames
ATTR_TOL = 3    # clut rows may legitimately shift ±1 (shading recompute);
                # a black/white poly is a WILD clut/tpage (garbage or 0x0000)


def _half_diff(x, y, tol):
    return (abs(_s16(_h(x)) - _s16(_h(y))) > tol or
            abs(_s16(_l(x)) - _s16(_l(y))) > tol)


def word_diffs(op, a, b):
    """Attribute-aware word compare. Returns list of differing word indices
    where the difference EXCEEDS per-class tolerance:
      - w[0] (cmd|color): exact
      - uv|attr words (textured polys): hi16 (clut/tpage) ATTR_TOL-or-zero,
        lo16 (u/v) COORD_TOL
      - coord words: hi16/lo16 as int16 with ±COORD_TOL tolerance
      - everything else: exact
    """
    out = []
    if len(a) != len(b):
        return [-1]
    tex = op in TEX_POLY
    for i, (x, y) in enumerate(zip(a, b)):
        if x == y:
            continue
        if i == 0:
            out.append(i)          # cmd/color must match exactly
            continue
        if tex and i % 2 == 0:     # uv|attr word (2,4,6)
            hx, hy = _h(x), _h(y)
            if hx != hy and (hx == 0 or hy == 0 or abs(hx - hy) > ATTR_TOL):
                out.append(i)      # wild clut/tpage change -> real
                continue
            # low half = u (bits0-7) | v (bits8-15): per-byte tolerance,
            # texcoords legitimately drift with terrain re-meshing
            ux, uy = int(x, 16) & 0xFF, (int(x, 16) >> 8) & 0xFF
            vx, vy = int(y, 16) & 0xFF, (int(y, 16) >> 8) & 0xFF
            if abs(ux - vx) > 32 or abs(uy - vy) > 32:
                out.append(i)
            continue
        # coord word (or generic): tolerate sub-pixel wobble per half
        if _half_diff(x, y, COORD_TOL):
            out.append(i)
    return out


SNAP_SLOTS = [0x1DB400, 0x1DB480, 0x1DB500, 0x1DB580]
TRACE_LO, TRACE_HI = 0x1DB400, 0x1DB600


def derive_snap_slots(lo, hi):
    """4 evenly-spaced snapshot slots across the [lo, hi) range.
    Kept in sync with the live SNAP_SLOTS layout for the default worldmap
    constants so the on-wire payload is byte-identical to the original."""
    n = 4
    span = hi - lo
    return [lo + span * i // n for i in range(n)]


def build_arm_payload(lo, hi):
    """Pure: derive snap slots + the wtrace_arm wire payload for a range.
    The wire protocol is unchanged (one command per TCP connection); only
    the address range becomes parameterizable."""
    snaps = derive_snap_slots(lo, hi)
    payload = {"cmd": "wtrace_arm",
               "lo": f"0x{lo:08X}", "hi": f"0x{hi:08X}"}
    return snaps, payload


def arm_traps(lo=None, hi=None):
    """Arm RAM snapshots (per-frame, in the 36000-frame ring) over the OT
    strip + a write-trace range, so a future glitch frame can be autopsied.
    Persists until the game exits; re-run after each reboot.

    lo/hi default to the worldmap constants (TRACE_LO/TRACE_HI); pass
    overrides to arm a different region (e.g. a field-map OT strip)."""
    if lo is None:
        lo = TRACE_LO
    if hi is None:
        hi = TRACE_HI
    snaps, payload = build_arm_payload(lo, hi)
    for i, a in enumerate(snaps):
        r = query(HOST, PORT, {"cmd": "set_snapshot", "slot": i, "addr": f"0x{a:08X}"})
        print(f"  snapshot[{i}] @ 0x{a:08X}: {'ok' if r.get('ok') else r}")
    r = query(HOST, PORT, payload)
    print(f"  wtrace 0x{lo:08X}-0x{hi:08X}: "
          f"{'ok slot=' + str(r.get('slot')) if r.get('ok') else r}")


def frame_forensics(f):
    """Autopsy of a glitch frame: OT packet bytes AT that frame (from the
    armed snapshots) + every guest write to the strip around the frame."""
    snaps, _payload = build_arm_payload(TRACE_LO, TRACE_HI)
    print(f"=== OT strip bytes as of frame {f} (from per-frame snapshots) ===")
    for a in snaps:
        r = query(HOST, PORT, {"cmd": "read_frame_ram", "frame": f,
                               "addr": f"0x{a:08X}", "len": 128})
        if not r.get("ok"):
            print(f"  0x{a:08X}: {r.get('error', 'n/a')} (snapshots armed? "
                  f"run: gp0_diff.py --arm)")
            continue
        data = r.get("data", "")
        try:
            raw = bytes.fromhex(data)
        except Exception:
            raw = b""
        # parse 0x20-strided packets: w0 = len<<24 | next_ptr
        for off in range(0, len(raw) - 3, 0x20):
            w0 = int.from_bytes(raw[off:off + 4], "little")
            plen, pnext = w0 >> 24, w0 & 0xFFFFFF
            body = raw[off + 4:off + 0x20].hex()
            print(f"  pkt @0x{a+off:08X}: len={plen} next=0x{pnext:06X} body={body}")
    print(f"\n=== guest writes to 0x{TRACE_LO:08X}-0x{TRACE_HI:08X}, "
          f"frames {f-2}..{f+2} ===")
    r = query(HOST, PORT, {"cmd": "wtrace_dump",
                           "addr_lo": f"0x{TRACE_LO:08X}", "addr_hi": f"0x{TRACE_HI:08X}",
                           "frame_lo": f - 2, "frame_hi": f + 2, "count": 2048})
    ents = r.get("entries", [])
    if not ents:
        print("  NO WRITES in that window (wtrace armed? gp0_diff.py --arm)")
    for e in ents:
        print(f"  f{e['frame']} {e['addr']}: {e['old']} -> {e['new']} "
              f"pc={e['pc']} ra={e['ra']} func={e['func']} w={e['w']}")


# ============================================================================
# Offline / parameterizable extensions (T1.2, T1.3)
#
# These functions exist so a captured burst dir (under /tmp/xg_cap/capture_*)
# can be post-mortem'd without the live debug server.  The MISSING / ALTERED
# / cadence logic is identical to the live path; the OFFLINE bits only
# re-host the data source (jsonl files vs. gpu_frame_dump) and add the
# parameterizable arm range (--strip-lo/--strip-hi).
# ============================================================================

ENV_OPS = ("0xE1", "0xE2", "0xE3", "0xE4", "0xE5", "0xE6")


def _load_jsonl_entries(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def _load_offline_frames(dir_path):
    """Return {frame_int: [entry, ...]} for every gp0_f*.jsonl in dir_path."""
    frames = {}
    p = Path(dir_path)
    for f in sorted(p.glob("gp0_f*.jsonl")):
        try:
            num = int(f.stem[len("gp0_f"):])
        except ValueError:
            continue
        frames[num] = _load_jsonl_entries(f)
    return frames


def _scan_one(frames, build, i):
    """MISSING+ALTERED for build[i] against build[i-2], build[i+2].
    Returns (F, missing, altered, opdips, remesh_noise) or None on cadence skip.
    Identical math to the live-ring path; factored so --from-dir reuses it."""
    F = build[i]
    P, N = build[i - 2], build[i + 2]
    if F - P != 4 or N - F != 4:
        return None

    def msig(e):
        return (e["op"], e["src"], tuple(e["w"])) if is_mmio(e) else sig(e)

    sigF = Counter(msig(e) for e in frames[F])
    sigP = Counter(msig(e) for e in frames[P])
    sigN = Counter(msig(e) for e in frames[N])

    outer_ok = i >= 4 and i + 4 < len(build) and \
        build[i - 4] == P - 4 and build[i + 4] == N + 4
    if outer_ok:
        sigP2 = Counter(msig(e) for e in frames[build[i - 4]])
        sigN2 = Counter(msig(e) for e in frames[build[i + 4]])
        both = sigP & sigN & sigP2 & sigN2
    else:
        both = sigP & sigN
    missing = []
    for s, c in (both - sigF).items():
        ref = next(e for e in frames[P] if msig(e) == s)
        missing.append((s, c, ref))

    altered = []
    remesh_noise = 0
    idxP, idxN = {}, {}
    for e in frames[P]:
        if not is_mmio(e):
            idxP.setdefault(sig(e), e)
    for e in frames[N]:
        if not is_mmio(e):
            idxN.setdefault(sig(e), e)
    for e in frames[F]:
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
        attr_words = []
        for wi in common:
            if wi == 0:
                attr_hit = True
                attr_words.append(wi)
            elif tex and wi % 2 == 0 and _h(e["w"][wi]) != _h(ep["w"][wi]):
                attr_hit = True
                attr_words.append(wi)
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
            altered.append((s, attr_words, e, ep))
        else:
            remesh_noise += 1

    opF = Counter(e["op"] for e in frames[F])
    opP = Counter(e["op"] for e in frames[P])
    opN = Counter(e["op"] for e in frames[N])
    opdips = {op: min(opP[op], opN[op]) - opF[op]
              for op in opP.keys() | opN.keys()
              if min(opP.get(op, 0), opN.get(op, 0)) - opF.get(op, 0) > 0}
    return F, missing, altered, opdips, remesh_noise


def offline_from_dir(dir_path):
    """Process every gp0_f*.jsonl under dir_path and classify anomalies
    (MISSING + ALTERED) per the same A-B-A parity rule the live ring uses.

    Returns a list of dicts:
        {frame, missing:[{op,src,count,neighbor}],
              altered:[{op,src,word_idx,frame_w,neighbor_w}],
              opdips, remesh_noise}"""
    frames = _load_offline_frames(dir_path)
    if len(frames) < 5:
        return []
    build = sorted(frames)
    out = []
    for i in range(2, len(build) - 2):
        result = _scan_one(frames, build, i)
        if result is None:
            continue
        F, missing, altered, opdips, remesh = result
        if missing or altered or opdips:
            out.append({
                "frame": F,
                "missing": [{"op": s[0], "src": s[1], "count": c,
                             "neighbor": ref}
                            for (s, c, ref) in missing],
                "altered": [{"op": s[0], "src": s[1],
                             "word_idx": widx,
                             "frame_w": e["w"],
                             "neighbor_w": ep["w"]}
                            for (s, widx, e, ep) in altered],
                "opdips": opdips,
                "remesh_noise": remesh,
            })
    return out


def env_diff_report(dir_path):
    """Compare the per-frame E1..E6 env command sequence between F and
    its parity refs (F-4, F+4).

    Returns a list of {frame, index, op, f_val, ref_val} for the FIRST
    divergent env word per anomaly frame, where divergence is defined
    as (P.env[i] == N.env[i]) AND (F.env[i] != P.env[i]). The
    'neighbors agree' precondition filters out scene-driven env churn
    that happens to change at F; the report is real, isolated F deltas."""
    frames = _load_offline_frames(dir_path)
    if len(frames) < 5:
        return []
    build = sorted(frames)

    def env_seq(entries):
        return [e for e in entries if e["op"] in ENV_OPS]

    out = []
    for i in range(2, len(build) - 2):
        F = build[i]
        P, N = build[i - 2], build[i + 2]
        if F - P != 4 or N - F != 4:
            continue
        seqF, seqP, seqN = env_seq(frames[F]), env_seq(frames[P]), env_seq(frames[N])
        n = min(len(seqF), len(seqP), len(seqN))
        for j in range(n):
            ef, ep, en = seqF[j], seqP[j], seqN[j]
            if ep["w"] == en["w"] and ef["w"] != ep["w"]:
                out.append({"frame": F, "index": j, "op": ef["op"],
                            "f_val": ef["w"][0], "ref_val": ep["w"][0]})
                break
    return out


def src_histogram(dir_path, bucket_size=0x100):
    """Bucket every numeric src address in dir_path's gp0_f*.jsonl into
    bucket_size-byte buckets. Returns [(bucket_addr, count), ...] sorted
    by count desc (ties: bucket addr ascending)."""
    frames = _load_offline_frames(dir_path)
    counts = Counter()
    for entries in frames.values():
        for e in entries:
            try:
                src = int(e["src"], 16)
            except (TypeError, ValueError):
                continue
            counts[(src // bucket_size) * bucket_size] += 1
    return sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))


def cadence_scan(base_dir, period=120, wake_window=5):
    """Walk base_dir for capture_*/detector.json, histogram flagged_frame
    modulo `period` (default 120 = 4 s @ 30 fps, the canonical Xenogears
    wake cycle), and print a one-line verdict.

      CLUSTERED at wake boundary : >= half of captures fall in
                                   [0..wake_window) mod period.
      UNIFORM                    : otherwise.

    Returns {'verdict': str, 'hist': Counter, 'frames': [int, ...]}."""
    frames = []
    p = Path(base_dir)
    for d in sorted(p.glob("capture_*")):
        det = d / "detector.json"
        if not det.is_file():
            continue
        try:
            data = json.loads(det.read_text())
        except (OSError, ValueError):
            continue
        f = int(data.get("flagged_frame", -1))
        if f >= 0:
            frames.append(f)
    hist = Counter(f % period for f in frames)
    near_wake = sum(hist.get(b, 0) for b in range(wake_window))
    total = len(frames)
    if total > 0 and near_wake * 2 >= total:
        verdict = "CLUSTERED at wake boundary"
    else:
        verdict = "UNIFORM"
    return {"verdict": verdict, "hist": hist, "frames": frames}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--builds", type=int, default=240,
                    help="number of build frames to scan back (default 240 = ~8s of gameplay; ring holds ~900)")
    ap.add_argument("--around", type=int, default=0,
                    help="just dump the full prim stream of this frame (plus neighbor counts)")
    ap.add_argument("--max-report", type=int, default=6,
                    help="max anomaly frames to detail")
    ap.add_argument("--arm", action="store_true",
                    help="arm OT-strip RAM snapshots + write-trace (run once per boot)")
    ap.add_argument("--frame-forensics", type=int, default=0, metavar="F",
                    help="autopsy frame F: snapshot bytes + write-trace window")
    ap.add_argument("--from-dir", metavar="DIR", default="",
                    help="offline: read gp0_f*.jsonl from DIR (no live server needed)")
    ap.add_argument("--env-diff", action="store_true",
                    help="(with --from-dir) report the first divergent E1..E6 env word per anomaly frame")
    ap.add_argument("--src-hist", action="store_true",
                    help="(with --from-dir) print a bucketed src-address histogram")
    ap.add_argument("--strip-lo", metavar="ADDR", default=0, type=lambda s: int(s, 0),
                    help="(with --arm) override TRACE_LO; snap slots are derived from this range")
    ap.add_argument("--strip-hi", metavar="ADDR", default=0, type=lambda s: int(s, 0),
                    help="(with --arm) override TRACE_HI; snap slots are derived from this range")
    ap.add_argument("--cadence", metavar="DIR", default="",
                    help="scan capture_*/detector.json under DIR; prints cadence verdict")
    args = ap.parse_args()

    if args.arm:
        lo = args.strip_lo if args.strip_lo else TRACE_LO
        hi = args.strip_hi if args.strip_hi else TRACE_HI
        arm_traps(lo=lo, hi=hi)
        return
    if args.frame_forensics:
        frame_forensics(args.frame_forensics)
        return
    if args.cadence:
        d = args.cadence
        result = cadence_scan(d, period=120)
        print(f"[cadence] {len(result['frames'])} captures under {d}")
        if result["frames"]:
            print(f"[cadence] frame%120 histogram: "
                  f"{dict(sorted(result['hist'].items()))}")
        print(f"[cadence] verdict: {result['verdict']}")
        return
    if args.from_dir:
        d = args.from_dir
        report = offline_from_dir(d)
        print(f"[offline] {len(report)} anomaly frames under {d}")
        for r in report:
            print(f"  frame {r['frame']}: "
                  f"missing={len(r['missing'])} altered={len(r['altered'])} "
                  f"opdips={r['opdips']} remesh-noise={r['remesh_noise']}")
            for m in r["missing"]:
                print(f"    MISSING op={m['op']} src={m['src']} count={m['count']}")
            for a in r["altered"]:
                print(f"    ALTERED op={a['op']} src={a['src']} "
                      f"word_idx={a['word_idx']}")
                print(f"      F : {a['frame_w']}")
                print(f"      nb: {a['neighbor_w']}")
        if args.env_diff:
            envs = env_diff_report(d)
            print(f"[env-diff] {len(envs)} anomaly frames")
            for e in envs:
                print(f"  frame {e['frame']} index={e['index']} op={e['op']} "
                      f"F={e['f_val']} ref={e['ref_val']}")
        if args.src_hist:
            hist = src_histogram(d, bucket_size=0x100)
            print(f"[src-hist] top buckets (size 0x100):")
            for addr, count in hist[:20]:
                print(f"  0x{addr:08X}: {count}")
        return

    st = query(HOST, PORT, {"cmd": "gpu_ring_stats"})
    if not st.get("ok"):
        print("[err] gpu_ring_stats failed; is build-dbg running?")
        return
    oldest, newest = int(st["oldest_frame"]), int(st["newest_frame"])
    print(f"[gp0] ring covers frames {oldest}..{newest} "
          f"({(newest-oldest)/60:.1f}s) total={st['total']}")

    if args.around:
        f = args.around
        ents = dump_frame(f)
        print(f"[gp0] frame {f}: {len(ents)} entries")
        for e in ents:
            print(f"  seq={e['seq']} op={e['op']} src={e['src']} ra={e['ra']} "
                  f"func={e['func']} w={e['w']}")
        return

    # --- collect build frames (walk back from newest) ----------------------
    frames = {}
    f = newest
    while len(frames) < args.builds and f >= oldest:
        ents = dump_frame(f)
        if ents:
            frames[f] = ents
        f -= 1
    build = sorted(frames)
    print(f"[gp0] collected {len(build)} build frames "
          f"({build[0]}..{build[-1]})")
    if len(build) < 5:
        print("[err] too few build frames — game must be actively rendering")
        return

    # --- scan triples -------------------------------------------------------
    # The OT alternates per BUILD frame (A,B,A,B over consecutive builds),
    # NOT per vblank: same-buffer neighbors of build[i] are build[i-2] and
    # build[i+2] (4 vblanks away). Comparing F against F±2 would diff the
    # two different OT buffers against each other (everything "missing").
    anomalies = []   # (frame, missing[(op,src,entry)], altered[(op,src,widx,entry,nentry)], opdips)
    cadence_skips = 0
    for i in range(2, len(build) - 2):
        F = build[i]
        P, N = build[i - 2], build[i + 2]
        if F - P != 4 or N - F != 4:
            cadence_skips += 1
            continue

        # MISSING detector: RAM prims matched by (op,src); MMIO direct writes
        # matched by (op,src,words) since their src is the GPU_DATA register.
        # Require presence in F-8/F-4/F+4/F+8 (four same-buffer frames) when
        # available: a periodically blinking sprite (water glint) fails that,
        # a true 1-frame dropout passes it.
        def msig(e):
            return (e["op"], e["src"], tuple(e["w"])) if is_mmio(e) else sig(e)

        sigF = Counter(msig(e) for e in frames[F])
        sigP = Counter(msig(e) for e in frames[P])
        sigN = Counter(msig(e) for e in frames[N])

        outer_ok = i >= 4 and i + 4 < len(build) and \
            build[i - 4] == P - 4 and build[i + 4] == N + 4
        if outer_ok:
            sigP2 = Counter(msig(e) for e in frames[build[i - 4]])
            sigN2 = Counter(msig(e) for e in frames[build[i + 4]])
            both = sigP & sigN & sigP2 & sigN2
        else:
            both = sigP & sigN
        missing = []
        for s, c in (both - sigF).items():      # absent (or fewer) in F
            ref = next(e for e in frames[P] if msig(e) == s)
            missing.append((s, c, ref))

        # ALTERED detector (RAM prims only) with 2-of-3 vote: F must differ
        # from BOTH neighbors at word i while the neighbors AGREE with each
        # other there. Then classify: legit terrain re-meshing changes only
        # vertex coords by a few px (suppressed as noise); the glitch is an
        # ATTR change (color word, clut/tpage half) or a HUGE coord jump.
        altered = []
        remesh_noise = 0
        idxP = {}
        idxN = {}
        for e in frames[P]:
            if not is_mmio(e):
                idxP.setdefault(sig(e), e)
        for e in frames[N]:
            if not is_mmio(e):
                idxN.setdefault(sig(e), e)
        for e in frames[F]:
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
            # Symptom-driven classification. The glitch is "prim unchanged
            # but its COLOR/CLUT/TPAGE went wrong for 1 frame" => require:
            #   attr_hit  : an ATTR word survived the vote (neighbors agree,
            #               F differs) — w[0] or clut/tpage hi-half;
            #   NOT coord : the prim did NOT move — no coord/uv word differs
            #               in the RAW F-vs-P diff (the vote strips neighbor
            #               drift from `common`, so it must NOT be used here).
            # Zone swaps / 15Hz tex animation move coords too -> noise.
            tex = e["op"] in TEX_POLY
            attr_hit, coord_hit = False, False
            attr_words = []
            for wi in common:
                if wi == 0:
                    attr_hit = True
                    attr_words.append(wi)
                elif tex and wi % 2 == 0 and _h(e["w"][wi]) != _h(ep["w"][wi]):
                    attr_hit = True
                    attr_words.append(wi)
            if attr_hit:
                for wi in dp:                       # raw F-vs-P differences
                    if wi == 0:
                        continue
                    if tex and wi % 2 == 0:
                        if _h(e["w"][wi]) == _h(ep["w"][wi]):
                            coord_hit = True        # uv moved
                            break
                        # attr hi moved: not coord evidence
                    else:
                        coord_hit = True            # vertex moved
                        break
            if attr_hit and not coord_hit:
                altered.append((s, attr_words, e, ep))
            else:
                remesh_noise += 1

        opF = Counter(e["op"] for e in frames[F])
        opP = Counter(e["op"] for e in frames[P])
        opN = Counter(e["op"] for e in frames[N])
        opdips = {op: min(opP[op], opN[op]) - opF[op]
                  for op in opP.keys() | opN.keys()
                  if min(opP.get(op, 0), opN.get(op, 0)) - opF.get(op, 0) > 0}

        if missing or altered or opdips or remesh_noise:
            anomalies.append((F, missing, altered, opdips, remesh_noise))

    if cadence_skips:
        print(f"[gp0] {cadence_skips} triples skipped (build cadence broke — scene change/menu)")

    if not anomalies:
        print("[scan] NO anomalies: every prim stream matches its same-parity "
              "neighbors. Glitch frame outside the scanned window (rerun with "
              "--builds 200+ fast) or the corruption is invisible to GP0 "
              "(would point at the raster/texture path instead).")
        return

    # The glitch is RARE by definition: a src flagged in MULTIPLE scanned
    # frames (periodic blinker sprite / straddling remesh) cannot be it.
    # Split candidates into UNIQUE (src seen once) vs repeat offenders.
    src_hits = Counter()
    for F, missing, altered, opdips, remesh in anomalies:
        for s, c, ref in missing:
            src_hits[("M", s[0], s[1])] += 1
        for s, widx, e, ref in altered:
            src_hits[("A", s[0], s[1])] += 1

    def uniq_count(a):
        return sum(1 for s, c, r in a[1] if src_hits[("M", s[0], s[1])] == 1) + \
               sum(1 for s, w, e, r in a[2] if src_hits[("A", s[0], s[1])] == 1)

    anomalies.sort(key=lambda a: (-uniq_count(a), len(a[1]) + len(a[2])))

    unique_frames = [a for a in anomalies if uniq_count(a) > 0]
    print(f"[scan] {len(unique_frames)} frames with UNIQUE candidates "
          f"(the rare glitch, if captured, is here); "
          f"{len(anomalies) - len(unique_frames)} frames only had repeat blinkers/noise")
    for F, missing, altered, opdips, remesh in unique_frames[:args.max_report]:
        print(f"\n  frame {F}: (remesh-noise-suppressed={remesh})")
        for s, c, ref in missing[:6]:
            tag = "UNIQUE" if src_hits[("M", s[0], s[1])] == 1 else f"repeat x{src_hits[('M', s[0], s[1])]}"
            print(f"    MISSING[{tag}] {c}x op={s[0]} src={s[1]} "
                  f"(neighbor copy: ra={ref['ra']} func={ref['func']} w={ref['w']})")
        for s, widx, e, ref in altered[:6]:
            tag = "UNIQUE" if src_hits[("A", s[0], s[1])] == 1 else f"repeat x{src_hits[('A', s[0], s[1])]}"
            print(f"    ALTERED[{tag}] op={s[0]} src={s[1]} ATTR-words@{widx}")
            print(f"      F : {e['w']}")
            print(f"      nb: {ref['w']}  (ra={ref['ra']} func={ref['func']})")

    repeats = {k: v for k, v in src_hits.items() if v > 1}
    if repeats:
        top = sorted(repeats.items(), key=lambda kv: -kv[1])[:20]
        print(f"\n  repeat blinkers/straddlers (legit, {len(repeats)} collapsed, "
              f"top 20): {dict(top)}")
    print("\n  [legend] KNOWN-BENIGN families (recur every scan):"
          "\n    - MISSING op=0x24, src 0x1DB4xx-0x1DB5xx/0x1EB4xx, y~173-230:"
          " worldmap bottom-edge terrain strip (near-plane edge culling)."
          "\n    - MISSING op=0x2E, color 0x262626, x<0 (screen edge): water/"
          "shadow glint sprite flicker."
          "\n    - MISSING op=0x01, src 0x1F801810: GP0(01h) cache-clear NOP,"
          " flow noise (no visual effect possible)."
          "\n  ANY candidate outside these families (or INSIDE if it matches"
          "\n  what you SAW) is the glitch: report frame + lines.")


if __name__ == "__main__":
    main()
