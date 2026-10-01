#!/usr/bin/env python3
"""Package a native mod, generating byte guards from local game code when supplied.

Neither the library nor the game code is executed. Game source images are not
included in the package.
"""
import argparse
from bisect import bisect_right
import hashlib
import json
import platform
import re
import sys
import zipfile
from pathlib import Path


def read_file(path, parser, maximum):
    try:
        with path.open("rb") as source:
            data = source.read(maximum + 1)
    except OSError as error:
        parser.error(f"cannot read {path}: {error}")
    if not data or len(data) > maximum:
        parser.error(f"{path} must be nonempty and at most {maximum} bytes")
    return data


def read_exe(path, parser):
    # A main-RAM image plus the PS-X EXE header and ordinary file padding.
    image = read_file(path, parser, 0x400000)
    if len(image) < 0x800 or image[:8] != b"PS-X EXE":
        parser.error("--exe must be a PS-X EXE; use --image ADDRESS:FILE for raw code")
    load = int.from_bytes(image[0x18:0x1c], "little") & 0x1fffffff
    size = int.from_bytes(image[0x1c:0x20], "little")
    if load % 4 or not 0 <= load < 0x200000 or not 0 < size <= 0x200000 - load:
        parser.error("EXE payload must map inside game RAM")
    if len(image) < 0x800 + size:
        parser.error("EXE payload is truncated")
    return (load, image[0x800:0x800 + size]), hashlib.sha256(image).hexdigest()


def image_guard(images, address, size, parser, reference=None, catalog=None):
    physical = address & 0x1fffffff
    if reference and reference.symbol and reference.symbol.image != "resident":
        owner = catalog.images[reference.symbol.image]
        load = int(owner["load_address"], 0) & 0x1fffffff
        loaded_size = owner["loaded_size"]
        if not load <= physical < physical + size <= load + loaded_size:
            parser.error(f"hook range escapes {reference.symbol.image}")
        # Overlays can share an address. Choose the image by its authenticated
        # identity, rather than whichever supplied file happens to cover it.
        images = [(base, data) for base, data in images
                  if base <= load and load + loaded_size <= base + len(data)
                  and hashlib.sha256(data[load - base:load - base + loaded_size]).hexdigest() == owner["sha256"]]
        if not images:
            parser.error(f"no source matches {reference.symbol.image}; provide its original overlay using --image")
    matches = [data[physical - base:physical - base + size]
               for base, data in images
               if base <= physical and physical + size <= base + len(data)]
    if not matches:
        parser.error(f"no source image covers the requested bytes at 0x{address:08x}")
    if len(matches) != 1:
        parser.error(f"source images overlap at 0x{address:08x}; choose one source or provide an explicit guard")
    return matches[0].hex()


