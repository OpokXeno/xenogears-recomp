from __future__ import annotations

from dataclasses import dataclass
import argparse
from pathlib import Path
import re
import tomllib


SCHEMA = "xg-render-ui-owners/v2"
OWNER_IDS = (
    "resident",
    "field-dialogue",
    "field-credits-generated-glyph-cache",
    "battle-font",
    "battle-ui-atlas-portraits",
    "battling-menu",
    "general-menu",
    "member-change",
    "enter-name",
    "shop",
    "gear-shop",
    "gear-helper",
)
OWNER_ENUMS = (
    "XG_RENDER_UI_OWNER_RESIDENT",
    "XG_RENDER_UI_OWNER_FIELD_DIALOGUE",
    "XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE",
    "XG_RENDER_UI_OWNER_BATTLE_FONT",
    "XG_RENDER_UI_OWNER_BATTLE_UI_ATLAS_PORTRAITS",
    "XG_RENDER_UI_OWNER_BATTLING_MENU",
    "XG_RENDER_UI_OWNER_GENERAL_MENU",
    "XG_RENDER_UI_OWNER_MEMBER_CHANGE",
    "XG_RENDER_UI_OWNER_ENTER_NAME",
    "XG_RENDER_UI_OWNER_SHOP",
    "XG_RENDER_UI_OWNER_GEAR_SHOP",
    "XG_RENDER_UI_OWNER_GEAR_HELPER",
)
NO_BLOCKER = "none"


