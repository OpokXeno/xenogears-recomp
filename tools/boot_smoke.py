#!/usr/bin/env python3
"""boot_smoke.py — headless boot smoke test for the recompiled runtime.

Launches the runtime with --headless for N seconds and asserts:
  1. the process stays alive for the whole window,
  2. frames advance (the runtime prints "[FPS] game: ... frames: N" every
     second to stderr, also in headless mode),
  3. no psx_crash.txt / psx_freeze_dump_*.json appears.

The binary is copied to a private temp dir before launch so ALL sidecar
writes (memcards, overlay_captures.json, psx_last_run_report.json, crash
dumps) land there and never touch the real build dirs.

Usage:
  tools/boot_smoke.py <path-to-exe> [seconds] [--game <game.toml>]

Exit code: 0 = PASS, 1 = FAIL.
"""

import argparse
import glob
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

FPS_RE = re.compile(rb"\[FPS\] game: .*? frames: (\d+)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe", help="path to the runtime binary (e.g. build/XenogearsRecomp)")
    ap.add_argument("seconds", nargs="?", type=int, default=30,
                    help="how long to run headless (default 30)")
    ap.add_argument("--game", default=None,
                    help="game.toml path (default: <repo>/game.toml)")
    ap.add_argument("--bios", default=None,
                    help="BIOS dump path (default: <repo>/psxrecomp/bios/SCPH1001.BIN)")
    args = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = os.path.abspath(args.exe)
    game = os.path.abspath(args.game) if args.game else os.path.join(repo, "game.toml")
    bios = (os.path.abspath(args.bios) if args.bios
            else os.path.join(repo, "psxrecomp", "bios", "SCPH1001.BIN"))
    for p, what in ((exe, "runtime binary"), (game, "game.toml"), (bios, "BIOS dump")):
        if not os.path.isfile(p):
            print(f"FAIL: {what} not found: {p}")
            return 1

    tmp = tempfile.mkdtemp(prefix="xg_boot_smoke_")
    log_path = os.path.join(tmp, "run.log")
    staged = os.path.join(tmp, os.path.basename(exe))
    shutil.copy2(exe, staged)

    # SDL needs no video driver headless; force it anyway for CI robustness.
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")

    print(f"boot_smoke: {staged} --headless --bios {bios} --game {game} ({args.seconds}s)")
    t0 = time.time()
    with open(log_path, "wb") as log:
        proc = subprocess.Popen(
            [staged, "--headless", "--bios", bios, "--game", game],
            cwd=tmp, env=env, stdout=log, stderr=subprocess.STDOUT)
        alive_whole_window = True
        while time.time() - t0 < args.seconds:
            rc = proc.poll()
            if rc is not None:
                alive_whole_window = False
                print(f"process exited early after {time.time() - t0:.1f}s, rc={rc}")
                break
            time.sleep(0.25)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

    log = open(log_path, "rb").read()
    frames = [int(m.group(1)) for m in FPS_RE.finditer(log)]
    last_frames = frames[-1] if frames else 0

    crashes = (glob.glob(os.path.join(tmp, "psx_crash.txt")) +
               glob.glob(os.path.join(tmp, "psx_freeze_dump_*.json")))

    report = {}
    report_path = os.path.join(tmp, "psx_last_run_report.json")
    if os.path.isfile(report_path):
        try:
            report = json.load(open(report_path))
        except Exception:
            pass

    ok = alive_whole_window and last_frames > 0 and not crashes

    print(f"result: frames={last_frames} "
          f"alive_whole_window={alive_whole_window} "
          f"crash_files={len(crashes)} "
          f"report_reason={report.get('reason', 'n/a')}")
    if crashes:
        for c in crashes:
            print(f"--- {os.path.basename(c)} ---")
            print(open(c, errors="replace").read()[:2000])
    if not frames:
        print("--- no [FPS] lines seen; last 40 log lines ---")
        print(b"\n".join(log.splitlines()[-40:]).decode(errors="replace"))
    print("PASS" if ok else "FAIL")
    print(f"artifacts: {tmp}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
