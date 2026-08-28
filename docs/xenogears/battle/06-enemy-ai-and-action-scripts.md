# Enemy AI And Action Scripts

## 1. Two Battle Languages

Battle uses two script systems:

| System | Stored in | Purpose |
|---|---|---|
| Enemy AI/action script | The selected enemy-set definition file, commonly called `battle.bin` | Select targets, inspect and change combat state, and build enemy actions. |
| Battle Event bytecode | Battle Event's script resource | Run scripted battle scenes, dialogue, music, sprites, cameras, and transitions. |

This chapter covers the enemy AI/action VM. The Battle Event VM is documented
in [`07-battle-event-vm.md`](07-battle-event-vm.md).

All multibyte values are little-endian. Every enemy instruction occupies four
bytes. The VM owns persistent per-enemy byte registers, word registers, and
dword registers.
Ordinary register indices receive their meaning from each
enemy's scripts, in the same way that Field VM variables receive a purpose from
the field script using them. A small set of reaction slots has an engine-defined
meaning because Battle writes those slots immediately before running an
attacked program.

## 2. Enemy-Set Definition File

The selected encounter's enemy-set definition file contains definitions,
strings, and scripts in one blob:

| Offset | Size | Meaning |
|---:|---:|---|
| `0x00` | `8 * 2` | `u16 script_table_offset[8]`, one per enemy definition selector. |
| `0x10` | `0x20` | Zero-filled structural gap between the selector offsets and string offset. |
| `0x30` | 2 | Blob-relative offset to battle-specific strings. |
| `0x32` | `8 * 0x170` | Eight serialized enemy combat definitions, ending at `0xBB1`. |
| `0xBB2` | variable | Start of the selector script blocks. Each block begins with four `u16` program offsets followed by four-byte instruction streams. |
| `u16(+0x30)` | to blob end | Enemy names and battle strings. |

For an occupied enemy battle slot, `BattleLoaderLoadEnemyDefinitions`
(`0x801E4870`) uses definition selector `d`:

```text
script_table = definition_file + u16(definition_file + d * 2)

turn_program           = script_table + u16(script_table + 0)
clone_retained_pointer = script_table + u16(script_table + 2)
reaction_program       = script_table + u16(script_table + 4)  when present
end_turn_program       = script_table + u16(script_table + 6)  when present

enemy_definition = definition_file + 0x32 + d * 0x170
```

The first-level offset is relative to the definition-file base. Each entry
offset is relative to its selected four-entry script table. Entries 2 and 3 use
`0xFFFF` as the absence sentinel and have runtime presence flags. Entries 0 and
1 are relocated directly.

Selector block `d` ends at selector block `d + 1`; block 7 ends at the string
offset from `+0x30`. At top-level dispatch boundaries, program terminators
delimit active instruction streams inside each block.

The four entries have distinct lifecycles:

| Entry | Role | Runtime lifecycle |
|---:|---|---|
| 0 | Ordinary-turn action program | Selected for every ordinary enemy turn. |
| 1 | Clone-retained pointer | Stored with the definition and copied from the source enemy to the destination enemy by action type `0x07`; its runtime lifecycle consists of retention and propagation by cloning. |
| 2 | Attacked/counter reaction program | Selected after an attack resolves against an enemy whose entry-2 presence flag is set. |
| 3 | End-of-turn program | Selected during end-of-turn processing when both entry-3 presence and per-enemy eligibility are set. |

Action type `0x07` copies all four pointer fields and the entry-2/entry-3
presence flags into the destination enemy slot.

At top-level dispatch boundaries, terminator instructions end the selected
instruction stream. Entry 0 runs for the acting enemy. Entry 2 runs after Battle
has installed the reaction inputs described below; it can stage and immediately
execute its own action list. Entry 3 temporarily makes each eligible enemy the
current entity, runs that enemy's program and staged actions, then invokes the
Battle Event update hook. Enemy slots are Battle entity slots `3..10`, and their
script-state index is `battle_slot - 3`.

Opcode `0x62` is an entry-2 reaction signal. Its lower-opcode handler preserves
VM state, while the entry-2 runner records its occurrence and returns it through
the attack routine. The character multi-hit command path uses that return to end
the current attack sequence.

## 3. Persistent VM State

Enemy script state begins at `0x800D3400` with stride `0x40`:

