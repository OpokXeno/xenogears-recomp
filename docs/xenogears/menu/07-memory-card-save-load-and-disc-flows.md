# Memory-Card Save, Load, And Disc Flows

## 1. Scope

This chapter specifies the General Menu module's complete memory-card coordinator: service startup, event delivery, two-port probing, directory and header scans, block selection, prompts, load, save, copy, delete, formatting, teardown, and the disc checks surrounding title-menu operation.

General Menu rendering and GPU packet construction are outside this chapter. PS1 texture and CLUT rules are in [`Graphics Resource Formats`](../graphics/03-resource-formats.md).

Addresses are qualified by their owning General Menu module.

## 2. Ownership, Lifetime, And Workspace

`ToggleMemoryCardWorkspaceAllocation` at `menu-overlay:0x801C5B54` allocates and zeroes exactly `0x5034` bytes, stores the pointer in the resident Menu context, and frees it when disabled. This is Menu-lifetime scratch, not resident game state and not serialized data.

```text
Menu entry
  -> allocate/clear 0x5034 card workspace and presentation objects
  -> initialize card services; detach and save CD callbacks
  -> scan -> select -> execute operation -> rescan as needed
  -> close events; restore callbacks; release 32 tiles/preview/workspace
```

Ranges not listed below are private workspace storage. Bytes cleared during allocation and never consumed by a card or presentation operation are padding.

| Offset | Size | Use |
|---:|---:|---|
| `+0x0000` | `32 * 0x5C` | Per-directory display/cache records |
| `+0x0B94` | `32 * 0x200` | Cached title frames, 16 per port |
| `+0x4B94` | `0x200` | New-save title/icon template |
| `+0x4D94` | `0x50` | Two buffered card-display quads |
| `+0x4F74` | `8` | Signed probe results for two ports |
| `+0x4F7C` | `4` | Current combined-grid display index |
| `+0x4F80` | `4` | Last previewed index; `0xFF` invalid |
| `+0x4F84` | `4` | Running block-owner count |
| `+0x4F88` | `2` | Directory-enumerated flags |
| `+0x4F8A` | `2` | Current directory counts |
| `+0x4F8C` | `2` | Header/icon cache counts |
| `+0x4F8E` | `32` | Entry-is-Xenogears-save flags |
| `+0x4FAE` | `32` | Block-to-directory map; `-1` is free |
| `+0x4FCE` | `12` | Operational filename prefix `BASLUS-00664` |
| `+0x4FE4` | `2` | Current card-presence flags |
| `+0x4FE6` | `1` | Refresh mode `0`, `1`, or `2` |
| `+0x4FE8` | `2` | Previous presence values |
| `+0x4FEC` | `16` | Four event handles |

Validation uses the allocation size, not the last listed field, as the hard bound.

## 3. Services, Events, And Exact Results

`RestartMemoryCardInterface` at `menu-overlay:0x801D9B08` renders and closes old handles, waits for VSync, calls `InitCARD(1)` (Resident `0x8004E794`), `StartCARD` (Resident `0x8004E7E8`), and `_bu_init` (Resident `0x80040464`), then opens and enables four events inside a critical section.

Every `OpenEvent` uses descriptor `0xF4000001`, mode `0x2000`, and a null callback:

| Handle offset | Spec | Event | Poll result |
|---:|---:|---|---:|
| `+0x4FEC` | `0x0004` | I/O complete | `0` |
| `+0x4FF0` | `0x8000` | Error | `1` |
| `+0x4FF4` | `0x0100` | Timeout | `2` |
| `+0x4FF8` | `0x2000` | New device/change | `3` |

`WaitAndConsumeMemoryCardEvent` at `menu-overlay:0x801C881C` tests new-device, error, completion, then timeout. It calls `UnDeliverEvent` for all four specs after the first delivery. There is no software loop limit; timeout is itself an event.

`RequestMemoryCardInfoStatus` at `menu-overlay:0x801C891C` starts `_card_info` (Resident `0x8004E784`). Start failure returns `-1`; delivered events map exactly as follows:

| Event result | Card-info result |
|---:|---:|
| Complete `0` | `0` |
| Error `1` | `-3` |
| Timeout `2` | `-1` |
| New-device `3` | `-2` |

`ConsumeMemoryCardEventDeliveries` at `menu-overlay:0x801C87C4` calls
`UnDeliverEvent` for all four deliveries; it does not close their handles.
`FinalizeMemoryCardEventHandles` at `menu-overlay:0x801C8960` renders once and
closes all handles in a critical section.

## 4. Two-Port Probe And Periodic Refresh

