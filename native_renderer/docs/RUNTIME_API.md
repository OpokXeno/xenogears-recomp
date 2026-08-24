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

`xg_render_auth_runtime.h` includes all focused headers.
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

## Configure the runtime

Call `psx_xg_render_auth_configure` once during startup.
Supply the requested timing mode and render mode.
Supply a presentation gate and the user data for that gate.

```c
bool psx_xg_render_auth_configure(
    GuestRenderTimingMode requested_timing_mode,
    GuestRenderRenderMode requested_render_mode,
    PsxXgRenderPresentationGate presentation_gate,
    void *presentation_user_data);
```

Check the return value.
Stop startup when the function returns `false`.

Unsupported timing or render values are normalized to their `ORIGINAL` values.
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
