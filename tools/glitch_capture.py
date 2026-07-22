#!/usr/bin/env python3
"""glitch_capture.py — XenogearsRecomp black/untextured-poly continuous recorder.

Talks to the psxrecomp native debug server (JSON-over-newline, port 4370).

Modes:
  monitor  Continuous capture while the user plays:
             * screenshot every --shot-interval s (rolling ring, --ring-size kept)
             * AUTO-DETECT: each shot is compared against a temporal baseline
               (median of the last ~1 s). A localized near-black patch that was
               not dark before = candidate "polygons turned black" frame ->
               automatic burst capture around it (rate-limited by --cooldown).
             * manual burst still available: `touch <out>/TRIGGER` or Enter.
             * slow telemetry log (gte_frame_stats + sljit_async) every 2 s.
  burst    One-shot burst capture now (optionally --frame N).

Burst contents (<out>/capture_* or auto_*):
  shot_*.png                  presented display frames around the event
  buf_y0.png / buf_y256.png   BOTH VRAM double-buffer halves (strobe check)
  gp0_f<frame>.jsonl          full GP0 command stream per frame
  vram_full.hex               32x vram_peek tiles = whole 1024x512 VRAM
  state.json                  gpu/gte/overlay/sljit/dma/irq/present state
  detector.json               (auto bursts) detection score + flagged frame

Stdlib + optional PIL/numpy (auto-detect needs both; without them the monitor
still records the ring + telemetry and only manual bursts work).
"""

import argparse
import collections
import json
import os
import select
import shutil
import socket
import sys
import time

try:
    from PIL import Image
    import numpy as np
    HAVE_CV = True
except Exception:
    HAVE_CV = False

DEFAULT_OUT = "/tmp/xg_cap"
TRIGGER_NAME = "TRIGGER"


class DebugClient:
    """Client for the psxrecomp debug server. The server is STRICTLY
    one-command-per-connection: accept -> recv one line -> reply -> close
    (io_thread_main in debug_server.c). So every command opens a fresh
    connection. The reply is the first JSON line (the lock-free ping
    fast-path always reports id:0, so never match on ids)."""

    def __init__(self, host="127.0.0.1", port=4370, timeout=180.0):
        self.host = host
        self.port = port
        self.timeout = timeout

    def cmd(self, command, **params):
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        try:
            f = s.makefile("rb")
            req = {"cmd": command}
            req.update(params)
            s.sendall((json.dumps(req) + "\n").encode())
            line = f.readline()
            if not line:
                raise ConnectionError("debug server: empty reply to %s" % command)
            return json.loads(line.decode("utf-8", "replace"))
        finally:
            s.close()

    def close(self):
        pass  # nothing persistent


def wait_for_server(port, seconds=3600):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            cli = DebugClient(port=port, timeout=30.0)
            cli.cmd("ping")
            return cli
        except (ConnectionRefusedError, socket.timeout, OSError, ConnectionError):
            time.sleep(2.0)
    return None


# ---------------------------------------------------------------------------
# Anomaly detection: localized "newly black" patch vs temporal baseline
# ---------------------------------------------------------------------------

class BlackPatchDetector:
    """Flags frames where a region that used to be textured/bright turns
    near-black while the rest of the scene is stable. Baseline = per-pixel
    temporal median of the last `history` downsampled grayscale frames.
    Camera cuts / fades darken >60% of the screen and are ignored; already
    dark scenes (median dark) can't produce a 'newly black' signal."""

    def __init__(self, history=12, small=(80, 60), drop=45, dark=30,
                 frac_lo=0.015, frac_hi=0.60):
        self.hist = collections.deque(maxlen=history)
        self.small = small
        self.drop = drop
        self.dark = dark
        self.frac_lo = frac_lo
        self.frac_hi = frac_hi

    def score(self, pil_img):
        """Returns (flagged: bool, frac: float). Call once per frame, in order."""
        g = pil_img.convert("L").resize(self.small, Image.BILINEAR)
        a = np.asarray(g, dtype=np.float32)
        if len(self.hist) >= 6:
            med = np.median(np.stack(self.hist), axis=0)
            mask = (med - a > self.drop) & (a < self.dark) & (med > 50)
            frac = float(mask.mean())
        else:
            frac = 0.0
        self.hist.append(a)
        return (self.frac_lo < frac < self.frac_hi), frac