Ports `0` and `1` use BIOS prefixes `bu00:` and `bu10:`. `ProbeMemoryCardPortState` at `menu-overlay:0x801C8A10` sets tentative presence, requests card info for that channel, and stores the signed result at `+0x4F74[port]`.

| Result | State transition |
|---:|---|
| `0` after previous `-1` | Normalize first successful transition to status `0` |
| `-1` | Clear presence, count, scan request, owners, validity, and visible entries |
| `-2` | Preserve status for format handling; reject as stable scan completion |
| Presence changed | Clear that port's enumerated flag and update previous presence |

Save and copy treat `-2` as requiring format. A physically responsive port can therefore lack a usable formatted directory.

`PeriodicallyRefreshMemoryCardPorts` at `menu-overlay:0x801C8BEC` runs only while refresh mode is nonzero. It increments an eight-bit timer, probes both ports when `threshold < timer`, clears aggregate availability if both statuses are `-1`, then resets the timer. Threshold is `1` during scanning and `0x1E` afterward; the strict comparison gives an ordinary probe on update 31. Initialization writes timer `0x3C` for an immediate probe.

`ShowMemoryCardAccessDelay` at `menu-overlay:0x801C8CA4` displays card activity for exactly 59 rendered frames when the selected port responds.

## 5. Directory Enumeration And Header Scan

`EnumerateMemoryCardDirectory` at `menu-overlay:0x801C8D78` resets 16 owner and occupancy bytes, then uses `firstfile` (Resident `0x80040584`) and `nextfile` (Resident `0x80040594`). Each BIOS record occupies one `0x5C` cache entry.

There are 16 directory cache records per port but only 15 allocatable PS1 blocks. Directory enumeration has no independent pre-copy bound check. More than 16 returned records exceed the cache; valid card block use remains at most 15.

| Refresh mode | Enumeration behavior |
|---:|---|
| `0` | None |
| `1` | Enumerate each pending port; derive aggregate availability from file count |
| `2` | Enumerate each pending port without that aggregate update |

`ReadMemoryCardTitleFrame` at `menu-overlay:0x801C9038` opens with mode `3`, reads exactly `0x200` bytes, closes, and rejects open failure or a short read.

`LoadMemoryCardSaveHeader` at `menu-overlay:0x801C90B0` builds the full path, caches the frame at `+0x0B94 + entry * 0x200`, and registers the owner once for each block declared at file byte `+0x03`:

```text
encoded_entry = port * 16 + directory_index
```

The `+0x4FAE` owner map repeats this value for every occupied block. The running
owner index resets once per port and accumulates across every file on that port;
it does not reset per file. Retail trusts the declared byte, performs one write
per declared block at:

```text
workspace + 0x4FAE + port * 16 + running_owner_count
```

and increments the 32-bit running count without a limit. The two intended owner
rows are 16 bytes each, although only 15 entries per row represent card blocks.
For a fresh count, port 0 index 16 first enters port 1's row and port 1 index 16
first enters the following workspace state. In the `0x5034`-byte workspace,
indices 134 and 118 respectively are the first writes beyond the allocation.
A declared count of 255 writes indices `0..254`, ending at workspace `+0x50AC`
for port 0 or `+0x50BC` for port 1. Format-valid files declare at least one
block, and cumulative declared use is at most 15. Retail enforces neither
condition before writing the owner map.

## 6. PS1 Header, Prefix, And Slot Mapping

The scanner's first `0x200`-byte read contains the title/icon header and the first `0x100` payload bytes for Xenogears' one-icon saves; it is not the complete save.

| File offset | Size | Meaning |
|---:|---:|---|
| `+0x000` | `2` | PS1 `SC` magic |
| `+0x002` | `1` | Icon mode `0x11`, `0x12`, or `0x13` |
| `+0x003` | `1` | File block count |
| `+0x004` | `0x40` | PS1 title area |
| `+0x060` | `0x20` | 16-entry icon CLUT |
| `+0x080` | `0x80` | First 16 by 16 4-bpp icon frame |
| `+0x100` | variable | Xenogears payload begins |

New saves use one static icon. The scanner also supports standard `0x12` two-frame and `0x13` three-frame icons; other mode values hide that entry.

`MarkMatchingGameSavesOnCard` at `menu-overlay:0x801C9270` compares exactly 12 filename bytes with `BASLUS-00664`. For matches, the format-defined zero-based save number `0..14` is payload `+0x23`, absolute file `+0x123`.

```text
final_filename = "BASLUS-00664" + char('0' + save_number)
```

Numbers `10..14` consequently use ASCII punctuation after `'9'`; the filename suffix is not decimal text. `ComposeMemoryCardSaveTitle` at `menu-overlay:0x801CA8C0` separately appends visible decimal `01..15` and a trailing space to the title template.