```c
struct EnemyScriptState {             /* size 0x40 */
    const uint8_t *turn_program;       /* +0x00 */
    const uint8_t *clone_retained_pointer; /* +0x04 */
    const uint8_t *reaction_program;   /* +0x08 */
    const uint8_t *end_turn_program;   /* +0x0C */
    uint32_t dword_register[4];        /* +0x10 */
    uint16_t word_register[8];         /* +0x20 */
    uint8_t byte_register[16];         /* +0x30 */
};
```

The loader clears all three register arrays for each occupied enemy. The
registers then persist with that enemy slot across turn, attacked, and
end-of-turn programs.
Action type `0x07` gives a cloned enemy its source enemy's three executable
program pointers, `clone_retained_pointer`, presence flags, and combat
definition while preserving the destination slot's existing register state.

The dword registers are unsigned 32-bit storage. Opcode `0x06` converts its
integer literal to four-fractional-bit form by storing `immediate << 4` in a
dword register. Transfers also place native 32-bit values such as Gear HP and
gold in dword registers. Condition `0x94` compares them unsigned, opcode `0x6D`
divides them unsigned, and dword add, subtract, and multiply retain the low 32
bits.

### Engine-injected attacked-program slots

`BattleUpdateMonsterScriptAttackVars` (`0x80079840`) writes these slots for the
enemy that is about to receive entry-2 processing:

| VM slot | Name | Value installed by Battle |
|---|---|---|
| `word_register[7]` | `attacker_mask` | One-bit entity mask for the attacker. |
| `byte_register[9]` | `descriptor_byte20` | Current attack descriptor byte from `+0x20`; encounter scripts assign its meaning. |
| `byte_register[10]` | `descriptor_byte21` | Current attack descriptor byte from `+0x21`; encounter scripts assign its meaning. |
| `byte_register[11]` | `descriptor_flags22` | Current attack descriptor flags byte from `+0x22`. |
| `byte_register[12]` | `descriptor_byte23` | Current attack descriptor byte from `+0x23`; encounter scripts assign its meaning. |
| `byte_register[13]` | `attacker_action_index` | The attacker's selected action-record index. |
| `byte_register[14]` | `attacker_target_match` | `1` when the attack's resulting target mask contains the attacker's current target, otherwise `0`. |

The enemy script assigns encounter-specific roles to the remaining register
slots. The injected slots become ordinary register storage after installation,
so script instructions can read, overwrite, or clear them.

Before injection, Battle conditionally augments `descriptor_flags22`. When
`(descriptor_flags22 & 0x3F) == 0`, the character path performs:

```text
descriptor_flags22 |= (attacker.+0x8C | attacker.+0x8E) >> 12
```

The Gear path instead combines Gear-attacker field `+0x84` with paired
character-attacker field `+0x8A`, then shifts that result right by 12. Neither
path reads target status. This places the combined attacker high-nibble bits in
the descriptor byte's low nibble. When any of the low six descriptor bits is
already set, Battle leaves the descriptor byte unchanged.

Opcodes `0x39..0x3C` update rewards and drop fields in the live enemy record.
Entity `+0x14C` holds the 32-bit EXP reward and `+0x156` holds the 16-bit gold
reward. Entity `+0x150/+0x151` hold primary and secondary drop chances;
`+0x152/+0x154` hold the primary item ID and inventory class; and
`+0x153/+0x155` hold the secondary pair.

Other VM-facing state includes:

| Address/state | Meaning |
|---|---|
| `0x800D2E5C` | 32 eight-byte staged action records, cleared before a script pass. |
| `0x800D39E0` | Target mask retained by action type `0x05` for forced-enemy-turn processing. |
| `*(BattleEventState **)0x800D3278 + 0x394` | Battle Event word-variable bank written by opcode `0x70`. |
| `0x8005A3A0` | Shared Battle word bank accessed by opcodes `0x07`, `0x30`, and `0x31`. |

## 4. Instruction Encoding

Every instruction has this physical form:

```text
+0  u8 opcode
+1  u8 operand_1
+2  u8 operand_2
+3  u8 operand_3
```

Each opcode assigns semantic names to the three operand bytes. Most word
assignment, arithmetic, random, and condition instructions encode their word as
`operand_2 | operand_3 << 8`; opcodes `0x39` and `0x3A` use
`operand_1 | operand_2 << 8`. The tables below use `byte register`, `word
register`, and `dword register` for indices into the executing enemy's
persistent registers. `BattleAdvanceEnemyScript` (`0x80079934`) advances the
program counter by four after each dispatched instruction.

