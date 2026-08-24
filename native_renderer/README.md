# Xenogears native renderer

The native renderer converts authenticated game data into host render commands.
It does not replace game logic.
It replaces only an authenticated render operation.

This documentation uses the principles of ASD-STE100 Simplified Technical English.
It is not an official ASD-STE100 compliance certificate.

## Safety rule

> **WARNING:** Do not use a native substitution when authentication is incomplete.
> An invalid substitution can change the displayed frame.

The renderer uses a fail-closed design.
If a required check fails, the renderer rejects the native path.
The game renderer remains the reference path.

## Start here

Read these documents in this order:

1. Read [the glossary](docs/GLOSSARY.md).
2. Read [the architecture](docs/ARCHITECTURE.md).
3. Read [the manifest guide](docs/MANIFESTS.md) before you change metadata.
4. Read [the application programming interface guide](docs/RUNTIME_API.md) before you change runtime integration.
5. Follow [the development guide](docs/DEVELOPMENT.md) before you add a producer.

## What the renderer does

The renderer observes selected game instructions.
It authenticates the game code and the source data.
It sends each observation to one producer.
The producer creates an intermediate representation (IR) primitive.
The submission service converts the primitive to a graphics processing unit (GPU) semantic.
The runtime then binds the semantic to the original game command.

```text
game instruction
      |
      v
authentication
      |
      v
producer capture
      |
      v
render IR primitive
      |
      v
transactional submission
      |
      v
GPU semantic
```

A GPU semantic is a host description of one game render command.

## Directory map

| Directory | Content |
|---|---|
| `include/` | Stable public headers and public data types. |
| `src/core/` | Math, render IR, backend conversion, and view state. |
| `src/infrastructure/` | Shared lookup, submission, resolver, and instrumentation services. |
| `src/auth/` | Static authentication and runtime authentication policy. |
| `src/runtime/` | Runtime wiring, dispatch, lifecycle, and invalidation. |
| `src/producers/field/` | Field render producers and their source data. |
| `src/producers/model/` | Model and sprite render producers. |
| `src/producers/world/` | World render producers and the world coordinator. |
| `tests/` | Tests grouped by the same ownership domains. |
| `docs/` | Operation, metadata, API, and development guides. |

## Main configuration files

| File | Purpose |
|---|---|
| `xg_render_manifest.toml` | Declares the game, overlays, fixed functions, producer, and capture site. |
| `xg_render_runtime_variants.toml` | Binds runtime-loaded code to the canonical manifest. |
| `xg_render_overlay_ranges.toml` | Declares authenticated overlay ranges and cutover sites. |
| `xg_3d_certification.toml` | Provides a template for full renderer certification. |

Do not put binary data in these files.
Do not put local file paths in public evidence.

## Important properties

- Authentication is mandatory.
- A producer owns its source data and lifecycle state.
- A code write invalidates affected authentication state.
- A scene change invalidates scene-owned state.
- Submission is transactional.
- A failed transaction does not leave partial commands.
- Diagnostic snapshots do not grant authority.
- Shadow comparison does not grant authority.
- Optional overlays do not become build requirements by default.
