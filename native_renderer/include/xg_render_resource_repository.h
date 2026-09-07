#ifndef XG_RENDER_RESOURCE_REPOSITORY_H
#define XG_RENDER_RESOURCE_REPOSITORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XG_RENDER_RESOURCE_REPOSITORY_CAPACITY
#define XG_RENDER_RESOURCE_REPOSITORY_CAPACITY 4096u
#endif

#define XG_RENDER_RESOURCE_IDENTITY_SIZE 32u
#define XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE 224u
#define XG_RENDER_RESOURCE_DESCRIPTOR_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderResourceKind {
    XG_RENDER_RESOURCE_TEXTURE = 0,
    XG_RENDER_RESOURCE_CLUT,
    XG_RENDER_RESOURCE_MODEL,
    XG_RENDER_RESOURCE_GLYPH_ATLAS,
    XG_RENDER_RESOURCE_GENERATED_SURFACE,
    XG_RENDER_RESOURCE_FRAMEBUFFER,
    XG_RENDER_RESOURCE_MOVIE_FRAME,
} XgRenderResourceKind;

typedef enum XgRenderResourceOwnerKind {
    XG_RENDER_RESOURCE_OWNER_STATIC = 0,
    XG_RENDER_RESOURCE_OWNER_DISC,
    XG_RENDER_RESOURCE_OWNER_MODULE,
    XG_RENDER_RESOURCE_OWNER_SCENE,
    XG_RENDER_RESOURCE_OWNER_SOURCE,
    XG_RENDER_RESOURCE_OWNER_BATCH,
} XgRenderResourceOwnerKind;

typedef enum XgRenderResourceState {
    XG_RENDER_RESOURCE_UNINITIALIZED = 0,
    XG_RENDER_RESOURCE_IMPORTING_NATIVE,
    XG_RENDER_RESOURCE_NATIVE_OWNED,
    XG_RENDER_RESOURCE_GUEST_GPU_CANONICAL,
    XG_RENDER_RESOURCE_GUEST_VISIBLE_MIRROR,
} XgRenderResourceState;

typedef enum XgRenderResourceProvenanceKind {
    XG_RENDER_RESOURCE_PROVENANCE_NONE = 0,
    XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT,
    XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
} XgRenderResourceProvenanceKind;

/* NONE is accepted only for explicitly synthetic resources with receipt zero.
 * Production ARTIFACT/SOURCE provenance requires a registered capability. */
typedef struct XgRenderResourceProvenance {
    XgRenderResourceProvenanceKind kind;
    uint64_t receipt;
    uint64_t capability;
    bool synthetic;
} XgRenderResourceProvenance;

typedef struct XgRenderResourceHandle {
    uint64_t resource_id;
    uint64_t generation;
} XgRenderResourceHandle;

typedef struct XgRenderResourceIdentity {
    uint8_t bytes[XG_RENDER_RESOURCE_IDENTITY_SIZE];
} XgRenderResourceIdentity;

/* Pointer-free interpretation metadata.  A zero-filled descriptor is an
 * intentionally opaque resource and is never sufficient for pixel sampling.
 * Versioned descriptors are immutable for the lifetime of one generation. */
typedef enum XgRenderResourcePixelFormat {
    XG_RENDER_RESOURCE_PIXEL_FORMAT_UNSPECIFIED = 0,
    XG_RENDER_RESOURCE_PIXEL_FORMAT_NON_PIXEL,
    XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8,
    XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24,
    XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
    XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4,
    XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8,
    XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555,
} XgRenderResourcePixelFormat;

typedef enum XgRenderResourceSampler {
    XG_RENDER_RESOURCE_SAMPLER_UNSPECIFIED = 0,
    XG_RENDER_RESOURCE_SAMPLER_NEAREST,
    XG_RENDER_RESOURCE_SAMPLER_LINEAR,
} XgRenderResourceSampler;

typedef enum XgRenderResourceWrap {
    XG_RENDER_RESOURCE_WRAP_UNSPECIFIED = 0,
    XG_RENDER_RESOURCE_WRAP_CLAMP,
    XG_RENDER_RESOURCE_WRAP_REPEAT,
    XG_RENDER_RESOURCE_WRAP_MIRROR,
} XgRenderResourceWrap;

enum {
    XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION = 1u << 0,
};