## 5. Interpreter And Conditional Blocks

`BattleExecuteMonsterTurnScript` uses this execution model:

```text
clear staged actions and per-action animation fields
pc = turn_program
action_cursor = 0

while opcode is different from 0xFD and 0xFF:
    when opcode < 0x80:
        execute lower opcode
        pc += 4
    otherwise:
        result = execute condition opcode or OR-chain
        when result is false:
            skip the associated lower-opcode body
```

Execution is synchronous and runs to a top-level terminator in one interpreter
call. Opcodes `0xFD` and `0xFF` terminate only when the outer interpreter loop
tests them at a top-level instruction boundary.
`BattleSkipEnemyScriptConditionalBlock` (`0x80079948`) applies these exact skip
loops:

```text
while opcode >= 0x80:
    pc += 4
while u8(opcode + 0x80) > 0x6F:
    pc += 4
```

The first loop consumes every remaining high-opcode record in `0x80..0xFF`,
including `0xFD` and `0xFF`. The second consumes lower-opcode body records and
values `0xF0..0xFF`, stopping at the next value in `0x80..0xEF`. Record ordering
therefore encodes the conditional structure. A skipped `0xFD` or `0xFF` does not
terminate execution.

Condition opcode `0x99` begins an OR-chain. The condition dispatcher consumes
following high opcodes and ORs their results until the next lower opcode. `0x80`
and `0x9A` produce true. High opcode values outside the condition switch retain
the dispatcher's current result and advance by one record. This includes
`0xFD` and `0xFF` while the OR-chain is active, so those values do not terminate
execution from inside the chain.

## 6. Lower Opcode Catalog

The catalog covers every explicit lower-opcode dispatch. Operand order follows
the physical bytes at `+1`, `+2`, and `+3`.

