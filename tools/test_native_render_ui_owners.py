from pathlib import Path

import pytest

from native_render_ui_owners import (
    NO_BLOCKER,
    OWNER_IDS,
    UiOwnerCatalogError,
    generate_c_table,
    load_ui_owner_catalog,
)


ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "native_renderer" / "xg_render_ui_owners.toml"
ENUM_HEADER = ROOT / "native_renderer" / "include" / "xg_render_ui_resources.h"
MANIFEST = ROOT / "native_renderer" / "xg_render_manifest.toml"
OVERLAY_INDEX = ROOT / "annotations" / "overlays" / "index.toml"


def _catalog_with(tmp_path: Path, old: str, new: str) -> Path:
    text = CATALOG.read_text(encoding="utf-8").replace(old, new, 1)
    assert text != CATALOG.read_text(encoding="utf-8")
    path = tmp_path / "owners.toml"
    path.write_text(text, encoding="utf-8")
    return path


def test_catalog_has_exact_closed_evidence_and_enum_parity() -> None:
    owners = load_ui_owner_catalog(CATALOG, ENUM_HEADER)

    assert tuple(owner.identifier for owner in owners) == OWNER_IDS
    assert tuple(owner.enum_value for owner in owners) == tuple(range(1, 13))
    assert all(owner.blocker == NO_BLOCKER for owner in owners)
    assert sum(len(owner.roots) for owner in owners) == 35


def test_generated_table_preserves_catalog_identity() -> None:
    generated = generate_c_table(
        CATALOG, ENUM_HEADER, MANIFEST, OVERLAY_INDEX)

    assert generated.count(".root_address =") == 35
    assert ".owner_domain = 12u," in generated
    assert ".root_address = UINT32_C(0x801e5384)," in generated
    assert ".sha256 = {" in generated
    assert ".crc32" not in generated


def test_flat_root_address_is_forbidden(tmp_path: Path) -> None:
    path = _catalog_with(
        tmp_path,
        '{ image_id = "main-disc1-exe", base = "0x80010000", artifact_size = 303104, sha256 = "dc0b2dd786203d4cce5927c5a3fc85a18f39a3f7406078860076ebb0bbae7119", address = "0x80033558" }',
        '"0x80033558"',
    )

    with pytest.raises(UiOwnerCatalogError, match="flat addresses are forbidden"):
        load_ui_owner_catalog(path)


@pytest.mark.parametrize(
    ("old", "new", "message"),
    (
        ('address = "0x80033558"', 'address = "0x80033559"', "not canonical"),
        ("artifact_size = 303104", "artifact_size = 1", "outside its artifact"),
        (
            'sha256 = "dc0b2dd786203d4cce5927c5a3fc85a18f39a3f7406078860076ebb0bbae7119"',
            'sha256 = "DC0B2DD786203D4CCE5927C5A3FC85A18F39A3F7406078860076EBB0BBAE7119"',
            "SHA-256 is invalid",
        ),
    ),
)
def test_root_shape_and_artifact_bounds_are_strict(
        tmp_path: Path, old: str, new: str, message: str) -> None:
    path = _catalog_with(tmp_path, old, new)

    with pytest.raises(UiOwnerCatalogError, match=message):
        load_ui_owner_catalog(path)


def test_artifact_identity_disambiguates_repeated_address(tmp_path: Path) -> None:
    owners = load_ui_owner_catalog(CATALOG)
    repeated = [
        root for owner in owners for root in owner.roots
        if root.address == 0x801E5384
    ]
    assert len(repeated) == 2
    assert repeated[0] == repeated[1]

    path = _catalog_with(
        tmp_path,
        'image_id = "battle-loader-overlay", base = "0x801E4000", artifact_size = 22528',
        'image_id = "other-overlay", base = "0x801E4000", artifact_size = 22528',
    )
    with pytest.raises(UiOwnerCatalogError, match="artifact parity"):
        generate_c_table(path, ENUM_HEADER, MANIFEST, OVERLAY_INDEX)


def test_none_or_missing_owner_is_rejected(tmp_path: Path) -> None:
    path = _catalog_with(tmp_path, "enum_value = 1", "enum_value = 0")

    with pytest.raises(UiOwnerCatalogError, match="enum identity"):
        load_ui_owner_catalog(path)


def test_enum_catalog_drift_is_rejected(tmp_path: Path) -> None:
    header = tmp_path / "xg_render_ui_resources.h"
    header.write_text(
        ENUM_HEADER.read_text(encoding="utf-8").replace(
            "XG_RENDER_UI_OWNER_GEAR_HELPER = 12",
            "XG_RENDER_UI_OWNER_GEAR_HELPER = 11",
        ),
        encoding="utf-8",
    )

    with pytest.raises(UiOwnerCatalogError, match="enum/catalog parity"):
        load_ui_owner_catalog(CATALOG, header)