typedef struct XgRenderResourceDescriptor {
    uint32_t version;
    XgRenderResourcePixelFormat pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t vram_x;
    uint32_t vram_y;
    uint32_t vram_width;
    uint32_t vram_height;
    XgRenderResourceSampler sampler;
    XgRenderResourceWrap wrap_u;
    XgRenderResourceWrap wrap_v;
    uint32_t flags;
} XgRenderResourceDescriptor;

typedef enum XgRenderResourceCapabilityLifetime {
    XG_RENDER_RESOURCE_CAPABILITY_STATIC = 1,
    XG_RENDER_RESOURCE_CAPABILITY_DISC,
    XG_RENDER_RESOURCE_CAPABILITY_MODULE,
    XG_RENDER_RESOURCE_CAPABILITY_SCENE,
    XG_RENDER_RESOURCE_CAPABILITY_TIMELINE,
} XgRenderResourceCapabilityLifetime;

typedef enum XgRenderResourceSourceClass {
    XG_RENDER_RESOURCE_SOURCE_NONE = 0,
    XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE,
    XG_RENDER_RESOURCE_SOURCE_MDEC_DMA_RAM_GPU,
    XG_RENDER_RESOURCE_SOURCE_GPU_SCANOUT,
    XG_RENDER_RESOURCE_SOURCE_MOVIE_OWNER,
    XG_RENDER_RESOURCE_SOURCE_UI_GENERATED,
    XG_RENDER_RESOURCE_SOURCE_VRAM_CARRY,
    XG_RENDER_RESOURCE_SOURCE_VRAM_TRANSFER,
} XgRenderResourceSourceClass;

typedef struct XgRenderArtifactIdentity {
    uint32_t base;
    uint32_t size;
    uint32_t crc32;
    uint8_t sha256[XG_RENDER_RESOURCE_IDENTITY_SIZE];
} XgRenderArtifactIdentity;

typedef struct XgRenderResourceSourceIdentity {
    XgRenderResourceSourceClass source_class;
    XgRenderResourceIdentity identity;
    uint64_t range_offset;
    uint64_t range_size;
    uint64_t range_content_digest;
    XgRenderArtifactIdentity origin_artifact;
} XgRenderResourceSourceIdentity;

typedef struct XgRenderResourceCapabilityMetadata {
    XgRenderResourceProvenanceKind kind;
    uint64_t receipt;
    XgRenderResourceCapabilityLifetime lifetime;
    XgRenderResourceOwnerKind owner_kind;
    uint64_t owner_generation;
    XgRenderArtifactIdentity artifact;
    XgRenderResourceSourceIdentity source;
} XgRenderResourceCapabilityMetadata;

/* Canonical, pointer-free authority carried by native checkpoints.  A raw
 * capability is deliberately insufficient to recreate authority after a
 * repository reset. */
typedef struct XgRenderResourceCapabilityCheckpoint {
    uint8_t bytes[XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE];
} XgRenderResourceCapabilityCheckpoint;

typedef enum XgRenderResourceCapabilityResult {
    XG_RENDER_RESOURCE_CAPABILITY_OK = 0,
    XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT,
    XG_RENDER_RESOURCE_CAPABILITY_CONFLICT,
    XG_RENDER_RESOURCE_CAPABILITY_CAPACITY_EXCEEDED,
    XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND,
    XG_RENDER_RESOURCE_CAPABILITY_REVOKED,
    XG_RENDER_RESOURCE_CAPABILITY_RESTORE_ACTIVE,
} XgRenderResourceCapabilityResult;

typedef struct XgRenderResourceImport {
    uint64_t resource_id;
    XgRenderResourceKind kind;
    XgRenderResourceOwnerKind owner_kind;
    uint64_t owner_generation;
    XgRenderResourceState state;
    XgRenderResourceProvenance provenance;
    uint64_t content_digest;
    const void *bytes;
    size_t byte_count;
    XgRenderResourceIdentity identity;
    XgRenderResourceDescriptor descriptor;
} XgRenderResourceImport;

typedef struct XgRenderResourceView {
    XgRenderResourceHandle handle;
    XgRenderResourceKind kind;
    XgRenderResourceOwnerKind owner_kind;
    uint64_t owner_generation;
    XgRenderResourceState state;
    XgRenderResourceProvenance provenance;
    uint64_t content_digest;
    const void *bytes;
    size_t byte_count;
    uint32_t retain_count;
    bool current;
    bool has_identity;
    XgRenderResourceIdentity identity;
    XgRenderResourceDescriptor descriptor;
} XgRenderResourceView;