| Op | Handler | Semantic operands | Behavior |
|---:|---:|---|---|
| `01` | `0x8007A828` | action-byte offset, immediate byte | Write the immediate into the current action. Writing offset 0 commits the record by advancing the action cursor. |
| `02` | `0x8007A874` | action-byte offset, source byte register | Copy a byte register into the current action. |
| `03` | `0x8007A8B4` | source action index, destination action index | Copy all eight action bytes. |
| `04` | `0x8007A900` | destination byte register, immediate byte | Assign the immediate. |
| `05` | `0x8007A92C` | destination word register, immediate word | Assign the immediate. |
| `06` | `0x8007A968` | destination dword register, immediate word | Store `immediate << 4`. |
| `07` | `0x8007A9A8` | shared-word index, immediate byte | Zero-extend the immediate into shared Battle word `0x8005A3A0[index]`. |
| `08..0F` | `0x8007A9D0..0x8007ABA0` | in/out byte register, immediate byte | In-place add, subtract, multiply, divide, modulo, AND, OR, XOR. Add clamps at `0xFF`; subtract floors at zero. |
| `10..17` | `0x8007ABD8..0x8007ADF4` | in/out word register, immediate word | In-place add, subtract, multiply, divide, modulo, AND, OR, XOR. Add clamps at `0xFFFF`; subtract floors at zero. |
| `18..1F` | `0x8007AE38..0x8007B084` | left byte register, right byte register, destination byte register | Add, subtract, multiply, divide, modulo, AND, OR, XOR. Add clamps at `0xFF`; subtract floors at zero. |
| `20..27` | `0x8007B0C8..0x8007B360` | left word register, right word register, destination word register | Add, subtract, multiply, divide, modulo, AND, OR, XOR. Add clamps at `0xFFFF`; subtract floors at zero. |
| `28` | `0x8007B3B0` | self byte-property selector, immediate byte | Set an 8-bit combat property on the executing enemy. |
| `29` | `0x8007B3E4` | self word-property selector, immediate word | Set a 16-bit combat property on the executing enemy. |
| `2A` | `0x8007B424` | destination byte register, byte-property selector, target-mask word register | Read the selected byte property from the first entity in the mask. |
| `2B` | `0x8007B4B8` | source byte register, byte-property selector, target-mask word register | Write the selected enemy property. A party target writes type `0x20` to staged record 0 and advances the action cursor. |
| `2C` | `0x8007B578` | destination word register, word-property selector, target-mask word register | Read the selected word property from the first entity in the mask. |
| `2D` | `0x8007B608` | source word register, word-property selector, target-mask word register | Write the selected enemy property. A party target writes type `0x20` to staged record 0 and advances the action cursor. |
| `2E` | `0x8007B6C0` | destination dword register, source mode, target-mask word register | Mode 1 loads current Gear HP from the first masked entity. Mode 2 loads current gold. |
| `2F` | `0x8007B7B0` | source dword register, Gear-HP field selector, target-mask word register | Write the first selected enemy's maximum Gear HP when the selector is zero, or current Gear HP otherwise. A party target writes type `0x20` to staged record 0 and advances the action cursor. |
| `30` | `0x8007B8D4` | destination word register, shared-word index | Load from `0x8005A3A0[index]`. |
| `31` | `0x8007B914` | source word register, shared-word index | Store into `0x8005A3A0[index]`. |
| `32` | `0x8007B958` | source byte register, destination byte register | Copy a byte register. |
| `33` | `0x8007B98C` | source word register, destination word register | Copy a word register. |
| `34` | `0x8007B9C8` | source dword register, destination dword register | Copy a dword register. |
| `35` | `0x8007BA04` | source byte register, destination word register | Zero-extend byte to word. |
| `36` | `0x8007BA44` | source word register, destination dword register | Zero-extend word to 32 bits. |
| `37` | `0x8007BA88` | none | Clear all sixteen byte registers. |
| `38` | `0x8007BAB8` | none | Clear all eight word registers. |
| `39` | `0x8007BAE8` | immediate EXP reward | Zero-extend the reward into enemy `+0x14C`. |
| `3A` | `0x8007BB2C` | immediate gold reward | Set enemy `+0x156`. |
| `3B` | `0x8007BB70` | secondary inventory class, secondary item ID, secondary chance | Set enemy drop fields `+0x155`, `+0x153`, and `+0x151`. |
| `3C` | `0x8007BBD8` | primary inventory class, primary item ID, primary chance | Set enemy drop fields `+0x154`, `+0x152`, and `+0x150`. |
| `3D` | `0x8007BC40` | action-byte offset, immediate word | Write a little-endian word in the current action. |
| `3E` | `0x8007BC84` | destination byte register, inclusive maximum | Store a random integer in `0..maximum`. |
| `3F` | `0x8007BCE8` | destination word register, inclusive maximum word | Store a random integer in `0..maximum`. |
| `40` | `0x8007BD5C` | destination target-mask word register, restricted-candidate permission | Random eligible on-foot party target. |
| `41` | `0x8007BEA8` | destination target-mask word register, restricted-candidate permission | Random eligible on-foot party target in the executing enemy's row. |
| `42` | `0x8007C040` | destination target-mask word register, restricted-candidate permission | Random eligible on-foot enemy target in the executing enemy's row. |
| `43` | `0x8007C1A4` | destination target-mask word register, restricted-candidate permission | Random eligible on-foot party target in another row. |
| `44` | `0x8007C33C` | destination target-mask word register, restricted-candidate permission | Random eligible on-foot enemy target in another row. |
| `45` | `0x8007C4A0` | destination target-mask word register, restricted-candidate permission | Eligible party member with the lowest readiness timer; use party-slot-0 mask when no candidate is found. |
| `46` | `0x8007C580` | destination target-mask word register, restricted-candidate permission | Other eligible enemy with the shortest ready delay; use party-slot-0 mask when no candidate is found. |
| `47` | `0x8007C678` | destination target-mask word register, restricted-candidate permission | Eligible party member with lowest current HP; use party-slot-0 mask when no candidate is found. |
| `48` | `0x8007C75C` | destination target-mask word register, restricted-candidate permission | Eligible enemy with lowest current HP; use party-slot-0 mask when no candidate is found. |
| `49` | `0x8007C840` | destination target-mask word register, restricted-candidate permission | Random eligible Gear party member in the executing enemy's row. |
| `4A` | `0x8007C9D4` | destination target-mask word register, restricted-candidate permission | Random eligible Gear party member. |
| `4B` | `0x8007CB20` | destination target-mask word register, restricted-candidate permission | Random eligible Gear enemy. |
| `4C` | `0x8007CC50` | destination byte register, row index | Count targetable party members in the row. |
| `4D` | `0x8007CD10` | destination byte register, row index | Count targetable enemies in the row. |
| `4E` | `0x8007CDD0` | destination byte register, selected-entity target-mask word register | Load the row-pair targeting restriction for the first selected entity. |
| `4F` | `0x8007CEA4` | destination byte register, selected-entity target-mask word register | Count targetable party members in the selected entity's row. |
| `50` | `0x8007CFB8` | destination byte register, selected-entity target-mask word register | Count targetable enemies in the selected entity's row. |
| `51` | `0x8007D0CC` | destination byte register, item ID | Load inventory quantity, using zero when the item is absent. |
| `52` | `0x8007D148` | action-byte offset, source word register | Copy a little-endian word register into the current action. |
| `53` | `0x8007D1A8` | destination dword register | Load current gold. |
| `54` | `0x8007D1DC` | destination target-mask word register, required entity identity/type `+0x56` | Random active combatant whose entity `+0x56` equals the required value. |
| `55` | `0x8007D30C` | destination dword register | Load the executing enemy's current control/phase code. |
| `56` | `0x8007D344` | destination target-mask word register, restricted-candidate permission | Random targetable hidden enemy. |
| `57` | `0x8007D478` | destination target-mask word register | Random defeated, retained party member. |
| `58` | `0x8007D5B0` | destination byte register | Count party slots whose entity `+0x7C` has both lifecycle bits `0x8000` and `0x4000` clear. |
| `59` | `0x8007D610` | destination byte register | Count active visible enemies. |
| `5A` | `0x8007D6A8` | destination target-mask word register, restricted-candidate permission | Eligible Gear party member with lowest current 32-bit Gear HP; use party-slot-0 mask when no candidate is found. |
| `5B` | `0x8007D7B4` | destination target-mask word register, restricted-candidate permission | Eligible Gear enemy with the lowest current 16-bit normal HP from entity `+0x4C`; use party-slot-0 mask when no candidate is found. |
| `5C` | `0x8007D8C0` | word-property selector, destination target-mask word register, required-bits word register | Random party member whose selected property intersects the required bits. |
| `5D` | `0x8007DA1C` | word-property selector, destination target-mask word register, required-bits word register | Random enemy whose selected property intersects the required bits. |
| `5E` | `0x8007DB78` | word-property selector, destination target-mask word register, required-bits word register | Random Gear party member whose selected property intersects the required bits. |
| `5F` | `0x8007DCF8` | word-property selector, destination target-mask word register, required-bits word register | Random Gear enemy whose selected property intersects the required bits. |
| `60` | `0x8007DE78` | word-property selector, destination target-mask word register, required-bits word register | Random party member matching the bits with the permissive eligibility mode. |
| `61` | `0x8007DFD4` | word-property selector, destination target-mask word register, required-bits word register | Random Gear party member matching the bits with the permissive eligibility mode. |
| `62` | dispatch-only | none | Preserve VM state and raise the entry-2 stop-sequence return flag. |
| `63` | `0x8007E154` | self visual-state byte | Set self visual state and update formation occupancy. |
| `64` | `0x8007E1D0` | destination target-mask word register | Store the executing enemy's entity mask. |
| `65` | `0x8007E234` | destination target-mask word register, form filter | Build the active party mask. Filter 1 selects Gear, filter 2 selects on-foot, and other values select both. |
| `66` | `0x8007E334` | destination target-mask word register, form filter | Build the active enemy mask with the same filter values. |
| `67` | `0x8007E438` | destination target-mask word register, selected-entity target-mask word register | Build the party mask for the selected entity's row. |
| `68` | `0x8007E554` | destination target-mask word register, selected-entity target-mask word register | Build the enemy mask for the selected entity's row. |
| `69` | `0x8007E674` | none | Clear the executing enemy's control accumulator and set its phase code to 4. |
| `6A` | `0x8007E6A0` | left dword register, right dword register, destination dword register | Add 32-bit values. |
| `6B` | `0x8007E6F0` | left dword register, right dword register, destination dword register | Subtract 32-bit values. |
| `6C` | `0x8007E740` | in/out dword register, immediate byte multiplier | Multiply in place. |
| `6D` | `0x8007E780` | in/out dword register, immediate byte divisor | Unsigned divide in place. |
| `70` | `0x8007E7C0` | Battle Event word-variable index, immediate byte | Store the zero-extended immediate into the Battle Event variable. |
| `71` | `0x8007E7E4` | command-bit index, enabled boolean | Set or clear the command-enable bit for all eleven combatants. |
| `72` | `0x8007E8AC` | source row index, target row index, restriction byte | Set one row-pair targeting restriction. |
| `73` | `0x8007E8E0` | source target-mask word register | Make the first selected entity the next combatant. |
| `74` | `0x8007E934` | none | Recompute occupied-slot readiness intervals and countdowns, then clear ready flags and Slow half-speed parity. |

