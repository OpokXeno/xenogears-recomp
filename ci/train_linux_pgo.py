#!/usr/bin/env python3
"""Train the glibc 2.31 GCC release on the graphical host, or verify PGO-use.

Compile both passes in the same container path. GCOV_PREFIX redirects the
instrumented executable's /workspace profiles into the host build directory
without changing GCC's encoded object names.
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


def profile_environment(profile_dir: Path, compiled_profile_dir: Path) -> dict[str, str]:
    if not compiled_profile_dir.is_absolute():
        raise ValueError("the compiled profile directory must be absolute")
    return {"GCOV_PREFIX": str(profile_dir.resolve()),
            "GCOV_PREFIX_STRIP": str(len(compiled_profile_dir.parts) - 1)}


def profile_snapshot(profile_dir: Path) -> dict[str, tuple[int, int]]:
    return {str(path): (path.stat().st_mtime_ns, path.stat().st_size)
            for path in profile_dir.rglob("*.gcda") if path.stat().st_size}


def train(args: argparse.Namespace) -> None:
    with (args.build_dir.resolve() / ".pgo-training.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("another PGO training run is using this build directory") from error
        _train(args)


def _train(args: argparse.Namespace) -> None:
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    profiles = build / "pgo"
    exe = build / "XenogearsRecomp"
    bios = build / "bios/openbios.bin"
    for path in (exe, bios, args.savestate, args.disc):
        if not path.is_file():
            raise ValueError(f"missing PGO input: {path}")
    mode = "use" if args.verify_only else "generate"
    cache = (build / "CMakeCache.txt").read_text()
    if f"PSX_PGO:STRING={mode}\n" not in cache or "PSX_RUNTIME_IPO:BOOL=ON\n" not in cache:
        raise ValueError(f"this pass requires PSX_PGO={mode} and PSX_RUNTIME_IPO=ON")
    if args.verify_only:
        verify_optimized_binary(exe, "nm")
    env = graphical_environment(os.environ)
    profiles.mkdir(parents=True, exist_ok=True)
    if not args.verify_only:
        for old in profiles.rglob("*.gcda"):
            old.unlink()
        compiled_profiles = args.compile_root / build.relative_to(root) / "pgo"
        env.update(profile_environment(profiles, compiled_profiles))
    else:
        env.pop("GCOV_PREFIX", None)
        env.pop("GCOV_PREFIX_STRIP", None)
        if not profile_snapshot(profiles):
            raise ValueError("PGO-use verification requires trained GCC profiles")
    env.pop("PSX_HEADLESS", None)
    env.pop("LLVM_PROFILE_FILE", None)
    env.pop("WAYLAND_DISPLAY", None)
    env.update(SDL_VIDEODRIVER="x11", PSX_LOAD_SLOT="0", PSX_HOST_MUTE="1",
               PSX_VSYNC="0", PSX_FRAME_INTERPOLATION="0", __GL_SYNC_TO_VBLANK="0")
    replay = profiles / ("verification.toml" if args.verify_only else "training.toml")
    replay.write_text(neutral_replay(args.vblanks))
    for fps in (240, 30):
        state = profiles / f"{'verify' if args.verify_only else 'train'}-{fps}"
        slot = state / "memcards/openbios/state_80019524_disc1_slot00.pst"
        slot.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.savestate, slot)
        (state / "settings.toml").write_text(SETTINGS.format(fps=fps))
        evidence = state / "evidence.json"
        evidence.unlink(missing_ok=True)
        env["PSX_NATIVE_INTERPOLATION_FPS"] = str(fps)
        before = profile_snapshot(profiles)
        log_path = state / "runtime.log"
        command = [str(exe), "--no-launcher", "--game", str(root / "game.toml"),
                   "--bios", str(bios), "--disc", str(args.disc.resolve()),
                   "--render-mode", "native", "--renderer", "opengl",
                   "--runtime-state", str(state), "--input-replay", str(replay),
                   "--evidence-out", str(evidence)]
        action = "Verifying optimized" if args.verify_only else "Training"
        print(f"{action} Linux Native at {fps} FPS ({args.vblanks} vblanks)...", flush=True)
        with log_path.open("w") as log:
            result = subprocess.run(command, cwd=build, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=args.timeout)
        if result.returncode not in (0, 3):
            raise ValueError(f"Linux training exited {result.returncode}; see {log_path}")
        validate_training(json.loads(evidence.read_text()), log_path.read_text(errors="replace"),
                          fps, args.vblanks)
        after = profile_snapshot(profiles)
        if not args.verify_only and (not after or after == before):
            raise ValueError(f"Linux training at {fps} FPS did not flush fresh GCC profiles")
        if args.verify_only and after != before:
            raise ValueError("the PGO-use executable still writes profiling counters")
    print("Optimized Linux runtime verified at 30 and 240 FPS" if args.verify_only
          else f"Linux PGO profiles ready: {len(profile_snapshot(profiles))} files in {profiles}",
          flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-linux-release"))
    parser.add_argument("--compile-root", type=Path, default=Path("/workspace"))
    parser.add_argument("--savestate", type=Path, required=True)
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
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
