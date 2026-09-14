#!/usr/bin/env python3
"""Check Native codegen and build inputs before either release is packaged.

This is a build consistency check, not a gameplay/rendering certification.
Read the source list selected by CMake so unused generated files cannot hide
missing hooks in the code actually compiled into the runtime.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tomllib


ROOT = Path(__file__).resolve().parents[1]
HOOK = re.compile(
    r"\bpsx_xg_render_auth_native_ft4_bypass\s*\(\s*cpu\s*,\s*"
    r"0x([0-9a-fA-F]{8})u\s*,\s*0x([0-9a-fA-F]{8})u\s*\)"
)


def read_toml(path: Path) -> dict:
    return tomllib.loads(path.read_text(encoding="utf-8"))


def verify_resident_hooks(plan: Path, sources: list[Path]) -> int:
    lines = plan.read_text(encoding="ascii").splitlines()
    if not lines or lines[0] != "psxrecomp-source-observation-plan-v5":
        raise ValueError("unsupported resident plan schema")
    expected = set()
    for line in lines[1:]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 5 or fields[0] != "cutover":
            raise ValueError("resident plan contains an unsupported record")
        expected.add((int(fields[1], 16), int(fields[2], 16)))
    if not expected:
        raise ValueError("resident plan is empty")
    observed = set()
    for source in sources:
        text = source.read_text(encoding="utf-8")
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)
        observed.update((int(pc, 16), int(word, 16)) for pc, word in HOOK.findall(text))
    missing = expected - observed
    if missing:
        addresses = ", ".join(f"0x{pc:08X}" for pc, _ in sorted(missing))
        raise ValueError(f"missing resident Native hooks: {addresses}")
    return len(expected)


def verify_identity_array(text: str, name: str, expected: str) -> None:
    match = re.search(rf"\b{re.escape(name)}\[32\]\s*=\s*\{{([^}}]*)\}}", text)
    actual = "" if match is None else "".join(
        re.findall(r"0x([0-9a-fA-F]{2})\b", match[1])
    ).lower()
    if actual != expected:
        raise ValueError(f"stale or disabled Native identity: {name}")


def verify_nonempty_table(path: Path, count_name: str) -> None:
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"\b{re.escape(count_name)}\s*=\s*(\d+)u\s*;", text)
    if match is None or int(match[1]) == 0:
        raise ValueError(f"empty Native table: {count_name}")


def verify_build(root: Path, build: Path) -> dict:
    cache = dict(re.findall(
        r"^([^/#\n][^:\n]*):[^=\n]+=(.*)$",
        (build / "CMakeCache.txt").read_text(encoding="utf-8"), re.MULTILINE,
    ))
    if cache.get("XG_RENDER_NATIVE", "").upper() not in {"ON", "TRUE", "1", "YES"}:
        raise ValueError("release requires XG_RENDER_NATIVE=ON")
    config = read_toml(root / "game.toml")
    runtime = config.get("runtime", {})
    if runtime.get("render_mode") != "native":
        raise ValueError("release game.toml must select render_mode = native")
    if runtime.get("overlay_cache") is not False:
        raise ValueError("AOT release game.toml must select overlay_cache = false")
    if any(runtime.get(key) for key in
           ("overlay_autocompile_cmd", "overlay_autocompile_cmd_tcc")):
        raise ValueError("AOT release must not configure overlay autocompilation")
    manifest_hash = hashlib.sha256(
        (root / "native_renderer/xg_render_manifest.toml").read_bytes()
    ).hexdigest()
    game_hash = hashlib.sha256((root / config["game"]["exe"]).read_bytes()).hexdigest()
    manifest_table = build / "generated/xg_render_manifest_table.c"
    text = manifest_table.read_text(encoding="utf-8")
    verify_identity_array(text, "xg_render_game_identity", game_hash)
    verify_identity_array(text, "xg_render_manifest_identity", manifest_hash)
    verify_nonempty_table(manifest_table, "xg_render_manifest_record_count")
    verify_nonempty_table(
        build / "generated/xg_render_runtime_variant_table.c",
        "xg_render_runtime_variant_descriptor_count",
    )
    sources = [Path(line) for line in
               (build / "psx-runtime_generated_sources.txt").read_text().splitlines()
               if line.strip()]
    hooks = verify_resident_hooks(
        root / "native_renderer/xg_render_resident_plan.txt", sources,
    )
    coverage = json.loads(
        (build / "overlay_aot/static/overlays_static_coverage.json").read_text()
    )
    if coverage.get("schema") != "psxrecomp static overlay coverage v3":
        raise ValueError("unsupported AOT overlay coverage schema")
    if (coverage.get("game_identity_sha256") != game_hash or
            coverage.get("manifest_identity_sha256") != manifest_hash):
        raise ValueError("stale AOT overlay identities")
    expected_images = {
        image["id"] for image in
        read_toml(root / "annotations/overlays/index.toml")["images"]
    }
    images = coverage.get("images", [])
    actual_images = {image["image_id"] for image in images}
    if (not expected_images or actual_images != expected_images or
            len(images) != len(actual_images) or
            any(not image.get("ranges") for image in images)):
        raise ValueError("incomplete AOT overlay coverage")
    return {"resident_hooks": hooks, "aot_images": len(images),
            "game_sha256": game_hash, "manifest_sha256": manifest_hash}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = verify_build(ROOT, args.build_dir.resolve())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Native release build verification failed: {error}", file=sys.stderr)
        return 1
    print("Native release build verified: " + json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
