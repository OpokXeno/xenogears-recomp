# Native renderer glossary

Use the terms in this glossary in all native renderer documents.
Use one term for one meaning.

## Terms

### Artifact

A binary file that contains game code or game data.
An artifact has a size and cryptographic identity.

### Authentication

The operation that proves a code identity, a data identity, and a valid lifecycle.
Authentication grants authority only after all required checks pass.

### Authority

Permission to use a native substitution for one authenticated operation.
Authority ends after an applicable invalidation event.

### Blocker

A numeric or named reason that prevents native use.
A blocker is diagnostic data.

### Candidate

Runtime data that can match an authenticated runtime variant.
A candidate does not have authority before a complete match.

### Capture

The operation that reads validated guest data for one producer.

### Cold hook

A hook reported through the cold interpreter or canonical execution tier.
It can establish or update authentication state.

### Commit

The single operation that makes prepared data active.
A failed operation must not commit partial data.

### Cutover

An authenticated point where the runtime can use native behavior.
A cutover matches a program counter and an instruction.

### Data transfer object

A structure that transfers data between modules.
The abbreviation is `DTO`.
A DTO does not own external resources.

### Exact semantic

A graphics processing unit (GPU) semantic that identifies one command in the current frame.

### Fail closed

To reject native use when proof or data is incomplete.

### Field

A normal game area where characters move and interact.
The field renderer is different from the world renderer.

### Game renderer

The game renderer that runs from recompiled game code.
It is the reference path for native renderer development.

### Guest

The recompiled PlayStation program and its memory.
A guest address is an address in PlayStation memory.

### GPU semantic

A host description of one PlayStation GPU operation.
The type name is `GpuRenderSemantic`.

### Hook

A runtime call that reports a selected game instruction or event.

### Host

The computer that runs the recompiled game.

### Intermediate representation

A renderer-owned representation of a native primitive.
The abbreviation is `IR`.

### Invalidation

The operation that removes state after a change makes the state unsafe.

### Lifecycle

The required order of events for one authenticated producer operation.
A typical lifecycle is entry, capture, and return.

### Manifest

A text file that declares identities, addresses, and authentication rules.
The renderer manifests use Tom's Obvious, Minimal Language (TOML) syntax.

### Native renderer

The host implementation that reconstructs authenticated game render operations.

### Ordering table

A linked list of PlayStation GPU packet addresses.
The abbreviation is `OT`.

### Overlay

Game code or data that the game loads into a reusable memory range.
An overlay record is optional unless a producer requires it.

### Primitive

One render item, such as a triangle, quad, sprite, or line.

### Producer

A module that converts one authenticated game operation into native primitives.

### Program counter

The address of the current guest instruction.
The abbreviation is `PC`.

### Provenance

Metadata that identifies the source of an artifact or observation.
Provenance can include a disc, directory, file, and sector.

### Rejection

A fail-closed result that prevents native use.

### Resolver

A service that supplies a native semantic when the native stream requests one.

### Runtime variant

An authenticated code layout that the game loads at runtime.

### Scene

A scene is an interval in renderer operation.
Each scene has one generation value.
A scene boundary invalidates scene-owned state.

### Shadow comparison

A comparison between reconstructed output and reference output.
It supplies diagnostic evidence only.

### Snapshot

A read-only copy of diagnostic state at one point in time.

### Source frame

The frame interval that supplies current and temporal render data.

### Temporal semantic

A semantic from a previous frame that remains eligible for presentation.

### Transaction

A group of staging operations with one commit or one abort.

### Visual

A transaction-owned group of GPU semantics.
A visual has a unique visual state identifier.

### Warm hook

A hook reported through the runtime-loaded native execution tier.
It can participate in activation, entry, capture, and return.
Its name does not imply that completed authority exists.

### World

The large navigation view between game areas.
The world renderer is different from the field renderer.

## Abbreviations

| Abbreviation | Meaning |
|---|---|
| `API` | Application programming interface. |
| `CRC` | Cyclic redundancy check. |
| `DMA` | Direct memory access. |
| `DTO` | Data transfer object. |
| `FT3` | Textured three-vertex polygon command. |
| `FT4` | Textured four-vertex polygon command. |
| `GPU` | Graphics processing unit. |
| `IR` | Intermediate representation. |
| `OT` | Ordering table. |
| `PC` | Program counter. |
| `SHA-256` | Secure Hash Algorithm with a 256-bit result. |
| `TOML` | Tom's Obvious, Minimal Language. |
| `UI` | User interface. |
