#!/usr/bin/env python3
"""Train the instrumented Windows release under Wine, then merge its profiles.

The savestate and all generated state/profiles stay on the private runner.
This exercises Native rendering; headless mode would omit the hot GL paths.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from pgo_training import (SETTINGS, graphical_environment, neutral_replay,
                          validate_training, verify_optimized_binary)


def windows_path(path: Path) -> str:
    # Wine's standard Z: mapping exposes absolute Unix paths, including spaces.
    return "Z:" + path.resolve().as_posix()


def train(args: argparse.Namespace) -> None:
    # Wine reuses Windows PIDs, so two invocations would overwrite the same
    # raw profile names even if their Unix process IDs differ.
    with (args.build_dir.resolve() / ".pgo-training.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("another PGO training run is using this build directory") from error
        _train(args)


def _train(args: argparse.Namespace) -> None:
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    profile_dir = build / "pgo"
    exe = build / "XenogearsRecomp.exe"
    bios = build / "bios/openbios.bin"
    profdata = args.llvm_root / "bin/llvm-profdata"
    for path in (exe, bios, profdata, args.savestate, args.disc):
        if not path.is_file():
            raise ValueError(f"missing PGO input: {path}")
    cache = (build / "CMakeCache.txt").read_text()
    pgo_mode = "use" if args.verify_only else "generate"
    if f"PSX_PGO:STRING={pgo_mode}\n" not in cache or "PSX_RUNTIME_IPO:BOOL=ON\n" not in cache:
        raise ValueError(f"this pass requires PSX_PGO={pgo_mode} and PSX_RUNTIME_IPO=ON")
    if args.verify_only:
        verify_optimized_binary(exe, args.llvm_root / "bin/llvm-nm")
    wine = shutil.which("umu-run" if args.proton else "wine")
    if not wine:
        raise ValueError("umu-run (Proton) or Wine is required to train the Windows executable")
    if args.proton and not (args.proton / "proton").is_file():
        raise ValueError(f"missing Proton runner: {args.proton}")
    env = graphical_environment(os.environ)
    profile_dir.mkdir(parents=True, exist_ok=True)
    # Never merge stale data left by a prior revision or a failed training run.
    if not args.verify_only:
        for path in [*profile_dir.glob("*.profraw"), profile_dir / "default.profdata"]:
            path.unlink(missing_ok=True)
    replay = profile_dir / ("verification.toml" if args.verify_only else "training.toml")
    replay.write_text(neutral_replay(args.vblanks))
    env.pop("PSX_HEADLESS", None)
    # Training selects an X11 DISPLAY. Wine's Wayland driver
    # can otherwise take precedence and fail WGL pixel-format creation.
    env.pop("WAYLAND_DISPLAY", None)
    env["WINEDLLOVERRIDES"] = env.get("WINEDLLOVERRIDES", "") + ";winewayland.drv=d"
    env.update(WINEPREFIX=str(profile_dir / ("proton-prefix" if args.proton else "wine")),
               WINEDEBUG="-all,+debugstr" if args.verify_only else "-all",
               PSX_LOAD_SLOT="0", PSX_HOST_MUTE="1", PSX_VSYNC="0",
               PSX_FRAME_INTERPOLATION="0")
    env["__GL_SYNC_TO_VBLANK"] = "0"
    if args.proton:
        env.update(PROTONPATH=str(args.proton.resolve()), GAMEID="umu-default", UMU_LOG="info",
                   UMU_RUNTIME_UPDATE="0", PROTON_USE_XALIA="0")
    for fps in (240, 30):
        state = profile_dir / f"{'verify' if args.verify_only else 'train'}-{fps}"
        slot = state / "memcards/openbios/state_80019524_disc1_slot00.pst"
        slot.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.savestate, slot)
        (state / "settings.toml").write_text(SETTINGS.format(fps=fps))
        evidence = state / "evidence.json"
        evidence.unlink(missing_ok=True)
        log_path = state / "runtime.log"
        env["PSX_NATIVE_INTERPOLATION_FPS"] = str(fps)
        if args.verify_only:
            env.pop("LLVM_PROFILE_FILE", None)
        else:
            env["LLVM_PROFILE_FILE"] = windows_path(profile_dir / f"train-{fps}-%p.profraw")
        if args.proton:
            # Proton may detach GUI stdout unless its logging is enabled.
            env.update(PROTON_LOG="1", PROTON_LOG_DIR=str(state))
            for old_log in state.glob("steam-*.log"):
                old_log.unlink()
        game_command = [windows_path(exe), "--no-launcher", "--game",
                   windows_path(root / "game.toml"), "--bios", windows_path(bios),
                   "--disc", windows_path(args.disc), "--render-mode", "native",
                   "--renderer", "opengl", "--runtime-state", windows_path(state),
                   "--input-replay", windows_path(replay), "--evidence-out", windows_path(evidence)]
        if args.proton:
            # A Windows path selects Proton's direct Wine launch. Its /unix
            # launcher detaches GUI stdout and can discard application logs.
            command = [wine, *game_command]
        else:
            command = [wine, str(exe), *game_command[1:]]
        action = "Verifying optimized" if args.verify_only else "Training"
        print(f"{action} Windows Native at {fps} FPS ({args.vblanks} vblanks)...", flush=True)
        with log_path.open("w") as log:
            try:
                result = subprocess.run(command, cwd=build, env=env, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=args.timeout)
            finally:
                # The prefix belongs exclusively to this training run. Stop
                # child processes too if Wine times out.
                server = (str(args.proton / "files/bin/wineserver") if args.proton
                          else shutil.which("wineserver"))
                if server:
                    subprocess.run([server, "-k"], env=env, check=False,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   timeout=15)
        # A trace ending without a game-specific checkpoint deliberately exits 3.
        if result.returncode not in (0, 3):
            raise ValueError(f"Windows training exited {result.returncode}; see {log_path}")
        runtime_log = log_path.read_text(errors="replace")
        for proton_log in state.glob("steam-*.log"):
            runtime_log += proton_log.read_text(errors="replace")
        validate_training(json.loads(evidence.read_text()), runtime_log,
                          fps, args.vblanks)
        if not args.verify_only and not any(p.stat().st_size for p in profile_dir.glob(f"train-{fps}-*.profraw")):
            raise ValueError(f"Windows training at {fps} FPS did not flush a profile")
    if args.verify_only:
        print("Optimized Windows runtime verified at 30 and 240 FPS", flush=True)
        return
    raw_profiles = sorted(profile_dir.glob("*.profraw"))
    output = profile_dir / "default.profdata"
    subprocess.run([str(profdata), "merge", "-sparse", *map(str, raw_profiles),
                    "-o", str(output)], check=True)
    subprocess.run([str(profdata), "show", str(output)], check=True)
    print(f"Windows PGO profile ready: {output}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-win"))
    parser.add_argument("--llvm-root", type=Path, required=True)
    parser.add_argument("--savestate", type=Path, required=True)
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--proton", type=Path, help="GE-Proton root; launch via umu-run")
    parser.add_argument("--verify-only", action="store_true", help="validate the final PGO-use executable without modifying profiles")
    parser.add_argument("--vblanks", type=int, default=1800)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if args.vblanks < 120 or args.timeout < 1:
        parser.error("training requires at least 120 vblanks and a positive timeout")
    try:
        train(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f"PGO training failed: {error}")


if __name__ == "__main__":
    main()