def toml_string(value):
    return json.dumps(value, ensure_ascii=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path, nargs="?")
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--id", default="example.xenogears.native-hook", help="stable package identity")
    parser.add_argument("--version", default="1.0.0", help="package release version")
    parser.add_argument("--name", default="Native Function Hook Example", help="launcher package name")
    parser.add_argument("--author", default="Example Author")
    parser.add_argument("--exe", type=Path, help="local PS-X EXE; generates guards and the target revision hash")
    parser.add_argument("--image", action="append", default=[], metavar="ADDRESS:FILE",
                        help="raw overlay/code image mapped at ADDRESS; repeat for separate images")
    parser.add_argument("--address", help="function name or address (alias for --hook)")
    parser.add_argument("--expected", help="optional explicit original bytes for --address, instead of automatic extraction")
    parser.add_argument("--hook", action="append", default=[], metavar="FUNCTION[:HEX]",
                        help="hook a function; bytes are generated from --exe/--image unless supplied")
    parser.add_argument("--block", action="append", default=[], metavar="START:RESUME[:HEX]",
                        help="replace a range; bytes are generated from --exe/--image unless supplied")
    parser.add_argument("--annotations", type=Path,
                        help="annotation directory (defaults to this repository's annotations)")
    parser.add_argument("--list-functions", nargs="?", const="", metavar="FILTER",
                        help="list qualified names and addresses; optionally filter by name")
    parser.add_argument("--guard-bytes", type=int, default=16,
                        help="advanced: bytes sampled for automatic function guards (default: 16); does not limit C/C++ code")
    platforms = [f"{os}-{cpu}" for os in ("linux", "windows", "macos") for cpu in ("x86_64", "aarch64")]
    parser.add_argument("--platform", choices=platforms)
    args = parser.parse_args()
    catalog = None
    def symbols():
        nonlocal catalog
        if catalog is None:
            sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
            try:
                from native_mod_symbols import Catalog, DEFAULT_ANNOTATIONS
                catalog = Catalog(args.annotations or DEFAULT_ANNOTATIONS)
            except (ImportError, OSError, ValueError, KeyError, TypeError) as error:
                parser.error(f"cannot read function annotations: {error}")
        return catalog
    def resolve(text):
        try:
            return int(text, 0), None
        except ValueError:
            try:
                reference = symbols().resolve(text)
            except ValueError as error:
                parser.error(str(error))
            return reference.address, reference
    if args.list_functions is not None:
        for symbol in symbols().symbols:
            if symbol.supported and args.list_functions.casefold() in symbol.qualified.casefold():
                print(f"{symbol.qualified}\t0x{symbol.address:08X}")
        return
    if args.library is None or args.output is None:
        parser.error("provide LIBRARY and OUTPUT.psxmod, or use --list-functions")
    if not re.fullmatch(r"[a-z0-9._-]{1,96}", args.id) or args.id.startswith(".") or args.id.endswith("."):
        parser.error("--id must be a valid lowercase package identity")
    version = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)(?:-[a-zA-Z0-9._+-]+)?", args.version)
    if not version or len(args.version) > 96 or any(int(part) > 0x7fffffffffffffff for part in version.groups()):
        parser.error("--version must be a supported semantic version, such as 1.0.0")
    if not args.name.strip() or not args.author.strip():
        parser.error("--name and --author must not be empty")
    if args.guard_bytes < 4 or args.guard_bytes % 4 or args.guard_bytes > 0x1f0000:
        parser.error("--guard-bytes must be whole instructions that fit in game RAM")
    images, exe_hash = [], None
    source_paths = [args.library]
    if args.exe:
        image, exe_hash = read_exe(args.exe, parser)
        images.append(image)
        source_paths.append(args.exe)
    try:
        for value in args.image:
            address, file = value.split(":", 1)
            base = int(address, 0)
            if not 0 <= base <= 0xffffffff or base % 4 or not 0 <= (base & 0x1fffffff) < 0x200000:
                parser.error("image address must be aligned inside game RAM")
            base &= 0x1fffffff
            source = Path(file)
            images.append((base, read_file(source, parser, 0x200000 - base)))
            source_paths.append(source)
    except ValueError:
        parser.error("use --image ADDRESS:FILE")
    hooks = []
    if args.expected is not None and args.address is None:
        parser.error("--expected requires --address")
    if args.address is not None:
        address, reference = resolve(args.address)
        hooks.append((address, None, args.expected, reference))
    try:
        for value in args.hook:
            fields = value.split(":")
            if len(fields) not in (1, 2): raise ValueError
            address, expected = fields[0], fields[1] if len(fields) == 2 else None
            address, reference = resolve(address)
            hooks.append((address, None, expected, reference))
        for value in args.block:
            fields = value.split(":")
            if len(fields) not in (2, 3): raise ValueError
            address, resume = fields[:2]
            expected = fields[2] if len(fields) == 3 else None
            address, reference = resolve(address)
            resume, continuation = resolve(resume)
            if reference and continuation and reference.symbol.image != continuation.symbol.image:
                parser.error("partial hook start and resume must belong to the same image")
            hooks.append((address, resume, expected, reference or continuation))
    except ValueError:
        parser.error("use --hook FUNCTION[:HEX] or --block START:RESUME[:HEX]; names and hex addresses are accepted")
    if not hooks:
        parser.error("declare at least one --hook, --block or --address/--expected pair")
    declarations, seen, ranges = [], set(), []
    if catalog:
        source_paths.extend(catalog.paths)
    if args.output.resolve() in {source.resolve() for source in source_paths}:
        parser.error("output archive must not overwrite a library, source image or annotation file")
    for address, resume, text, reference in hooks:
        phys = address & 0x1fffffff
        if not 0 <= address <= 0xffffffff or address % 4 or not 0x10000 <= phys < 0x200000:
            parser.error("hook address must be aligned inside game RAM")
        if resume is not None and (not 0 <= resume <= 0xffffffff or resume % 4 or resume <= address or
                                   (resume & 0xe0000000) != (address & 0xe0000000) or
                                   (resume & 0x1fffffff) >= 0x200000):
            parser.error("resume must be forward, aligned and inside the same RAM segment")
        if text is None:
            if not images:
                parser.error("provide --exe or --image to generate guards automatically, or supply an explicit guard")
            text = image_guard(images, address, resume - address if resume is not None else args.guard_bytes,
                               parser, reference, catalog)
        expected = re.sub(r"\s+", "", text).lower()
        if not re.fullmatch(r"[0-9a-f]{8,}", expected) or len(expected) % 8:
            parser.error("expected must contain whole instruction bytes in hex")
        size, phys = len(expected) // 2, address & 0x1fffffff
        if not 0 <= address <= 0xffffffff or address % 4 or not 0x10000 <= phys <= 0x200000 - size:
            parser.error("address and guard must be aligned inside game RAM")
        if phys in seen:
            parser.error("hook addresses must be unique, including RAM aliases")
        seen.add(phys)
        entry = f'\n[[native_module.hook]]\naddress = 0x{address:08x}\nexpected = "{expected}"\n'
        if resume is not None:
            if not 0 <= resume <= 0xffffffff or resume % 4 or resume != address + size:
                parser.error("resume must be exactly after the guarded block")
            if (address & 0xe0000000) != (resume & 0xe0000000):
                parser.error("block must stay in the same RAM segment")
            if (resume & 0x1fffffff) >= 0x200000:
                parser.error("resume must name an instruction inside game RAM")
            ranges.append((phys, phys + size))
            entry += f'resume_address = 0x{resume:08x}\n'
        declarations.append(entry)
    ranges.sort()
    if any(previous[1] > following[0] for previous, following in zip(ranges, ranges[1:])):
        parser.error("partial hook ranges must not overlap")
    range_starts = [start for start, _ in ranges]
    for address, resume, _, _ in hooks:
        if resume is not None:
            continue
        physical = address & 0x1fffffff
        index = bisect_right(range_starts, physical) - 1
        if index >= 0 and physical < ranges[index][1]:
            parser.error("function hook entry must not lie inside a partial replacement range")
    arch = {"amd64": "x86_64", "arm64": "aarch64"}.get(platform.machine().lower(), platform.machine().lower())
    os = {"win32": "windows", "darwin": "macos"}.get(sys.platform, sys.platform)
    target = args.platform or f"{os}-{arch}"
    if target not in platforms:
        parser.error("unsupported host; specify --platform for the compiled library")
    payload = read_file(args.library, parser, 256 * 1024 * 1024)
    digest = hashlib.sha256(payload).hexdigest()
    suffix = {"linux": ".so", "windows": ".dll", "macos": ".dylib"}[target.split("-")[0]]
    filename = f"native/hook{suffix}"
    manifest = f'''format_version = 9
id = {toml_string(args.id)}
version = {toml_string(args.version)}
name = {toml_string(args.name)}
author = {toml_string(args.author)}
description = "Hook selected functions or replace selected instruction ranges."
license = "MIT"

[[target]]
game_id = "SLUS-00664"
{f'exe_sha256 = "{exe_hash}"' if exe_hash else ''}

[[feature]]
id = "hook"
name = "Native Function Hook"
default_enabled = false

[[option]]
feature = "hook"
id = "replace"
label = "Replace function (advanced)"
type = "boolean"
default = "false"

[[option]]
feature = "hook"
id = "return_value"
label = "Replacement return value"
type = "integer"
default = 0
min = 0
max = 65535
step = 1

[[native_module]]
id = "hook"
feature = "hook"
platform = "{target}"
file = "{filename}"
sha256 = "{digest}"

'''
    manifest += "".join(declarations)
    entries = {"manifest.toml": manifest.encode(), filename: payload}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w") as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data, compresslevel=9)
    print(f"Created {args.output} for {target}; native code needs launcher trust.")


if __name__ == "__main__":
    main()