class MissingPatchDetector:
    """Polarity-agnostic missing-polygon detector.

    Temporal-median baseline (same as BlackPatchDetector) but the score
    is |cur - med| in BOTH polarities. A localized block of deviation
    (frac in [frac_lo, frac_hi) AND bbox density >= block_density) is
    flagged UNLESS the anti-blinker gate fires: the same bbox appeared
    more than `blink_k` times in the trailing `blink_window_s` of shots
    (real missing-poly events are rare; blinkers repeat).  The
    `reversion_window` is bookkeeping only — events that last 1-6 frames
    are all valid.

    `score(pil_img, now=None)` -> (flagged, frac, info_dict). When `now`
    is None the detector falls back to `time.time()`; tests pass an
    explicit timestamp to make the trailing-window math deterministic.
    """

    def __init__(self, history=12, small=(80, 60), drop=18,
                 frac_lo=0.0008, frac_hi=0.60,
                 block_density=0.4, min_block_px=3,
                 blink_k=3, blink_window_s=5.0, blink_iou=0.5,
                 shot_interval_s=0.08, reversion_window=6):
        self.hist = collections.deque(maxlen=history)
        self.small = small
        self.drop = drop
        self.frac_lo = frac_lo
        self.frac_hi = frac_hi
        self.block_density = block_density
        self.min_block_px = min_block_px
        self.blink_k = blink_k
        self.blink_window_s = blink_window_s
        self.blink_iou = blink_iou
        self.shot_interval_s = shot_interval_s
        self.reversion_window = reversion_window
        self.recent_flags = collections.deque()
        self._reversion_buf = collections.deque(maxlen=reversion_window)
        self.last_info = {}

    @staticmethod
    def _mask_bbox(mask):
        ys, xs = np.where(mask)
        if ys.size == 0:
            return None
        return (int(xs.min()), int(ys.min()),
                int(xs.max()) + 1, int(ys.max()) + 1)

    @staticmethod
    def _bbox_iou(a, b):
        if a is None or b is None:
            return 0.0
        ax0, ay0, ax1, ay1 = a
        bx0, by0, bx1, by1 = b
        ix0 = max(ax0, bx0)
        iy0 = max(ay0, by0)
        ix1 = min(ax1, bx1)
        iy1 = min(ay1, by1)
        if ix1 <= ix0 or iy1 <= iy0:
            return 0.0
        inter = (ix1 - ix0) * (iy1 - iy0)
        area_a = (ax1 - ax0) * (ay1 - ay0)
        area_b = (bx1 - bx0) * (by1 - by0)
        union = area_a + area_b - inter
        if union <= 0:
            return 0.0
        return inter / union

    @staticmethod
    def _polarity(cur, med, mask):
        if not mask.any():
            return "none"
        delta = float((cur.astype(np.float32) - med)[mask].mean())
        if delta > 1.0:
            return "brighter"
        if delta < -1.0:
            return "darker"
        return "mixed"

    def _drop_expired(self, now):
        cutoff = now - self.blink_window_s
        while self.recent_flags and self.recent_flags[0][0] < cutoff:
            self.recent_flags.popleft()

    def _count_same_region(self, bbox):
        if bbox is None:
            return 0
        n = 0
        for _, prev_mask in self.recent_flags:
            prev_bbox = self._mask_bbox(prev_mask)
            if self._bbox_iou(bbox, prev_bbox) >= self.blink_iou:
                n += 1
        return n

    def score(self, pil_img, now=None):
        g = pil_img.convert("L").resize(self.small, Image.BILINEAR)
        a = np.asarray(g, dtype=np.float32)

        if len(self.hist) >= 6:
            med = np.median(np.stack(self.hist), axis=0)
            dev = np.abs(a - med)
            mask = dev > self.drop
            count = int(mask.sum())
            frac = float(mask.mean())
            bbox = self._mask_bbox(mask)
            bbox_area = ((bbox[2] - bbox[0]) * (bbox[3] - bbox[1])) if bbox else 0
            density = (count / bbox_area) if bbox_area > 0 else 0.0
            polarity = self._polarity(a, med, mask)
            localized = (
                self.frac_lo < frac < self.frac_hi
                and density >= self.block_density
                and count >= self.min_block_px
            )
        else:
            frac = 0.0
            count = 0
            density = 0.0
            bbox = None
            polarity = "none"
            localized = False
            mask = None

        # Anti-blinker: trailing-window IoU count.  Clock injection
        # (now=...) keeps this deterministic for tests; the production
        # path falls back to wall-clock below.
        t = now if now is not None else time.time()
        self._drop_expired(t)
        same_region = self._count_same_region(bbox) if localized else 0
        periodic = same_region > self.blink_k

        # Reversion is bookkeeping only; real events last 1-6 frames.
        self._reversion_buf.append(bool(localized))
        consecutive_dev = 0
        for v in reversed(self._reversion_buf):
            if v:
                consecutive_dev += 1
            else:
                break

        flagged = bool(localized) and not periodic

        if flagged and mask is not None:
            self.recent_flags.append((t, mask.copy()))

        self.hist.append(a)

        self.last_info = {
            "frac": frac,
            "block_count": count,
            "block_density": density,
            "bbox": bbox,
            "polarity": polarity,
            "same_region_in_window": same_region,
            "periodic_suppressed": bool(periodic),
            "consecutive_dev_frames": consecutive_dev,
            "history_len": len(self.hist),
        }
        return flagged, frac, self.last_info


