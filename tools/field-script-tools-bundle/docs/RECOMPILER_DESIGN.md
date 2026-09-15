# Field Script Recompiler Design

## 1. Implemented Architecture

The toolchain separates editable source, lossless assembly and game injection:

```text
script.xgs -> symbolic IR -> scripts.bin -> compressed Field section 5
                    |                              |
                    +-> script.xga / link map       +-> runtime override
```

XGS is standalone. Compilation never matches source lines to an old binary or
looks up original instruction positions in a sidecar. The linker assigns current
addresses from the parsed program. Entity event bindings and entity-owned code
are displayed together; shared code is emitted once.

| Module | Responsibility |
|---|---|
| `decompile_field_scripts.py` | Bytecode analysis, symbols, ownership and low-level rendering helpers |
| `editable_field_scripts.py` | Standalone XGS rendering/parsing, entity grouping, variables and event defaults |
| `field_instruction_codec.py` | Shared operand forms, encoding and verified encoding equivalences |
| `compile_field_scripts.py` | Assembly IR, XGA parser/serializer and ScriptsFile construction |
| `field_linker.py` | Placement, fixups, fallthroughs and generated routing code |
| `repack_field_scripts.py` | LZSS, section replacement and indexed-file override packages |
| `field_script.py` | CLI, regeneration and regression checks |

All runtime tooling uses Python 3.11+ and the standard library. See
[`RECOMPILER_USAGE.md`](RECOMPILER_USAGE.md) for commands and editing examples.

## 2. Source Information Versus Provenance

The source retains information with actual VM meaning:

- Variable types and explicit bindings to existing VM slots.
- Entity IDs, 32-slot routine tables and symbolic entry labels.
- Operation operands and control-flow destinations.
- Arrival records, preserved data and explicit symbolic data references.

New scene variables can request allocation with `new`. Unnamed unsigned slots
are represented by `unsigned slots[...]`, preserving bitmap meaning without
embedding a historical bitmap blob in the source.

Historical PCs and bytes are comments (`// PC: bytes`), as are original event
aliases (`// event: ...`). They are useful for inspection but ignored by the
compiler. Handler descriptions stay in the operation catalog.

The extraction manifest and JSON reports describe the input assets and disc
routes. They are inputs to regeneration/corpus verification, not compilation.

## 3. Semantic Encoding And Exact Assembly

The front end re-encodes each statement using shared `InstructionForm` and
`Operand` schemas. These cover encoded bytes/words, signed immediates, variable
references, tagged evaluated operands, masked operands, actor selectors, packed
bit/routine fields and address references. Typed destinations are checked,
including fixed implicit VM outputs.

The encoded instruction is decoded and rendered again to check that it represents
the requested form and has a valid size. Unclassified fields remain explicit
bytes rather than being guessed from an operation name or a trailing `80`.

XGS prioritizes readable semantics over irrelevant encoding differences. For
opcodes `35`, `38`, `39`, `3A`, `3B`, `3E`, `3F`, `40`, `DE` and `DF`, executable
inspection confirms that only bit `0x40` of the control byte is read by the
shared source-operand helper. Canonicalizing other bits is permitted. The opcode,
destination, source value/reference and immediate-versus-variable selection
must still match. This exemption is never applied to arbitrary data bytes.

For example, both `35 3A 04 01 00 40` and `35 3A 04 01 00 C0` express assignment
of immediate `1` to VM slot `0x043A`; both are rendered as an assignment, not raw
instructions. Likewise, `38 10 04 12 04 00` is a variable-to-variable addition.

Other distinctions are preserved where meaningful: `49` exposes unsigned versus
signed reads as `state.read_script_u16` / `state.read_script_s16`; `5B` is
`movement.park_actor_movement_update()`, not an interchangeable inert stall.
An encoding without a verified semantic equivalent remains an explicit raw
operation. Address-bearing raw forms include symbolic target arguments.

XGA is the byte-exact representation. It contains the full bitmap, routine rows,
instruction encodings, data and optional original address assertions. Exact
assembly is an XGA mode; ordinary XGS compilation is always symbolic.

## 4. Entities, Variables And Validation

Entity IDs are contiguous from zero. Each entity has 32 routine slots; the
engine-assigned roles occupy 0–3. Unbound slots receive a generated empty event.
Aliases are preserved rather than expanded into duplicate bodies.