class UiOwnerCatalogError(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class UiOwnerRoot:
    image_id: str
    base: int
    artifact_size: int
    sha256: str
    address: int


@dataclass(frozen=True, slots=True)
class UiOwner:
    identifier: str
    enum_value: int
    roots: tuple[UiOwnerRoot, ...]
    blocker: str


def _address(value: object, field: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise UiOwnerCatalogError(f"UI owner {field} must be a hexadecimal string")
    try:
        address = int(value, 16)
    except ValueError as error:
        raise UiOwnerCatalogError(f"UI owner {field} is invalid") from error
    if value != f"0x{address:08X}" or address > 0xFFFFFFFF or address & 3:
        raise UiOwnerCatalogError(f"UI owner {field} is not canonical")
    return address


def _parse_root(value: object) -> UiOwnerRoot:
    fields = {"image_id", "base", "artifact_size", "sha256", "address"}
    if not isinstance(value, dict) or set(value) != fields:
        raise UiOwnerCatalogError(
            "UI owner root fields are not closed; flat addresses are forbidden")
    image_id = value["image_id"]
    artifact_size = value["artifact_size"]
    sha256 = value["sha256"]
    if (not isinstance(image_id, str) or
            re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", image_id) is None):
        raise UiOwnerCatalogError("UI owner image id is invalid")
    if (not isinstance(artifact_size, int) or isinstance(artifact_size, bool) or
            artifact_size <= 0 or artifact_size > 0xFFFFFFFF):
        raise UiOwnerCatalogError("UI owner artifact size is invalid")
    if not isinstance(sha256, str) or re.fullmatch(r"[0-9a-f]{64}", sha256) is None:
        raise UiOwnerCatalogError("UI owner SHA-256 is invalid")
    base = _address(value["base"], "base")
    address = _address(value["address"], "root")
    if artifact_size > 0x100000000 - base or not base <= address < base + artifact_size:
        raise UiOwnerCatalogError("UI owner root is outside its artifact")
    return UiOwnerRoot(image_id, base, artifact_size, sha256, address)


def validate_ui_owner_enum(header_path: Path,
                           owners: tuple[UiOwner, ...]) -> None:
    try:
        text = header_path.read_text(encoding="utf-8")
    except OSError as error:
        raise UiOwnerCatalogError(str(error)) from error
    match = re.search(
        r"typedef enum XgRenderUiOwnerDomain\s*\{(?P<body>.*?)"
        r"\}\s*XgRenderUiOwnerDomain\s*;", text, re.DOTALL)
    if match is None:
        raise UiOwnerCatalogError("UI owner enum is missing")
    entries = tuple(
        (name, int(value))
        for name, value in re.findall(
            r"\b(XG_RENDER_UI_OWNER_[A-Z0-9_]+)\s*=\s*(\d+)\s*,",
            match.group("body"))
    )
    expected = (
        ("XG_RENDER_UI_OWNER_NONE", 0),
        *((name, owner.enum_value) for name, owner in zip(OWNER_ENUMS, owners)),
        ("XG_RENDER_UI_OWNER_COUNT", len(owners) + 1),
    )
    if entries != expected:
        raise UiOwnerCatalogError("UI owner enum/catalog parity failed")


def load_ui_owner_catalog(
        path: Path, enum_header_path: Path | None = None) -> tuple[UiOwner, ...]:
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise UiOwnerCatalogError(str(error)) from error
    if not isinstance(raw, dict) or set(raw) != {"schema", "owners"}:
        raise UiOwnerCatalogError("UI owner catalog fields are not closed")
    if raw["schema"] != SCHEMA:
        raise UiOwnerCatalogError("UI owner catalog schema is unsupported")
    raw_owners = raw["owners"]
    if not isinstance(raw_owners, list) or len(raw_owners) != len(OWNER_IDS):
        raise UiOwnerCatalogError("UI owner catalog is incomplete")

    owners: list[UiOwner] = []
    for index, value in enumerate(raw_owners):
        if not isinstance(value, dict) or set(value) != {
            "id", "enum_value", "roots", "blocker",
        }:
            raise UiOwnerCatalogError("UI owner fields are not closed")
        identifier = value["id"]
        enum_value = value["enum_value"]
        roots = value["roots"]
        blocker = value["blocker"]
        if identifier != OWNER_IDS[index] or enum_value != index + 1:
            raise UiOwnerCatalogError("UI owner enum identity is invalid")
        if not isinstance(roots, list) or not isinstance(blocker, str):
            raise UiOwnerCatalogError("UI owner evidence shape is invalid")
        parsed_roots = tuple(_parse_root(root) for root in roots)
        if not parsed_roots or blocker != NO_BLOCKER:
            raise UiOwnerCatalogError("UI owner has no closed evidence")
        owners.append(UiOwner(identifier, enum_value, parsed_roots, blocker))

    result = tuple(owners)
    if enum_header_path is not None:
        validate_ui_owner_enum(enum_header_path, result)
    return result


def _artifact_records(manifest_path: Path,
                       overlay_index_path: Path) -> dict[str, tuple[int, int, str]]:
    try:
        manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
        overlay_index = tomllib.loads(
            overlay_index_path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise UiOwnerCatalogError(str(error)) from error
    game = manifest.get("game", {})
    records = {
        game.get("id"): (
            int(game.get("base_address", "0"), 16),
            game.get("size"), game.get("sha256"),
        ),
    }
    for image in overlay_index.get("images", []):
        records[image.get("id")] = (
            int(image.get("load_address", "0"), 16),
            image.get("size"), image.get("sha256"),
        )
    return records


def validate_ui_owner_artifact_parity(
        owners: tuple[UiOwner, ...], manifest_path: Path,
        overlay_index_path: Path) -> dict[str, tuple[int, int, str]]:
    records = _artifact_records(manifest_path, overlay_index_path)
    for owner in owners:
        for root in owner.roots:
            expected = records.get(root.image_id)
            actual = (root.base, root.artifact_size, root.sha256)
            if expected != actual:
                raise UiOwnerCatalogError(
                    f"UI owner artifact parity failed for {root.image_id}")
    return records


def generate_c_table(catalog_path: Path, enum_header_path: Path,
                     manifest_path: Path, overlay_index_path: Path) -> str:
    owners = load_ui_owner_catalog(catalog_path, enum_header_path)
    records = validate_ui_owner_artifact_parity(
        owners, manifest_path, overlay_index_path)
    lines = [
        '/* Generated from xg_render_ui_owners.toml; do not edit. */',
        '#include "xg_render_ui_owner_catalog.h"',
        '',
        'const XgRenderUiOwnerCatalogEntry xg_render_ui_owner_catalog[] = {',
    ]
    for owner in owners:
        for root in owner.roots:
            expected = records.get(root.image_id)
            assert expected is not None
            sha_bytes = ", ".join(
                f"0x{value:02x}" for value in bytes.fromhex(root.sha256))
            lines.extend((
                '    {',
                f'        .owner_domain = {owner.enum_value}u,',
                f'        .root_address = UINT32_C(0x{root.address:08x}),',
                '        .artifact = {',
                f'            .base = UINT32_C(0x{root.base:08x}),',
                f'            .size = {root.artifact_size}u,',
                f'            .sha256 = {{ {sha_bytes} }},',
                '        },',
                '    },',
            ))
    lines.extend((
        '};',
        'const size_t xg_render_ui_owner_catalog_count =',
        '    sizeof(xg_render_ui_owner_catalog) /',
        '    sizeof(xg_render_ui_owner_catalog[0]);',
        '',
    ))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("generate-c",))
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--enum-header", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--overlay-index", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    text = generate_c_table(
        args.catalog, args.enum_header, args.manifest, args.overlay_index)
    args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
