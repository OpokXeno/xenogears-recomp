# Native renderer runtime interface

## Purpose

This guide describes the public runtime application programming interface (API).
It does not describe producer-private functions.

New code must include one focused header.
Do not include the compatibility umbrella in new code.

## Focused headers

| Header | Purpose |
|---|---|
| `xg_render_auth_runtime_control.h` | Configuration and frame control. |
| `xg_render_auth_runtime_hooks.h` | Guest instruction observations. |
| `xg_render_auth_runtime_invalidation.h` | Loader and memory-change notifications. |
| `xg_render_auth_runtime_diagnostics.h` | Read-only snapshots and names. |
| `xg_render_native_work.h` | Guest-owned immutable FIFO collection and flush/drain. |
| `xg_render_source_frame.h` / `xg_render_source_commit.h` | Source builders and sealed, retained work. |
| `xg_render_semantic_presentation.h` | Worker ACK/fence contract and presenter authorization. |
| `xg_render_presentation_host.h` | Asynchronous worker and owner-thread presentation lifecycle. |
| `xg_render_motion.h` | Source model/camera pose evaluation. |
| `xg_render_fragment_runtime.h` | Generic fragment lane outside Native Work. |

`xg_render_auth_runtime.h` includes the focused authentication runtime headers.
It exists for compatibility only.

## Host integration order

Configure the current host integration in this order:

1. Register code watches.
2. Set the execution-phase exchange.
3. Register `psx_xg_render_auth_before_gpu_submission` as the GPU submission hook.
4. Register `psx_xg_render_auth_prepare_ui_ot` as the ordering-table hook.
5. Register `psx_xg_render_auth_note_gpu_semantic_current` as the current-semantic hook.
6. Configure `XgRenderRuntimeHostServices`.
7. Call `psx_xg_render_auth_configure`.
8. Configure the renderer and host native views.


## Native Work ownership

Use `psx_xg_render_auth_set_native_work_mode`,
`psx_xg_render_auth_describe_native_work`, and
`psx_xg_render_auth_accept_native_draw` at the host/authentication boundary.
The collector in `xg_render_native_work.h` copies borrowed inputs before return.
DRAW/UPLOAD/COPY/FILL/TARGET operations stay in FIFO order, including offscreen
and display-disabled mutations. A rejected operation stops the caller; it must
not be silently omitted.

`xg_render_native_work_temporal_coverage` retains pointer-free component/sample
metadata without adding a draw. `out_coverage` receives one caller retain. A
producer scope replaces its complete snapshot, including empty arrays. World
model, terrain/water, and cloud coverage uses this path through shared
submission; `psx_xg_render_auth_set_terrain_temporal_coverage` controls terrain
coverage. Consumers use operation-gap ordering and exact retained generations,
not stale last-seen geometry.

`psx_xg_render_auth_source_boundary` flushes a Native Work display boundary.
`xg_render_native_work_flush` refreshes publication identity and retains sealed
work across backpressure. `xg_render_native_work_drain` preserves mutations before
ordinary timeline invalidation; `xg_render_native_work_cancel_pending` discards
only unqueued collector work. Restore is different: invalidate first, cancel and
drain, then record the complete replacement VRAM image in the new epoch.
Disabling collector services alone does not cancel work.

The worker compile contract is explicit:

| Result | Meaning |
|---|---|
| `XG_RENDER_COMPILE_ENDPOINT_READY` | Transfers an endpoint and compile fence; ACK only after READY. |
| `XG_RENDER_COMPILE_APPLIED` | Synchronous mutation-only ACK, with zero endpoint/fence outputs. |
| `XG_RENDER_COMPILE_WOULD_BLOCK` | No committed effects; retry the same FIFO head. |
| `XG_RENDER_COMPILE_FAILED` | Blocks the lane until invalidation; never skip later deltas. |

Lifecycle cancellation is accounted separately from ACK. Late GPU duplicate
detection can suppress a visual endpoint only after actual storage pixels and
temporal metadata are equivalent; all operations still commit and ACK. The
retained visual history and source timestamp must not advance for that duplicate.

## Presenter ownership

`xg_render_presentation_host_start` binds the calling thread as presenter owner
and starts one asynchronous worker. Notify wakes only the worker; it never
advances the guest or presents. The owner calls `gl_renderer_native_service` and
`xg_render_presentation_host_pump`, including during yield/EOF draining. Service
must continue even when no endpoint is presentable so FIFO capacity can retire.

The host source-clock synchronization dates work without advancing simulation.
Guest VBlank/IRQ stays at 60 Hz; host presentation has its own period. Motion
phases evaluate local TRS/quaternion slerp, camera/model hierarchy, and shared
source vertex identities, not image blending. The backend approves up to seven
intermediate images plus the authored 1/1 endpoint. The presenter requires a
matching retained logical base and epoch/scene/layout; otherwise it uses 1/1.
Generated phases are not a count of actual presentations or successful swaps.