The restricted-candidate permission controls combat-state word `+0x84`, bit
`0x0020`: zero filters candidates carrying that bit, while a nonzero value
admits them. Occupancy, visibility, defeat, form, and row checks continue to
follow each opcode's listed selector family.

Opcodes `0x45` and `0x46` initialize their best readiness value to `0x00FF` and
compare candidate readiness as signed 16-bit values. Eligible candidates above
that initial threshold do not replace the slot-0 fallback. Equal best values
replace the earlier candidate, so the last tied candidate wins. Opcodes
`0x47`, `0x48`, `0x5A`, and `0x5B` likewise keep the last tied candidate.

### Property selector maps

Byte- and word-property selectors are valid in `0x00..0x17`. The accessors use
these entity-relative offsets:

| Selector | Byte property offset | Word property offset |
|---:|---:|---:|
| `00` | `+0x04` | `+0x4E` |
| `01` | `+0x56` | `+0x4C` |
| `02` | `+0xE0` | `+0x7C` |
| `03` | `+0xE3` | `+0x7E` |
| `04` | `+0x58` | `+0x80` |
| `05` | `+0x59` | `+0x82` |
| `06` | `+0x5A` | `+0x84` |
| `07` | `+0x2D` | `+0x86` |
| `08` | `+0x5D` | `+0x88` |
| `09` | `+0x5B` | `+0x8A` |
| `0A` | `+0x5C` | `+0x8C` |
| `0B` | `+0x5E` | `+0x8E` |
| `0C` | `+0x5F` | `+0x110` |
| `0D` | `+0x60` | `+0x114` |
| `0E` | `+0x61` | `+0x116` |
| `0F` | `+0x64` | `+0x120` |
| `10` | `+0x65` | `+0x122` |
| `11` | `+0x66` | `+0x124` |
| `12` | `+0x67` | `+0x126` |
| `13` | `+0x13C` | `+0x128` |
| `14` | `+0xB6` | `+0x12A` |
| `15` | `+0x140` | `+0x34` |
| `16` | `+0x141` | `+0x36` |
| `17` | `+0x142` | `+0x38` |

