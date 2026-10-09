"""Standalone XGS compiler, XGA assembler and relocating linker for Battle Event scripts."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field

from battle_event_codec import BY_NAME, COMPARISON_FUNCTIONS, COMPARISONS, FIELD_SIZES, SPECS, Instruction, encode
from decompile_battle_events import ENGINE_VARIABLES, VARIABLE_BANK_END, analyze
from extract_disc_battle_scripts import (
    ENTRIES_PER_ENTITY, MAX_BYTECODE_SIZE, RUNTIME_ENTITY_CAPACITY, SCRIPT_PREFIX_SIZE, parse_script_resource,
)

NEW_VARIABLE_FIRST = 0x0200   # above the 0..255 indices Enemy AI opcode 0x70 can write
IDENT = r"[A-Za-z_][A-Za-z0-9_]*"
NUMBER = r"-?(?:0[xX][0-9A-Fa-f]+|\d+)"


class SourceError(ValueError):
    def __init__(self, line: int | None, message: str):
        super().__init__(f"line {line}: {message}" if line else message)


# --------------------------------------------------------------------------- IR

@dataclass
class Record:
    kind: str                                   # "op" or "data"
    raw: bytes                                  # label fields are zero
    fixups: list[tuple[int, str]] = field(default_factory=list)
    labels: list[str] = field(default_factory=list)
    pc: int | None = None                       # XGA position assertion
    line: int | None = None
    inserted: bool = False

    def fingerprint(self) -> tuple:
        return (self.kind, self.raw, tuple(self.fixups))


@dataclass
class Assembly:
    prefix: bytes
    rows: list[list[object]]                    # label name or raw int per entry
    records: list[Record]
    aliases: dict[str, tuple[str, int]] = field(default_factory=dict)
    end_labels: list[str] = field(default_factory=list)
    report: dict = field(default_factory=dict)

    def link(self, layout: str = "relocate") -> "LinkedAssembly":
        if layout not in {"relocate", "exact"}:
            raise ValueError("layout must be relocate or exact")
        addresses: dict[str, int] = {}
        pc = 0
        for record in self.records:
            if layout == "exact" and record.pc is not None and record.pc != pc:
                raise SourceError(record.line, f"record asserted at 0x{record.pc:04X} is placed at 0x{pc:04X}")
            for label in record.labels:
                if label in addresses:
                    raise SourceError(record.line, f"duplicate label {label}")
                addresses[label] = pc
            pc += len(record.raw)
        for label in self.end_labels:
            if label in addresses:
                raise SourceError(None, f"duplicate label {label}")
            addresses[label] = pc
        if pc > MAX_BYTECODE_SIZE:
            raise SourceError(None, f"bytecode is {pc} bytes; the VM addresses at most {MAX_BYTECODE_SIZE}")
        pending = dict(self.aliases)
        while pending:
            progressed = False
            for name, (base, delta) in list(pending.items()):
                if base in addresses:
                    if name in addresses:
                        raise SourceError(None, f"alias {name} duplicates a label")
                    addresses[name] = addresses[base] + delta
                    del pending[name]
                    progressed = True
            if not progressed:
                raise SourceError(None, "unresolved alias base: " + ", ".join(f"{name} = {base}" for name, (base, _) in pending.items()))
        return LinkedAssembly(self, addresses, pc)


@dataclass
class LinkedAssembly:
    assembly: Assembly
    addresses: dict[str, int]
    size: int

    def resolve(self, target: object, line: int | None = None) -> int:
        if isinstance(target, int):
            return target
        if target not in self.addresses:
            raise SourceError(line, f"unresolved label {target}")
        value = self.addresses[target]
        if value > 0xFFFF:
            raise SourceError(line, f"label {target} lies beyond the 16-bit address space")
        return value

    def bytecode(self) -> bytes:
        output = bytearray()
        for record in self.assembly.records:
            raw = bytearray(record.raw)
            for offset, label in record.fixups:
                struct.pack_into("<H", raw, offset, self.resolve(label, record.line))
            output += raw
        return bytes(output)

    def build(self, layout: str = "exact") -> bytes:
        rows = self.assembly.rows
        if not rows:
            raise SourceError(None, "at least one entity is required")
        if len(rows) > RUNTIME_ENTITY_CAPACITY:
            raise SourceError(None, f"{len(rows)} entities exceed the runtime's {RUNTIME_ENTITY_CAPACITY} entity slots")
        output = bytearray(self.assembly.prefix)
        output += struct.pack("<I", len(rows))
        for row in rows:
            output += struct.pack("<8H", *(self.resolve(target) for target in row))
        output += self.bytecode()
        result = bytes(output)
        parse_script_resource(result)
        return result

    @property
    def link_report(self) -> dict:
        return {
            "bytecode_size": self.size,
            "entities": len(self.assembly.rows),
            "labels": {name: f"0x{address:04X}" for name, address in sorted(self.addresses.items(), key=lambda item: (item[1], item[0]))},
            "generated_jumps": sum(1 for record in self.assembly.records if record.inserted and record.raw[0] == 0x01),
            **self.assembly.report,
        }

    def text(self) -> str:
        assembly = self.assembly
        lines = ["// Xenogears Battle Event assembly", ".xga 1", f'.prefix "{assembly.prefix.hex(" ").upper()}"',
                 f".size 0x{self.size:04X}", f".entities {len(assembly.rows)}"]
        for entity, row in enumerate(assembly.rows):
            lines.append(f".row {entity} = " + ", ".join(_target_text(target) for target in row))
        for name, (base, delta) in sorted(assembly.aliases.items()):
            lines.append(f".alias {name} = {base} + {delta}")
        pc = 0
        for record in assembly.records:
            for label in record.labels:
                lines.append(f"{label}:")
            fixups = "".join(f" @{offset}={label}" for offset, label in record.fixups)
            lines.append(f'.at 0x{pc:04X} {record.kind} "{record.raw.hex(" ").upper()}"{fixups}')
            pc += len(record.raw)
        for label in assembly.end_labels:
            lines.append(f"{label}:")
        return "\n".join(lines) + "\n"


def _target_text(target: object) -> str:
    return f"0x{target:04X}" if isinstance(target, int) else str(target)


# ------------------------------------------------------------------ disassembly

def disassemble(script: bytes) -> LinkedAssembly:
    """Lossless XGA view of a binary script resource."""
    analysis = analyze(script)
    labels = {block.start: block.label for block in analysis.blocks}
    records = []
    for block in analysis.blocks:
        if block.kind == "data":
            records.append(Record("data", analysis.bytecode[block.start:block.end], labels=[block.label], pc=block.start))
            continue
        for index, instruction in enumerate(block.instructions):
            raw = bytearray(analysis.bytecode[instruction.pc:instruction.pc + instruction.size])
            fixups = []
            offset = 1
            for operand, value in zip(instruction.spec.operands, instruction.values):
                if operand.kind == "label":
                    name = labels.get(value) or (f"L_{value:04X}" if value in analysis.aliases else None)
                    if name is not None:
                        raw[offset:offset + 2] = b"\0\0"
                        fixups.append((offset, name))
                offset += FIELD_SIZES[operand.kind]
            records.append(Record("op", bytes(raw), fixups, [block.label] if index == 0 else [], instruction.pc))
    aliases = {f"L_{target:04X}": (labels[owner], delta) for target, (owner, delta) in analysis.aliases.items()}
    rows = [[labels.get(offset) or (f"L_{offset:04X}" if offset in analysis.aliases else offset) for offset in row]
            for row in analysis.rows]
    assembly = Assembly(analysis.prefix, rows, records, aliases)
    linked = assembly.link("exact")
    if linked.build() != script:
        raise ValueError("internal disassembly round-trip failure")
    return linked


def parse_assembly(text: str) -> Assembly:
    prefix = bytes(SCRIPT_PREFIX_SIZE)
    rows: dict[int, list[object]] = {}
    declared_entities = None
    records: list[Record] = []
    aliases: dict[str, tuple[str, int]] = {}
    pending: list[str] = []
    for number, original in enumerate(text.splitlines(), 1):
        line = _strip_comment(original).strip()
        if not line:
            continue
        if re.fullmatch(rf"{IDENT}:", line):
            pending.append(line[:-1])
            continue
        if match := re.fullmatch(r"\.xga\s+(\d+)", line):
            if match.group(1) != "1":
                raise SourceError(number, "unsupported XGA version")
        elif match := re.fullmatch(r'\.prefix\s+"([0-9A-Fa-f ]*)"', line):
            prefix = bytes.fromhex(match.group(1))
            if len(prefix) != SCRIPT_PREFIX_SIZE:
                raise SourceError(number, f".prefix must contain {SCRIPT_PREFIX_SIZE} bytes")
        elif re.fullmatch(rf"\.size\s+{NUMBER}", line):
            pass
        elif match := re.fullmatch(r"\.entities\s+(\d+)", line):
            declared_entities = int(match.group(1))
        elif match := re.fullmatch(r"\.row\s+(\d+)\s*=\s*(.+)", line):
            targets = [item.strip() for item in match.group(2).split(",")]
            if len(targets) != ENTRIES_PER_ENTITY:
                raise SourceError(number, f".row needs {ENTRIES_PER_ENTITY} targets")
            rows[int(match.group(1))] = [_int(item, number) if re.fullmatch(NUMBER, item) else item for item in targets]
        elif match := re.fullmatch(rf"\.alias\s+({IDENT})\s*=\s*({IDENT})\s*\+\s*({NUMBER})", line):
            aliases[match.group(1)] = (match.group(2), _int(match.group(3), number))
        elif match := re.fullmatch(rf'\.at\s+({NUMBER}|auto)\s+(op|data)\s+"([0-9A-Fa-f ]*)"((?:\s+@\d+={IDENT})*)', line):
            raw = bytes.fromhex(match.group(3))
            if not raw:
                raise SourceError(number, "empty record")
            fixups = [(int(offset), label) for offset, label in re.findall(rf"@(\d+)=({IDENT})", match.group(4))]
            for offset, _ in fixups:
                if offset + 2 > len(raw):
                    raise SourceError(number, "fixup leaves its record")
            if match.group(2) == "op":
                if raw[0] not in SPECS or SPECS[raw[0]].size != len(raw):
                    raise SourceError(number, "op record does not match its opcode size")
            pc = None if match.group(1) == "auto" else _int(match.group(1), number)
            records.append(Record(match.group(2), raw, fixups, pending, pc, number))
            pending = []
        else:
            raise SourceError(number, f"unrecognized XGA line: {line}")
    count = declared_entities if declared_entities is not None else len(rows)
    if sorted(rows) != list(range(count)):
        raise SourceError(None, "entity rows must be consecutive from zero")
    return Assembly(prefix, [rows[index] for index in range(count)], records, aliases, pending)


# ------------------------------------------------------------------ XGS parser

def _strip_comment(line: str) -> str:
    quoted = False
    for index, char in enumerate(line):
        if char == '"':
            quoted = not quoted
        elif not quoted and line.startswith("//", index):
            return line[:index]
    return line


def _int(text: str, line: int | None) -> int:
    try:
        return int(text, 0)
    except ValueError as error:
        raise SourceError(line, f"invalid number {text}") from error


@dataclass
class Token:
    kind: str          # "open", "close", "stmt", "label"
    text: str
    line: int


def _tokenize(text: str) -> list[Token]:
    tokens = []
    buffer = []
    start_line = 1
    depth = 0
    quoted = False
    for number, original in enumerate(text.splitlines(), 1):
        for char in _strip_comment(original) + "\n":
            if not buffer and char.isspace():
                continue
            if not buffer:
                start_line = number
            if char == '"':
                quoted = not quoted
            if quoted:
                buffer.append(char)
                continue
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            if depth == 0 and char in "{};":
                content = "".join(buffer).strip()
                if char == "{":
                    tokens.append(Token("open", content, start_line))
                elif char == "}":
                    if content:
                        raise SourceError(start_line, f"missing ';' after {content}")
                    tokens.append(Token("close", "", start_line))
                else:
                    tokens.append(Token("stmt", content, start_line))
                buffer = []
                continue
            if depth == 0 and char == ":" and re.fullmatch(IDENT, "".join(buffer).strip()):
                tokens.append(Token("label", "".join(buffer).strip(), start_line))
                buffer = []
                continue
            buffer.append(char)
    if "".join(buffer).strip():
        raise SourceError(start_line, "unterminated statement")
    return tokens


class Parser:
    def __init__(self, text: str):
        self.tokens = _tokenize(text)
        self.position = 0

    def peek(self) -> Token | None:
        return self.tokens[self.position] if self.position < len(self.tokens) else None

    def next(self) -> Token:
        token = self.peek()
        if token is None:
            raise SourceError(None, "unexpected end of source")
        self.position += 1
        return token

    def expect_open(self, pattern: str) -> re.Match:
        token = self.next()
        match = re.fullmatch(pattern, token.text) if token.kind == "open" else None
        if match is None:
            raise SourceError(token.line, f"expected '{pattern} {{', found {token.text or token.kind}")
        return match

    def block(self) -> list[Token]:
        """Consume tokens up to the matching close brace (already opened)."""
        depth = 1
        content = []
        while True:
            token = self.next()
            if token.kind == "open":
                depth += 1
            elif token.kind == "close":
                depth -= 1
                if depth == 0:
                    return content
            content.append(token)


@dataclass
class SourceBlock:
    labels: list[str]
    statements: list[Token]
    section: str
    data: bool = False


class Frontend:
    def __init__(self) -> None:
        self.variables: dict[str, int] = {}
        self.new_variables: list[tuple[str, int]] = []
        self.raw_slots: set[int] = set()

    # ---- values

    def variable(self, text: str, line: int) -> int:
        text = text.strip()
        if match := re.fullmatch(rf"slot\(({NUMBER})\)", text):
            value = _int(match.group(1), line)
            if not 0 <= value <= 0x7FFF:
                raise SourceError(line, "slot() offsets are limited to 0..0x7FFF")
            self.raw_slots.add(value)
            return value
        if text in self.variables:
            return self.variables[text]
        raise SourceError(line, f"unknown variable {text}")

    def is_variable(self, text: str) -> bool:
        text = text.strip()
        return text in self.variables or re.fullmatch(rf"slot\({NUMBER}\)", text) is not None

    def number(self, text: str, line: int, low: int, high: int) -> int:
        text = text.strip()
        if not re.fullmatch(NUMBER, text):
            raise SourceError(line, f"expected a number, found {text}")
        value = _int(text, line)
        if not low <= value <= high:
            raise SourceError(line, f"value {text} is outside {low}..{high}")
        return value

    def encode_operand(self, kind: str, text: str, line: int) -> tuple[int, bool]:
        """Return (raw field value, immediate flag)."""
        if kind == "v15":
            if self.is_variable(text):
                return self.variable(text, line), False
            return 0x8000 | self.number(text, line, 0, 0x7FFF), True
        if kind == "typed":
            if self.is_variable(text):
                return self.variable(text, line), False
            return self.number(text, line, -0x8000, 0xFFFF) & 0xFFFF, True
        if kind in {"dest", "var"}:
            return self.variable(text, line), False
        if kind == "s8":
            return self.number(text, line, -128, 127) & 0xFF, True
        if kind in {"u8", "entity"}:
            return self.number(text, line, 0, 0xFF), True
        if kind == "u16":
            return self.number(text, line, 0, 0xFFFF), True
        raise SourceError(line, f"operand kind {kind} cannot be written directly")

    # ---- statements

    def instruction(self, opcode: int, args: list[str], keywords: dict[str, str], line: int,
                    condition: int | None = None) -> tuple[bytes, list[tuple[int, str]]]:
        spec = SPECS[opcode]
        values = []
        fixups = []
        mask = 0
        mask_index = None
        offset = 1
        arguments = list(args)
        for operand in spec.operands:
            size = FIELD_SIZES[operand.kind]
            if operand.kind == "mask":
                mask_index = len(values)
                values.append(0)
            elif operand.kind == "label":
                if not arguments:
                    raise SourceError(line, f"{spec.qualified}: missing {operand.name}")
                target = arguments.pop(0).strip()
                if re.fullmatch(NUMBER, target):
                    values.append(self.number(target, line, 0, 0xFFFF))
                elif re.fullmatch(IDENT, target):
                    values.append(0)
                    fixups.append((offset, target))
                else:
                    raise SourceError(line, f"invalid jump target {target}")
            elif operand.kind == "packed":
                if len(arguments) < 2:
                    raise SourceError(line, f"{spec.qualified}: expected entry and slot state")
                entry = self.number(arguments.pop(0), line, 0, 31)
                state = self.number(arguments.pop(0), line, 0, 7)
                values.append(entry | state << 5)
            elif operand.name == "reserved":
                values.append(self.number(keywords.pop("reserved", "0"), line, 0, 0xFF))
            else:
                if not arguments:
                    raise SourceError(line, f"{spec.qualified}: missing {operand.name}")
                value, immediate = self.encode_operand(operand.kind, arguments.pop(0), line)
                if operand.kind == "typed" and immediate:
                    mask |= operand.bit
                values.append(value)
            offset += size
        if arguments:
            raise SourceError(line, f"{spec.qualified}: too many arguments")
        reserved = self.number(keywords.pop("reserved_flags", "0"), line, 0, 0xFF)
        if keywords:
            raise SourceError(line, f"{spec.qualified}: unknown keyword {', '.join(keywords)}")
        if mask_index is not None:
            derivable = sum(operand.bit for operand in spec.operands if operand.kind == "typed")
            selector_bits = 0x0F if opcode == 0x02 else 0
            if reserved & (derivable | selector_bits):
                raise SourceError(line, "reserved_flags overlaps operand type or comparison bits")
            values[mask_index] = mask | reserved | (condition or 0)
        elif reserved:
            raise SourceError(line, f"{spec.qualified} has no type mask for reserved_flags")
        return encode(Instruction(opcode, values)), fixups

    def statement(self, token: Token) -> tuple[str, object]:
        """Return ("op", (raw, fixups)) or ("fallthrough", label)."""
        text = re.sub(r"\s+", " ", token.text).strip()
        line = token.line
        atom = rf"(?:{IDENT}(?:\.{IDENT})?|slot\({NUMBER}\)|{NUMBER})"
        if text == "stop":
            return "op", self.instruction(0x00, [], {}, line)
        if match := re.fullmatch(rf"goto ({IDENT}|{NUMBER})", text):
            return "op", self.instruction(0x01, [match.group(1)], {}, line)
        if match := re.fullmatch(rf"fallthrough ({IDENT})", text):
            return "fallthrough", match.group(1)
        if match := re.fullmatch(rf"if \(!\((.+)\)\) goto ({IDENT}|{NUMBER})", text):
            left, right, selector, keywords = self.condition(match.group(1), line, atom)
            return "op", self.instruction(0x02, [left, right, match.group(2)], keywords, line, selector)
        if match := re.fullmatch(rf"({atom}) = (true|false)", text):
            return "op", self.instruction(0x07 if match.group(2) == "true" else 0x08, [match.group(1)], {}, line)
        if match := re.fullmatch(rf"({atom}) ?(\+\+|--)", text):
            return "op", self.instruction(0x0D if match.group(2) == "++" else 0x0E, [match.group(1)], {}, line)
        if match := re.fullmatch(rf"({atom}) = random\(\)", text):
            return "op", self.instruction(0x14, [match.group(1)], {}, line)
        if match := re.fullmatch(rf"({atom}) = random\(({NUMBER})\)", text):
            return "op", self.instruction(0x15, [match.group(2), match.group(1)], {}, line)
        if match := re.fullmatch(rf"({atom}) &= ~({atom})", text):
            return "op", self.instruction(0x0C, [match.group(1), match.group(2)], {}, line)
        if match := re.fullmatch(rf"({atom}) (<<=|>>=) ({atom})", text):
            return "op", self.instruction(0x12 if match.group(2) == "<<=" else 0x13, [match.group(1), match.group(3)], {}, line)
        if match := re.fullmatch(rf"({atom}) (=|\+=|-=|\|=|&=|\^=|\*=|/=) ({atom})", text):
            opcode = {"=": 0x06, "+=": 0x09, "-=": 0x0A, "|=": 0x0B, "&=": 0x0F, "^=": 0x11, "*=": 0x16, "/=": 0x17}[match.group(2)]
            if not self.is_variable(match.group(1)):
                raise SourceError(line, f"assignment destination {match.group(1)} is not a variable")
            return "op", self.instruction(opcode, [match.group(1), match.group(3)], {}, line)
        if match := re.fullmatch(rf"({IDENT}\.{IDENT}) ?\((.*)\)", text):
            spec = BY_NAME.get(match.group(1))
            if spec is None:
                raise SourceError(line, f"unknown operation {match.group(1)}")
            args, keywords = self.arguments(match.group(2), line)
            if spec.opcode == 0x02:
                raise SourceError(line, "use 'if (!(condition)) goto label;' for flow.jump_unless")
            return "op", self.instruction(spec.opcode, args, keywords, line)
        raise SourceError(line, f"unrecognized statement: {text}")

    def arguments(self, text: str, line: int) -> tuple[list[str], dict[str, str]]:
        items = []
        depth = 0
        current = ""
        for char in text:
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            if char == "," and depth == 0:
                items.append(current.strip())
                current = ""
            else:
                current += char
        if current.strip():
            items.append(current.strip())
        args, keywords = [], {}
        for item in items:
            if match := re.fullmatch(rf"({IDENT}) ?= ?(.+)", item):
                keywords[match.group(1)] = match.group(2)
            elif keywords:
                raise SourceError(line, "positional argument after keyword argument")
            else:
                args.append(item)
        return args, keywords

    def condition(self, text: str, line: int, atom: str) -> tuple[str, str, int, dict[str, str]]:
        text = text.strip()
        if match := re.fullmatch(r"(compare|mask_contains|mask_lacks) ?\((.*)\)", text):
            args, keywords = self.arguments(match.group(2), line)
            if match.group(1) == "compare":
                if len(args) != 3:
                    raise SourceError(line, "compare(left, right, selector) needs three arguments")
                return args[0], args[1], self.number(args[2], line, 0, 15), keywords
            if len(args) != 2 or keywords:
                raise SourceError(line, f"{match.group(1)}(mask, entity) needs two arguments")
            selector = next(key for key, name in COMPARISON_FUNCTIONS.items() if name == match.group(1))
            return args[0], args[1], selector, {}
        for selector, operator in sorted(COMPARISONS.items(), key=lambda item: -len(item[1])):
            if match := re.fullmatch(rf"({atom}) ?{re.escape(operator)} ?({atom})", text):
                return match.group(1), match.group(2), selector, {}
        raise SourceError(line, f"unsupported condition {text}")


def compile_xgs(text: str) -> Assembly:
    parser = Parser(text)
    header = parser.expect_open(r"battle_event\s+(\d+)")
    del header
    frontend = Frontend()
    prefix = bytes(SCRIPT_PREFIX_SIZE)
    entities: dict[int, tuple[dict[int, tuple[object, int]], list[Token]]] = {}
    shared: list[Token] = []
    data: list[tuple[str, bytes, int]] = []
    aliases: dict[str, tuple[str, int]] = {}
    declared: dict[int, str] = {}
    while (token := parser.peek()) is not None and token.kind != "close":
        token = parser.next()
        if token.kind == "stmt":
            if match := re.fullmatch(r'prefix\s+"([0-9A-Fa-f ]*)"', token.text):
                prefix = bytes.fromhex(match.group(1))
                if len(prefix) != SCRIPT_PREFIX_SIZE:
                    raise SourceError(token.line, f"prefix must contain {SCRIPT_PREFIX_SIZE} bytes")
            elif match := re.fullmatch(rf"alias\s+({IDENT})\s*=\s*({IDENT})\s*\+\s*({NUMBER})", token.text):
                aliases[match.group(1)] = (match.group(2), _int(match.group(3), token.line))
            else:
                raise SourceError(token.line, f"unexpected statement {token.text}")
        elif token.kind == "open" and token.text == "state":
            _parse_state(parser, frontend, declared)
        elif token.kind == "open" and token.text == "entities":
            while parser.peek() is not None and parser.peek().kind != "close":
                match = parser.expect_open(r"entity\s+(\d+)")
                entity = int(match.group(1))
                if entity in entities:
                    raise SourceError(parser.tokens[parser.position - 1].line, f"duplicate entity {entity}")
                entities[entity] = _parse_entity(parser)
            parser.next()
        elif token.kind == "open" and token.text == "shared_code":
            shared = parser.block()
        elif token.kind == "open" and token.text == "data":
            while parser.peek() is not None and parser.peek().kind != "close":
                match = parser.expect_open(rf"({IDENT})")
                payload = bytearray()
                for item in parser.block():
                    bytes_match = re.fullmatch(r'bytes\s+"([0-9A-Fa-f ]*)"', item.text) if item.kind == "stmt" else None
                    if bytes_match is None:
                        raise SourceError(item.line, "data objects contain only bytes statements")
                    payload += bytes.fromhex(bytes_match.group(1))
                if not payload:
                    raise SourceError(None, f"data object {match.group(1)} is empty")
                data.append((match.group(1), bytes(payload), parser.tokens[parser.position - 1].line))
            parser.next()
        else:
            raise SourceError(token.line, f"unexpected section {token.text or token.kind}")
    parser.next()
    if parser.peek() is not None:
        raise SourceError(parser.peek().line, "content after the battle_event block")
    if sorted(entities) != list(range(len(entities))) or not entities:
        raise SourceError(None, "entity IDs must be consecutive from zero")

    # Allocate new variables after every explicit binding and raw slot.
    reserved = set(declared) | frontend.raw_slots | set(ENGINE_VARIABLES)
    allocations = {}
    for name, line in frontend.new_variables:
        offset = next((candidate for candidate in range(NEW_VARIABLE_FIRST, VARIABLE_BANK_END, 2)
                       if candidate not in reserved), None)
        if offset is None:
            raise SourceError(line, "no free Battle Event variable remains")
        reserved.add(offset)
        frontend.variables[name] = offset
        allocations[name] = f"0x{offset:04X}"

    blocks: list[SourceBlock] = []
    for entity in range(len(entities)):
        blocks.extend(_split_blocks(entities[entity][1], f"entity {entity}"))
    blocks.extend(_split_blocks(shared, "shared_code"))
    for name, payload, line in data:
        blocks.append(SourceBlock([name], [Token("data", payload.hex(), line)], "data", True))

    # Deterministic placement: generated names keep their numeric order; a
    # user-named block follows the block preceding it in the source.
    keyed = []
    previous = (-1, 0)
    for sequence, block in enumerate(blocks):
        generated = re.fullmatch(r"[LD]_([0-9A-F]{4})", block.labels[0]) if block.labels else None
        key = (int(generated.group(1), 16), 0) if generated else (previous[0], previous[1] + 1)
        keyed.append((key, sequence, block))
        previous = key
    keyed.sort(key=lambda item: (item[0], item[1]))
    ordered = [block for _, _, block in keyed]

    records: list[Record] = []
    end_labels: list[str] = []
    generated_jumps = 0
    known_labels = {label for block in ordered for label in block.labels} | set(aliases)
    for index, block in enumerate(ordered):
        labels = list(block.labels)
        if block.data:
            records.append(Record("data", bytes.fromhex(block.statements[0].text), labels=labels, line=block.statements[0].line))
            continue
        if not block.statements:
            if index + 1 < len(ordered):
                ordered[index + 1].labels[:0] = labels
            else:
                end_labels.extend(labels)
            continue
        for position, token in enumerate(block.statements):
            kind, payload = frontend.statement(token)
            last = position == len(block.statements) - 1
            if kind == "fallthrough":
                if not last:
                    raise SourceError(token.line, "fallthrough must end its block")
                if payload not in known_labels:
                    raise SourceError(token.line, f"unresolved label {payload}")
                following = ordered[index + 1].labels if index + 1 < len(ordered) else []
                if payload not in following:
                    raw, fixups = frontend.instruction(0x01, [payload], {}, token.line)
                    records.append(Record("op", raw, fixups, labels, line=token.line, inserted=True))
                    labels = []
                    generated_jumps += 1
                continue
            raw, fixups = payload
            records.append(Record("op", raw, fixups, labels, line=token.line))
            labels = []
            if last and not SPECS[raw[0]].terminal:
                raise SourceError(token.line, f"block {block.labels[0] if block.labels else ''} ends without stop, goto or fallthrough")

    # Entity entry rows; unassigned entries share one generated empty event.
    rows = []
    empty_label = "__empty_event"
    need_empty = False
    for entity in range(len(entities)):
        bindings = entities[entity][0]
        row = []
        for slot in range(ENTRIES_PER_ENTITY):
            if slot in bindings:
                target, line = bindings[slot]
                if isinstance(target, str) and target not in known_labels:
                    raise SourceError(line, f"unresolved event target {target}")
                row.append(target)
            else:
                row.append(empty_label)
                need_empty = True
        rows.append(row)
    if need_empty:
        records.append(Record("op", bytes([0x00]), labels=[empty_label], inserted=True))
    report = {"allocated_variables": allocations, "generated_empty_event": need_empty}
    return Assembly(prefix, rows, records, aliases, end_labels, report)


def _parse_state(parser: Parser, frontend: Frontend, declared: dict[int, str]) -> None:
    while parser.peek() is not None and parser.peek().kind != "close":
        group = parser.expect_open(r"(engine|script)").group(1)
        for token in parser.block():
            if token.kind != "stmt":
                raise SourceError(token.line, "state groups contain declarations")
            if match := re.fullmatch(rf"signed\s+({IDENT})\s+at\s+({NUMBER})", token.text):
                offset = _int(match.group(2), token.line)
                if offset & 1 or not 0 <= offset < VARIABLE_BANK_END:
                    raise SourceError(token.line, "variable bindings are even offsets in 0x0000..0x03FE")
                if offset in declared:
                    raise SourceError(token.line, f"offset 0x{offset:04X} is already bound to {declared[offset]}")
                qualified = f"{group}.{match.group(1)}"
                if qualified in frontend.variables:
                    raise SourceError(token.line, f"duplicate variable {qualified}")
                declared[offset] = qualified
                frontend.variables[qualified] = offset
            elif match := re.fullmatch(rf"new\s+signed\s+({IDENT})", token.text):
                if group != "script":
                    raise SourceError(token.line, "new variables belong to the script group")
                qualified = f"script.{match.group(1)}"
                if qualified in frontend.variables or any(name == qualified for name, _ in frontend.new_variables):
                    raise SourceError(token.line, f"duplicate variable {qualified}")
                frontend.new_variables.append((qualified, token.line))
            else:
                raise SourceError(token.line, f"invalid declaration {token.text}")
    parser.next()


_SLOT_NAMES = {"start": 0, "idle": 1}


def _parse_entity(parser: Parser) -> tuple[dict[int, tuple[object, int]], list[Token]]:
    bindings: dict[int, tuple[object, int]] = {}
    code: list[Token] = []
    while parser.peek() is not None and parser.peek().kind != "close":
        token = parser.next()
        if token.kind == "open" and token.text == "events":
            for item in parser.block():
                match = re.fullmatch(rf"(start|idle|entry\[(\d+)(?:\.\.(\d+))?\])\s*->\s*({IDENT}|{NUMBER})", item.text)
                if item.kind != "stmt" or match is None:
                    raise SourceError(item.line, f"invalid event binding {item.text}")
                if match.group(1) in _SLOT_NAMES:
                    slots = [_SLOT_NAMES[match.group(1)]]
                else:
                    first = int(match.group(2))
                    last = int(match.group(3) or first)
                    slots = list(range(first, last + 1))
                target_text = match.group(4)
                target: object = _int(target_text, item.line) if re.fullmatch(NUMBER, target_text) else target_text
                for slot in slots:
                    if not 0 <= slot < ENTRIES_PER_ENTITY:
                        raise SourceError(item.line, f"entry {slot} is outside 0..{ENTRIES_PER_ENTITY - 1}")
                    if slot in bindings:
                        raise SourceError(item.line, f"entry {slot} is bound twice")
                    bindings[slot] = (target, item.line)
        elif token.kind == "open" and token.text == "code":
            code = parser.block()
        else:
            raise SourceError(token.line, f"unexpected entity content {token.text or token.kind}")
    parser.next()
    return bindings, code


def _split_blocks(tokens: list[Token], section: str) -> list[SourceBlock]:
    blocks: list[SourceBlock] = []
    current: SourceBlock | None = None
    for token in tokens:
        if token.kind == "label":
            if current is not None and current.statements:
                last = current.statements[-1]
                if not last.text.startswith("fallthrough ") and not _terminal_text(last.text):
                    raise SourceError(token.line, f"code before label {token.text} needs stop, goto or fallthrough")
            if current is None or current.statements:
                current = SourceBlock([], [], section)
                blocks.append(current)
            current.labels.append(token.text)
        elif token.kind == "stmt":
            if current is None:
                raise SourceError(token.line, f"{section}: statement before the first label")
            current.statements.append(token)
        else:
            raise SourceError(token.line, f"{section}: nested blocks are not allowed")
    return blocks


def _terminal_text(text: str) -> bool:
    text = text.strip()
    return text == "stop" or text.startswith("goto ") or text.startswith("flow.end_script") or text.startswith("flow.jump(")