At scales above 1, the native GPU service rasterizes the complete ordered journal
into scaled storage while canonical CPU device/reference state stays 1x.
`GlRendererNativeEndpointMetadata.width/height` are guest dimensions;
`storage_width/storage_height` describe actual pixels. Scales 1..8 are supported;

Join the presentation host on its bound owner before backend shutdown. Keep
callbacks and resources alive until join completes; destroy must not race other
host API calls. See [the development guide](DEVELOPMENT.md) for current FPS and
replay validation limits.

## Configure the runtime

Call `psx_xg_render_auth_configure` once during startup.
 Supply the requested render mode.
Supply a presentation gate and the user data for that gate.

```c
bool psx_xg_render_auth_configure(
    GuestRenderRenderMode requested_render_mode,
    PsxXgRenderPresentationGate presentation_gate,
    void *presentation_user_data);
```

Check the return value.
Stop startup when the function returns `false`.

Unsupported render values are normalized to `ORIGINAL`.
They do not necessarily make the call return `false`.
Validate enum values when silent normalization is not acceptable.

Configuration is transactional.
A failed call does not commit a partial configuration.
An identical successful call is idempotent.
A conflicting configuration is rejected.

Only `psx_xg_render_auth_configure` provides transactional composition configuration.
Previously installed callbacks and host services are not rolled back.

Call `psx_xg_render_auth_reset` to remove the configuration state.

## Configure host services

The runtime composition needs a frame counter and a guest word reader.
The host runtime supplies these services before authentication configuration.

Include this header:

```c
#include "xg_render_runtime_host_services.h"
```

Configure `XgRenderRuntimeHostServices` with this function:

```c
bool xg_render_runtime_configure_host_services(
    const XgRenderRuntimeHostServices *services);
```

Register code watches with this function:

```c
void psx_xg_render_auth_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
```

You can register code watches before or after composition configuration.
If registration occurs early, the runtime stores the callback.
The runtime registers each configured range only once.

## Configure the native view

Use this function to configure the host view:

```c
bool psx_xg_render_auth_configure_native_view(
    bool enabled,
    uint16_t aspect_num,
    uint16_t aspect_den,
    uint16_t canonical_width,
    uint16_t canonical_height);
```

When `enabled` is false, the function ignores the dimensions.
When `enabled` is true, all dimensions must be nonzero.
The target aspect must be wider than the canonical aspect.

A failed call leaves the native view disabled.
It does not preserve the previous configuration.

## Enable or disable cold hooks

Use this function:

```c
void psx_xg_render_auth_cold_enable(bool enabled);
```

Disabling cold hooks stops cold-hook ingress.
It aborts an active world submission.
It also dispatches disable invalidation to state owners.
It does not unconfigure composition or unregister host services.

Read the current state with this function:

```c
bool psx_xg_render_auth_cold_enabled(void);
```

The writable global is a legacy application binary interface (ABI) symbol.
Do not use it in new code.

## Report a scene boundary

Call this function after an event starts a new render scene:

```c
void psx_xg_render_auth_scene_boundary(void);
```

Examples include a savestate load or a timing-generation change.
The call invalidates scene-owned authentication and producer state.

Scene and artifact changes may retain the last immutable Movie surface for an
authenticated Movie-to-Field transition. The retained generation is a snapshot,
not a current resource: only a retained `MOVIE_FRAME` may cross the source-owner
generation check, and hard timeline invalidations release it.
Movie publisher replacement sessions and scene boundaries cancel in-flight
assembly but keep that complete publication and retire its capability only
after the retained resource is released.

Outside Native Work, complete MDEC publication is transactional across the Movie
publisher, generic phase/compositor, resource repository, and surface graph.
The Movie generation is committed reversibly so semantic ingress can acquire it;
a rejected ingress or graph commit restores the exact prior semantic bank and
Movie publication.

## Report guest hooks

1. Call `psx_xg_render_auth_cold_hook_relevant` to check hook relevance:

```c
bool psx_xg_render_auth_cold_hook_relevant(
    uint32_t hook,
    uint32_t pc,
    uint32_t instruction_word);
```

2. If the hook is relevant, call `psx_xg_render_auth_cold_hook` with the complete instruction context:

```c
void psx_xg_render_auth_cold_hook(
    CPUState *cpu,
    uint32_t hook,
    uint32_t pc,
    uint32_t instruction_word,
    uint32_t delay_slot_word);
```

Use the warm hook only for an applicable warm route:

```c
void psx_xg_render_auth_warm_hook(
    CPUState *cpu,
    uint32_t hook,
    uint32_t pc,
    uint32_t instruction_word,
    uint32_t delay_slot_word);
```

Do not call a producer-private hook from the host runtime.
The cutover dispatcher selects the producer.

## Report source observations

Use the source lookup operation before you report a source observation.
This operation supplies the operation type, width, and auxiliary rules.

A `pre-stage` observation occurs before the guest operation.
A `commit-stage` observation occurs after the guest operation.
Report both observations in this order.

The two stages must use the same authenticated route.
An incomplete pair does not authorize source data.

## Prepare GPU submission

Call this function before the graphics processing unit (GPU) consumes the command stream:

```c
void psx_xg_render_auth_before_gpu_submission(void);
```

This function finalizes applicable standalone producer submissions.

For a linked user interface (UI) ordering table, call this function before traversal:

> **WARNING:** Do not continue native submission if this function returns `false`.
> A partial visual can produce incorrect runtime output.

```c
bool psx_xg_render_auth_prepare_ui_ot(uint32_t start_addr);
```

If the function returns `false`, stop native submission.
Do not continue with a partial UI visual.

## Report current GPU semantics

Report each current semantic with this function:

```c
void psx_xg_render_auth_note_gpu_semantic_current(
    const GpuRenderSemantic *semantic);
```

This report covers temporal candidates with the same identity.

Complete the source frame with this function:

```c
void psx_xg_render_auth_complete_gpu_source_frame(void);
```

The call flushes the remaining eligible temporal candidates.
This is not the Native Work FIFO publication or presenter pump. Use
`psx_xg_render_auth_source_boundary` for the source publication boundary.

## Report invalidation inputs

Report a code write with this function:

```c
void psx_xg_render_auth_note_code_write(
    uint64_t previous_generation,
    uint64_t current_generation,
    uint32_t guest_pc,
    uint32_t write_size);
```

Report a loader mismatch with this function:

```c
void psx_xg_render_auth_loader_mismatch(uint32_t pc);
```

Report an invalid native entry with this function:

```c
void psx_xg_render_auth_native_bad_entry(uint32_t owner, uint32_t pc);
```

These calls can remove authority.
Do not delay them until the next frame.

## Report a runtime candidate

Use `PsxXgRenderAuthCandidate` to report an authenticated loader candidate.
The candidate identifies the artifact and range.
It also identifies the authenticated entry-and-return pair and runtime variant.

Report the candidate before dispatch:

```c
void psx_xg_render_auth_note_artifact_candidate(
    const PsxXgRenderAuthCandidate *candidate);
```

Report the matching dispatch separately:

```c
void psx_xg_render_auth_note_candidate_dispatch(
    const PsxXgRenderAuthCandidate *candidate);
```

The two reports do not grant authority by themselves.
The complete runtime lifecycle must also match.

## Diagnostic snapshots

Diagnostic functions copy current state to caller-owned storage.
Initialize the storage before the call.

> **WARNING:** Do not use diagnostic data to grant authority.
> Diagnostic data does not prove that a native substitution is valid.

Use these functions for general diagnosis:

```c
psx_xg_render_auth_runtime_snapshot(...);
psx_xg_render_auth_mode_snapshot(...);
psx_xg_render_auth_provenance_snapshot(...);
psx_xg_render_auth_rejection_snapshot(...);
psx_xg_render_auth_completed_proof_snapshot(...);
psx_xg_render_auth_instrumentation_snapshot(...);
```

Use producer-specific snapshot functions to diagnose source data and shadow comparisons.
The diagnostics header declares all producer snapshot functions.

Do not modify renderer state through a snapshot.
Do not infer authority from one counter.
Use the completed proof and rejection receipt together.

### Intent-to-screen correlation

`xg_render_semantic_presentation_diagnostics()` retains coherent receipts for
the latest published source, compile input, compile output, composed endpoint,
and swap-authorized endpoint. Each receipt uses `XgPresentationIdentity` as the
join key and carries the semantic digest, dimensions, format, endpoint handle,
and backend generation available at that stage.

`last_endpoint_mismatch_mask` and `endpoint_mismatches[]` classify compile
output differences in this order: callback, compile fence, endpoint handle,
presentation epoch, source sequence, guest VBlank, guest cycle, scene
generation, semantic digest, width, height, format, and backend generation.
Counters are historical within the presentation lifecycle; receipts describe
the latest observed operation and remain diagnostic-only.

`xg_render_presentation_trace_total()` and
`xg_render_presentation_trace_get()` expose a bounded, always-on ring keyed by
monotonic trace sequence. One entry is created for every accepted publication
and accumulates compile callback/validation/fence state, work ACK, composition,
retirement before swap, swap authorization and callback completion, stale state,
and invalidation. An entry also retains the
source and endpoint receipts, fence handles, mismatch mask, and latest worker
and presenter result. A sequence older than the ring capacity is evicted and
`get` returns false.