Retail does not enforce that range before writing byte `1` to
`0x801EA6D0 + port * 16 + save_number`. Each intended occupancy row is 16 bytes,
so an out-of-range value writes subsequent global state. Port 0 values `72..83`
and port 1 values `56..67` reach the three retained CD callback words at
`0x801EA718..0x801EA720`. Card-service initialization saves those callbacks
before scanning and cleanup restores them afterward, so this automatic scan can
alter a callback pointer without the crafted entry being selected or loaded.

`ResolveSaveFileNumber` at `menu-overlay:0x801CB9E8` preserves an overwritten save's explicit number. For `0xFF`, it returns the first unused occupancy number in `0..14`.

## 7. Scan State Machine

`ScanMemoryCardsAndSaveSlots` at `menu-overlay:0x801C93A8` is both initial scan and refresh transaction.

```text
START
  -> mode 2; draw
  -> no ports: show no-card status; wait 59 frames or restart services
  -> threshold 1; snapshot both presence bytes
  -> for port 0 then port 1:
       directory count changed?
         restart services; clear 16 owners; erase "__tmp_file"
         read each 0x200 header; validate/upload icon; draw
         either presence changed? abort and invalidate
       clear unused entries; cache represented count
  -> mark exact BASLUS-00664 matches
  -> enable input; threshold 0x1E
DONE
```

Snapshotting both presence bytes prevents directory data from one card being combined with headers from a replacement card. Any change aborts the two-port pass. Rescan erases stale `__tmp_file`, cleaning interrupted save/copy debris.

## 8. 32-Entry Grid, Eligibility, And Navigation

The UI allocates 32 tile/cache objects, but a card has 15 blocks. Exactly 30 positions are selectable; raw indices `15` and `31` are off-screen sentinels.

```text
display  0..14 -> raw  0..14 -> port 0, local block 0..14
display 15..29 -> raw 16..30 -> port 1, local block 0..14
port = raw >> 4; local_block = raw & 0x0F
```

| Mode | Operation | Eligibility |
|---:|---|---|
| `0` | Copy source/delete | Port present and owner is not `-1` |
| `1` | Load | Present, occupied, and owner matches the game prefix |
| `2` | Save destination | Port present, free or occupied |

Save later rejects an occupied foreign file with prompt `0xC4`; only a matching game file can reach overwrite confirmation.

`FindInitialMemoryCardSelection` at `menu-overlay:0x801C9D34` begins on port 0 unless absent, returns the first eligible display index, or `0xFF`. Navigation is a three-column scan: down `+3`, up `-3`, right `+1`, left `-1`; it skips ineligible positions, remains in `0..29`, and never selects raw sentinel 15 or 31.

`ProcessMemoryCardGridInput` at `menu-overlay:0x801CA750` uses exact command codes:

| Command | Action | Return |
|---:|---|---:|
| `0` | Down | `0` |
| `1` | Right | `0` |
| `2` | Up | `0` |
| `3` | Left | `0` |
| `4` | Confirm | `1` |
| `5` | Cancel | `2` |

A private jump table intentionally remaps the shared directional vocabulary for
this grid: shared Right/Down/Left/Up commands `0/1/2/3` call the grid's
Down/Right/Up/Left search helpers respectively. This rotation is executable
behavior, not a second controller translator.

A changed selection rebuilds its preview once and stores the index at `+0x4F80`.

## 9. Prompts And Timeouts

`RunMemoryCardPromptInput` at `menu-overlay:0x801CAA38` snapshots both presence bytes and renders until:

| Command | Result |
|---:|---|
| `0` | Select No; retain result `0` and continue |
| `2` | Select Yes; set result `1` and continue |
| `4` | Confirm the currently selected result and close |
| `5` | Cancel/card-change restart; set latch and return `0` |
| `8` | Continue waiting |

With card-sensitive argument nonzero, mode becomes `2`; a presence change clears the port's enumerated flag and emits command `5`. There is no software inactivity timeout. With argument zero, the initial counter `0xB4` covers 180 idle command samples, including the command already present on entry. For command `8`, the routine decrements and tests the counter before drawing; if input remains idle throughout, it performs 179 new `MenuDraw` calls and then returns the current choice, initially `0`.

`PresentMemoryCardPromptSequence` at `menu-overlay:0x801CACF8` displays one prompt and optionally a second after acceptance, dismissing each presentation. `ConfirmMemoryCardFormat` at `menu-overlay:0x801CB8AC` monitors both ports in command state `8`; change cancels, otherwise prompt `0x2F` asks final confirmation.

