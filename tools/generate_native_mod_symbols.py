#!/usr/bin/env python3
"""Generate the game's native-mod name catalog and live-code identity checks."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from native_mod_symbols import Catalog, DEFAULT_ANNOTATIONS


def read_bytes(path: Path, maximum: int) -> bytes:
    with path.open("rb") as source:
        data = source.read(maximum + 1)
    if not data or len(data) > maximum:
        raise ValueError(f"{path}: file must be nonempty and at most {maximum} bytes")
    return data


def exe_image(path: Path) -> tuple[int, bytes]:
    data = read_bytes(path, 0x400000)
    if len(data) < 0x800 or data[:8] != b"PS-X EXE":
        raise ValueError("expected a PS-X EXE")
    base = int.from_bytes(data[0x18:0x1c], "little") & 0x1fffffff
    size = int.from_bytes(data[0x1c:0x20], "little")
    if base % 4 or not 0 < size <= 0x200000 - base or len(data) < size + 0x800:
        raise ValueError("invalid or truncated EXE payload")
    return base, data[0x800:0x800 + size]


def fingerprint(ranges, base: int, data: bytes):
    normalized = tuple((int(address) & 0x1fffffff, int(size)) for address, size in ranges)
    if not normalized or len(normalized) > 4096:
        raise ValueError("invalid code range count")
    digest = hashlib.sha256()
    previous = base
    for address, size in normalized:
        if address < previous or address % 4 or size <= 0 or size % 4 or address + size > base + len(data):
            raise ValueError("code ranges must be ordered whole instructions inside their source image")
        digest.update(data[address - base:address - base + size])
        previous = address + size
    return normalized, digest.hexdigest()


def generate(catalog: Catalog, exe: Path, aot: Path) -> str:
    base, resident = exe_image(exe)
    identities, scopes = {}, {}
    owners_by_code = {}
    units = {}
    for owner, image in catalog.images.items():
        if owner == "resident" or not any(symbol.image == owner and symbol.supported for symbol in catalog.symbols):
            continue
        data = read_bytes(aot / "inputs" / f"{owner}.bin", 0x800000)
        unit = json.loads(read_bytes(aot / "metadata" / f"{owner}.json", 64 * 1024 * 1024))
        if len(data) != image["size"] or hashlib.sha256(data).hexdigest() != image["sha256"] or unit["input_sha256"] != image["sha256"]:
            raise ValueError(f"{owner}: source identity does not match the annotation index")
        load = int(image["load_address"], 0) & 0x1fffffff
        scope = fingerprint(unit["image"]["ranges"], load, data)
        if scope[1] != unit["image"]["code_sha256"]:
            raise ValueError(f"{owner}: image code fingerprint mismatch")
        scopes[owner] = scope
        candidates = {}
        for variant in unit["variants"]:
            candidates.setdefault(variant["addr"], []).append(variant)
            # Continuation entries can also occupy another image's annotated
            # function address. Include them when detecting shared code owners.
            variant_code = (tuple((int(address) & 0x1fffffff, int(size))
                                  for address, size in variant["ranges"]), variant["code_sha256"])
            owners_by_code.setdefault((variant["addr"] & 0x1fffffff, variant_code), set()).add(owner)
        units[owner] = (load, data, candidates)
    for symbol in catalog.symbols:
        if not symbol.supported:
            continue
        if symbol.image == "resident":
            address = symbol.address & 0x1fffffff
            # Resident identity is already bound to the executable by the game
            # build. This local entry guard additionally detects changed code.
            size = min(16, base + len(resident) - address)
            code = fingerprint([(address, size)], base, resident)
        else:
            load, data, candidates = units[symbol.image]
            choices = candidates.get(symbol.address, [])
            if not choices:
                raise ValueError(f"{symbol.qualified}: missing authenticated function ranges")
            variant = min(choices, key=lambda v: (v["producer_entry"] != symbol.address,
                                                  v["resume"] != 0, len(v["ranges"]), str(v["ranges"])))
            code = fingerprint(variant["ranges"], load, data)
            if code[1] != variant["code_sha256"]:
                raise ValueError(f"{symbol.qualified}: function code fingerprint mismatch")
        identities[symbol] = code
        key = (symbol.address & 0x1fffffff, code)
        owners_by_code.setdefault(key, set()).add(symbol.image)

    lines = [
        "// Generated from indexed annotations and authenticated local code. Do not edit.",
        '#include "mod_native_runtime.h"',
        'extern "C" int psx_overlay_static_code_matches(const uint32_t*, uint32_t, const uint8_t*);',
        "namespace {",
        "struct Guard { const uint32_t* ranges; uint32_t count; const uint8_t* digest; const Guard* scope; };",
    ]
    guards = {}
    def guard(code, scope=None):
        key = (code, scope)
        if key in guards:
            return guards[key]
        scope_symbol = guard(scope) if scope else "nullptr"
        name = f"guard_{len(guards)}"
        guards[key] = name
        ranges, digest = code
        values = ", ".join(f"0x{value:X}u" for pair in ranges for value in pair)
        sha = ", ".join(f"0x{value:02X}" for value in bytes.fromhex(digest))
        lines.extend([
            f"static const uint32_t {name}_ranges[] = {{{values}}};",
            f"static const uint8_t {name}_sha[32] = {{{sha}}};",
            f"static const Guard {name} = {{{name}_ranges, {len(ranges)}u, {name}_sha, {('nullptr' if scope_symbol == 'nullptr' else '&' + scope_symbol)}}};",
        ])
        return name
    symbol_guards = {}
    for symbol, code in identities.items():
        # Shared code at a reused address needs the owning image's code identity,
        # just as the static overlay dispatcher requires for identical variants.
        owners = owners_by_code[(symbol.address & 0x1fffffff, code)]
        scope = scopes[symbol.image] if len(owners) > 1 and symbol.image != "resident" else None
        symbol_guards[symbol] = guard(code, scope)
    lines.extend([
        "static int available(const void* identity) {",
        "    const auto* g = static_cast<const Guard*>(identity);",
        "    return g && (!g->scope || available(g->scope)) &&",
        "        psx_overlay_static_code_matches(g->ranges, g->count, g->digest);",
        "}",
        "static const ModNativeSymbol symbols[] = {",
    ])
    for alias, matches in sorted(catalog.by_name.items()):
        for symbol in sorted(matches, key=lambda item: item.qualified):
            if symbol in symbol_guards:
                lines.append(f"    {{{json.dumps(alias)}, 0x{symbol.address:08X}u, &{symbol_guards[symbol]}}},")
    lines.extend([
        "};",
        "struct Register { Register() {",
        "    mod_native_register_symbols(symbols, static_cast<uint32_t>(sizeof symbols / sizeof symbols[0]), available);",
        "} };",
        "static Register register_symbols;",
        "} // namespace",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--annotations", type=Path, default=DEFAULT_ANNOTATIONS)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--aot", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        catalog = Catalog(args.annotations)
        if args.output.resolve() == args.exe.resolve() or args.output.resolve() in catalog.paths or args.output.resolve().is_relative_to(args.aot.resolve()):
            raise ValueError("catalog output must not overwrite source inputs")
        result = generate(catalog, args.exe, args.aot).encode("ascii")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_bytes() != result:
            temporary = args.output.with_suffix(args.output.suffix + ".tmp")
            temporary.write_bytes(result)
            temporary.replace(args.output)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
