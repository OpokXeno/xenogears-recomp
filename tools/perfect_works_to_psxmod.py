#!/usr/bin/env python3
"""Convert authenticated Perfect Works Build 0.11.2 patches into psxmods."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
import zipfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Iterable


RAW_SECTOR_SIZE = 2352
USER_SECTOR_SIZE = 2048
USER_OFFSET = 24
TABLE_LBA = 0x18
TABLE_SECTORS = 0x10
SENTINEL_LBA = 0xFFFFFF
PSX_EXE_HEADER_SIZE = 0x800
MAX_ACTIVE_PAYLOAD = 128 * 1024 * 1024
MAX_EXPANDED_ARCHIVE = 256 * 1024 * 1024
MAX_ARCHIVE_FILES = 4096
MAX_VIRTUAL_SECTORS = 64 * 1024
SUPPORTED_PWB_VERSION = "0.11.2"

DISC_BIN_SHA256 = {
    1: "39c547a9afc6da15d847ef81a2c6cea1a6516bdfa562cf13b0999b04e8598bda",
    2: "5eab85c683d4d7087d345b587472db9c44df29b35ce66553c2626d26018b947e",
}
DISC_CANONICAL_SHA256 = {
    1: "74265236654985f8d5d76f79767ca62a9b2b6ba299c995211ff94588928a6235",
    2: "b5fce68b407e9f4ae7474b3487a3d9a35ccd2c98e8b377374dd1fc1060450e30",
}
DISC_EXECUTABLE_INDEX = {1: 22, 2: 17}

# SHA-256 over sorted "relative-path\0size\0file-sha256\n" rows from the
# published Xenogears_Perfect_Works_Edition.0.11.2.7z extraction.
EDITION_DIRECTORY_SHA256 = {
    "gamefiles/Monsters": "b36c5ae48114db58bd724e39746341afac0985204a828554a298d21ccf598e29",
    "gamefiles/Script_items": "538152d96da8110f9627b429a85c01d068969fd1b1c02ddf3cae2894655a11b0",
    "gamefiles/Script_items2": "92d6393cdcafdd0705d7bd307831cc46bb3501525940a3a00a92b939fa6ba0d4",
    "gamefiles/bug_fix1": "8dac4123d35267b27f9d59580b0ba7d8336c661d817570d95bc3a0364dfd75d2",
    "gamefiles/bug_fix2": "e0a327dd66baca39575d4a4e80a6a9ee9ad3780b6184525b347ef0180cc3c59f",
    "gamefiles/control_files_1": "bd25f50aa1e4ffcfefa0184b4c39b5c34cd24068b1442bb3413dc261c77dd214",
    "gamefiles/control_files_2": "93d37828065c49150fbc2767412d0336d46277e14687d28073305280e941f8a5",
    "gamefiles/emeralda_fix/new_script": "24ded43fe4f3843b53d914b8d8faf354864bff9aa128d8ada5be154800c0289a",
    "gamefiles/emeralda_fix/og_script": "8f0ef818136eeff90d945cf0583a5c3df3208c7781cadfcb0df46d45ac30eabf",
    "gamefiles/encounter_rate_1": "0e9f074cca05cca3107a137da159438ceb5107889fe23d9904c5d3e3db654fb1",
    "gamefiles/encounter_rate_2": "30b515ab82ddee9c3a1a2f8ff82b8aa3f6630c09779794d2c17e320e4f652ac9",
    "gamefiles/encounterone_script": "f5c1976426d09d7523995ad538ef15a792a0f5d11fa75c719622054f737d8871",
    "gamefiles/encountertwo_script": "c133f11323582ed0ad2a956bccab8477f7d0d4b4a731b0c3a6b970e31ea47e9f",
    "gamefiles/exp_data": "425f28e1a523cbc6ef3bca8d1b134b6cde7fc7e5c006963753bd824cb6adcdad",
    "gamefiles/filesbasic": "e522ffa863e730142d6688d86f49fea590eb6051e5cea0d61a24420ff8534b0d",
    "gamefiles/filesbasic_script": "dd6aa0935a8b3ff83f38f1970152e9b832ea07551f3179dcd0a8710d012fd333",
    "gamefiles/filesexpert": "f855a4a86b13c5d50bdc3c1d0bdf9a7879daaa2955ffdc3e461cd6dd727c49c4",
    "gamefiles/filesexpert_script": "a3e29dc2f2727074553d8f33d4c3e45a067c2f2f99c9f6e8f05f647a94dbcefe",
    "gamefiles/items1": "a4636d5d15a27d1ecfcb19ecb6ca3b1a2ddeb77e75ff995ef435cb75b4bde8ff",
    "gamefiles/items2": "10f76d28168e11b07208b0be8d41823c819928a553e8cfb27a2d6cb381267954",
    "gamefiles/jpn_ctrl_subfiles": "58f8181e03cf24cc93f28604aafd7073403f7bc317242494704aff73abf4c66d",
    "gamefiles/jpn_items_1": "8c784938eb42667409099e613ef7c828837e65fab09e0da5db9bfcab025c238b",
    "gamefiles/jpn_items_2": "8c784938eb42667409099e613ef7c828837e65fab09e0da5db9bfcab025c238b",
    "gamefiles/jpn_script_1": "aace99bffb8ec7879d18b4d1f38f00e0d15c13b93de0f86fdaa273065bc71b1d",
    "gamefiles/jpn_script_2": "9dab6a9c85efb66ff2bfb4e931e74c6dce74a6512603567c7315d8ae1c138ab8",
    "gamefiles/monsters_both": "278758ecfaa6f8f63455e599dab5d54999448fa4a99e198f7681f1947ae68f4b",
    "gamefiles/monsters_items": "a7056327ea594586f2373ced1733500fd81b967863989afbd52d2a494b18fc32",
    "gamefiles/monsters_script": "ac5390e95f909bef3f5fae09929f8b6832bb9fa7c95f610186cfa8283c267e53",
    "gamefiles/music_1": "0b84816683932fb2d9fd1791fb302d6488c3a796e2015954b5af54ce610c2938",
    "gamefiles/music_2": "2370eca3bcf9e33ba1b99c52b6a5a7133c31c9ac3fcf67841783930df4cab4c8",
    "gamefiles/music_subfiles": "22acb6513235adb8d7b05ba9b69e75514839a96b464d9949f36f75fab2d7d395",
    "gamefiles/og_monsters": "dd8239eb8d543ff08c566fa2268ecef79d4b115972de6200bfa651fd30f8ea1c",
    "gamefiles/portraits": "3820f9ed7980c1373587adfc6e43ab136939ae61208cfd706249d764701aaeef",
    "gamefiles/resized_portraits": "cd368ba4164286e25a57cfbde1523b0d5dc0aebbe44389236d1461cc5ec5b620",
    "gamefiles/roni_pw/default": "d1739128f755047e00e612ef9371dd0e1d39e7a2d4eeaade0f60307f1ce329e9",
    "gamefiles/roni_pw/resized": "619f188465aa37d21743d59da3315b48bc5fccc7000b716e7d43f591b2974cac",
    "gamefiles/script1": "ef8864b07313bf4a6e074bc4fe61e50e870456e4cad0ea6280fa0f470ced19d2",
    "gamefiles/script2": "37705e5acc3dc19ed32345e4a05d0852c3de219d34185810170e73d68392f1b3",
    "gamefiles/storyfiles_cd1": "b7f721531c438456503e578a569bd4d879a2515fa7ddcc9c1b7d62f77ce54e61",
    "gamefiles/storyfiles_cd2": "8d6237162938b54d50e33626994d6d96c3ae6965002f04ba08765cf3c4468a9a",
    "gamefiles/storyfiles_script_cd1": "78cdb27df588d93b179c28a90180cfd705f87af9c4d7b1b0855820e6775c3157",
    "gamefiles/storyfiles_script_cd2": "816aa1d729fa576e206db94f61d8902f6bb09b23f0a3e6fbbe2d4d14c8efbc31",
    "gamefiles/text_cd1": "ea4e389f3df46ad5950244e8b72255701e7238b53641e6b4d43199df174547f6",
    "gamefiles/text_cd2": "a9b8185627e1a92de861c88a5e17238a2e20b86c7a6ac612adf20e5a2b319f37",
    "gamefiles/text_old1": "e02ffb71d23f1f8c00e9a11cae8fbf199bba21f2c2f6515178a324fd788ca669",
    "gamefiles/title_screen": "c1fc11f44ae197b4a0fc672421954c91d52b76b781eb95993a796642e57946c0",
    "gamefiles/voice": "0b569e7e1c5bdc394205413ac03ef2aa779fe0746f2d68cea00a2efc7baee440",
    "data/controls": "8a0e9098417e7613be1341daa9d31fe577a0e063bc25104568cc19520751bfca",
}
EDITION_FILE_SHA256 = {
    "README.md": "7d2b3f87a6a30eff35b4e78921178e8a9a59bd47f3664b611bc0a40fc296f0a9",
    "Tools/xenocomp.exe": "738e8dd4f1e46af13af026bd17d7af2138a0bba0c6f8197ae7ef01b97f446933",
    "Tools/xenopack.exe": "6a20de13c87e5439d9ad4072e64398965e3c8f65a13775ffe4059590ae9f2623",
}


@dataclass(frozen=True)
class TableEntry:
    lba: int
    size: int


@dataclass(frozen=True)
class IsoFile:
    path: str
    lba: int
    size: int


@dataclass(frozen=True)
class PatchOptions:
    bug_fixes: bool = False
    title_screen: bool = False
    script: bool = False
    half_encounters: bool = False
    exp_factor: str = "off"
    gold_factor: str = "off"
    rebalanced_items: bool = False
    rebalanced_enemies: bool = False
    no_deathblow_levels: bool = False
    no_damage_cap: bool = False
    arena: str = "normal"
    face_fix: str = "none"
    no_battle_flashes: bool = False
    pw_roni: bool = False
    emeralda_cafe_fix: bool = False
    text_speed: str = "normal"
    battle_undub: bool = False
    music_changes: bool = False
    story_mode: bool = False
    jpn_controls: bool = False
    fmv_undub: bool = False


@dataclass(frozen=True)
class Profile:
    name: str
    description: str
    options: PatchOptions
    implicit_patches: bool = True


@dataclass(frozen=True)
class IndividualMod:
    key: str
    name: str
    description: str
    options: PatchOptions | None = None
    option_id: str | None = None
    option_label: str | None = None
    choices: tuple[IndividualChoice, ...] = ()


@dataclass(frozen=True)
class IndividualChoice:
    value: str
    label: str
    options: PatchOptions


@dataclass(frozen=True)
class ChoiceOption:
    id: str
    label: str
    default: str
    choices: tuple[tuple[str, str], ...]


@dataclass(frozen=True)
class PreparedInputs:
    raw_hashes: dict[int, str]
    tables: dict[int, tuple[list[TableEntry], int]]
    iso_files: dict[int, list[IsoFile]]
    executables: dict[int, bytes]


@dataclass
class StagedFile:
    listed_index: int
    index: int
    path: Path
    sources: list[str] = field(default_factory=list)
    transforms: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class IndexedOperation:
    disc: int
    index: int
    listed_index: int
    payload_path: Path
    payload_sha256: str
    expected_sha256: str
    payload_size: int
    sources: tuple[str, ...]
    transforms: tuple[str, ...]
    when: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True)
class ExePatch:
    address: int
    expected: bytes
    replacement: bytes
    purpose: str
    when: tuple[tuple[str, str], ...] = ()


ALL_COMMON = dict(
    script=True,
    half_encounters=True,
    rebalanced_items=True,
    rebalanced_enemies=True,
    no_deathblow_levels=True,
    no_damage_cap=True,
    arena="basic",
    battle_undub=True,
    music_changes=True,
    jpn_controls=True,
    fmv_undub=True,
)


def all_profile(factor: str, graphics: bool, text_speed: str) -> PatchOptions:
    return PatchOptions(
        **ALL_COMMON,
        exp_factor=factor,
        gold_factor=factor,
        face_fix="resize" if graphics else "none",
        no_battle_flashes=graphics,
        pw_roni=graphics,
        emeralda_cafe_fix=graphics,
        text_speed=text_speed,
    )


PROFILES = {
    "retranslation": Profile(
        "Perfect Works Retranslation",
        "Script and terminology changes only.",
        PatchOptions(script=True),
        False,
    ),
    "bug_fixes": Profile(
        "Perfect Works Bug Fixes",
        "The upstream bug-fix-only selection.",
        PatchOptions(),
    ),
    "story_mode": Profile(
        "Perfect Works Story Mode",
        "Story mode with the original English script.",
        PatchOptions(story_mode=True),
    ),
    "story_mode_script": Profile(
        "Perfect Works Story Mode + Retranslation",
        "Story mode composed with script and terminology changes.",
        PatchOptions(story_mode=True, script=True),
    ),
    "all_50_exp_gold_fast_text": Profile(
        "Perfect Works All 1.5x + Fast Text",
        "Indexed port of the upstream all-patches 1.5x preset.",
        all_profile("1.5", True, "fast"),
    ),
    "all_100_exp_gold_fast_text": Profile(
        "Perfect Works All 2x + Fast Text",
        "Indexed port of the upstream all-patches 2x preset.",
        all_profile("2", True, "fast"),
    ),
    "all_50_exp_gold_no_gfx": Profile(
        "Perfect Works All 1.5x, No Graphics",
        "Indexed port of the upstream no-graphics 1.5x preset.",
        all_profile("1.5", False, "fast"),
    ),
    "all_100_exp_gold_no_gfx": Profile(
        "Perfect Works All 2x, No Graphics",
        "Indexed port of the upstream no-graphics 2x preset.",
        all_profile("2", False, "fast"),
    ),
    "all_50_exp_gold_normal_text": Profile(
        "Perfect Works All 1.5x + Normal Text",
        "Indexed port of the upstream normal-text 1.5x preset.",
        all_profile("1.5", True, "normal"),
    ),
    "all_100_exp_gold_normal_text": Profile(
        "Perfect Works All 2x + Normal Text",
        "Indexed port of the upstream normal-text 2x preset.",
        all_profile("2", True, "normal"),
    ),
}


# Each entry represents one patcher control, not one of the upstream combined
# presets. Mutually exclusive values from one control share a choice selector.
INDIVIDUAL_MODS = (
    IndividualMod(
        "bug-fixes",
        "Perfect Works - Bug Fixes",
        f"Original-game bug fixes from Perfect Works Build {SUPPORTED_PWB_VERSION}.",
        PatchOptions(bug_fixes=True),
    ),
    IndividualMod(
        "title-screen",
        "Perfect Works - Title Screen",
        "Perfect Works title-screen replacement.",
        PatchOptions(title_screen=True),
    ),
    IndividualMod(
        "retranslation",
        "Perfect Works - Retranslation",
        "Script and terminology changes.",
        PatchOptions(script=True),
    ),
    IndividualMod(
        "half-encounters",
        "Perfect Works - Half Encounters",
        "Reduces the random encounter rate by half.",
        PatchOptions(half_encounters=True),
    ),
    IndividualMod(
        "exp",
        "Perfect Works - EXP Multiplier",
        "Selects the Perfect Works battle experience multiplier.",
        option_id="multiplier",
        option_label="EXP multiplier",
        choices=(
            IndividualChoice("1x", "1x", PatchOptions(exp_factor="1")),
            IndividualChoice("1-5x", "1.5x", PatchOptions(exp_factor="1.5")),
            IndividualChoice("2x", "2x", PatchOptions(exp_factor="2")),
        ),
    ),
    IndividualMod(
        "gold",
        "Perfect Works - Gold Multiplier",
        "Selects the Perfect Works battle gold multiplier.",
        option_id="multiplier",
        option_label="Gold multiplier",
        choices=(
            IndividualChoice("1x", "1x", PatchOptions(gold_factor="1")),
            IndividualChoice("1-5x", "1.5x", PatchOptions(gold_factor="1.5")),
            IndividualChoice("2x", "2x", PatchOptions(gold_factor="2")),
        ),
    ),
    IndividualMod(
        "rebalanced-items",
        "Perfect Works - Rebalanced Party and Items",
        "Applies the Perfect Works party, equipment, and item rebalance.",
        PatchOptions(rebalanced_items=True),
    ),
    IndividualMod(
        "rebalanced-enemies",
        "Perfect Works - Rebalanced Enemies",
        "Applies the Perfect Works enemy rebalance.",
        PatchOptions(rebalanced_enemies=True),
    ),
    IndividualMod(
        "no-deathblow-levels",
        "Perfect Works - No Deathblow Levels",
        "Removes level requirements from Deathblow learning.",
        PatchOptions(no_deathblow_levels=True),
    ),
    IndividualMod(
        "no-damage-cap",
        "Perfect Works - No Damage Cap",
        "Removes the 9999 character and Gear damage caps.",
        PatchOptions(no_damage_cap=True),
    ),
    IndividualMod(
        "arena",
        "Perfect Works - Battle Arena",
        "Selects the basic or expert Battle Arena rebalance.",
        option_id="mode",
        option_label="Arena mode",
        choices=(
            IndividualChoice("basic", "Basic", PatchOptions(arena="basic")),
            IndividualChoice("expert", "Expert", PatchOptions(arena="expert")),
        ),
    ),
    IndividualMod(
        "portraits",
        "Perfect Works - Portraits",
        "Selects corrected portraits at original or consistent dimensions.",
        option_id="size",
        option_label="Portrait size",
        choices=(
            IndividualChoice(
                "original", "Original dimensions", PatchOptions(face_fix="normal")
            ),
            IndividualChoice(
                "resized", "Consistent dimensions", PatchOptions(face_fix="resize")
            ),
        ),
    ),
    IndividualMod(
        "no-battle-flashes",
        "Perfect Works - No Battle Flashes",
        "Disables the targeted bright battle flash.",
        PatchOptions(no_battle_flashes=True),
    ),
    IndividualMod(
        "pw-roni",
        "Perfect Works - Perfect Works Roni",
        "Applies the Perfect Works Roni portrait at the original dimensions.",
        PatchOptions(pw_roni=True),
    ),
    IndividualMod(
        "emeralda-cafe-fix",
        "Perfect Works - Emeralda Cafe Fix",
        "Fixes Emeralda's cafe appearance for the original English script.",
        PatchOptions(emeralda_cafe_fix=True),
    ),
    IndividualMod(
        "text-speed",
        "Perfect Works - Text Speed",
        "Selects fast or instant text display.",
        option_id="speed",
        option_label="Text speed",
        choices=(
            IndividualChoice("fast", "Fast", PatchOptions(text_speed="fast")),
            IndividualChoice(
                "instant", "Instant", PatchOptions(text_speed="instant")
            ),
        ),
    ),
    IndividualMod(
        "battle-undub",
        "Perfect Works - Battle Undub",
        "Restores the selected Japanese battle voices.",
        PatchOptions(battle_undub=True),
    ),
    IndividualMod(
        "music-changes",
        "Perfect Works - Music Changes",
        "Applies the Perfect Works field music corrections.",
        PatchOptions(music_changes=True),
    ),
    IndividualMod(
        "story-mode",
        "Perfect Works - Story Mode",
        "Applies Story Mode with the original English script.",
        PatchOptions(story_mode=True),
    ),
    IndividualMod(
        "jpn-controls",
        "Perfect Works - Japanese Controls",
        "Applies Japanese field, image, and battle controls where representable.",
        PatchOptions(jpn_controls=True),
    ),
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def directory_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    files = sorted(item for item in path.rglob("*") if item.is_file())
    if not files:
        raise RuntimeError(f"Perfect Works directory is missing or empty: {path}")
    for item in files:
        data_hash = sha256_file(item)
        digest.update(item.relative_to(path).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(item.stat().st_size).encode("ascii"))
        digest.update(b"\0")
        digest.update(data_hash.encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def detect_pwb_version(edition_root: Path) -> str:
    readme = edition_root / "README.md"
    if not readme.is_file():
        raise RuntimeError(
            f"cannot detect Perfect Works Build version: missing {readme}"
        )
    try:
        text = readme.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError as exc:
        raise RuntimeError(
            f"cannot detect Perfect Works Build version: {readme} is not UTF-8"
        ) from exc
    match = re.search(
        r"(?im)^###\s+Version\s+(\d+\.\d+\.\d+)\s*$",
        text,
    )
    if match is None:
        match = re.search(r"/releases/tag/(\d+\.\d+\.\d+)(?:\s|$)", text)
    if match is None:
        raise RuntimeError(
            "cannot detect Perfect Works Build version from README.md; "
            f"this converter requires {SUPPORTED_PWB_VERSION}"
        )
    return match.group(1)


def require_supported_pwb_version(edition_root: Path) -> str:
    detected = detect_pwb_version(edition_root)
    if detected != SUPPORTED_PWB_VERSION:
        raise RuntimeError(
            f"unsupported Perfect Works Build version {detected}; this converter "
            f"only supports {SUPPORTED_PWB_VERSION}. Use an unmodified "
            f"Xenogears_Perfect_Works_Edition_{SUPPORTED_PWB_VERSION} extraction."
        )
    return detected


def required_edition_paths(
    options: PatchOptions, implicit_patches: bool = True
) -> tuple[set[str], set[str]]:
    directories = {
        f"gamefiles/{relative}"
        for disc in (1, 2)
        for relative in selected_directories(disc, options, implicit_patches)
    }
    files = {"README.md"}
    music_insertion = (
        options.music_changes
        and (options.script or options.text_speed == "fast" or options.arena != "normal")
    )
    if music_insertion:
        directories.update(
            {"gamefiles/music_1", "gamefiles/music_2", "gamefiles/music_subfiles"}
        )
    if options.jpn_controls:
        directories.update({"gamefiles/jpn_ctrl_subfiles", "data/controls"})
    if options.no_battle_flashes or options.no_damage_cap or options.jpn_controls:
        files.add("Tools/xenocomp.exe")
    if options.no_deathblow_levels or options.jpn_controls:
        files.add("Tools/xenopack.exe")
    return directories, files


def authenticate_edition(
    edition_root: Path,
    options: PatchOptions,
    implicit_patches: bool = True,
    digest_cache: dict[tuple[str, str], str] | None = None,
) -> None:
    directories, files = required_edition_paths(options, implicit_patches)
    cache = digest_cache if digest_cache is not None else {}
    for relative in sorted(directories):
        expected = EDITION_DIRECTORY_SHA256.get(relative)
        if expected is None:
            raise RuntimeError(
                f"no {SUPPORTED_PWB_VERSION} authentication record for {relative}"
            )
        key = ("directory", relative)
        actual = cache.get(key)
        if actual is None:
            actual = directory_sha256(edition_root / relative)
            cache[key] = actual
        if actual != expected:
            raise RuntimeError(
                f"Perfect Works {SUPPORTED_PWB_VERSION} directory authentication "
                f"failed: {relative}"
            )
    for relative in sorted(files):
        path = edition_root / relative
        key = ("file", relative)
        actual = cache.get(key)
        if actual is None and path.is_file():
            actual = sha256_file(path)
            cache[key] = actual
        if actual != EDITION_FILE_SHA256[relative]:
            raise RuntimeError(
                f"Perfect Works {SUPPORTED_PWB_VERSION} file authentication failed: "
                f"{relative}"
            )


def validate_sha256(value: str, label: str) -> str:
    if len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        raise ValueError(f"{label} must be a lowercase SHA-256 digest")
    return value


def read_user_sectors(image, lba: int, count: int) -> bytes:
    output = bytearray()
    for sector in range(lba, lba + count):
        image.seek(sector * RAW_SECTOR_SIZE + USER_OFFSET)
        data = image.read(USER_SECTOR_SIZE)
        if len(data) != USER_SECTOR_SIZE:
            raise RuntimeError(f"short read at LBA {sector}")
        output.extend(data)
    return bytes(output)


def read_entries(image_path: Path) -> tuple[list[TableEntry], int]:
    with image_path.open("rb") as image:
        table = read_user_sectors(image, TABLE_LBA, TABLE_SECTORS)
    entries: list[TableEntry] = []
    for offset in range(0, len(table) - 6, 7):
        lba = int.from_bytes(table[offset : offset + 3], "little")
        size = struct.unpack_from("<i", table, offset + 3)[0]
        if lba == SENTINEL_LBA and size == 0:
            if not entries or entries[0].size >= 0:
                raise RuntimeError("invalid Xenogears XA root")
            xa_children = -entries[0].size
            if xa_children >= len(entries):
                raise RuntimeError("Xenogears XA root exceeds the table")
            return entries, xa_children
        if lba == SENTINEL_LBA:
            raise RuntimeError("malformed Xenogears table entry")
        entries.append(TableEntry(lba, size))
    raise RuntimeError("Xenogears table sentinel not found")


def read_entry(image, entry: TableEntry) -> bytes:
    if entry.size <= 0:
        raise RuntimeError("cannot read a non-file Xenogears entry")
    count = (entry.size + USER_SECTOR_SIZE - 1) // USER_SECTOR_SIZE
    return read_user_sectors(image, entry.lba, count)[: entry.size]


def read_iso_files(image_path: Path) -> list[IsoFile]:
    with image_path.open("rb") as image:
        primary = None
        for lba in range(16, 32):
            descriptor = read_user_sectors(image, lba, 1)
            if descriptor[1:6] != b"CD001":
                continue
            if descriptor[0] == 1:
                primary = descriptor
                break
            if descriptor[0] == 255:
                break
        if primary is None:
            raise RuntimeError(f"ISO9660 primary descriptor not found in {image_path}")
        root = primary[156:]
        if not root or root[0] < 34:
            raise RuntimeError(f"ISO9660 root record is invalid in {image_path}")

        files: list[IsoFile] = []
        visited: set[tuple[int, int]] = set()

        def walk(lba: int, size: int, prefix: str) -> None:
            identity = (lba, size)
            if identity in visited:
                return
            visited.add(identity)
            sectors = (size + USER_SECTOR_SIZE - 1) // USER_SECTOR_SIZE
            data = read_user_sectors(image, lba, sectors)[:size]
            offset = 0
            while offset < len(data):
                length = data[offset]
                if length == 0:
                    offset = ((offset // USER_SECTOR_SIZE) + 1) * USER_SECTOR_SIZE
                    continue
                if offset + length > len(data) or length < 34:
                    raise RuntimeError(f"invalid ISO9660 directory record in {image_path}")
                record = data[offset : offset + length]
                extent = int.from_bytes(record[2:6], "little")
                data_size = int.from_bytes(record[10:14], "little")
                flags = record[25]
                name_length = record[32]
                if 33 + name_length > len(record):
                    raise RuntimeError(f"invalid ISO9660 filename record in {image_path}")
                raw_name = record[33 : 33 + name_length]
                offset += length
                if raw_name in (b"\x00", b"\x01"):
                    continue
                name = raw_name.decode("ascii", errors="strict").split(";", 1)[0]
                path = f"{prefix}/{name}" if prefix else f"/{name}"
                if flags & 2:
                    walk(extent, data_size, path)
                else:
                    files.append(IsoFile(path, extent, data_size))

        walk(
            int.from_bytes(root[2:6], "little"),
            int.from_bytes(root[10:14], "little"),
            "",
        )
    return files


def source_index(path: Path) -> int:
    stem = path.name.split(".", 1)[0]
    if not stem.isdigit():
        raise RuntimeError(f"replacement filename has no numeric index: {path}")
    return int(stem)


def table_index(disc: int, listed_index: int) -> int:
    return listed_index - 5 if disc == 2 else listed_index


class StagingArea:
    def __init__(self, disc: int, path: Path):
        self.disc = disc
        self.path = path
        self.files: dict[int, StagedFile] = {}

    def overlay(self, source: Path, label: str) -> None:
        if not source.is_dir():
            raise RuntimeError(f"missing Perfect Works payload directory: {source}")
        children = sorted(path for path in source.iterdir() if path.is_file())
        if not children:
            raise RuntimeError(f"Perfect Works payload directory is empty: {source}")
        seen: set[int] = set()
        for source_path in children:
            listed = source_index(source_path)
            index = table_index(self.disc, listed)
            if index in seen:
                raise RuntimeError(f"duplicate logical index {index} in {source}")
            seen.add(index)
            destination = self.path / source_path.name
            previous = self.files.get(index)
            sources = [] if previous is None else previous.sources.copy()
            if previous is not None and previous.path != destination:
                previous.path.unlink()
            shutil.copyfile(source_path, destination)
            sources.append(label)
            self.files[index] = StagedFile(listed, index, destination, sources)

    def logical(self, listed_index: int) -> StagedFile:
        index = table_index(self.disc, listed_index)
        try:
            return self.files[index]
        except KeyError as exc:
            raise RuntimeError(
                f"Disc {self.disc} selection did not stage required file {listed_index:04d}"
            ) from exc

    def seed(self, listed_index: int, name: str, payload: bytes) -> None:
        index = table_index(self.disc, listed_index)
        if index in self.files:
            return
        destination = self.path / name
        destination.write_bytes(payload)
        self.files[index] = StagedFile(
            listed_index,
            index,
            destination,
            [f"stock-disc{self.disc}"],
        )


class UpstreamToolRunner:
    def __init__(self, edition_root: Path, mode: str):
        self.tools = edition_root / "Tools"
        if mode == "auto":
            mode = "native" if os.name == "nt" else "wine"
        self.mode = mode

    def run(self, executable: str, arguments: Iterable[str], cwd: Path) -> None:
        tool = self.tools / executable
        if not tool.is_file():
            raise RuntimeError(f"missing Perfect Works helper: {tool}")
        command = [str(tool), *arguments]
        env = os.environ.copy()
        if self.mode == "wine":
            if shutil.which("wine") is None:
                raise RuntimeError(
                    f"Wine is required to run Perfect Works helper {executable}"
                )
            command.insert(0, "wine")
            env.setdefault("WINEDEBUG", "-all")
        result = subprocess.run(
            command,
            cwd=cwd,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        accepted_codes = {0, 1} if executable in {"xenocomp.exe", "xenopack.exe"} else {0}
        if result.returncode not in accepted_codes:
            detail = result.stdout.strip()
            raise RuntimeError(
                f"{executable} failed with exit code {result.returncode}"
                + (f": {detail}" if detail else "")
            )


def validate_options(options: PatchOptions) -> None:
    if options.exp_factor not in {"off", "1", "1.5", "2"}:
        raise ValueError("invalid EXP factor")
    if options.gold_factor not in {"off", "1", "1.5", "2"}:
        raise ValueError("invalid gold factor")
    if options.arena not in {"normal", "basic", "expert"}:
        raise ValueError("invalid arena mode")
    if options.face_fix not in {"none", "normal", "resize"}:
        raise ValueError("invalid face-fix mode")
    if options.text_speed not in {"normal", "fast", "instant"}:
        raise ValueError("invalid text speed")
    if options.story_mode:
        gameplay = (
            options.half_encounters
            or options.exp_factor != "off"
            or options.gold_factor != "off"
            or options.rebalanced_items
            or options.rebalanced_enemies
            or options.no_deathblow_levels
            or options.no_damage_cap
            or options.arena != "normal"
        )
        if gameplay:
            raise ValueError("story mode is incompatible with gameplay and arena patches")


def selected_directories(
    disc: int, options: PatchOptions, implicit_patches: bool = True
) -> list[str]:
    script = options.script
    items = options.rebalanced_items
    selected: list[str] = []

    if options.half_encounters:
        selected.append(
            ("encounterone_script" if disc == 1 else "encountertwo_script")
            if script
            else ("encounter_rate_1" if disc == 1 else "encounter_rate_2")
        )
    if items:
        selected.append(
            ("Script_items" if disc == 1 else "Script_items2")
            if script
            else ("items1" if disc == 1 else "items2")
        )
    if options.jpn_controls:
        if script:
            selected.append("jpn_script_1" if disc == 1 else "jpn_script_2")
        elif items:
            selected.append("jpn_items_1" if disc == 1 else "jpn_items_2")
        else:
            selected.append("control_files_1" if disc == 1 else "control_files_2")
    if script and not items:
        selected.append("script1" if disc == 1 else "script2")

    if options.story_mode:
        if script:
            selected.append(
                "storyfiles_script_cd1" if disc == 1 else "storyfiles_script_cd2"
            )
        else:
            selected.append("storyfiles_cd1" if disc == 1 else "storyfiles_cd2")
    if options.bug_fixes or (implicit_patches and not items and not script):
        selected.append("bug_fix1" if disc == 1 else "bug_fix2")
    if options.face_fix == "resize":
        selected.append("resized_portraits")
    if options.face_fix == "normal":
        selected.append("portraits")
    if options.exp_factor != "off":
        selected.append("og_monsters")
    if options.gold_factor != "off":
        selected.append("og_monsters")
    if options.rebalanced_enemies:
        if script and items:
            selected.append("monsters_both")
        elif script:
            selected.append("monsters_script")
        elif items:
            selected.append("monsters_items")
        else:
            selected.append("Monsters")
    if options.music_changes and not script:
        selected.append("music_1" if disc == 1 else "music_2")
    if options.arena == "basic":
        selected.append("filesbasic_script" if script else "filesbasic")
    if options.arena == "expert":
        selected.append("filesexpert_script" if script else "filesexpert")
    if options.text_speed == "fast":
        if disc == 1:
            selected.append("text_cd1" if script else "text_old1")
        else:
            selected.append("text_cd2")
    elif options.text_speed == "instant" and script:
        selected.append("text_cd1" if disc == 1 else "text_cd2")
    if options.battle_undub:
        selected.append("voice")
    if options.title_screen or implicit_patches:
        selected.append("title_screen")
    if options.pw_roni:
        selected.append("roni_pw/resized" if options.face_fix == "resize" else "roni_pw/default")
    if options.emeralda_cafe_fix and disc == 1:
        selected.append("emeralda_fix/new_script" if script else "emeralda_fix/og_script")
    if options.no_deathblow_levels:
        selected.append("exp_data")
    return selected


def seed_stock_dependencies(
    stage: StagingArea,
    image_path: Path,
    entries: list[TableEntry],
    xa_children: int,
    options: PatchOptions,
) -> None:
    required: dict[int, str] = {}
    if options.no_battle_flashes or options.no_damage_cap:
        required[38] = "0038"
    if options.jpn_controls:
        required.update(
            {
                38: "0038",
                2593: "2593.unk8",
                3958: "3958.unk8",
                2614: "2614",
            }
        )
    if not required:
        return
    with image_path.open("rb") as image:
        for listed_index, name in required.items():
            index = table_index(stage.disc, listed_index)
            if index in stage.files:
                continue
            if index <= xa_children or index >= len(entries) or entries[index].size <= 0:
                raise RuntimeError(
                    f"Disc {stage.disc} stock dependency {listed_index:04d} "
                    "is not a replaceable ordinary file"
                )
            stage.seed(listed_index, name, read_entry(image, entries[index]))


def write_at(data: bytearray, offset: int, replacement: bytes, label: str) -> None:
    end = offset + len(replacement)
    if offset < 0 or end > len(data):
        raise RuntimeError(f"{label} write is outside its target")
    data[offset:end] = replacement


def scaled(value: int, factor: str) -> int:
    if factor == "1":
        return value
    if factor == "1.5":
        return value * 3 // 2
    if factor == "2":
        return value * 2
    raise ValueError(f"cannot scale by {factor}")


def apply_exp_gold(stage: StagingArea, options: PatchOptions) -> None:
    if options.exp_factor == "off" and options.gold_factor == "off":
        return
    for staged in stage.files.values():
        if not 2618 <= staged.listed_index <= 2768:
            continue
        data = bytearray(staged.path.read_bytes())
        if len(data) < 2:
            raise RuntimeError(f"monster file is truncated: {staged.path}")
        data_length = int.from_bytes(data[0:2], "little")
        for record in range(126, data_length, 368):
            if record + 0x10C > len(data):
                raise RuntimeError(f"monster record is truncated: {staged.path}")
            if options.exp_factor != "off":
                offset = record + 0x100
                # This deliberately reproduces the effective 0.11.2 MSVC build:
                # a four-byte read into a two-byte wchar_t, then a four-byte write.
                value = int.from_bytes(data[offset : offset + 4], "little") & 0xFFFF
                write_at(
                    data,
                    offset,
                    scaled(value, options.exp_factor).to_bytes(4, "little"),
                    "EXP",
                )
            if options.gold_factor != "off":
                offset = record + 0x10A
                value = int.from_bytes(data[offset : offset + 2], "little")
                # The patcher writes only the low two bytes of its uint64_t result.
                result = scaled(value, options.gold_factor) & 0xFFFF
                write_at(data, offset, result.to_bytes(2, "little"), "gold")
        staged.path.write_bytes(data)
        staged.transforms.append(
            f"exp={options.exp_factor},gold={options.gold_factor}"
        )


def clean_unpack_files(directory: Path) -> None:
    for path in directory.glob("file*"):
        if path.is_file():
            path.unlink()


def unpack_edit_repack(
    staged: StagedFile,
    runner: UpstreamToolRunner,
    editor,
    transform: str,
) -> None:
    clean_unpack_files(staged.path.parent)
    runner.run("xenopack.exe", ["-u", staged.path.name], staged.path.parent)
    editor(staged.path.parent)
    runner.run("xenopack.exe", ["-p", staged.path.name], staged.path.parent)
    if not staged.path.is_file() or staged.path.stat().st_size == 0:
        raise RuntimeError(f"xenopack did not rebuild {staged.path.name}")
    clean_unpack_files(staged.path.parent)
    staged.transforms.append(transform)


def apply_deathblow_levels(stage: StagingArea, runner: UpstreamToolRunner) -> None:
    staged = stage.logical(2607)

    def edit(directory: Path) -> None:
        path = directory / "file0"
        if not path.is_file():
            raise RuntimeError("xenopack did not extract Deathblow file0")
        data = bytearray(path.read_bytes())
        for start in (0x100, 0x210, 0x320, 0x430, 0x540, 0x650, 0x760, 0xA90, 0xBA0):
            if start >= len(data):
                raise RuntimeError("Deathblow table offset is outside file0")
            position = start
            data[position] = 1
            position += 1
            while position < len(data) and data[position] not in (0, 0xFF):
                data[position] = 1
                position += 1
            if position >= len(data):
                raise RuntimeError("unterminated Deathblow level table")
        path.write_bytes(data)

    unpack_edit_repack(staged, runner, edit, "no-deathblow-levels")


def read_csv_edits(path: Path) -> list[tuple[int, int]]:
    if not path.is_file():
        raise RuntimeError(f"missing Perfect Works edit table: {path}")
    edits: list[tuple[int, int]] = []
    with path.open("r", encoding="ascii", newline="") as stream:
        for row in csv.reader(stream):
            if len(row) != 2:
                raise RuntimeError(f"invalid Perfect Works edit row in {path}")
            edits.append((int(row[0], 10), int(row[1], 0)))
    return edits


def apply_battle_edits(
    stage: StagingArea,
    edition_root: Path,
    runner: UpstreamToolRunner,
    options: PatchOptions,
) -> None:
    if not (options.no_battle_flashes or options.no_damage_cap or options.jpn_controls):
        return
    staged = stage.logical(38)

    def round_trip(editor, transform: str) -> None:
        decompressed = staged.path.with_name(staged.path.name + ".dec")
        decompressed.unlink(missing_ok=True)
        runner.run(
            "xenocomp.exe",
            ["-d", staged.path.name, decompressed.name],
            staged.path.parent,
        )
        if not decompressed.is_file():
            raise RuntimeError(
                "xenocomp did not produce the decompressed battle executable"
            )
        data = bytearray(decompressed.read_bytes())
        editor(data)
        decompressed.write_bytes(data)
        staged.path.unlink()
        runner.run(
            "xenocomp.exe",
            ["-c", decompressed.name, staged.path.name],
            staged.path.parent,
        )
        decompressed.unlink(missing_ok=True)
        if not staged.path.is_file() or staged.path.stat().st_size == 0:
            raise RuntimeError("xenocomp did not rebuild the battle executable")
        staged.transforms.append(transform)

    if options.no_battle_flashes:
        round_trip(
            lambda data: write_at(data, 334032, b"\x00\x00", "battle flash"),
            "no-battle-flashes",
        )
    if options.no_damage_cap:
        def remove_damage_cap(data: bytearray) -> None:
            write_at(data, 154460, b"\x00" * 4, "character damage cap")
            write_at(data, 186400, b"\x00" * 4, "Gear damage cap")

        round_trip(remove_damage_cap, "no-damage-cap")
    if options.jpn_controls:
        edits = read_csv_edits(edition_root / "data/controls/0038.csv")

        def apply_controls(data: bytearray) -> None:
            for offset, value in edits:
                write_at(data, offset, bytes([value]), "JPN battle controls")

        round_trip(apply_controls, "jpn-battle-controls")


def apply_music(stage: StagingArea, edition_root: Path, options: PatchOptions) -> None:
    if not options.music_changes:
        return
    needs_insertion = (
        options.script or options.text_speed == "fast" or options.arena != "normal"
    )
    if not needs_insertion:
        return
    targets = edition_root / "gamefiles" / ("music_1" if stage.disc == 1 else "music_2")
    snippets = edition_root / "gamefiles/music_subfiles"
    if not targets.is_dir() or not snippets.is_dir():
        raise RuntimeError("missing Perfect Works music composition inputs")
    for target in sorted(path for path in targets.iterdir() if path.is_file()):
        listed = source_index(target)
        staged = stage.logical(listed)
        snippet = snippets / f"{listed:04d}_5"
        if not snippet.is_file():
            raise RuntimeError(f"missing music snippet: {snippet}")
        data = bytearray(staged.path.read_bytes())
        if len(data) < 332:
            raise RuntimeError(f"music target is truncated: {staged.path}")
        script_offset = int.from_bytes(data[324:328], "little")
        sprite_offset = int.from_bytes(data[328:332], "little")
        if script_offset > sprite_offset or sprite_offset > len(data):
            raise RuntimeError(f"invalid music region in {staged.path}")
        amount = sprite_offset - script_offset
        replacement = snippet.read_bytes()[:amount].ljust(amount, b"\x00")
        data[script_offset:sprite_offset] = replacement
        staged.path.write_bytes(data)
        staged.transforms.append("music-placement")


def apply_jpn_control_images(
    stage: StagingArea, edition_root: Path, runner: UpstreamToolRunner
) -> None:
    common = edition_root / "gamefiles/jpn_ctrl_subfiles"
    for listed in (2593, 3958):
        staged = stage.logical(listed)

        def edit_image(directory: Path, source=common / "2593_3958/file1") -> None:
            if not source.is_file() or not (directory / "file1").is_file():
                raise RuntimeError("missing JPN control image input")
            shutil.copyfile(source, directory / "file1")

        unpack_edit_repack(staged, runner, edit_image, "jpn-control-image")

    staged = stage.logical(2614)

    def edit_battle_images(directory: Path) -> None:
        for name in ("file0", "file1"):
            source = common / "2614" / name
            if not source.is_file() or not (directory / name).is_file():
                raise RuntimeError("missing JPN battle control image input")
            shutil.copyfile(source, directory / name)

    unpack_edit_repack(staged, runner, edit_battle_images, "jpn-battle-images")


def exe_offset_to_address(executable: bytes, offset: int) -> int:
    if executable[:8] != b"PS-X EXE" or len(executable) < PSX_EXE_HEADER_SIZE:
        raise RuntimeError("stock executable has an invalid PS-X EXE header")
    load_address = int.from_bytes(executable[0x18:0x1C], "little")
    text_size = int.from_bytes(executable[0x1C:0x20], "little")
    if offset < PSX_EXE_HEADER_SIZE or offset >= PSX_EXE_HEADER_SIZE + text_size:
        raise RuntimeError("Perfect Works executable edit is outside loaded text")
    return load_address + offset - PSX_EXE_HEADER_SIZE


def build_exe_patches(executables: dict[int, bytes], options: PatchOptions) -> list[ExePatch]:
    if options.text_speed == "normal":
        return []
    speed = 0x05 if options.text_speed == "fast" else 0xFF
    specifications = ((151908, speed.to_bytes(2, "little")), (151911, b"\x34\x00"))
    patches: list[ExePatch] = []
    for offset, replacement in specifications:
        expected_values = {data[offset : offset + len(replacement)] for data in executables.values()}
        if len(expected_values) != 1:
            raise RuntimeError(
                f"stock executables disagree at Perfect Works text offset {offset}"
            )
        expected = expected_values.pop()
        if len(expected) != len(replacement):
            raise RuntimeError("stock executable is truncated at a text-speed edit")
        addresses = {exe_offset_to_address(data, offset) for data in executables.values()}
        if len(addresses) != 1:
            raise RuntimeError("stock executables disagree on the text load address")
        patches.append(
            ExePatch(addresses.pop(), expected, replacement, f"text-speed@{offset}")
        )
    return patches


def mutate_stage(
    stage: StagingArea,
    edition_root: Path,
    runner: UpstreamToolRunner,
    options: PatchOptions,
) -> None:
    apply_battle_edits(stage, edition_root, runner, options)
    apply_exp_gold(stage, options)
    if options.no_deathblow_levels:
        apply_deathblow_levels(stage, runner)
    apply_music(stage, edition_root, options)
    if options.jpn_controls:
        apply_jpn_control_images(stage, edition_root, runner)


def collect_operations(
    disc: int,
    image_path: Path,
    entries: list[TableEntry],
    xa_children: int,
    stage: StagingArea,
    package_root: Path,
    iso_files: list[IsoFile],
) -> tuple[list[IndexedOperation], dict[str, int]]:
    operations: list[IndexedOperation] = []
    unchanged = 0
    payload_bytes = 0
    virtual_sectors = 0
    with image_path.open("rb") as image:
        for index, staged in sorted(stage.files.items()):
            if index <= xa_children or index >= len(entries) or entries[index].size <= 0:
                raise RuntimeError(
                    f"Disc {disc} index {index} is not a replaceable ordinary file"
                )
            indexed_begin = entries[index].lba
            indexed_end = indexed_begin + (
                entries[index].size + USER_SECTOR_SIZE - 1
            ) // USER_SECTOR_SIZE
            visible = next(
                (
                    item
                    for item in iso_files
                    if indexed_begin
                    < item.lba + (item.size + USER_SECTOR_SIZE - 1) // USER_SECTOR_SIZE
                    and item.lba < indexed_end
                ),
                None,
            )
            if visible is not None:
                raise RuntimeError(
                    f"Disc {disc} index {index} overlaps ISO-visible file {visible.path}"
                )
            stock = read_entry(image, entries[index])
            payload = staged.path.read_bytes()
            if not payload:
                raise RuntimeError(f"empty replacement: {staged.path}")
            if payload == stock:
                unchanged += 1
                continue
            relative = Path("assets") / f"disc{disc}" / f"{index:04d}.bin"
            destination = package_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(staged.path, destination)
            operations.append(
                IndexedOperation(
                    disc,
                    index,
                    staged.listed_index,
                    relative,
                    sha256_bytes(payload),
                    sha256_bytes(stock),
                    len(payload),
                    tuple(staged.sources),
                    tuple(staged.transforms),
                )
            )
            payload_bytes += len(payload)
            virtual_sectors += (len(payload) + USER_SECTOR_SIZE - 1) // USER_SECTOR_SIZE
    if payload_bytes > MAX_ACTIVE_PAYLOAD:
        raise RuntimeError(f"Disc {disc} active payload exceeds 128 MiB")
    if virtual_sectors > MAX_VIRTUAL_SECTORS:
        raise RuntimeError(f"Disc {disc} virtual extension exceeds 64K sectors")
    return operations, {
        "staged": len(stage.files),
        "changed": len(operations),
        "unchanged": unchanged,
        "payload_bytes": payload_bytes,
        "virtual_sectors": virtual_sectors,
        "table_entries": len(entries),
        "xa_children": xa_children,
    }


def toml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def bytes_text(value: bytes) -> str:
    return " ".join(f"{byte:02x}" for byte in value)


def toml_condition(values: tuple[tuple[str, str], ...]) -> str:
    for key, _value in values:
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", key):
            raise ValueError(f"invalid option id in condition: {key}")
    return "{ " + ", ".join(
        f"{key} = {toml_string(value)}" for key, value in values
    ) + " }"


def package_id_for(profile_key: str) -> str:
    if profile_key == "retranslation":
        return "org.perfectworksbuild.retranslation"
    return "org.perfectworksbuild.indexed." + profile_key.replace("_", "-")


def make_manifest(
    package_id: str,
    package_name: str,
    description: str,
    disc_hashes: dict[int, str],
    operations: list[IndexedOperation],
    exe_patches: list[ExePatch],
    isolated_saves: bool,
    choice_option: ChoiceOption | None = None,
) -> str:
    lines = [
        "format_version = 6",
        f"id = {toml_string(package_id)}",
        f"version = {toml_string(SUPPORTED_PWB_VERSION)}",
        f"name = {toml_string(package_name)}",
        'author = "Perfect Works Build Team"',
        f"description = {toml_string(description)}",
        'license = "Upstream terms apply"',
        f"source_name = {toml_string(f'Perfect Works Build {SUPPORTED_PWB_VERSION}')}",
        f"source_url = {toml_string('https://github.com/PWBuild-Team/Perfect_Works_Build/releases/tag/' + SUPPORTED_PWB_VERSION)}",
        'resolver = "declarative"',
        f'save_compatibility = {toml_string("isolated" if isolated_saves else "shared")}',
        "",
        "[[author_link]]",
        'name = "Perfect Works Build Team"',
        'url = "https://github.com/PWBuild-Team/Perfect_Works_Build"',
        "",
    ]
    for disc in (1, 2):
        lines.extend(
            [
                "[[target]]",
                'game_id = "SLUS-00664"',
                f"disc_sha256 = {toml_string(disc_hashes[disc])}",
                "",
            ]
        )
    lines.extend(
        [
            "[[feature]]",
            'id = "perfect-works"',
            f"name = {toml_string(package_name)}",
            f"description = {toml_string(description)}",
            'group = "Perfect Works Build"',
            "default_enabled = false",
            "",
        ]
    )
    if choice_option is not None:
        lines.extend(
            [
                "[[option]]",
                'feature = "perfect-works"',
                f"id = {toml_string(choice_option.id)}",
                f"label = {toml_string(choice_option.label)}",
                'type = "choice"',
                f"default = {toml_string(choice_option.default)}",
                "",
            ]
        )
        for value, label in choice_option.choices:
            lines.extend(
                [
                    "[[option.choice]]",
                    f"value = {toml_string(value)}",
                    f"label = {toml_string(label)}",
                    "",
                ]
            )
    for patch in exe_patches:
        lines.extend(
            [
                "[[patch]]",
                'feature = "perfect-works"',
                'target = "main_exe"',
                f"address = 0x{patch.address:08x}",
                f"expected = {toml_string(bytes_text(patch.expected))}",
                f"replace = {toml_string(bytes_text(patch.replacement))}",
            ]
        )
        if patch.when:
            lines.append(f"when = {toml_condition(patch.when)}")
        lines.append("")
    for operation in sorted(
        operations, key=lambda item: (item.disc, item.index, item.when)
    ):
        lines.extend(
            [
                "[[indexed_file]]",
                'feature = "perfect-works"',
                'format = "xenogears"',
                f"index = {operation.index}",
                f"disc_sha256 = {toml_string(disc_hashes[operation.disc])}",
                f"file = {toml_string(operation.payload_path.as_posix())}",
                f"sha256 = {toml_string(operation.payload_sha256)}",
                f"expected_sha256 = {toml_string(operation.expected_sha256)}",
            ]
        )
        if operation.when:
            lines.append(f"when = {toml_condition(operation.when)}")
        lines.append("")
    return "\n".join(lines)


def verify_oracle(
    disc: int,
    oracle_path: Path,
    stage: StagingArea,
) -> dict[str, int]:
    entries, _ = read_entries(oracle_path)
    mismatches: list[int] = []
    verified = 0
    with oracle_path.open("rb") as image:
        for index, staged in sorted(stage.files.items()):
            if index >= len(entries) or entries[index].size <= 0:
                mismatches.append(index)
                continue
            if staged.path.read_bytes() != read_entry(image, entries[index]):
                mismatches.append(index)
            else:
                verified += 1
    if mismatches:
        preview = ", ".join(str(index) for index in mismatches[:10])
        raise RuntimeError(
            f"Disc {disc} differs from oracle at {len(mismatches)} staged indices: {preview}"
        )
    return {"verified": verified, "mismatched": 0}


def is_gameplay_profile(options: PatchOptions) -> bool:
    return any(
        (
            options.half_encounters,
            options.exp_factor != "off",
            options.gold_factor != "off",
            options.rebalanced_items,
            options.rebalanced_enemies,
            options.no_deathblow_levels,
            options.no_damage_cap,
            options.arena != "normal",
            options.story_mode,
        )
    )


def coverage_notes(options: PatchOptions) -> list[str]:
    notes: list[str] = []
    if options.fmv_undub:
        notes.append(
            "FMV undub omitted: it applies raw-disc xdelta changes, expands the stream table, "
            "and replaces the ISO-visible SLUS; indexed-file plans cannot express that path."
        )
    if options.jpn_controls:
        notes.append(
            "JPN executable edits omitted: the two stock SLUS files require different guards, "
            "while main_exe patches have no per-disc condition. Field images and battle controls "
            "are included."
        )
    return notes


def make_report(
    profile_key: str,
    options: PatchOptions,
    disc_hashes: dict[int, str],
    raw_hashes: dict[int, str],
    stats: dict[int, dict[str, int]],
    notes: list[str],
    oracle_stats: dict[int, dict[str, int]],
    implicit_patches: bool,
) -> str:
    lines = [
        f"Perfect Works Build {SUPPORTED_PWB_VERSION} indexed-file port",
        "",
        f"Profile: {profile_key}",
        (
            f"Composition semantics: released {SUPPORTED_PWB_VERSION} patcher order"
            if implicit_patches
            else "Composition semantics: isolated patch only"
        ),
        "Source discs were read only and were not modified.",
        "",
        "Selected options:",
    ]
    for key, value in asdict(options).items():
        lines.append(f"- {key}: {str(value).lower() if isinstance(value, bool) else value}")
    lines.extend(["", "Coverage:"])
    if notes:
        lines.extend(f"- PARTIAL: {note}" for note in notes)
    else:
        lines.append("- All selected Perfect Works operations are represented.")
    for disc in (1, 2):
        disc_stats = stats[disc]
        lines.extend(
            [
                "",
                f"Disc {disc} raw BIN SHA-256: {raw_hashes[disc]}",
                f"Disc {disc} canonical runtime SHA-256: {disc_hashes[disc]}",
                f"Disc {disc} staged files: {disc_stats['staged']}",
                f"Disc {disc} changed indexed files: {disc_stats['changed']}",
                f"Disc {disc} stock-identical files omitted: {disc_stats['unchanged']}",
                f"Disc {disc} active payload bytes: {disc_stats['payload_bytes']}",
                f"Disc {disc} virtual sectors: {disc_stats['virtual_sectors']}",
                f"Disc {disc} table entries before sentinel: {disc_stats['table_entries']}",
                f"Disc {disc} XA root children: {disc_stats['xa_children']}",
            ]
        )
        if disc in oracle_stats:
            lines.append(
                f"Disc {disc} staged files matched against oracle: "
                f"{oracle_stats[disc]['verified']}"
            )
    return "\n".join(lines) + "\n"


def make_index_report(operations: list[IndexedOperation]) -> str:
    lines = [
        "disc\tlisted_index\ttable_index\tpayload_bytes\tpayload_sha256\t"
        "expected_sha256\tsources\ttransforms"
    ]
    for item in sorted(operations, key=lambda operation: (operation.disc, operation.index)):
        lines.append(
            "\t".join(
                (
                    str(item.disc),
                    str(item.listed_index),
                    str(item.index),
                    str(item.payload_size),
                    item.payload_sha256,
                    item.expected_sha256,
                    ",".join(item.sources),
                    ",".join(item.transforms),
                )
            )
        )
    return "\n".join(lines) + "\n"


def pack_archive(source: Path, output: Path) -> None:
    files = sorted(path for path in source.rglob("*") if path.is_file())
    if len(files) > MAX_ARCHIVE_FILES:
        raise RuntimeError("package exceeds the 4096-file archive limit")
    expanded = sum(path.stat().st_size for path in files)
    if expanded > MAX_EXPANDED_ARCHIVE:
        raise RuntimeError("package exceeds the 256 MiB expanded archive limit")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in files:
                info = zipfile.ZipInfo(path.relative_to(source).as_posix())
                info.date_time = (1980, 1, 1, 0, 0, 0)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, path.read_bytes(), compresslevel=9)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def ensure_safe_outputs(
    edition_root: Path,
    disc_paths: dict[int, Path],
    output: Path,
    source_output: Path | None,
    oracle_paths: dict[int, Path] | None,
) -> None:
    edition_root = edition_root.resolve()
    output = output.resolve()
    source_output = source_output.resolve() if source_output is not None else None
    temporary_output = output.with_name(output.name + ".tmp")
    protected = {path.resolve() for path in disc_paths.values()}
    if oracle_paths:
        protected.update(path.resolve() for path in oracle_paths.values())
    candidates = {output, temporary_output}
    if source_output is not None:
        if source_output.exists():
            raise RuntimeError(f"source output already exists: {source_output}")
        if source_output == temporary_output:
            raise RuntimeError("--source-output collides with the archive temporary path")
        candidates.add(source_output)
        if output.is_relative_to(source_output):
            raise RuntimeError("package output must not be inside --source-output")
    for candidate in candidates:
        if candidate in protected:
            raise RuntimeError(f"output path aliases a read-only input: {candidate}")
        if candidate.is_relative_to(edition_root):
            raise RuntimeError(f"output path must not be inside the edition tree: {candidate}")


def write_utf8(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8"))


def prepare_stock_inputs(
    disc_paths: dict[int, Path],
    expected_bin_hashes: dict[int, str],
    progress: Callable[[str], None] | None = None,
) -> PreparedInputs:
    notify = progress or (lambda _message: None)
    raw_hashes: dict[int, str] = {}
    tables: dict[int, tuple[list[TableEntry], int]] = {}
    iso_files: dict[int, list[IsoFile]] = {}
    executables: dict[int, bytes] = {}
    for disc in (1, 2):
        notify(f"Validating stock Disc {disc}...")
        path = disc_paths[disc]
        if not path.is_file():
            raise RuntimeError(f"Disc {disc} BIN does not exist: {path}")
        raw_hashes[disc] = sha256_file(path)
        if raw_hashes[disc] != expected_bin_hashes[disc]:
            raise RuntimeError(
                f"Disc {disc} BIN SHA-256 mismatch: expected {expected_bin_hashes[disc]}, "
                f"got {raw_hashes[disc]}"
            )
        tables[disc] = read_entries(path)
        iso_files[disc] = read_iso_files(path)
        entries, _ = tables[disc]
        with path.open("rb") as image:
            executables[disc] = read_entry(
                image, entries[DISC_EXECUTABLE_INDEX[disc]]
            )
    return PreparedInputs(raw_hashes, tables, iso_files, executables)


def build_package(
    edition_root: Path,
    disc_paths: dict[int, Path],
    disc_hashes: dict[int, str],
    expected_bin_hashes: dict[int, str],
    output: Path,
    profile_key: str,
    profile: Profile,
    package_id: str,
    package_name: str,
    tool_mode: str,
    oracle_paths: dict[int, Path] | None = None,
    source_output: Path | None = None,
    progress: Callable[[str], None] | None = None,
    verify_edition: bool = True,
    implicit_patches: bool | None = None,
    prepared_inputs: PreparedInputs | None = None,
    authentication_cache: dict[tuple[str, str], str] | None = None,
    pack_output: bool = True,
) -> str:
    notify = progress or (lambda _message: None)
    options = profile.options
    if implicit_patches is None:
        implicit_patches = profile.implicit_patches
    validate_options(options)
    require_supported_pwb_version(edition_root)
    gamefiles = edition_root / "gamefiles"
    if not gamefiles.is_dir():
        raise RuntimeError(f"edition root has no gamefiles directory: {edition_root}")
    ensure_safe_outputs(
        edition_root, disc_paths, output, source_output, oracle_paths
    )
    if verify_edition:
        notify(
            f"Authenticating Perfect Works Build {SUPPORTED_PWB_VERSION} inputs..."
        )
        authenticate_edition(
            edition_root,
            options,
            implicit_patches,
            authentication_cache,
        )

    prepared = prepared_inputs or prepare_stock_inputs(
        disc_paths, expected_bin_hashes, notify
    )
    raw_hashes = prepared.raw_hashes
    tables = prepared.tables
    iso_files = prepared.iso_files
    executables = prepared.executables

    runner = UpstreamToolRunner(edition_root, tool_mode)
    notes = coverage_notes(options)
    suffix = " (FMV excluded)" if options.fmv_undub else ""
    final_name = package_name + suffix
    final_description = profile.description
    if notes:
        final_description += " See PORTING_REPORT.txt for omitted operations."

    with tempfile.TemporaryDirectory(prefix="pw-psxmod-") as temporary:
        temporary_root = Path(temporary)
        package_root = temporary_root / "package"
        package_root.mkdir()
        stages: dict[int, StagingArea] = {}
        all_operations: list[IndexedOperation] = []
        stats: dict[int, dict[str, int]] = {}
        oracle_stats: dict[int, dict[str, int]] = {}
        for disc in (1, 2):
            notify(f"Composing Perfect Works files for Disc {disc}...")
            stage_root = temporary_root / f"stage-disc{disc}"
            stage_root.mkdir()
            stage = StagingArea(disc, stage_root)
            for relative in selected_directories(disc, options, implicit_patches):
                stage.overlay(gamefiles / relative, relative)
            entries, xa_children = tables[disc]
            seed_stock_dependencies(
                stage,
                disc_paths[disc],
                entries,
                xa_children,
                options,
            )
            mutate_stage(stage, edition_root, runner, options)
            stages[disc] = stage
            operations, disc_stats = collect_operations(
                disc,
                disc_paths[disc],
                entries,
                xa_children,
                stage,
                package_root,
                iso_files[disc],
            )
            all_operations.extend(operations)
            stats[disc] = disc_stats
            if oracle_paths and disc in oracle_paths:
                notify(f"Checking Disc {disc} against the supplied oracle...")
                oracle_stats[disc] = verify_oracle(disc, oracle_paths[disc], stage)

        exe_patches = build_exe_patches(executables, options)
        manifest = make_manifest(
            package_id,
            final_name,
            final_description,
            disc_hashes,
            all_operations,
            exe_patches,
            is_gameplay_profile(options),
        )
        try:
            tomllib.loads(manifest)
        except tomllib.TOMLDecodeError as exc:
            raise RuntimeError(f"generated manifest is invalid TOML: {exc}") from exc
        report = make_report(
            profile_key,
            options,
            disc_hashes,
            raw_hashes,
            stats,
            notes,
            oracle_stats,
            implicit_patches,
        )
        write_utf8(package_root / "manifest.toml", manifest)
        write_utf8(package_root / "PORTING_REPORT.txt", report)
        write_utf8(package_root / "PORTING_INDEX.tsv", make_index_report(all_operations))
        if source_output is not None:
            if source_output.exists():
                raise RuntimeError(f"source output already exists: {source_output}")
            shutil.copytree(package_root, source_output)
        if pack_output:
            notify("Packing deterministic psxmod archive...")
            pack_archive(package_root, output)
    return report


def individual_choices(individual: IndividualMod) -> tuple[IndividualChoice, ...]:
    if individual.options is not None:
        if individual.choices or individual.option_id or individual.option_label:
            raise RuntimeError(f"invalid mixed individual mod definition: {individual.key}")
        return (IndividualChoice("", "", individual.options),)
    if not individual.choices or not individual.option_id or not individual.option_label:
        raise RuntimeError(f"incomplete selector definition: {individual.key}")
    values = [choice.value for choice in individual.choices]
    if len(values) != len(set(values)):
        raise RuntimeError(f"duplicate selector value in {individual.key}")
    return individual.choices


def build_selector_package(
    individual: IndividualMod,
    edition_root: Path,
    disc_paths: dict[int, Path],
    disc_hashes: dict[int, str],
    expected_bin_hashes: dict[int, str],
    output: Path,
    tool_mode: str,
    prepared_inputs: PreparedInputs,
    authentication_cache: dict[tuple[str, str], str],
) -> list[str]:
    choices = individual_choices(individual)
    if len(choices) < 2 or individual.option_id is None or individual.option_label is None:
        raise RuntimeError(f"{individual.key} is not a selector mod")
    reverse_disc_hashes = {digest: disc for disc, digest in disc_hashes.items()}
    combined_operations: list[IndexedOperation] = []
    combined_patches: list[ExePatch] = []
    reports: list[str] = []
    index_lines: list[str] = []
    notes: list[str] = []
    isolated_saves = False

    with tempfile.TemporaryDirectory(
        prefix=f"pw-selector-{individual.key}-", dir=output.parent
    ) as temporary:
        temporary_root = Path(temporary)
        package_root = temporary_root / "package"
        package_root.mkdir()
        for choice in choices:
            condition = ((individual.option_id, choice.value),)
            source_root = temporary_root / f"source-{choice.value}"
            variant_profile = Profile(
                f"{individual.name} - {choice.label}",
                individual.description,
                choice.options,
                False,
            )
            report = build_package(
                edition_root,
                disc_paths,
                disc_hashes,
                expected_bin_hashes,
                temporary_root / f"unused-{choice.value}.psxmod",
                f"{individual.key}-{choice.value}",
                variant_profile,
                f"org.perfectworksbuild.variant.{individual.key}.{choice.value}",
                variant_profile.name,
                tool_mode,
                source_output=source_root,
                progress=None,
                implicit_patches=False,
                prepared_inputs=prepared_inputs,
                authentication_cache=authentication_cache,
                pack_output=False,
            )
            reports.append(f"=== {choice.label} ===\n{report}")
            notes.extend(coverage_notes(choice.options))
            isolated_saves = isolated_saves or is_gameplay_profile(choice.options)

            parsed = tomllib.loads(
                (source_root / "manifest.toml").read_text(encoding="utf-8")
            )
            for patch in parsed.get("patch", []):
                combined_patches.append(
                    ExePatch(
                        patch["address"],
                        bytes.fromhex(patch["expected"]),
                        bytes.fromhex(patch["replace"]),
                        f"selector:{choice.value}",
                        condition,
                    )
                )
            for indexed in parsed.get("indexed_file", []):
                disc = reverse_disc_hashes[indexed["disc_sha256"]]
                source = source_root / indexed["file"]
                relative = (
                    Path("assets")
                    / choice.value
                    / f"disc{disc}"
                    / f"{indexed['index']:04d}.bin"
                )
                destination = package_root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
                combined_operations.append(
                    IndexedOperation(
                        disc,
                        indexed["index"],
                        indexed["index"],
                        relative,
                        indexed["sha256"],
                        indexed["expected_sha256"],
                        source.stat().st_size,
                        (f"selector:{choice.value}",),
                        (),
                        condition,
                    )
                )
            variant_index = (source_root / "PORTING_INDEX.tsv").read_text(
                encoding="utf-8"
            ).splitlines()
            if not index_lines:
                index_lines.append("choice\t" + variant_index[0])
            index_lines.extend(
                f"{choice.value}\t{line}" for line in variant_index[1:]
            )

        choice_option = ChoiceOption(
            individual.option_id,
            individual.option_label,
            choices[0].value,
            tuple((choice.value, choice.label) for choice in choices),
        )
        manifest = make_manifest(
            f"org.perfectworksbuild.individual.{individual.key}",
            individual.name,
            individual.description,
            disc_hashes,
            combined_operations,
            combined_patches,
            isolated_saves,
            choice_option,
        )
        try:
            tomllib.loads(manifest)
        except tomllib.TOMLDecodeError as exc:
            raise RuntimeError(f"generated selector manifest is invalid TOML: {exc}") from exc
        selector_report = (
            f"Perfect Works Build {SUPPORTED_PWB_VERSION} individual selector mod\n\n"
            f"Mod: {individual.key}\n"
            f"Selector: {individual.option_label}\n"
            "Choices: "
            + ", ".join(f"{choice.label} ({choice.value})" for choice in choices)
            + "\nComposition semantics: each choice is composed in isolation.\n"
            "Source discs were read only and were not modified.\n\n"
            + "\n".join(reports)
        )
        write_utf8(package_root / "manifest.toml", manifest)
        write_utf8(package_root / "PORTING_REPORT.txt", selector_report)
        write_utf8(package_root / "PORTING_INDEX.tsv", "\n".join(index_lines) + "\n")
        pack_archive(package_root, output)
    return sorted(set(notes))


def package_claims(path: Path) -> dict[str, set[str]]:
    with zipfile.ZipFile(path) as archive:
        manifest = tomllib.loads(archive.read("manifest.toml").decode("utf-8"))
    claims: dict[str, set[str]] = {}
    for patch in manifest.get("patch", []):
        expected = bytes.fromhex(patch["expected"])
        replacement = bytes.fromhex(patch["replace"])
        if len(expected) != len(replacement):
            raise RuntimeError(f"invalid patch width in {path}")
        for offset in range(len(replacement)):
            resource = f"{patch['target']}@0x{patch['address'] + offset:08x}"
            claims.setdefault(resource, set()).add(
                f"{expected[offset]:02x}>{replacement[offset]:02x}"
            )
    for indexed in manifest.get("indexed_file", []):
        resource = f"disc:{indexed['disc_sha256']}:index:{indexed['index']}"
        claims.setdefault(resource, set()).add(
            f"{indexed['format']}:{indexed['expected_sha256']}:{indexed['sha256']}"
        )
    return claims


def incompatible_claims(
    first: dict[str, set[str]], second: dict[str, set[str]]
) -> set[str]:
    return {
        resource
        for resource in first.keys() & second.keys()
        if first[resource] != second[resource]
    }


def write_catalog_conflicts(catalog_root: Path) -> int:
    claims = {
        individual.key: package_claims(
            catalog_root
            / f"perfect-works-{individual.key}-{SUPPORTED_PWB_VERSION}.psxmod"
        )
        for individual in INDIVIDUAL_MODS
    }
    conflicts: list[tuple[str, str, set[str]]] = []
    for offset, first in enumerate(INDIVIDUAL_MODS):
        for second in INDIVIDUAL_MODS[offset + 1 :]:
            shared = incompatible_claims(claims[first.key], claims[second.key])
            if shared:
                conflicts.append((first.key, second.key, shared))
    lines = ["first\tsecond\tshared_resources\texamples"]
    for first, second, shared in conflicts:
        examples = ",".join(sorted(shared)[:8])
        lines.append(f"{first}\t{second}\t{len(shared)}\t{examples}")
    write_utf8(catalog_root / "CONFLICTS.tsv", "\n".join(lines) + "\n")
    return len(conflicts)


def build_individual_catalog(
    edition_root: Path,
    disc_paths: dict[int, Path],
    disc_hashes: dict[int, str],
    expected_bin_hashes: dict[int, str],
    output_directory: Path,
    tool_mode: str,
    progress: Callable[[str], None] | None = None,
) -> str:
    notify = progress or (lambda _message: None)
    resolved_output = output_directory.resolve()
    resolved_edition = edition_root.resolve()
    if resolved_output.exists():
        raise RuntimeError(f"individual output already exists: {resolved_output}")
    if resolved_output.is_relative_to(resolved_edition):
        raise RuntimeError("individual output must not be inside the edition tree")
    if resolved_output in {path.resolve() for path in disc_paths.values()}:
        raise RuntimeError("individual output aliases a read-only disc")

    require_supported_pwb_version(edition_root)
    prepared = prepare_stock_inputs(disc_paths, expected_bin_hashes, notify)
    authentication_cache: dict[tuple[str, str], str] = {}
    resolved_output.parent.mkdir(parents=True, exist_ok=True)
    rows: list[tuple[IndividualMod, Path, str, list[str]]] = []
    with tempfile.TemporaryDirectory(
        prefix="pw-individual-", dir=resolved_output.parent
    ) as temporary:
        catalog_root = Path(temporary) / "catalog"
        catalog_root.mkdir()
        for number, individual in enumerate(INDIVIDUAL_MODS, start=1):
            notify(
                f"[{number}/{len(INDIVIDUAL_MODS)}] Building {individual.name}..."
            )
            filename = (
                f"perfect-works-{individual.key}-{SUPPORTED_PWB_VERSION}.psxmod"
            )
            package = catalog_root / filename
            choices = individual_choices(individual)
            if len(choices) > 1:
                notes = build_selector_package(
                    individual,
                    edition_root,
                    disc_paths,
                    disc_hashes,
                    expected_bin_hashes,
                    package,
                    tool_mode,
                    prepared,
                    authentication_cache,
                )
            else:
                profile = Profile(
                    individual.name,
                    individual.description,
                    choices[0].options,
                    False,
                )
                build_package(
                    edition_root,
                    disc_paths,
                    disc_hashes,
                    expected_bin_hashes,
                    package,
                    individual.key,
                    profile,
                    f"org.perfectworksbuild.individual.{individual.key}",
                    individual.name,
                    tool_mode,
                    progress=None,
                    implicit_patches=False,
                    prepared_inputs=prepared,
                    authentication_cache=authentication_cache,
                )
                notes = coverage_notes(choices[0].options)
            rows.append(
                (
                    individual,
                    package,
                    sha256_file(package),
                    notes,
                )
            )

        catalog_lines = ["key\tfile\tsha256\tcoverage\tname"]
        for individual, package, digest, notes in rows:
            coverage = "partial" if notes else "complete"
            catalog_lines.append(
                "\t".join(
                    (
                        individual.key,
                        package.name,
                        digest,
                        coverage,
                        individual.name,
                    )
                )
            )
        write_utf8(catalog_root / "CATALOG.tsv", "\n".join(catalog_lines) + "\n")

        conflict_count = write_catalog_conflicts(catalog_root)

        readme = (
            f"Perfect Works Build {SUPPORTED_PWB_VERSION} individual mods\n"
            "\n"
            f"This directory contains {len(rows)} independent, default-disabled psxmod files.\n"
            "Each archive represents one patcher option; mutually exclusive values use a launcher selector.\n"
            "CATALOG.tsv records package identities and hashes.\n"
            "CONFLICTS.tsv lists packages with differing claims over the same indexed files or executable bytes.\n"
            "\n"
            "Indexed Xenogears files are atomic resources. Overlapping individual mods cannot be merged at runtime; "
            "use one of the converter's composed profiles when a combination appears in CONFLICTS.tsv.\n"
            "FMV undubbing has no package because it requires raw-disc and ISO-visible executable replacement.\n"
            "The Japanese-controls package omits its disc-specific executable edits; its package report identifies the partial coverage.\n"
        )
        write_utf8(catalog_root / "README.txt", readme)
        os.replace(catalog_root, resolved_output)

    return (
        f"Individual packages: {len(rows)}\n"
        f"Overlapping package pairs: {conflict_count}\n"
        f"Catalog: {resolved_output}\n"
    )


def options_from_args(args) -> PatchOptions:
    return PatchOptions(
        bug_fixes=args.bug_fixes,
        title_screen=args.title_screen,
        script=args.script,
        half_encounters=args.half_encounters,
        exp_factor=args.exp_factor,
        gold_factor=args.gold_factor,
        rebalanced_items=args.rebalanced_items,
        rebalanced_enemies=args.rebalanced_enemies,
        no_deathblow_levels=args.no_deathblow_levels,
        no_damage_cap=args.no_damage_cap,
        arena=args.arena,
        face_fix=args.face_fix,
        no_battle_flashes=args.no_battle_flashes,
        pw_roni=args.pw_roni,
        emeralda_cafe_fix=args.emeralda_cafe_fix,
        text_speed=args.text_speed,
        battle_undub=args.battle_undub,
        music_changes=args.music_changes,
        story_mode=args.story_mode,
        jpn_controls=args.jpn_controls,
        fmv_undub=args.fmv_undub,
    )


def add_custom_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--bug-fixes", action="store_true")
    parser.add_argument("--title-screen", action="store_true")
    parser.add_argument("--script", action="store_true")
    parser.add_argument("--half-encounters", action="store_true")
    parser.add_argument("--exp-factor", choices=("off", "1", "1.5", "2"), default="off")
    parser.add_argument("--gold-factor", choices=("off", "1", "1.5", "2"), default="off")
    parser.add_argument("--rebalanced-items", action="store_true")
    parser.add_argument("--rebalanced-enemies", action="store_true")
    parser.add_argument("--no-deathblow-levels", action="store_true")
    parser.add_argument("--no-damage-cap", action="store_true")
    parser.add_argument("--arena", choices=("normal", "basic", "expert"), default="normal")
    parser.add_argument("--face-fix", choices=("none", "normal", "resize"), default="none")
    parser.add_argument("--no-battle-flashes", action="store_true")
    parser.add_argument("--pw-roni", action="store_true")
    parser.add_argument("--emeralda-cafe-fix", action="store_true")
    parser.add_argument("--text-speed", choices=("normal", "fast", "instant"), default="normal")
    parser.add_argument("--battle-undub", action="store_true")
    parser.add_argument("--music-changes", action="store_true")
    parser.add_argument("--story-mode", action="store_true")
    parser.add_argument("--jpn-controls", action="store_true")
    parser.add_argument("--fmv-undub", action="store_true")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            f"Compose Perfect Works Build {SUPPORTED_PWB_VERSION} assets without "
            "modifying the stock "
            "discs, then create a deterministic format-6 psxmod."
        )
    )
    parser.add_argument("--edition-root", type=Path, required=True)
    parser.add_argument("--disc1", type=Path, required=True, help="clean raw MODE2/2352 Disc 1 BIN")
    parser.add_argument("--disc2", type=Path, required=True, help="clean raw MODE2/2352 Disc 2 BIN")
    outputs = parser.add_mutually_exclusive_group(required=True)
    outputs.add_argument("--output", type=Path)
    outputs.add_argument(
        "--individual-output",
        type=Path,
        metavar="DIRECTORY",
        help="build one psxmod per Perfect Works patcher option",
    )
    parser.add_argument("--source-output", type=Path)
    parser.add_argument("--profile", choices=(*PROFILES, "custom"))
    parser.add_argument("--package-id")
    parser.add_argument("--name")
    parser.add_argument(
        "--no-implicit-patches",
        action="store_true",
        help="do not add the patcher's implicit bug-fix and title-screen payloads",
    )
    parser.add_argument("--tool-mode", choices=("auto", "native", "wine"), default="auto")
    parser.add_argument("--disc1-sha256", default=DISC_CANONICAL_SHA256[1])
    parser.add_argument("--disc2-sha256", default=DISC_CANONICAL_SHA256[2])
    parser.add_argument("--disc1-bin-sha256", default=DISC_BIN_SHA256[1])
    parser.add_argument("--disc2-bin-sha256", default=DISC_BIN_SHA256[2])
    parser.add_argument("--oracle-disc1", type=Path)
    parser.add_argument("--oracle-disc2", type=Path)
    add_custom_arguments(parser)
    args = parser.parse_args(argv)

    try:
        disc_hashes = {
            1: validate_sha256(args.disc1_sha256, "Disc 1 canonical hash"),
            2: validate_sha256(args.disc2_sha256, "Disc 2 canonical hash"),
        }
        expected_bin_hashes = {
            1: validate_sha256(args.disc1_bin_sha256, "Disc 1 BIN hash"),
            2: validate_sha256(args.disc2_bin_sha256, "Disc 2 BIN hash"),
        }
        disc_paths = {1: args.disc1.resolve(), 2: args.disc2.resolve()}
        edition_root = args.edition_root.resolve()
        custom_options = options_from_args(args)
        def progress(message: str) -> None:
            print(message, file=sys.stderr, flush=True)

        if args.individual_output is not None:
            if args.profile is not None or custom_options != PatchOptions():
                parser.error(
                    "--individual-output builds the fixed individual catalog; "
                    "do not combine it with --profile or patch options"
                )
            if any(
                (
                    args.package_id,
                    args.name,
                    args.source_output,
                    args.oracle_disc1,
                    args.oracle_disc2,
                    args.no_implicit_patches,
                )
            ):
                parser.error(
                    "package naming, source output, oracles, and implicit-patch "
                    "controls are not valid with --individual-output"
                )
            report = build_individual_catalog(
                edition_root,
                disc_paths,
                disc_hashes,
                expected_bin_hashes,
                args.individual_output,
                args.tool_mode,
                progress,
            )
            print(report, end="")
            return 0

        profile_key = args.profile or "retranslation"
        if profile_key == "custom":
            if not args.package_id:
                parser.error("--package-id is required for a custom profile")
            profile = Profile(
                args.name or "Perfect Works Custom Port",
                f"Custom Perfect Works Build {SUPPORTED_PWB_VERSION} selection.",
                custom_options,
                not args.no_implicit_patches,
            )
        else:
            if custom_options != PatchOptions():
                parser.error("individual patch options may only be used with --profile custom")
            profile = PROFILES[profile_key]
        if (args.oracle_disc1 is None) != (args.oracle_disc2 is None):
            parser.error("both oracle discs must be supplied together")
        if args.output.suffix.lower() != ".psxmod":
            parser.error("--output must end in .psxmod")

        package_id = args.package_id or package_id_for(profile_key)
        package_name = args.name or profile.name
        if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,95}", package_id):
            raise ValueError("package id has invalid characters or length")
        if not package_name.strip():
            raise ValueError("package name must not be empty")
        report = build_package(
            edition_root,
            disc_paths,
            disc_hashes,
            expected_bin_hashes,
            args.output.resolve(),
            profile_key,
            profile,
            package_id,
            package_name,
            args.tool_mode,
            ({1: args.oracle_disc1.resolve(), 2: args.oracle_disc2.resolve()}
             if args.oracle_disc1 else None),
            args.source_output.resolve() if args.source_output else None,
            progress,
            implicit_patches=(
                False if args.no_implicit_patches else profile.implicit_patches
            ),
        )
    except (OSError, RuntimeError, ValueError) as exc:
        parser.error(str(exc))
    print(report, end="")
    print(f"Package: {args.output.resolve()}")
    print(f"SHA-256: {sha256_file(args.output.resolve())}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