| Flow | Exact prompt/status IDs |
|---|---|
| No card / no eligible save | `0x23` / `0x62` |
| Load confirm / activity / failure / success | `0x65`, `0x3B`, `0x3E`, `0x5C` |
| Format port 0/1 / confirm / activity | `0x29`/`0x2C`, `0x2F`, `0x26` |
| Save new / overwrite / foreign refusal | `0x5F`, `0x38`, `0xC4` |
| Save activity / failure / success | `0x32`, `0x35`, `0x5C` |
| Delete confirmations / activity / success | `0x56`, `0x59`, `0x50`, `0x5C` |
| Copy source / destination | `0x47`/`0xBB`, `0x4A`/`0xBE` |
| Copy duplicate / activity / I/O / full / success | `0x4D`/`0xC1`, `0x41`, `0x44`, `0xAC`, `0x5C` |

## 10. Load Transaction

`RunMemoryCardLoadOperation` at `menu-overlay:0x801CB304` scans until valid selection, cancellation, or card-change restart. After prompt `0x65` it:

1. Builds selected path and shows activity `0x3B`.
2. Tries open up to five times, applying the 59-frame access delay on failure.
3. Allocates `0x2100` bytes.
4. Reads `block_count << 13` bytes in `0x100` chunks, with up to five attempts per failed/short chunk.
5. Closes, validates checksum, and applies only a valid payload.
6. Frees the buffer and invalidates both port caches.

```text
sum = 0
for file_offset in 0x0100..0x1FFE:
    sum = (sum + file[file_offset]) & 0xFF
valid = (sum == file[0x1FFF])
```

This covers exactly `0x1EFF` bytes. Bad checksum, exhausted retry, short read, or card change produces `0x3E`; resident gameplay state is not applied.

Retail allocates only `0x2100` bytes and does not reject an oversized declared
block count before the `block_count << 13` read. Valid Xenogears saves declare
exactly one `0x2000`-byte block. Two or more blocks overflow the retail-sized
destination. The multi-block behavior in Copy is separate because Copy allocates
according to the source extent and does not apply a game payload.

`ApplyLoadedSavePayload` at `menu-overlay:0x801CB28C` imports the snapshot, restores 16 persistent configuration values, and decodes names. `DecodeSaveNameRecords` at `menu-overlay:0x801CB184` converts exactly 31 fixed `0x14`-byte records from storage to runtime encoding. Success leaves the card loop; state application begins only after the complete read and checksum succeed.

## 11. Save Block Layout

A Xenogears save is one PS1 block (`0x2000` bytes):

```text
0x0000..0x00FF  PS1 title, CLUT, and one static icon
0x0100..0x1FFE  Xenogears payload bytes covered by the checksum
0x1FFF          wrapping byte-sum checksum
```

The `0x100`-byte header is:

| File offset | Size | Meaning |
|---:|---:|---|
| `+0x000` | `2` | `SC` magic |
| `+0x002` | `1` | Icon mode `0x11` |
| `+0x003` | `1` | Block count `1` |
| `+0x004` | `0x40` | Encoded title |
| `+0x044` | `0x1C` | Padding |
| `+0x060` | `0x20` | 16-entry icon CLUT |
| `+0x080` | `0x80` | One 16 by 16 4-bpp icon |

Payload offsets below are relative to file `+0x100`. The summary fields use `i = 0..2`:

| Payload offset | Size | Meaning |
|---:|---:|---|
| `+0x000` | `4` | Play time rendered as seven digits |
| `+0x004 + i*2` | `2` | Current HP |
| `+0x00A + i*2` | `2` | Maximum HP |
| `+0x010 + i` | `1` | Current EP |
| `+0x013 + i` | `1` | Maximum EP |
| `+0x016 + i` | `1` | Primary level |
| `+0x019 + i` | `1` | Secondary level |
| `+0x01C + i` | `1` | Character ID; `0xFF` absent |
| `+0x01F` | `1` | Padding, explicitly cleared |
| `+0x020` | `3` | Padding, cleared with the payload buffer |
| `+0x023` | `1` | Save number `0..14` |
| `+0x024..+0x28F` | `31 * 0x14` | Stored name records |
| `+0x290..+0x99B` | `11 * 0xA4` | Complete character records |
| `+0x99C..+0xE4B` | `20 * 0x3C` | Compact Gear records |
| `+0xE4C..+0xEC3` | `0x78` | Resident game-state `+0x1648..+0x16BF` |
| `+0xEC4..+0x1023` | `11 * 0x20` | Character ability/progression records from resident `+0x16C0` |
| `+0x1024..+0x1123` | `0x100` | World-map and travel state from resident `+0x1820..+0x191F` |
| `+0x1124..+0x1B5B` | `0xA38` | Resident persistent state `+0x1920..+0x2357` |
| `+0x1B5C..+0x1EFE` | `0x3A3` | Padding, cleared with the payload buffer |
| `+0x1EFF` | `1` | Checksum |