Selectors `0x18..0xFF` reach the accessor's final load or store without assigning
a property field address and must not be used. Lower opcodes `0x28..0x2D` and
`0x5C..0x61`, plus action types `0x08..0x0B`, share these maps.

Every other lower opcode, including `00`, `6E`, `6F`, and `75..7F`, invokes
`BattleEnemyScriptAppendRawAction` (`0x8007A7BC`). It appends action type `0x80`
and copies the complete instruction into action bytes `1..4`. The action
dispatcher routes type `0x80` and type `0x20` to
`BattleHandleInvalidEnemyActionType` (`0x800792F8`), which clears pending action
markers and enters the optional language-error display path.

The multiply opcodes use the native signed intermediate chosen by each handler.
Byte multiply uses a signed 16-bit intermediate; word multiply uses a signed
32-bit intermediate. A positive intermediate above the unsigned destination
range clamps, while a signed-overflowed negative intermediate reaches the final
low-byte or low-word store. Divide and modulo execute directly with their
encoded or register-sourced divisor.

## 7. Condition Opcode Catalog

`B[n]`, `W[n]`, and `D[n]` denote the persistent byte, word, and dword registers.

| Op | Handler | Semantic operands and result |
|---:|---:|---|
| `80` | inline | True. |
| `81` | `0x8007E954` | byte register index, immediate byte: `B[index] == immediate`. |
| `82` | `0x8007E98C` | word register index, immediate word: `W[index] == immediate`. |
| `83` | `0x8007E9D0` | byte register index, immediate byte: `B[index] <= immediate` unsigned. |
| `84` | `0x8007EA08` | word register index, immediate word: `W[index] <= immediate` unsigned. |
| `85` | `0x8007EA4C` | byte register index, immediate byte: `B[index] >= immediate` unsigned. |
| `86` | `0x8007EA84` | word register index, immediate word: `W[index] >= immediate` unsigned. |
| `87` | `0x8007EAC8` | left byte register index, right byte register index: equality. |
| `88` | `0x8007EB08` | left word register index, right word register index: equality. |
| `89` | `0x8007EB50` | left byte register index, right byte register index: unsigned `<=`. |
| `8A` | `0x8007EB90` | left word register index, right word register index: unsigned `<=`. |
| `8B` | `0x8007EBD8` | byte register index, immediate mask: `(B[index] & mask) != 0`. |
| `8C` | `0x8007EC10` | word register index, immediate mask word: `(W[index] & mask) != 0`. |
| `8D` | `0x8007EC54` | left byte register index, right byte register index: bit intersection. |
| `8E` | `0x8007EC94` | left word register index, right word register index: bit intersection. |
| `8F` | `0x8007ECDC` | byte register index, immediate byte: inequality. |
| `90` | `0x8007ED14` | word register index, immediate word: inequality. |
| `91` | `0x8007ED58` | left byte register index, right byte register index: inequality. |
| `92` | `0x8007ED98` | left word register index, right word register index: inequality. |
| `93` | `0x8007EDE0` | left dword register index, right dword register index: 32-bit equality. |
| `94` | `0x8007EE28` | left dword register index, right dword register index: unsigned 32-bit `<=`. |
| `95` | `0x8007EE70` | entity-slot index: high defeat/lifecycle bit is set. |
| `96` | `0x8007EEA8` | formation-slot index: occupancy count is zero. |
| `97` | `0x8007EED0` | Neither party slot 0 nor party slot 1 is in the living mask. |
| `98` | `0x8007EEE8` | True when `(living_mask >> 5) == 0`, or when all eight enemy visual-state bytes have bit `0x80` set. |
| `99` | inline | Begin OR-chained condition evaluation. |
| `9A` | inline | True. |
| `9B` | `0x8007EF44` | Executing enemy is hidden. |

