# Native renderer architecture

## Purpose

The native renderer converts authenticated Xenogears render data into host render semantics.
The game remains the source of render intent.
The game renderer remains the reference renderer.

The native renderer uses four separate operations:

1. It authenticates the game code.
2. It captures the source data.
3. It constructs render primitives.
4. It submits the primitives in a transaction.

Do not combine these operations in one module.

## Design rules

### Fail closed

The renderer must reject a native substitution when proof is incomplete.
The renderer must also reject stale or invalid source data.

A rejection can result from these conditions:

- The game identity is incorrect.
- The code range is incorrect.
- The observed instruction is incorrect.
- The producer lifecycle is incomplete.
- A guest address is invalid.
- A code write invalidates the proof.
- A transaction has insufficient capacity.
- A service is not configured.

### One owner for each state

Each producer owns its templates, captures, pending state, and snapshots.
Each producer also owns its invalidation policy.

The runtime does not read producer state directly.
The runtime uses producer functions and data transfer objects.

### Explicit dependencies

A lower layer does not read authentication globals.
The caller supplies values or callbacks to the lower layer.

Private headers stay in their source domain.
Public headers stay in `include/`.

### Transactional submission

A submission operation must have one commit point.
The operation must remove partial data after a failure.
Only a successful activation makes a visual available.

### Diagnostics have no authority

A snapshot reports state.
A snapshot does not authorize a native path.
A shadow comparison reports a difference.
A shadow comparison does not authorize a native path.

## Layer model

A layer can depend only on layers that appear before it in this list.

### Layer 1: Core

Location: `src/core/`

The core layer contains render functions with few dependencies.

| Component | Purpose |
|---|---|
| `xg_host_3d.c` | Applies fixed-point transforms and projection. |
| `xg_native_view.c` | Stores native view configuration. |
| `xg_visual_state.c` | Manages visual state identifiers. |
| `xg_render_ir.c` | Stores authenticated intermediate primitives. |
| `xg_render_quad_builder.c` | Constructs common quad primitives. |
| `xg_render_backend.c` | Converts an intermediate representation (IR) primitive to a graphics processing unit (GPU) semantic. |
| `xg_native_render_baseline.c` | Supports reference render comparisons. |

This layer does not make authentication decisions.
This layer does not own producer lifecycles.

### Layer 2: Infrastructure

Location: `src/infrastructure/`

The infrastructure layer contains services that multiple producers use.

| Component | Purpose |
|---|---|
| `xg_render_address_lookup.c` | Finds aligned guest words by address and epoch. |
| `xg_render_resource_watch.c` | Tracks memory ranges that can invalidate data. |
| `xg_render_resolver_registry.c` | Registers native stream resolvers. |
| `xg_render_shared_packet_resolver.c` | Resolves packet data shared by producers. |
| `xg_render_submission.c` | Stages exact, standalone, and pre-scene data. |
| `xg_render_temporal_submission.c` | Stages previous-frame temporal candidates. |
| `xg_render_ui_ot.c` | Traverses and stages a user interface (UI) ordering table. |
| `xg_render_instrumentation.c` | Stores synchronized counters and failure data. |

This layer receives authentication state through callbacks.
It does not read runtime authentication storage.

### Layer 3: Producers

Locations:

- `src/producers/field/`
- `src/producers/model/`
- `src/producers/world/`

A producer converts one authenticated game operation into render primitives.
A producer owns all data that is specific to that operation.

Field producers handle field characters, effects, sprites, lines, and overlays.
Model producers handle textured three-vertex polygon (FT3) operations.
They also handle textured four-vertex polygon (FT4) and sprite FT4 operations.
World producers handle terrain, models, actors, clouds, effects, and the minimap.

The world coordinator controls world preparation and commit operations.
It finalizes world submissions before GPU submission.

### Layer 4: Authentication

Location: `src/auth/`

Authentication proves that a native substitution is valid.

| Component | Purpose |
|---|---|
| `xg_render_static_auth.c` | Checks fixed manifest records. |
| `xg_render_auth.c` | Controls the entry, capture, and return proof. |
| `xg_render_runtime_variant_auth.c` | Checks runtime-loaded code and ranges. |
| `xg_render_local_producer_auth.c` | Checks producer-local lifecycles. |

Authentication does not decode producer-specific data.
Authentication does not own producer templates.

### Layer 5: Runtime

Location: `src/runtime/`

The runtime layer connects host events to authentication and producers.

| Component | Purpose |
|---|---|
| `xg_render_auth_runtime.c` | Controls authentication and scene lifecycle. |
| `xg_render_runtime_composition.c` | Connects services and producer adapters. |
| `xg_render_cutover_dispatch.c` | Matches a program counter and an instruction. |
| `xg_render_mutation_classifier.c` | Classifies a memory change. |
| `xg_render_invalidation_dispatch.c` | Sends an invalidation event to registered owners. |
| `xg_render_invalidation_modules.c` | Registers invalidation owners. |