Each compact Gear record stores these bytes from its resident `0xA4`-byte Gear record:

| Compact offset | Size | Resident Gear offset |
|---:|---:|---:|
| `+0x00` | `0x28` | `+0x00` |
| `+0x28` | `4` | `+0x5C` |
| `+0x2C` | `4` | `+0x60` |
| `+0x30` | `4` | Padding |
| `+0x34` | `2` | `+0x38` |
| `+0x36` | `2` | Padding |
| `+0x38` | `1` | `+0x99` |
| `+0x39` | `1` | `+0x74` |
| `+0x3A` | `1` | `+0x75` |
| `+0x3B` | `1` | Padding |

For the final persistent block, `payload_offset = resident_offset - 0x7FC`:

| Payload range | Resident range | Meaning |
|---:|---:|---|
| `+0x1124..+0x1127` | `+0x1920..+0x1923` | Persistent prefix |
| `+0x1128..+0x112B` | `+0x1924..+0x1927` | Gold |
| `+0x112C..+0x1133` | `+0x1928..+0x192F` | Persistent prefix |
| `+0x1134..+0x1533` | `+0x1930..+0x1D2F` | 512 Field variables; `+0x11D8` stores the zero-based disc request, valid values `0..1` |
| `+0x1534..+0x1537` | `+0x1D30..+0x1D33` | Party membership and party-frame masks |
| `+0x1538..+0x153A` | `+0x1D34..+0x1D36` | Three current party IDs; each valid value is `0..10` or absent sentinel `0xFF` |
| `+0x153B` | `+0x1D37` | Persistent party byte |
| `+0x153C..+0x1603` | `+0x1D38..+0x1DFF` | 100 weapon quantities followed by 100 weapon IDs |
| `+0x1604..+0x1793` | `+0x1E00..+0x1F8F` | 200 accessory quantities followed by 200 accessory IDs |
| `+0x1794..+0x18BF` | `+0x1F90..+0x20BB` | 150 item quantities followed by 150 item IDs |
| `+0x18C0..+0x1987` | `+0x20BC..+0x2183` | 100 Gear-part quantities followed by 100 Gear-part IDs |
| `+0x1988..+0x1AB3` | `+0x2184..+0x22AF` | 150 Gear-weapon quantities followed by 150 Gear-weapon IDs |
| `+0x1AB4` | `+0x22B0` | Persistent party byte |
| `+0x1AB5..+0x1ABF` | `+0x22B1..+0x22BB` | Character mount-state bytes |
| `+0x1AC0..+0x1B1B` | `+0x22BC..+0x2317` | Persistent party and travel tail |
| `+0x1B1C..+0x1B27` | `+0x2318..+0x2323` | Party-frame lock, Field ID, camera yaw, World initial position, World mode, and previous music |
| `+0x1B28..+0x1B47` | `+0x2324..+0x2343` | 16 configuration halfwords |
| `+0x1B48..+0x1B5B` | `+0x2344..+0x2357` | Persistent trailing block |

Retail classification requires a complete title-frame read and the twelve-byte filename prefix. It trusts header byte `+0x03` as the block count, payload byte `+0x23` as the save number, and the title bytes without checking `SC` or icon mode. Load allocates `0x2100` bytes, reads `block_count << 13` bytes, and checks the checksum over the first block before applying state. Generated saves use one block and save numbers `0..14`.

The summary IDs at payload `+0x01C..+0x01E` are display metadata. The gameplay
party IDs at `+0x1538..+0x153A` are restored verbatim by
`RestoreMenuGameState`; retail does not require `0..10` or `0xFF`. Checksum
acceptance therefore does not establish that restored identifiers are valid for
the eleven-character array or for later Field resource selection.

Only the persistent ranges copied by the snapshot routines round-trip. Save construction clears the payload first, so compact-Gear holes and payload `+0x1B5C..+0x1EFE` are regenerated as zero.

## 12. Save Snapshot And Commit

`BuildMemoryCardSaveImage` at `menu-overlay:0x801CBA4C` runs after temporary-entry creation, reopen, and header write. It first copies 16 active settings into the resident configuration shadow at `+0x2324..+0x2343`. It then snapshots three party summaries, stores play time/save number, converts all 31 names to storage encoding, snapshots game state into `0x1F00` bytes, and restores runtime name encoding. The configuration-shadow synchronization survives subsequent payload-write or rename failure; those failures do not leave names encoded. Failure before `BuildMemoryCardSaveImage` leaves the shadow unchanged.