At a top-level instruction boundary, `0xFD` and `0xFF` terminate the selected
program. Inside a false-condition skip or active `0x99` OR-chain they are high
opcode records and are consumed instead. Values `0x9C..0xFF` preserve the
current condition result when the condition dispatcher receives them.

## 8. Staged Action Byte Layouts

The nominal action buffer at `0x800D2E5C` contains 32 eight-byte records. The VM
uses an 8-bit action cursor and performs no capacity check, so action writes can
continue past record 31 into adjacent memory. The type in byte 0 selects the
active fields in bytes `1..7`:

| `+0` type | `+1` | `+2` | `+3` | `+4` | `+5` | `+6` | `+7` |
|---:|---|---|---|---|---|---|---|
| `00` | - | - | - | - | - | - | - |
| `01` | attack-record index | attack calculation/animation code | optional action-name string ID | sequence ID low | sequence ID high | requested target mask low | requested target mask high |
| `02` | - | - | - | movement sequence low; low byte `1` suppresses the extra approach/shot call | movement sequence high | movement target mask low | movement target mask high |
| `03` | - | - | - | active-command replacement payload low | active-command replacement payload high | - | - |
| `04` | - | - | - | animation ID low and queued marker | animation ID high | - | - |
| `05` | - | - | - | - | - | forced-turn target mask low | forced-turn target mask high |
| `06` | - | - | - | despawn sequence payload low | despawn sequence payload high | - | - |
| `07` | - | - | - | - | - | clone-source enemy mask low | clone-source enemy mask high |
| `08` | byte-property selector | - | - | byte assignment value | - | - | - |
| `09` | byte-property selector | - | - | byte addend | - | - | - |
| `0A` | word-property selector | - | - | word assignment low | word assignment high | - | - |
| `0B` | word-property selector | - | - | word addend low | word addend high | - | - |
| `0C` | - | - | monster-name string ID | - | - | - | - |
| `0D` | - | - | optional action-name string ID | extended-animation ID low | extended-animation ID high | - | - |
| `0E` | - | - | - | status-action code | - | - | - |
| `0F` | - | - | - | visibility sequence payload low | visibility sequence payload high | visibility/camera target mask low | visibility/camera target mask high |
| `10` | - | - | - | - | - | - | - |
| `20` | - | - | - | - | - | - | - |
| `80` | raw script opcode | raw operand 1 | raw operand 2 | raw operand 3 | - | - | - |

Types `0x20` and `0x80` enter the fallback-action dispatch path. Property and
Gear-HP writes aimed at party targets produce type `0x20` in staged record 0;
the other bytes retain their current values and begin the script pass cleared.
A fallback-encoded lower opcode produces type `0x80` with its complete four-byte
instruction in bytes `1..4`.

Opcode `0x01` writes an immediate byte at an action offset. A write to offset
zero commits that record and advances the 8-bit action cursor. Opcodes `0x02`,
`0x03`, `0x3D`, and `0x52` provide writes sourced from byte registers, record
copies, immediate-word writes, and writes sourced from word registers.