# ---------------------------------------------------------------------------
# Burst capture
# ---------------------------------------------------------------------------

def burst(cli, out_dir, center_frame, lookback, tag="", frames=None,
          post_shots=True, detector_info=None):
    ts = time.strftime("%Y%m%d_%H%M%S")
    cap = os.path.join(out_dir, "capture_%s%s" % (ts, ("_" + tag) if tag else ""))
    os.makedirs(cap, exist_ok=True)
    print("[burst] %s (center frame ~%d)" % (cap, center_frame))

    log = open(os.path.join(cap, "burst.log"), "w")

    def note(msg):
        print("[burst] " + msg)
        log.write(msg + "\n")
        log.flush()

    manifest = {"center_frame": center_frame, "time": ts,
                    "detector": detector_info or {}}

    # 1. Presented display + both VRAM halves
    for name, cmd, params in [
        ("shot_pre.png", "screenshot", {}),
        ("buf_y0.png", "dump_buffer", {"y": 0}),
        ("buf_y256.png", "dump_buffer", {"y": 256}),
    ]:
        p = os.path.join(cap, name)
        try:
            r = cli.cmd(cmd, path=p, **params)
            note("%s: %s" % (name, "ok" if r.get("ok") else "ERR %s" % r.get("error")))
        except Exception as e:
            note("%s: EXC %s" % (name, e))

    # 2. State snapshots (incl. overlay telemetry + GL coherency diagnostics)
    state = {}
    for cmd in ["gpu_state", "gte_state", "gte_frame_stats", "dma_state",
                "irq_state"]:
        try:
            state[cmd] = cli.cmd(cmd)
        except Exception as e:
            state[cmd] = {"error": str(e)}
    # overlay telemetry — the native ring tags each dispatch event with a
    # frame number, so we can correlate an overlay install/transition with
    # the glitch frame. (sljit_async/overlay_state are NOT registered at pin
    # 678c71f; these are the correct command names.)
    for cmd, params in [("overlay_native_ring", {}),
                        ("overlay_dump", {}),
                        ("overlay_loader_status", {}),
                        ("overlay_shadow_dump", {}),
                        ("present_ring", {"n": 120}),
                        ("gl_vram_diff", {}),
                        ("gl_coh_ring", {"n": 400}),
                        ("gl_present_ring", {"n": 600})]:
        try:
            state[cmd] = cli.cmd(cmd, **params)
        except Exception as e:
            state[cmd] = {"error": str(e)}
    with open(os.path.join(cap, "state.json"), "w") as f:
        json.dump(state, f, indent=1)
    note("state.json written")

    # 3. GP0 streams: explicit frame list, else lookback window
    if frames is None:
        frames = range(max(0, center_frame - lookback), center_frame + 1)
    got = 0
    for fr in frames:
        try:
            r = cli.cmd("gpu_frame_dump", frame=fr, count=65536)
            entries = r.get("entries", [])
            if entries:
                got += 1
                with open(os.path.join(cap, "gp0_f%06d.jsonl" % fr), "w") as f:
                    for e in entries:
                        f.write(json.dumps(e) + "\n")
        except Exception as e:
            note("gp0 frame %d: EXC %s" % (fr, e))
    note("gp0 dumps: %d frames with entries" % got)
    manifest["gp0_frames"] = got

    # 4. Full VRAM hex (32 tiles of 128x128)
    try:
        with open(os.path.join(cap, "vram_full.hex"), "w") as f:
            for ty in range(0, 512, 128):
                for tx in range(0, 1024, 128):
                    r = cli.cmd("vram_peek", x=tx, y=ty, w=128, h=128)
                    f.write("# tile x=%d y=%d\n%s\n" % (tx, ty, r.get("hex", "")))
        note("vram_full.hex written")
    except Exception as e:
        note("vram dump: EXC %s" % e)

    # 5. Post-event screenshots
    if post_shots:
        for i in (1, 2, 3):
            time.sleep(0.5)
            p = os.path.join(cap, "shot_post%d.png" % i)
            try:
                cli.cmd("screenshot", path=p)
            except Exception as e:
                note("post shot %d: EXC %s" % (i, e))

    with open(os.path.join(cap, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    if detector_info is not None:
        with open(os.path.join(cap, "detector.json"), "w") as f:
            json.dump(detector_info, f, indent=1)
    log.close()
    print("[burst] DONE -> %s" % cap)
    return cap


# ---------------------------------------------------------------------------
# Monitor (continuous)
# ---------------------------------------------------------------------------

def monitor(args):
    os.makedirs(args.out, exist_ok=True)
    ring = os.path.join(args.out, "ring")
    os.makedirs(ring, exist_ok=True)
    trigger_path = os.path.join(args.out, TRIGGER_NAME)
    if os.path.exists(trigger_path):
        os.remove(trigger_path)

    print("[monitor] waiting for debug server on port %d ..." % args.port)
    cli = wait_for_server(args.port)
    if cli is None:
        print("FATAL: no debug server after 1h")
        return 2
    print("[monitor] connected")

    r = cli.cmd("ping")
    if not r.get("ok"):
        print("FATAL: ping failed: %s" % r)
        return 2
    detector = None
    if args.auto and HAVE_CV:
        if args.detector == "black":
            detector = BlackPatchDetector()
        elif args.detector == "missing":
            detector = MissingPatchDetector()
    if args.auto and not HAVE_CV:
        print("[monitor] WARNING: PIL/numpy missing — auto-detect OFF "
              "(run with ~/xenogears-port/.venv/bin/python)")
    print("[monitor] trigger: touch %s | Enter here | auto=%s" %
          (trigger_path, "on" if detector else "off"))

    ring_log = open(os.path.join(args.out, "ring.log"), "a")
    shots_kept = collections.deque()
    last_shot = 0.0
    last_telem = 0.0
    last_auto = 0.0
    auto_count = 0

    try:
        while True:
            try:
                r = cli.cmd("frame")
            except (ConnectionError, ConnectionRefusedError, socket.timeout, OSError):
                # game not running / restarted — wait for it to come back
                time.sleep(1.0)
                continue
            fr = r.get("frame", 0)
            now = time.time()

            if now - last_shot >= args.shot_interval:
                last_shot = now
                p = os.path.join(ring, "shot_f%08d.png" % fr)
                try:
                    cli.cmd("screenshot", path=p)
                    shots_kept.append((fr, p))
                    flagged, frac = (False, 0.0)
                    if detector:
                        try:
                            flagged, frac = detector.score(Image.open(p))
                        except Exception:
                            pass
                    ring_log.write("%d shot %s frac=%.4f %s\n" %
                                   (fr, p, frac, "FLAG" if flagged else ""))
                    ring_log.flush()
                    if flagged and detector and \
                            now - last_auto >= args.cooldown and \
                            auto_count < args.max_auto:
                        last_auto = now
                        auto_count += 1
                        print("[monitor] AUTO flag frame %d frac=%.3f -> burst" %
                              (fr, frac))
                        info = {"flagged_frame": fr, "frac": frac,
                                "shot": p,
                                "detector": args.detector}
                        extra = getattr(detector, "last_info", None)
                        if isinstance(extra, dict):
                            info.update(extra)
                        burst(cli, args.out, fr, 0, tag="auto_f%d" % fr,
                              frames=range(max(0, fr - 4), fr + 3),
                              post_shots=False, detector_info=info)
                    while len(shots_kept) > args.ring_size:
                        try:
                            os.remove(shots_kept.popleft()[1])
                        except OSError:
                            pass
                except Exception as e:
                    ring_log.write("%d shot ERR %s\n" % (fr, e))
                    ring_log.flush()

            if now - last_telem >= 2.0:
                last_telem = now
                try:
                    gs = cli.cmd("gte_frame_stats")
                    sa = cli.cmd("sljit_async")
                    ring_log.write("%d telem gte=%s sljit=%s\n" % (
                        fr, json.dumps(gs)[:400], json.dumps(sa)[:400]))
                    ring_log.flush()
                except Exception:
                    pass

            triggered = os.path.exists(trigger_path)
            if not triggered:
                r_, _, _ = select.select([sys.stdin], [], [], 0)
                if r_:
                    # EOF (e.g. stdin=/dev/null under nohup) is NOT a trigger:
                    # readline() returns '' at EOF vs '\n' for a real Enter.
                    if sys.stdin.readline() != "":
                        triggered = True
            if triggered:
                if os.path.exists(trigger_path):
                    os.remove(trigger_path)
                print("[monitor] MANUAL trigger at frame %d" % fr)
                burst(cli, args.out, fr, args.lookback, tag="manual")

            time.sleep(0.02)
    except KeyboardInterrupt:
        print("\n[monitor] stopped")
    finally:
        ring_log.close()
        cli.close()
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", nargs="?", default="monitor", choices=["monitor", "burst"])
    ap.add_argument("--port", type=int, default=4370)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--lookback", type=int, default=300,
                    help="manual burst: frames of GP0 history to dump")
    ap.add_argument("--shot-interval", type=float, default=0.08,
                    help="seconds between ring screenshots (default 0.08 = 12.5Hz)")
    ap.add_argument("--ring-size", type=int, default=250,
                    help="rolling screenshots kept (default 250 = ~20s)")
    ap.add_argument("--auto", dest="auto", action="store_true", default=True)
    ap.add_argument("--no-auto", dest="auto", action="store_false")
    ap.add_argument("--cooldown", type=float, default=4.0,
                    help="min seconds between auto bursts")
    ap.add_argument("--max-auto", type=int, default=60)
    ap.add_argument("--frame", type=int, default=-1, help="burst mode: center frame")
    ap.add_argument("--detector", choices=["black", "missing"], default="black",
                    help="auto-detector flavor in monitor mode: "
                         "'black' (default) catches polygons that turned "
                         "BLACK (DARK-polarity, needs PIL+numpy); "
                         "'missing' is polarity-agnostic and catches "
                         "polygons that VANISH for 1-6 frames in either "
                         "direction (also needs PIL+numpy).")
    args = ap.parse_args()

    if args.mode == "burst":
        cli = wait_for_server(args.port, seconds=10)
        if cli is None:
            print("FATAL: no debug server on port %d" % args.port)
            return 2
        fr = args.frame
        if fr < 0:
            fr = cli.cmd("frame").get("frame", 0)
        burst(cli, args.out, fr, args.lookback)
        cli.close()
        return 0
    return monitor(args)


if __name__ == "__main__":
    sys.exit(main())