typedef enum XgRenderResourceResult {
    XG_RENDER_RESOURCE_OK = 0,
    XG_RENDER_RESOURCE_INVALID_ARGUMENT,
    XG_RENDER_RESOURCE_DIGEST_MISMATCH,
    XG_RENDER_RESOURCE_CAPACITY_EXCEEDED,
    XG_RENDER_RESOURCE_OUT_OF_MEMORY,
    XG_RENDER_RESOURCE_NOT_FOUND,
    XG_RENDER_RESOURCE_STALE,
    XG_RENDER_RESOURCE_INVALID_STATE,
    XG_RENDER_RESOURCE_IDENTITY_COLLISION,
} XgRenderResourceResult;

typedef struct XgRenderResourceDiagnostics {
    uint64_t import_starts;
    uint64_t imports;
    uint64_t import_cancellations;
    uint64_t generations;
    uint64_t invalidations;
    uint64_t retirements;
    uint64_t stale_accesses;
    uint64_t ownership_violations;
    uint64_t registered_resource_ids;
    uint64_t identity_collisions;
    size_t live_resources;
    size_t retained_resources;
    size_t live_capabilities;
    size_t capability_slots;
    /* Failed retain/acquire_snapshot/release operations, plus malformed,
     * digest-mismatched or overflowing acquire_current requests. A missing or
     * superseded current generation is an availability miss, not a failed retain. */
    uint64_t reference_failures;
} XgRenderResourceDiagnostics;

typedef enum XgRenderResourceRestoreFault {
    XG_RENDER_RESOURCE_RESTORE_FAULT_NONE = 0,
    XG_RENDER_RESOURCE_RESTORE_FAULT_ALLOCATION,
    XG_RENDER_RESOURCE_RESTORE_FAULT_CAPACITY,
} XgRenderResourceRestoreFault;

/* Destructive initialization/teardown only; all resource users must be quiescent.
 * Runtime scene/restore paths use invalidate_from_owner, not this reset. */
void xg_render_resource_repository_reset(void);
XgRenderResourceCapabilityResult xg_render_resource_capability_register(
    const XgRenderResourceCapabilityMetadata *metadata,
    XgRenderResourceProvenance *out_provenance);
/* Reports whether this call minted the returned capability so transactional
 * producers can revoke only authority created by their failed preparation. */
XgRenderResourceCapabilityResult xg_render_resource_capability_register_tracked(
    const XgRenderResourceCapabilityMetadata *metadata,
    XgRenderResourceProvenance *out_provenance,
    bool *out_created);
XgRenderResourceCapabilityResult xg_render_resource_capability_validate(
    const XgRenderResourceProvenance *provenance,
    XgRenderResourceOwnerKind owner_kind,
    uint64_t owner_generation,
    XgRenderResourceCapabilityMetadata *out_metadata);
XgRenderResourceCapabilityResult xg_render_resource_capability_revoke(
    XgRenderResourceProvenance provenance);
/* Marks superseded authority for retirement. Revocation occurs immediately or
 * after the final referencing resource retires; restore staging preserves it. */
XgRenderResourceCapabilityResult xg_render_resource_capability_retire(
    XgRenderResourceProvenance provenance);
XgRenderResourceCapabilityResult xg_render_resource_capability_checkpoint(
    const XgRenderResourceProvenance *provenance,
    XgRenderResourceOwnerKind owner_kind,
    uint64_t owner_generation,
    XgRenderResourceCapabilityCheckpoint *out_checkpoint);
XgRenderResourceCapabilityResult
xg_render_resource_capability_checkpoint_validate(
    const XgRenderResourceCapabilityCheckpoint *checkpoint,
    const XgRenderResourceProvenance *provenance,
    XgRenderResourceOwnerKind owner_kind,
    uint64_t owner_generation,
    XgRenderResourceCapabilityMetadata *out_metadata);
bool xg_render_resource_capability_restore_begin(void);
XgRenderResourceCapabilityResult xg_render_resource_capability_restore_stage(
    XgRenderResourceProvenance provenance,
    XgRenderResourceOwnerKind owner_kind,
    uint64_t restored_owner_generation);
XgRenderResourceCapabilityResult
xg_render_resource_capability_checkpoint_restore_stage(
    const XgRenderResourceCapabilityCheckpoint *checkpoint,
    const XgRenderResourceProvenance *checkpoint_provenance,
    XgRenderResourceOwnerKind owner_kind,
    uint64_t original_owner_generation,
    uint64_t restored_owner_generation,
    XgRenderResourceProvenance *out_restored_provenance);
