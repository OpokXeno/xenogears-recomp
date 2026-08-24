# Native renderer development guide

## Purpose

Use this guide when you change renderer source code.
Read [the architecture](ARCHITECTURE.md) before you change module ownership.

## Select the correct directory

Put a new file in the directory that owns its state.

| Change | Directory |
|---|---|
| Math or generic primitive conversion | `src/core/` |
| Shared submission or lookup mechanism | `src/infrastructure/` |
| Authentication policy | `src/auth/` |
| Runtime wiring or dispatch | `src/runtime/` |
| Field-specific producer state | `src/producers/field/` |
| Model-specific producer state | `src/producers/model/` |
| World-specific producer state | `src/producers/world/` |

Do not put producer state in `xg_render_auth_runtime.c`.
Do not put authentication decisions in infrastructure.

## Add a producer

Use this procedure for a new producer.

1. Identify the exact game operation.
2. Obtain trusted code and data evidence.
3. Define the producer lifecycle.
4. Define a data transfer object (DTO) for the source.
5. Validate all guest address ranges.
6. Implement source capture in the producer domain.
7. Implement primitive construction in the producer domain.
8. Add a local authorization policy when necessary.
9. Add a route descriptor to the dispatcher.
10. Register the producer resolver when necessary.
11. Add producer-owned invalidation handling.
12. Add diagnostic snapshots.
13. Add focused tests.
14. Add an integration test for the route.
15. Update this documentation.

Do not add a route before the lifecycle is defined.
Do not authorize a route from its program counter (PC) only.

## Define a lifecycle

A lifecycle specifies the required event order.
Use explicit states and generation values.

A typical lifecycle contains these events:

1. start;
2. source capture;
3. primitive construction;
4. submission;
5. completion.

Reject an event that belongs to an old generation.
Reject an event that occurs in the wrong order.
Clear pending state at the applicable invalidation event.

## Capture guest data

Validate a guest range before you read it.
If the data type requires alignment, check the guest address alignment.
Check the complete range, not only the first address.

Copy source data into a producer-owned DTO.
Do not keep an unsafe pointer into guest memory.

Record the source generation with the DTO.
Reject the DTO after the generation changes.

## Construct primitives

Use `XgRenderIrNativePrimitive` as the common intermediate representation (IR) output type.
Use core builders for common geometry.
Keep producer policy outside the core builders.

Set material and interpolation identities explicitly.
Record the packet address and the index of the source primitive.

Do not stage a primitive before its source data is complete.

## Submit primitives

Use `xg_render_submission` for shared submission behavior.
If a shared application programming interface (API) exists, do not stage the native stream directly from a producer.

Use one of these submission types:

- authenticated exact submission;
- standalone submission;
- pre-scene submission;
- temporal submission.

Use one commit point.
If staging fails, abort the transaction.
Do not leave inactive commands in the stream.

## Add a route

Add the route to the route descriptor data.
Match both PC and instruction when the contract requires both values.

The route action must call the owning producer through composition.
The dispatcher must return only a generic result.

Use `CONTINUE` when the route does not consume execution.
Use `OBSERVED` after a successful observation.
If an authenticated native replacement succeeds, return `BYPASS`.

## Add invalidation

First, identify the state owner.
Then, add the smallest applicable invalidation handler.

The mutation classifier describes the change.
It does not clear producer state.

The invalidation dispatcher sends a structured event.
The producer handler clears its own state.

Do not create a central switch for producer-specific cleanup.

## Add diagnostics

Put snapshot data types in a focused public type header.
Put snapshot functions in the diagnostics API.

A snapshot must copy state to caller-owned storage.
A snapshot must not change authority.

If later failures can hide the first cause, record the first failure in a separate field.
Use a counter only when the count has a clear operational meaning.

## Public headers

Keep public include paths stable.
Put implementation-only declarations in the applicable source directory.

Use a type-only header when a module needs data but not operations.
Do not include an operational umbrella for one DTO.

New runtime integrations must include focused runtime headers.
Do not add new declarations to `xg_render_auth_runtime.h`.

## State storage

Keep mutable state private to its owner.
Do not export a new writable global.

Use reset functions for lifecycle control.
Make repeated reset operations safe.

Use fixed capacities when deterministic storage is required.
Reject capacity exhaustion without overwriting old data.

## Test location

Put each test in the directory that matches its owner.

| Test type | Directory |
|---|---|
| Authentication runtime | `tests/auth/` |
| Core math or IR | `tests/core/` |
| Manifest or dependency contract | `tests/contracts/` |
| Field producer | `tests/field/` |
| Shared infrastructure | `tests/infrastructure/` |
| End-to-end runtime path | `tests/integration/` |
| Model producer | `tests/model/` |
| World producer | `tests/world/` |

Production builds must not enable a test-only control path.
Use a test adapter when practical.
Use a test adapter in `tests/` when a seam is necessary.

## Select tests

Run the smallest applicable test first.

Examples:

```sh
ctest --test-dir build --output-on-failure -R '^xg_render_auth$'
ctest --test-dir build --output-on-failure -R '^xg_render_static_auth$'
ctest --test-dir build --output-on-failure -R '^xg_world_models$'
```

Run manifest tests after a manifest or parser change:

```sh
python -m pytest -q native_renderer/tests/contracts/test_native_render_manifest.py
python -m pytest -q native_renderer/tests/contracts/test_native_render_runtime_variants.py
```

Run the overlay-free build contract only after CMake or manifest build changes.
That test can take several minutes.

Run the complete suite before a release or a large integration change.

## Review checklist

Before you finish a change, answer each question.

- Does one module own each new state value?
- Does each guest read have a complete range check?
- Does each route have authenticated code evidence?
- Does each lifecycle reject stale generations?
- Does each transaction have one commit or one abort?
- Does each invalidation handler clear only owned state?
- Does a diagnostic path avoid granting authority?
- Does default CMake avoid private overlay files?
- Do focused public headers remain independent?
- Do all new tests run from CTest or Pytest discovery?
- Does `git diff --check` pass?
- Is the documentation still correct?

## Prohibited changes

Do not make these changes:

- a hard-coded exception for one optional overlay name;
- a fallback that uses an unauthenticated native substitution;
- a global dispatch resolver that selects routes from writers;
- a producer call that bypasses runtime composition;
- a partial transaction without cleanup;
- an exported mutable state value;
- a test-only macro in production source;
- an inclusion directive for a `.c` or `.inc` implementation file;
- an edit to a generated source table.