## 9. Action Dispatch

`BattleExecuteScriptedActions` (`0x800793F0`) visits staged records, finds the
first entity in each record's target mask, clears the current damage entry, and
dispatches on byte zero:

| Type | Handler | Effect |
|---:|---:|---|
| `00` | inline | Mark that the 32-record scan has encountered a zero record. This permits return when the scan reaches record 32. |
| `01` | `0x80078998` | Display the optional action name, calculate and apply the selected attack, queue its animation, and retain the resulting hit mask. |
| `02` | `0x80078B34` | Stage approach/shot movement, camera visibility, and the movement action record. |
| `03` | `0x80078C9C` | Queue marker `0xFC` to replace the actor's active command. |
| `04` | `0x80078CEC` | Queue the selected animation. |
| `05` | `0x80078D48` | Store the target mask for deferred/forced enemy turns. |
| `06` | `0x80078D6C` | Queue marker `0xF9`, make the actor unavailable, and update combat flags for despawn. |
| `07` | `0x80078E24` | Clone identity, the three executable program pointers, `clone_retained_pointer`, entry presence flags, and `0x170` bytes of combat state into the destination; queue marker `0xFB`. |
| `08` | `0x80079054` | Set a selector-mapped byte combat property. |
| `09` | `0x80079098` | Add to a selector-mapped byte combat property with modulo-256 wrapping. |
| `0A` | `0x80079114` | Set a selector-mapped word combat property. |
| `0B` | `0x8007916C` | Add to a selector-mapped word combat property with modulo-65536 wrapping. |
| `0C` | `0x80078658` | Render, upload, and queue a monster-name display. |
| `0D` | `0x800791FC` | Queue marker `0xF4` with the extended-animation identifier. |
| `0E` | `0x800787E0` | Queue a status-effect action using the status-action code. |
| `0F` | `0x80079270` | Queue marker `0xF6` carrying target visibility/camera mask state. |
| `10` | `0x8007887C` | Queue removal of the most recently allocated monster-name strip. |
| other | `0x800792F8` | Clear pending action markers and enter the optional fallback language/actor display. |

The dispatcher scans at least 32 records. Encountering type zero sets its
termination permission; the scan returns after record 32 once that permission
is set. A 32-record list composed entirely of nonzero types continues into
adjacent memory.

After staging, `BattleMonsterJumpToTarget` (`0x80079778`) executes the action
sequence and waits for completion. `BattleStageEnemyReturnAndSequenceEnd`
(`0x80079674`) appends terminal marker `0xFE` and the applicable Gear marker.

## 10. Battle Event Variable Bridge

Enemy opcode `0x70` writes the Battle Event VM's word-variable bank:

```text
event_word_variable[index] = immediate_byte
```

The address calculation is:

```text
*(u16 *)(battle_event_runtime + 0x394 + index * 2) = immediate_byte
```

Enemy scripts use an 8-bit element index spanning `0..255`. Battle Event
bytecode addresses the same bank by byte offset, so enemy index `N` corresponds
to Battle Event variable operand `N * 2`. The handler dereferences the Battle
Event runtime pointer directly.

## 11. Main Functions

| Address | Function |
|---:|---|
| `0x801E4870` | `BattleLoaderLoadEnemyDefinitions` |
| `0x800793F0` | `BattleExecuteScriptedActions` |
| `0x80079674` | `BattleStageEnemyReturnAndSequenceEnd` |
| `0x80079778` | `BattleMonsterJumpToTarget` |
| `0x80079840` | `BattleUpdateMonsterScriptAttackVars` |
| `0x80079934` | `BattleAdvanceEnemyScript` |
| `0x80079948` | `BattleSkipEnemyScriptConditionalBlock` |
| `0x800799C8` | `BattleExecuteMonsterTurnScript` |
| `0x80079AB0` | `BattleExecuteMonsterAttackedScript` |
| `0x80079C24` | `BattleExecuteMonsterEndTurnScripts` |
| `0x80079E7C` | `BattleFindFirstEntityInTargetMask` |
| `0x80079ED8` | `BattleAccessEntityByteAttribute` |
| `0x8007A280` | `BattleAccessEntityWordAttribute` |
| `0x8007EF6C` | `BattleExecuteMonsterScriptLowerOpcode` |
| `0x8007F8C0` | `BattleExecuteMonsterScriptCondition` |