`RunMemoryCardSaveOperation` at `menu-overlay:0x801CBD90` performs:

1. Select any block on a present port.
2. For status `-2`, confirm and call `format` (Resident `0x80040574`).
3. Prompt `0x5F` for free space, `0x38` for matching overwrite, or refuse foreign occupancy with `0xC4`.
4. Preserve overwrite number or choose first unused `0..14`.
5. Build final prefix/suffix path and `<port>:__tmp_file`; erase stale temp.
6. Create temp with BIOS mode `(block_count << 16) | 0x0200`, then reopen.
7. Compose title and write first `0x100` bytes.
8. Allocate/clear `0x1F00`, build snapshot, store checksum at payload `+0x1EFF`.
9. Write payload in `0x100` chunks, close, and rename temp to final.
10. Free memory, report status, and invalidate both scans.

Create, reopen, header write, each payload write, and rename use up to three attempts around access delays.

### Commit And Failure Guarantees

For a new save, the final filename appears only after the complete temporary data is written and rename succeeds. Failure after temporary-entry creation leaves `__tmp_file` until later scan cleanup and cannot expose a partial final-name file.

Overwrite is not rollback-safe: the old final filename is erased before temp creation/writing. Removal, write failure, or rename failure can leave neither old nor new save. Rename is a publication boundary, not a backup transaction. Format is likewise destructive and has no Menu rollback.

## 13. Copy: Duplicate Prevention And All Blocks

`RunMemoryCardCopyOperation` at `menu-overlay:0x801CC6D8` uses the opposite port as destination:

1. Select any occupied source (mode `0`) and confirm with its port-specific prompt.
2. Require opposite port; 179 absent-port vertical waits leave the copy path.
3. Reject occupied total 15; offer format when destination status is `-2`.
4. Compare full source filename with every destination filename; reject an exact duplicate.
5. Read source `0x200` header and retain declared block count.
6. Create destination `__tmp_file` with that count.
7. Copy `block_count << 13` bytes in `0x200` chunks, up to three attempts per chunk.
8. Close both files and call `rename` at `menu-overlay:0x801CD090` to give the temp entry the original filename. The rename return is ignored; the test at `menu-overlay:0x801CD098` still uses the prior `s7`/handle state.
9. When that prior state indicates completion, show success `0x5C`, clear all 32 caches, invalidate both scans/counts, and rescan.

Duplicate prevention is filename-based, not checksum-based. BIOS creation is final authority on sufficient free blocks. Copy transfers every declared block verbatim, including foreign files, and does not recompute Xenogears checksum. Detected create/read/write failures can leave the temp entry and do not alter the source. The final rename is not an observed commit boundary: rename failure can still present success `0x5C` and trigger invalidation/rescan, leaving only `__tmp_file`, which the scan may erase. The source remains unchanged.

## 14. Delete

`RunMemoryCardDeleteOperation` at `menu-overlay:0x801CD2AC` accepts any occupied file, not only Xenogears saves. After prompts `0x56` and `0x59`, it shows `0x50`, builds the cached path, and calls `erase` (Resident `0x800405B4`). The erase return is ignored: the routine always shows success `0x5C`, clears 32 owner/validity/visibility entries, invalidates both scans/counts, and requests rebuild. Cancellation before the erase call preserves the file. A successful erase has no backup or undelete, while a failed erase can make the supposedly deleted entry reappear on rescan.

## 15. Preview, Icons, And Header Facts

Cached metadata can show title (up to 32 decoded characters), standard icon animation, one icon per owned block, all-block highlight, and port availability without applying the save.

For a valid game save, `BuildMemoryCardSavePreview` at `menu-overlay:0x801E76EC` builds up to three rows: emblem, stored name, two levels, current/maximum HP, and current/maximum EP; ID `0xFF` hides the row. `BuildMemoryCardSaveHeaderDisplay` at `menu-overlay:0x801E68AC` shows seven play-time digits, fixed labels, and two-digit one-based save number. `RefreshMemoryCardSavePreview` at `menu-overlay:0x801E781C` clears the previous preview first. Foreign files receive title/icon display but no party summary.

These are logical facts. Primitive, ordering-table, texture packing, and VRAM details remain in graphics documentation.

## 16. Mode And Action Dispatch

| Resident mode | Relevant behavior |
|---:|---|
| `0` | Normal in-game Menu; load/save is a central action |
| `2` | Three-entry title load menu, then disc enforcement |
| `6` | Dedicated save-point/title save-load flow, then disc enforcement |

`ProcessLoadSaveMenu` at `menu-overlay:0x801D9F98` defaults action to `2`; normal mode with no open argument starts at `1`. `ExecuteSelectedMemoryCardAction` at `menu-overlay:0x801CD710` maps action `0` delete, `1` copy, and `2` to load in resident mode `2` or save otherwise.