The runtime TCP command `native_pipeline_diag` adds backend transport evidence:
compiled endpoint pixel digest, GL endpoint readback hash, composed framebuffer hash,
actual swap result, and compositor feedback. Pixel readback is asynchronous and
opt-in through `PSX_GL_PRESENT_HASH=1`; a newest event may legitimately report
its comparison as pending until a later presentation collects its fence.
The TCP response includes the core `trace_events` and GL `events` rings; join
them using `XgPresentationIdentity`, because their ring sequence counters are
independent. `trace_failure_events` separately returns the first anomalous core
entries still retained, so newer successful frames do not hide the first
available source-to-screen divergence. Stale or invalidated work that already
completed swap remains history rather than a false loss; unswapped stale or
invalidated work is reported as anomalous evidence.

With `PSX_NATIVE_VISUAL_TRUTH=1`, revision 3 additionally enables the GL
backend's canonical software-raster mirror and captures its display rectangle
at each source boundary. The worker joins that complete GP0-fed, persistent
VRAM capture to the compiled endpoint by the full `XgPresentationIdentity` and
reports exact hashes, mismatch bounds/count, pixel samples, scene identity, and
compiler record/pass metrics. `guest_reference_failure_events` preserve the
first 16 failures independently of the recent-event ring. This can expose an
incomplete sealed source commit as well as a native raster error, but it is not an
independent proof of GP0 translation or producer authentication.

For scaled GPU endpoints the comparison samples the logical grid from the actual
GPU result. Storage-image hashes come from fenced GPU readback, not a CPU
stand-in. Read compiler phase generation, compose alpha, and successful swap
receipts separately; neither source ACKs nor generated phase counts prove the
displayed frame rate. Equal image hashes alone do not establish equivalent
motion history.

Replay evidence exposes `runtime_status`, `runtime_reason`, and `native_work`
accounting, including EOF drain completion. These are runtime-health receipts,
not authorization or pixel-equivalence proofs. The retained C API
`psx_xg_render_auth_completed_proof_snapshot` is distinct from the removed replay
JSON `auth_proof` member; do not require that member from current runtime output.

## Checkpoint the native renderer

The control header exposes the aggregate checkpoint operations:

```c
size_t psx_xg_render_auth_checkpoint_size(void);
bool psx_xg_render_auth_checkpoint_write(void *out, size_t size);
bool psx_xg_render_auth_checkpoint_prepare(
    const void *checkpoint,
    size_t size,
    PsxXgRenderCheckpointRestore **out_restore);
void psx_xg_render_auth_checkpoint_commit(
    PsxXgRenderCheckpointRestore *restore);
void psx_xg_render_auth_checkpoint_cancel(
    PsxXgRenderCheckpointRestore *restore);
```

Aggregate wire version 6 contains the canonical VRAM journal before the VRAM
resource, Movie, surface-graph, and UI sections. The journal section preserves
the observed and authoritative VRAM bitmaps plus the canonical value of every
observed word. Mutation and event serials remain process-monotonic: commit
installs the saved map under one fresh restore mutation instead of restoring an
old serial. The following public boot-restore VRAM event consumes the preapplied
marker and must not clear the map or advance it again.

Checkpoint sizing and writing fail closed while a Native journal transfer is
active. Prepare allocates and validates all replacement state without changing
the live journal. Commit is allocation-free; cancel discards only staged state.
The saved journal generation must equal the source generation recorded by the
VRAM-resource section.

Version 6 also records whether the runtime held the final Movie surface. Such a
checkpoint is valid only when the Movie section contains that complete immutable
frame. Restore remaps it to the fresh owner generation and re-establishes exactly
one runtime retain before later Movie-to-Field composition.

Execution authority is deliberately absent from checkpoints. Restored producer
routes must be observed again before they can publish Native work.

## Compatibility symbols

The API keeps two legacy symbols:

- `g_psx_xg_render_auth_cold_enabled`;
- `psx_xg_render_auth_capture_model_ft3_link`.

Do not use these symbols in new integrations.
Use the focused getter and the normal hook dispatcher.

## Thread behavior

Do not assume that all runtime API functions are thread-safe.
Call control and hook functions from the established runtime thread.

Instrumentation snapshots use internal synchronization.
This synchronization does not make concurrent access to all producer state safe.

## Error policy

Most hook functions return no error value.
They record a rejection and remove authority when necessary.

Read the rejection snapshot after the renderer unexpectedly rejects native use.
Do not add an alternate native path around a rejection.