`xg_render_auth_runtime.c` is the public runtime facade.
It must not contain primitive decoding or producer repositories.

## Runtime flow

### Startup

The host runtime completes these operations:

1. It registers code watches.
2. It sets the execution-phase exchange.
3. It registers the GPU submission hook.
4. It registers the UI ordering-table hook.
5. It registers the current-semantic hook.
6. It configures host services.
7. It configures the authentication runtime.
8. It configures the renderer and host native views.

Authentication configuration is transactional.
A failed authentication configuration does not commit partial renderer-composition state.
A caller can correct the inputs and try the configuration again.
Previously installed host callbacks and services remain installed after this failure.

### Guest observation

The runtime receives a cold hook or a warm hook.
The runtime first checks route relevance.
It then checks the applicable authentication tier.

The dispatcher matches these values:

- program counter;
- instruction word;
- hook type;
- route action.

The dispatcher sends the observation to the owning producer.

### Source capture

The producer checks each guest address before it reads the data.
The producer stores only data for its current lifecycle.
The producer rejects incomplete source data.

### Primitive construction

Most producers convert captured data to `XgRenderIrNativePrimitive` records.
The backend converts each record to one `GpuRenderSemantic` value.

Some infrastructure paths construct `GpuRenderSemantic` values directly.
UI ordering-table preparation is one such path.

The authenticated IR keeps the packet binding and the source primitive index.
These values bind the host semantic to the original game command.

### Exact submission

An exact semantic identifies one game command in the current frame.
The native stream consumes it when it sees the matching command.

The visual state must be active.
The visual identifier must match.
The command identifier must be unique.

### Temporal submission

A temporal candidate can use data from the previous frame.
The current frame must not contain a primitive with the same interpolation identity.
An interpolation identity identifies equivalent primitives in different frames.

The renderer reports each current semantic to the temporal service.
The service records each current interpolation identity.
At the source-frame boundary, it skips covered candidates.
It flushes the remaining candidates.

### UI ordering table

The runtime arms UI preparation after an authenticated UI source observation.
Direct memory access channel 2 (DMA2) calls the preparation hook before it traverses the ordering table.

The UI service stages all commands in one visual transaction.
It activates the visual only after all staging operations succeed.
It abandons the visual after a failure.
It keeps pending source data so the caller can retry.

### Return proof

The canonical return hook completes these operations:

1. It closes the producer.
2. It finalizes the authenticated IR.
3. It finalizes the visual state.
4. It checks all packet bindings.
5. It publishes the completed proof.

The runtime rejects the native path if one operation fails.

## Invalidation

Invalidation removes state that is not valid after a runtime change.

The mutation classifier describes a memory change.
The invalidation dispatcher sends the resulting event to each owner.

Typical invalidation events include:

- renderer disable;
- scene boundary;
- code write;
- resource write;
- loader mismatch;
- authority loss;
- runtime reset.

Each handler clears only the state that it owns.
The dispatcher does not interpret producer state.

Code-watch registration does not depend on operation order.
If the callback arrives before source configuration, the runtime stores it.
The runtime registers each range only once.

## Authentication tiers

The renderer can use fixed code or runtime-loaded code.
Each tier has its own identity proof.

The fixed tier uses the canonical game manifest.
The runtime tier also uses a runtime variant descriptor.

The runtime tier binds these items:

- canonical game identity;
- canonical manifest identity;
- runtime companion identity;
- artifact identity;
- loaded address and range;
- entry, capture, and return sites;
- source and cutover instructions.

One mismatch rejects the runtime tier.

## Public application programming interface (API) boundary

New code must include a focused public header.

| Header | Use |
|---|---|
| `xg_render_auth_runtime_control.h` | Configuration and frame control. |
| `xg_render_auth_runtime_hooks.h` | Guest instruction ingress. |
| `xg_render_auth_runtime_invalidation.h` | Loader and memory-change notifications. |
| `xg_render_auth_runtime_diagnostics.h` | Read-only diagnostic snapshots. |

`xg_render_auth_runtime.h` is a compatibility umbrella.
Do not add new declarations to the umbrella.

## Build boundary

The source directories define ownership boundaries.
They are not all separate link libraries.

The `xg_render_auth` target aggregates authentication, runtime, infrastructure, and producer modules.
Source ownership is more strict than current linker isolation.

## Capacity behavior

Many renderer stores use fixed capacities.
A full store must reject the new item.
A full store must not overwrite authenticated data.

Capacity exhaustion is a normal fail-closed condition.
Use a diagnostic snapshot to identify the applicable blocker.

## Test boundary

Production builds do not enable test-only control paths.
Composition has a compile-time registration-failure test seam.
The macro `XG_RENDER_RUNTIME_COMPOSITION_TESTING` controls this seam.
Test adapters stay in `tests/`.
Tests use the same producer modules as production.

See [the development guide](docs/DEVELOPMENT.md) for the required test groups.