`RunLoadSaveTitleSelectionLoop` at `menu-overlay:0x801C58EC` displays New Game, Continue, and Sound from top to bottom, using internal indices `2`, `1`, and `0`. It wraps movement and resets inactivity on movement or a confirmed action. Only while current disc is Disc 1, `counter > 600` exits with return state `1`; strict comparison means rendered iteration 601. Resident dispatch requests game state `1`, and the title Field consumes this as its idle-attract route to replay the opening sequence. This counter is separate from card I/O and prompt timeouts.

Resident byte `0x800594D0` carries the mode-2 outcome:

| Value | Outcome |
|---:|---|
| `0` | New Game or another exit without load/attract; install the title Field transition with Field ID `4` |
| `1` | Disc 1 idle-attract exit; request game state `1` and replay the opening sequence |
| `2` | Accepted load; restore the saved Field ID and persistent destination values |

Mode 6 first presents card-sensitive `0x7D` and `0x80`, enters save with save-number policy enabled, then requests the recorded disc. Mode 2 sets return state `2` after successful load.

## 17. Callback Backup, Restore, And Cleanup

`InitializeLoadSaveMemoryCards` at `menu-overlay:0x801D9C84` shows checking status `0x20`, waits for transition completion, restarts card services, and in a critical section calls `CdSyncCallback(NULL)`, `CdReadyCallback(NULL)`, and `CdReadCallback(NULL)`, saving all returned callbacks. It invalidates both presence/enumeration states and forces immediate two-port scan.

`ShutdownLoadSaveMemoryCards` at `menu-overlay:0x801D9E3C` clears preview, releases 32 tiles and preview storage, clears visible flags/counts, and restores all three callbacks in a critical section. `ProcessLoadSaveMenu` finalizes four event handles on every exit; top-level shutdown frees `0x5034`. No event, cache pointer, or detached callback survives Menu lifetime.

## 18. Disc Validation And Index Reload

`EnsureRequestedDiscLoaded` at `menu-overlay:0x801C8694` converts the low byte of the zero-based request to expected disc `(request & 0xFF) + 1`. If resident current-disc helper already matches, it returns. Otherwise:

```text
initialize inactive CD interface
show disc-specific request
validate marker and expected number
  valid -> dismiss and return
  invalid/read failure -> show status for 29 frames -> retry
```

`InitializeDiscSwapInterface` at `menu-overlay:0x801E92CC` resets inactive CD state and repeats command `8` until accepted. The disc-marker validator at `menu-overlay:0x801E93A0` waits for shell-open, shell-close, and media-ready states; reads a 16-byte marker; verifies word `0x4E45585F` (little-endian `_XEN`) and the expected ASCII disc digit; then issues the main and secondary index loads.

The runtime does not validate the persisted request before this loop and offers
no cancellation path. Requests `0` and `1` select the two supported discs. Any
other low-byte value produces an expected disc number and ASCII digit that no
supported disc can satisfy, so every insertion returns to the retry loop
indefinitely.

| Disc result | Meaning |
|---:|---|
| `0` | Disc accepted and main/secondary index loads issued |
| `2` | CD readiness/error condition or `_XEN` marker mismatch |
| `3` | Expected ASCII disc digit mismatch only |

Card prefix `BASLUS-00664` and disc marker `_XEN` are independent values.

## 19. Format Domains And Retail Omissions

1. Port, display index, raw block, directory index, and save number are distinct
   index domains.
2. Valid ranges are port `0..1`, display `0..29`, local block `0..14`, cache
   index `0..15`, and save number `0..14`.
3. Raw entries `15` and `31` are not selectable. A valid directory contains at
   most 16 cache records, and cumulative declared block ownership is at most 15
   per port.
4. A complete `0x200` title read, valid `SC`, a supported icon mode, and a block
   count consistent with the selected operation define a valid title frame;
   retail checks only read completion at this boundary.
5. Every owner used to index directory/header arrays belongs to the corresponding
   port row.
6. Load and game overwrite classification uses the exact 12-byte prefix;
   title/icon resemblance is insufficient.
7. The format save number is `0..14`; the retail marker write uses an unchecked
   byte index and can reach retained callback words.
8. A valid Xenogears load has block count `1`, save number `0..14`, each gameplay
   party ID in `0..10` or equal to `0xFF`, zero-based disc request `0..1`, and a
   valid checksum. Retail validates only I/O completion and the first-block
   checksum before applying state.
9. Disc requests outside `0..1` have no satisfiable result or cancellation path
   in disc enforcement.