Existing variables retain their VM bindings. New scene variables are allocated
after reserving declared slots and possible direct/packed variable references
in the source, including opaque operands. Temporary analysis addresses are never
emitted. Persistent storage requires an explicit or documented engine binding.

Validation rejects unknown syntax, duplicate symbols/bindings, unresolved
references, invalid selectors, unencodable values, exhausted variable space,
conflicting fixups and missing continuations. Diagnostics include source lines
where applicable. This is structural and encoding validation, not proof that
an authored event has the intended gameplay or valid external resource IDs.

## 5. Placement And Control Flow

The parser builds internal fragments from labels and source continuations.
Grouped sources use deterministic symbol-based ordering; label names are never
interpreted as mandatory addresses. Within each fragment, instruction order is
preserved. Renaming or inserting code does not require editing historical PCs.

The linker resolves routine rows, primary/extended branches, calls and the
script-data bases of `48`/`49`. Their evaluated byte index is a value, not a
pointer. Symbolic aliases may describe byte-relative locations inside data.

`fallthrough label;` expresses a continuation independently of presentation
order. It disappears when physically adjacent, or becomes a jump. The linker
does not silently discard an authored wait or duplicate shared event bodies.

XGS computed dispatch explicitly names its case slots and targets. The compiler
creates the fixed three-byte jumps, while case bodies can grow separately. The
low-level XGA linker also supports enlarging existing triplet bodies via veneers.
A fixed `skip_triplets_to` resolves to an immediate A6 count or an equivalent
absolute jump, accounting for the VM's 16-bit PC arithmetic.

Some handlers have hardcoded relative alternatives. XGS exposes these with
`otherwise goto`. `9A` can complete at `PC+3` or `PC+6`; `D4`/`FC` can complete at
`PC+5` or `PC+6`. Routing code preserves the requested paths when edits change
their relative distance. For the one-byte-separated dialogue entries:

```text
PC+5: 01 01 HH LL     -> jump HH01
PC+6:    01 HH LL     -> jump LLHH
```

Aligned islands route to the real continuations without scratch variables or
call-stack changes. A decompiled `landing(normal, alternate)` data object
expresses these constraints symbolically; the linker derives alignment rather
than requiring saved addresses. Consistent overlapping fixups are supported,
including forward alignment dependencies.

Generated routing jumps consume VM dispatches. They are visible in XGA and the
link report. One-byte FE no-ops are replaced by an equivalent primary no-op if
an insertion would otherwise change the meaning of their lookahead.

The retail VM has a 65,536-byte code address space. Overflow is reported, never
truncated. XGA exact mode separately checks fixed positions and full coverage.

## 6. Binary Construction And Repacking

```text
0x0000  variable signedness bitmap       0x80 bytes
0x0080  routine-row count                u32
0x0084  32-entry routine rows            count * 0x40 bytes
...     shared code and data
```

The builder validates the result with the extractor's structural parser.
Compression uses distances 1–4095, lengths 3–18 and complete eight-token groups.
Matches can be split into literals to preserve the exact expanded payload. If
necessary, at most seven zero trailer bytes are included in the expanded size;
the strict compression API instead rejects such padding.

The repacker replaces section 5, updates its size and section 6–8 offsets,
preserves other payloads, and checks extraction of the result. Routine-row counts
may change independently of placement-record counts. The override command uses
the port's existing format-6 indexed-file handler and authenticated stock files.

## 7. Verification Scope

- XGA round trips compare complete files byte-for-byte.
- XGS reconstruction compares complete files, permitting only the verified
  instruction-bit normalizations from section 3.
- LZSS and repacking checks compare the compiled payload plus any accounted-for
  compression trailer.
- Tests cover edits, new variables/events, label resolution, data reads, triplet
  tables, alternate paths, alignment, raw operands, CLI workflows and comments.

`verify --resize --repack` is an explicit whole-corpus regression, not part of
normal compilation. Recorded full-corpus runs and executable evidence are in
[`RELOCATION_EVIDENCE.md`](RELOCATION_EVIDENCE.md). Later targeted checks should
not be presented as a rerun of that exhaustive suite. Gameplay behavior still
needs to be tested after installing an authored modification.
