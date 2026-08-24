from __future__ import annotations

import re
import tomllib
from pathlib import Path

from validate_overlay_annotations import DEFAULT_INDEX, ROOT, validate_index


def test_repository_overlay_annotations_are_valid() -> None:
    assert validate_index(DEFAULT_INDEX, ROOT) == []


def test_repository_catalog_contains_general_gameplay_images() -> None:
    document = tomllib.loads(DEFAULT_INDEX.read_text(encoding="utf-8"))
    assert [(image["id"], image["subsystem"]) for image in document["images"]] == [
        ("field-overlay", "field"),
        ("world-overlay", "world"),
        ("battle-overlay", "battle"),
        ("battling-overlay", "battling"),
        ("menu-overlay", "menu"),
        ("movie-overlay", "movie"),
        ("member-change-menu-overlay", "member-change-menu"),
        ("shop-menu-overlay", "shop-menu"),
        ("gear-shop-menu-overlay", "gear-shop-menu"),
        ("gear-helper-overlay", "gear-helper"),
        ("battle-event-overlay", "battle-event"),
    ]


def test_overlay_coverage_headers_match_function_rows() -> None:
    document = tomllib.loads(DEFAULT_INDEX.read_text(encoding="utf-8"))
    for image in document["images"]:
        text = (ROOT / image["annotations"]).read_text(encoding="utf-8")
        match = re.search(r"^# annotated_functions/total_functions: (\d+)/(\d+)$", text, re.MULTILINE)
        assert match is not None, image["id"]
        annotated, total = map(int, match.groups())
        assert annotated == sum(", function start" in line for line in text.splitlines())
        assert annotated <= total


def test_same_address_is_allowed_in_distinct_images(tmp_path: Path) -> None:
    first_sha = "1" * 64
    second_sha = "2" * 64
    (tmp_path / "source.toml").write_text(
        f'''[[variants]]
id = "first-source"
base_address = "0x80010000"
artifact_size = 4
artifact_sha256 = "{first_sha}"

[[variants]]
id = "second-source"
base_address = "0x80010000"
artifact_size = 4
artifact_sha256 = "{second_sha}"
''',
        encoding="utf-8",
    )
    annotations = tmp_path / "annotations" / "overlays"
    annotations.mkdir(parents=True)
    for image_id, source_id, sha256, filename in (
        ("first", "first-source", first_sha, "first.csv"),
        ("second", "second-source", second_sha, "second.csv"),
    ):
        (annotations / filename).write_text(
            "\n".join(
                (
                    f"# image-id: {image_id}",
                    f"# sha256: {sha256}",
                    "# load-address: 0x80010000",
                    "# identity-source: source.toml",
                    f"# source-record-id: {source_id}",
                    "0x80010000, function start — SharedAddress",
                    "",
                )
            ),
            encoding="utf-8",
        )
    (annotations / "index.toml").write_text(
        f'''schema = "xenogears-overlay-annotations/v1"

[[images]]
id = "first"
subsystem = "field"
logical_name = "first"
sha256 = "{first_sha}"
size = 4
load_address = "0x80010000"
image_format = "raw"
header_size = 0
loaded_size = 4
annotations = "annotations/overlays/first.csv"
identity_source = "source.toml"
source_kind = "overlay-range-variant"
source_record_id = "first-source"

[[images]]
id = "second"
subsystem = "world"
logical_name = "second"
sha256 = "{second_sha}"
size = 4
load_address = "0x80010000"
image_format = "raw"
header_size = 0
loaded_size = 4
annotations = "annotations/overlays/second.csv"
identity_source = "source.toml"
source_kind = "overlay-range-variant"
source_record_id = "second-source"
''',
        encoding="utf-8",
    )

    assert validate_index(annotations / "index.toml", tmp_path) == []


def test_duplicate_address_in_one_image_is_rejected(tmp_path: Path) -> None:
    sha256 = "3" * 64
    (tmp_path / "source.toml").write_text(
        f'''[[variants]]
id = "source"
base_address = "0x80010000"
artifact_size = 4
artifact_sha256 = "{sha256}"
''',
        encoding="utf-8",
    )
    annotations = tmp_path / "annotations" / "overlays"
    annotations.mkdir(parents=True)
    (annotations / "image.csv").write_text(
        f'''# image-id: image
# sha256: {sha256}
# load-address: 0x80010000
# identity-source: source.toml
# source-record-id: source
0x80010000, function start — First
0x80010000, instruction site — Duplicate
''',
        encoding="utf-8",
    )
    (annotations / "index.toml").write_text(
        f'''schema = "xenogears-overlay-annotations/v1"

[[images]]
id = "image"
subsystem = "field"
logical_name = "image"
sha256 = "{sha256}"
size = 4
load_address = "0x80010000"
image_format = "raw"
header_size = 0
loaded_size = 4
annotations = "annotations/overlays/image.csv"
identity_source = "source.toml"
source_kind = "overlay-range-variant"
source_record_id = "source"
''',
        encoding="utf-8",
    )

    errors = validate_index(annotations / "index.toml", tmp_path)
    assert any("duplicate address" in error for error in errors)
