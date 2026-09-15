# Relocation Evidence And Validation

## Executable References

The size-changing linker was checked against the USA Field overlay in Ghidra
and the opcode/scheduler references
under `docs/xenogears/field` in the repository. The relevant handler behavior is:

| Handler | Encoded operation | Address calculation |
|---|---|---|
| `0x800A1E74` | `01` | Absolute `u16` PC at `+1` |
| `0x800A1BD0` | `02` | Absolute false-target `u16` at `+6` |
| `0x800A17F4` / `0x800A1730` | `05` / `06` | Absolute target; save `PC+3` / `PC+5` |
| `0x80093CD0` | `48` | Read bytecode byte at `(base(+1) + value(+5)) & 0xFFFF` |
| `0x80093D48` | `49` | Read bytecode halfword at the same sum; byte `+7` controls signed reconstruction |
| `0x80097410` | `A6` | `PC += 3 + 3 * evaluated_count` |
| `0x8008FC4C` | `9A` | Camera mode 1 with duration zero advances twice by 3; other completing paths advance once |
| `0x8009C01C` / `0x8009BF8C` | `D4` / `FC` | Normal dialogue completion advances 5; absent resolved actor advances 6 |
| `0x8009CDB4` | Actor selector helper | `FD..FF` resolve party mappings; `FB` resolves current actor; other selectors return their literal ID |
| `0x800ACDEC` | Evaluated operand helper | High bit selects a 15-bit immediate; otherwise read the VM variable |
| `0x8009CFBC` | Source operand of masked variable operations | Tests only control bit `0x40`; other control bits are ignored |

The dialogue common path at `0x8009C5A8` adds 4 after `D4` has already advanced
over its actor selector, confirming the five-byte successful path. Only party
selectors can resolve to the absent sentinel under normal actor counts; literal
selectors do not need the alternate landing veneer.

These facts distinguish address operands from evaluated values. In particular,
moving a `49` data table changes its base, not its dynamic index; preserving
an A6 index requires retaining three-byte physical dispatch slots.

The control-byte rule was checked at all relevant call sites: `35`/`8009D9A4`,
`38`/`8009D890`, `39`/`8009D804`, `3A`/`8009D644`, `3B`/`8009D408`,
`3E`/`8009D5B8`, `3F`/`8009D52C`, `40`/`8009D4A0`, `DE`/`8009D6D8` and
`DF`/`8009D768`. They read the destination directly and use `8009CFBC` for the
source. XGS can normalize ignored control bits while retaining the opcode,
operands and immediate/variable mode. XGA still preserves the exact encoding.

## Generated Routing Code

- Enlarged A6 cases retain one three-byte jump per table entry and put their
  edited bodies in an out-of-line region.
- An immediate A6 whose relocated distance no longer fits a triplet count is
  equivalent to one three-byte absolute jump: neither form writes VM state.
- A changed `9A` relative span receives normal and alternate jumps at `+3/+6`.
- A changed `D4`/`FC` span receives the shared bytes `01 01 HH LL` at `+5`.
  Starting at `+5` jumps to `HH01`; starting at `+6` jumps to `LLHH`. Aligned
  three-byte islands route those addresses to the two real continuations. No
  scratch variable or extra call-stack entry is required.

The generated jumps add dispatches on affected paths, and are visible in the
assembly. The linker does not conceal them as if they were zero-cost operations.
Alignment records survive assembly export/reimport and are recomputed after
further edits. Overlapping relocation fields must agree on every shared byte.
The decompiler recognizes the two-jump signature and its routing islands. XGS
represents it as `landing(normal, alternate)` in a named data object; XGA retains
explicit overlapping fixups and alignment constraints. Tests re-decompile a modified binary,
rebuild it exactly, then grow it again and check both landing paths.

## Fast Checks And Optional Corpus Regression

From the bundle directory:

```bash
python3 -m pytest tests -q
python3 tools/field_script.py compile script.xgs \
  --output scripts.modified.bin --xga script.modified.xga --map script.map.json
```

Normal compilation reads one script and does not run the corpus regression.
The following is a separate, potentially lengthy development check:

```bash
python3 tools/field_script.py verify extracted-field-scripts --repack --resize
```

The corpus check validates both unchanged and resized resources. For resizing,
it inserts a NOP before every decoded instruction, transfers entry labels to
those inserted instructions, links the program, then verifies:

1. Every original opcode and non-address operand survives (one-byte prefix
   no-ops have their separately documented equivalent lowering).
2. Every encoded symbolic reference points to its linked label.
3. Reassembling the exported linked XGA reproduces the resized binary exactly.
4. Repacking and extracting the original Field container recovers the resized
   script, accounting for any LZSS trailer.

Recorded completed implementation runs passed **729/729** unchanged resources
and **729/729** resized resources.
The original ScriptsFiles total **4,453,533 bytes**; the resized ones total
**5,126,213 bytes**. All corresponding original and resized containers pass
repacking validation. Focused tests additionally exercise insertion, deletion,
different-size opcode/mode replacement, data/arrival growth, table body growth,
both relative-handler paths, repeated edits of aligned landing islands, changed
routine-row counts and arbitrary LZSS payload lengths. These totals describe
those completed runs, not an automatic certification of later revisions. After
the final entity-grouped presentation change, focused checks covered Fields 0,
70, 211 and 305 plus the fast unit suite. The later exhaustive run was stopped;
it is not counted as a completed validation.

Current XGS comparisons allow only the verified masked-variable control-bit
normalization above, confined to decoded instructions. Headers, bitmap, rows,
operand values and preserved data remain exact in that comparison. Tests check
both operand modes and reject changes to significant bits or data bytes.

`--map` emits a per-edit diagnostic artifact: final size, symbol offsets,
source-line mapping, allocated variables, defaulted event slots and generated
routing code. Original-address maps apply to inputs that actually carry such
addresses, such as XGA; standalone XGS does not require them. Behavioral testing
of an authored gameplay change remains a game
test, distinct from proving relocation and binary reconstruction.
