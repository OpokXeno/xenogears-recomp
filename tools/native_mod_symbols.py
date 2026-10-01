"""Read the indexed function annotations shared by native mod tools and builds.

Only the main CSV and CSVs named in the overlay index are read. Annotation
notes are not ordinary quoted CSV: everything after the first comma is text.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import tomllib

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ANNOTATIONS = ROOT / "annotations"


@dataclass(frozen=True)
class Symbol:
    name: str
    address: int
    image: str

    @property
    def qualified(self) -> str:
        return f"{self.image}/{self.name}"

    @property
    def supported(self) -> bool:
        return 0x10000 <= (self.address & 0x1fffffff) < 0x200000


@dataclass(frozen=True)
class Reference:
    address: int
    symbol: Symbol | None = None
    offset: int = 0


def annotation_name(note: str) -> str:
    # Drop prose and signatures; give the rare names containing spaces or
    # punctuation a printable identifier (Kernel MENU -> Kernel_MENU).
    title = re.split(r"[—;(]", note, maxsplit=1)[0].strip()
    name = title if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", title) else re.sub(r"[^A-Za-z0-9_]+", "_", title).strip("_")
    if not name or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", name):
        raise ValueError(f"annotation has no usable function name: {note!r}")
    return name


class Catalog:
    def __init__(self, annotations: Path = DEFAULT_ANNOTATIONS):
        self.annotations = annotations.resolve()
        self.paths = [self.annotations / "slus_006.64_annotations.csv",
                      self.annotations / "overlays/index.toml"]
        document = tomllib.loads(self.paths[1].read_text(encoding="utf-8"))
        if document.get("schema") != "xenogears-overlay-annotations/v1":
            raise ValueError("unsupported overlay annotation index")
        self.images = {"resident": None}
        self.symbols: list[Symbol] = []
        self.by_name: dict[str, list[Symbol]] = {}
        self._read(self.paths[0], "resident", False)
        for image in document.get("images", []):
            owner = image["id"]
            if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,95}", owner) or owner in self.images:
                raise ValueError(f"invalid or duplicate image id: {owner!r}")
            path = (self.annotations.parent / image["annotations"]).resolve()
            if not path.is_relative_to(self.annotations):
                raise ValueError("indexed annotations must stay inside the annotation directory")
            self.images[owner] = image
            self.paths.append(path)
            self._read(path, owner, True)
        for symbol in self.symbols:
            # Address-style aliases remain useful when descriptive names change.
            for key in (symbol.name, symbol.qualified,
                        f"func_{symbol.address:08X}", f"{symbol.image}/func_{symbol.address:08X}"):
                candidates = self.by_name.setdefault(key, [])
                if symbol not in candidates:
                    candidates.append(symbol)
        self.symbols.sort(key=lambda symbol: symbol.qualified)

    def _read(self, path: Path, owner: str, overlay: bool) -> None:
        names, addresses = set(), set()
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                address_text, note = line.split(",", 1)
                note = note.strip()
                if overlay:
                    if not note.startswith("function start — "):
                        continue  # Instruction/call/return sites are not functions.
                    note = note.removeprefix("function start — ")
                address = int(address_text, 0)
                name = annotation_name(note)
                if not 0 <= address <= 0xffffffff or address % 4 or not 0x10000 <= (address & 0x1fffffff) < 0x800000:
                    raise ValueError("function address must be aligned inside PS1 RAM or its mirrors")
                if name in names or address in addresses:
                    raise ValueError("duplicate function name or address in one image")
                names.add(name)
                addresses.add(address)
                self.symbols.append(Symbol(name, address, owner))
            except ValueError as error:
                raise ValueError(f"{path}:{number}: {error}") from error

    def resolve(self, text: str) -> Reference:
        # Hexadecimal and decimal addresses keep their existing meaning.
        try:
            return Reference(int(text, 0))
        except ValueError:
            pass
        fields = text.split("+")
        if len(fields) > 2 or not fields[0]:
            raise ValueError("use NAME, IMAGE/NAME, ADDRESS or NAME+OFFSET")
        key = fields[0]
        offset = 0
        if len(fields) == 2:
            try:
                offset = int(fields[1], 0)
            except ValueError as error:
                raise ValueError(f"invalid offset in {text!r}") from error
            if offset < 0 or offset % 4:
                raise ValueError("function offsets must be nonnegative whole instructions")
        matches = self.by_name.get(key, [])
        if not matches:
            raise ValueError(f"unknown function {key!r}; use --list-functions to see available names")
        if len(matches) != 1:
            choices = ", ".join(symbol.qualified for symbol in matches)
            raise ValueError(f"ambiguous function {key!r}; choose one of: {choices}")
        symbol = matches[0]
        if not symbol.supported:
            raise ValueError(f"{symbol.qualified} lies outside the native API's supported 2 MiB game RAM")
        address = symbol.address + offset
        if address > 0xffffffff or (address & 0x1fffffff) >= 0x200000 or (address & 0xe0000000) != (symbol.address & 0xe0000000):
            raise ValueError("function offset escapes game RAM")
        return Reference(address, symbol, offset)