void xg_render_resource_capability_restore_commit(void);
void xg_render_resource_capability_restore_cancel(void);
uint64_t xg_render_resource_digest(const void *bytes, size_t byte_count);
/* Accepts the canonical all-zero opaque descriptor or a complete descriptor
 * at XG_RENDER_RESOURCE_DESCRIPTOR_VERSION.  Authenticated texture, CLUT, and
 * movie imports require the latter; synthetic resources and opaque resource
 * kinds may retain the zero form. Pixel consumers must reject version zero. */
bool xg_render_resource_descriptor_validate(
    const XgRenderResourceDescriptor *descriptor);
uint64_t xg_render_resource_identity_id(
    const XgRenderResourceIdentity *identity);
XgRenderResourceResult xg_render_resource_import(
    const XgRenderResourceImport *import,
    XgRenderResourceHandle *out_handle);
XgRenderResourceResult xg_render_resource_import_begin(
    const XgRenderResourceImport *import,
    XgRenderResourceHandle *out_handle);
XgRenderResourceResult xg_render_resource_import_commit(
    XgRenderResourceHandle handle);
/* Validates every handle before making any pending import current. Existing
 * current handles returned by import_begin are accepted as deduplicated. */
XgRenderResourceResult xg_render_resource_import_commit_many(
    const XgRenderResourceHandle *handles,
    size_t handle_count);
XgRenderResourceResult xg_render_resource_import_cancel(
    XgRenderResourceHandle handle);
/* Convenience for Native producers; internally performs the observable
 * IMPORTING_NATIVE -> NATIVE_OWNED transition. */
XgRenderResourceResult xg_render_resource_import_native(
    const XgRenderResourceImport *import,
    XgRenderResourceHandle *out_handle);
/* Restore reservations own all allocation and capacity risk before a scene
 * boundary. They reserve a private generation even when current bytes match.
 * Reserved imports remain non-current until commit and are the only pending
 * imports protected from boundary invalidation. */
XgRenderResourceResult xg_render_resource_restore_stage(
    const XgRenderResourceImport *import,
    XgRenderResourceHandle *out_handle);
XgRenderResourceResult xg_render_resource_restore_commit(
    XgRenderResourceHandle handle);
void xg_render_resource_restore_cancel(XgRenderResourceHandle handle);
void xg_render_resource_restore_fault_inject(
    XgRenderResourceRestoreFault fault, uint32_t fail_after);
/* Makes exactly this current generation stale. Existing retains keep its
 * immutable bytes alive until their final release. */
XgRenderResourceResult xg_render_resource_retire_current(
    XgRenderResourceHandle handle);
/* Transactional producer rollback only.  retained_old_handle must name a
 * retained, immutable predecessor of new_handle.  Under the repository lock,
 * this reactivates that exact generation and retires new_handle only when the
 * latter is still current and both handles have the same registered identity.
 * A third-party replacement therefore makes the operation fail closed. */
XgRenderResourceResult xg_render_resource_rollback_current(
    XgRenderResourceHandle new_handle,
    XgRenderResourceHandle retained_old_handle);
XgRenderResourceResult xg_render_resource_retain(
    XgRenderResourceHandle handle);
XgRenderResourceResult xg_render_resource_acquire_current(
    XgRenderResourceHandle handle,
    uint64_t content_digest);
/* Acquires an immutable snapshot generation. A retired generation is accepted
 * only while an existing owner still retains it. */
XgRenderResourceResult xg_render_resource_acquire_snapshot(
    XgRenderResourceHandle handle,
    uint64_t content_digest);
XgRenderResourceResult xg_render_resource_release(
    XgRenderResourceHandle handle);
XgRenderResourceResult xg_render_resource_view(
    XgRenderResourceHandle handle,
    XgRenderResourceView *out_view);
void xg_render_resource_invalidate_owner(
    XgRenderResourceOwnerKind owner_kind,
    uint64_t owner_generation);
void xg_render_resource_invalidate_transient(void);
void xg_render_resource_invalidate_scene_boundary(void);
void xg_render_resource_invalidate_from_owner(
    XgRenderResourceOwnerKind first_owner_kind);
void xg_render_resource_repository_diagnostics(
    XgRenderResourceDiagnostics *out_diagnostics);

#ifdef __cplusplus
}
#endif

#endif
