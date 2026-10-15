"""Shared real-renderer training inputs and checks for release PGO."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess


def verify_optimized_binary(executable, nm) -> None:
    symbols = subprocess.check_output([str(nm), "--defined-only", str(executable)], text=True)
    if any(name in symbols for name in ("__gcov_init", "__llvm_profile_write_file")):
        raise ValueError(f"the release executable still contains profiling counters: {executable}")


def graphical_environment(environment: dict[str, str]) -> dict[str, str]:
    env = dict(environment)
    if env.get("DISPLAY"):
        return env
    # System services also lack XDG_RUNTIME_DIR, which systemctl --user needs
    # to reach the graphical user's bus. Only use a live bus owned by this UID.
    if not env.get("XDG_RUNTIME_DIR"):
        runtime = Path("/run/user") / str(os.getuid())
        if runtime.is_dir() and runtime.stat().st_uid == os.getuid() and (runtime / "bus").exists():
            env["XDG_RUNTIME_DIR"] = str(runtime)
    # A self-hosted runner service does not necessarily inherit the desktop's
    # environment. Import only display-related values, never evaluate shell
    # text or copy unrelated session variables into the build.
    systemctl = shutil.which("systemctl")
    if systemctl:
        result = subprocess.run([systemctl, "--user", "show-environment"],
                                env=env, capture_output=True, text=True, check=False, timeout=5)
        if result.returncode == 0:
            for line in result.stdout.splitlines():
                key, separator, value = line.partition("=")
                if separator and key in {"DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR"}:
                    env[key] = value
    if not env.get("DISPLAY"):
        raise ValueError("PGO rendering needs an accessible X11/XWayland desktop; configure DISPLAY and XAUTHORITY for the runner")
    return env


def validate_training(evidence: dict, log: str, fps: int, vblanks: int) -> None:
    if "savestate: LOADED slot 0" not in log:
        raise ValueError("PGO training did not load slot 0")
    if (evidence.get("completion") != "trace_complete" or
            evidence.get("runtime_status") != "PASS" or
            evidence.get("render_mode") != "native" or
            evidence.get("backend") != "opengl"):
        raise ValueError("PGO training did not complete healthy Native/OpenGL gameplay")
    replay = evidence.get("replay", {})
    if replay.get("trace_index", 0) < vblanks:
        raise ValueError("PGO training stopped before the vblank budget")
    # The legacy midpoint diagnostic clamps its reported target to >= 60,
    # even when Native presentation is configured to run at 30 FPS.
    if fps > 30 and evidence.get("native_midpoint", {}).get("target_fps") != fps:
        raise ValueError("PGO training used the wrong Native FPS setting")
    work = evidence.get("native_work", {})
    if (not work.get("executed") or work.get("health") != "healthy" or
            not work.get("errors") or any(work["errors"].values())):
        raise ValueError("PGO training reported Native work errors")
    if fps > 30:
        motion = work.get("motion", {})
        if (not work.get("presentation", {}).get("presented_holds", 0) or
                not motion.get("last_endpoint_phase_count", 0) or
                not motion.get("evaluations", 0)):
            raise ValueError("PGO training did not exercise interpolated presentation")


def neutral_replay(vblanks: int) -> str:
    return f'''schema = "xenogears.native-render-replay/v3"
complete = true
record_on_close = true
vblank_budget = {vblanks}

[[vblank]]
repeat = {vblanks}
p1_connected = true
p1_mode = "analog"
p1_buttons = []
p1_left_x = 0
p1_left_y = 0
p1_right_x = 0
p1_right_y = 0
p1_trigger_left = 0
p1_trigger_right = 0
p2_connected = false
p2_mode = "digital"
p2_buttons = []
p2_left_x = 0
p2_left_y = 0
p2_right_x = 0
p2_right_y = 0
p2_trigger_left = 0
p2_trigger_right = 0
'''


SETTINGS = '''[video]
renderer = "opengl"
supersampling = 3
window_width = 960
fullscreen = 0
vsync = "immediate"
fps = {fps}
aspect_ratio = "16:9"
z_buffer = true
backdrop_stretch = true
antialiasing = "smaa"
antialiasing_factor = 16
texture_filtering = "nearest"
sprite_filtering = "nearest"
anisotropic_filtering = 16
bios_hle = true
dithering = false
[controller]
p1_device = "keyboard"
p1_mode = "analog"
p2_device = "none"
'''