10. Snapshot construction copies persistent ranges and regenerates cleared
    padding as zero.
11. A change to either presence value aborts and invalidates the scan.
12. Format, overwrite, and delete pass through card-sensitive confirmation.
13. Image construction restores runtime name encoding on every outcome.
14. Normal card-service shutdown restores three retained CD callbacks and closes
    four card events.

## 20. Result And Failure Guarantees

| Operation | Success | Failure guarantee |
|---|---|---|
| Probe/scan | Stable ports, complete headers, cumulative declared blocks at most 15, and matching save numbers `0..14` | Retail malformed counts or save numbers can corrupt workspace state before any load selection |
| Load | Full read, checksum equal, apply complete | Existing file unchanged; gameplay state not applied |
| New save | Temp fully written and renamed | Final name absent; temp may remain |
| Overwrite | Old erased, temp written and renamed | Failure after old-entry erasure leaves the old entry absent; no rollback |
| Format | BIOS format after confirmation | Destructive; no rollback |
| Copy | All declared blocks copied; rename called; prior state permits success `0x5C` and rescan | Rename return is ignored, so reported success can leave only temp, which rescan may erase; source unchanged |
| Delete | Two confirmations followed by erase call, success `0x5C`, and cache invalidation | Erase return is ignored; successful erase is irreversible, while failed erase can reappear on rescan |
| Disc swap | Code `0`: disc accepted and index loads issued | Code `2` is CD readiness/error or `_XEN` mismatch; code `3` is expected ASCII digit mismatch; card untouched |

## 21. Grouped Function Index

All addresses below belong to the General Menu module.

### Services And Scan

| Address | Function |
|---:|---|
| `menu-overlay:0x801C5B54` | Allocate/free `0x5034` workspace |
| `menu-overlay:0x801C87C4`, `0x801C881C`, `0x801C891C`, `0x801C8960` | Consume, wait/classify, request info, finalize events |
| `menu-overlay:0x801C8A10`, `0x801C8BEC`, `0x801C8CA4` | Probe, periodic refresh, 59-frame access delay |
| `menu-overlay:0x801C8D78`, `0x801C8EE8` | Enumerate one/requested directories |
| `menu-overlay:0x801C9038`, `0x801C90B0`, `0x801C9270`, `0x801C93A8` | Read/cache headers, mark prefix, coordinate scan |

### Selection And Operations

| Address | Function |
|---:|---|
| `menu-overlay:0x801C9BCC`, `0x801C9D34` | Validate/find eligible selection |
| `menu-overlay:0x801C9EF4`, `0x801CA1D4`, `0x801CA480`, `0x801CA5F0`, `0x801CA750` | Down/up/right/left/input dispatch |
| `menu-overlay:0x801CA8C0`, `0x801CAA38`, `0x801CACF8`, `0x801CB8AC` | Compose title, prompt input/sequence, format confirm |
| `menu-overlay:0x801CB184`, `0x801CB28C`, `0x801CB304` | Decode names, apply payload, load |
| `menu-overlay:0x801CB9E8`, `0x801CBA4C`, `0x801CBD90` | Resolve number, build image, save |
| `menu-overlay:0x801CC6D8`, `0x801CD2AC`, `0x801CD710` | Copy, delete, action dispatch |
| `menu-overlay:0x801E4A28`, `0x801E4D10` | Snapshot and restore the memory-card payload |

### Mode, Preview, Cleanup, And Disc

| Address | Function |
|---:|---|
| `menu-overlay:0x801C58EC`, `0x801C62A8`, `0x801C8694` | Title timeout, mode dispatch, requested-disc loop |
| `menu-overlay:0x801D9B08`, `0x801D9C84`, `0x801D9E3C`, `0x801D9F98` | Restart, initialize, shutdown, action menu |
| `menu-overlay:0x801E68AC`, `0x801E76EC`, `0x801E781C`, `0x801E78C8` | Header/party preview, refresh, icon frames |
| `menu-overlay:0x801E92CC`, `0x801E93A0` | Disc interface and marker/index validation |

### Resident Services

| Address group | Services |
|---|---|
| Resident `0x80040474`, `0x80040494`, `0x800404A4`, `0x800404C4` | `OpenEvent`, `TestEvent`, `EnableEvent`, `UnDeliverEvent` |
| Resident `0x80040534`..`0x800405B4` | `open`, `read`, `write`, `close`, `format`, `firstfile`, `nextfile`, `rename`, `erase` |
| Resident `0x80040FB4`, `0x80040FCC`, `0x8004373C` | CD sync, ready, and read callback setters |
| Resident `0x8004E784`, `0x8004E794`, `0x8004E7E8` | `_card_info`, `InitCARD`, `StartCARD` |
