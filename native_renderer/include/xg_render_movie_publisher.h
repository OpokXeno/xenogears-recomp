#ifndef XG_RENDER_MOVIE_PUBLISHER_H
#define XG_RENDER_MOVIE_PUBLISHER_H

#include "xg_render_resource_repository.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderMovieOwnerKind {
    XG_RENDER_MOVIE_OWNER_NONE = 0,
    XG_RENDER_MOVIE_OWNER_STANDALONE = 1,
    XG_RENDER_MOVIE_OWNER_FIELD = 2,
} XgRenderMovieOwnerKind;

typedef struct XgRenderMovieFrameDescription {
    uint64_t movie_id;
    XgRenderMovieOwnerKind owner_kind;
    uint64_t owner_receipt;
    uint64_t owner_capability;
    uint64_t owner_generation;
    uint64_t guest_cycle;
    uint32_t width;
    uint32_t height;
    uint32_t expected_strips;
    size_t byte_count;
    bool depth24;
    bool odd_field;
} XgRenderMovieFrameDescription;

typedef struct XgRenderMovieFrameHandle {
    uint32_t generation;
} XgRenderMovieFrameHandle;

typedef enum XgRenderMovieDiscontinuity {
    XG_RENDER_MOVIE_DISCONTINUITY_NONE = 0,
    XG_RENDER_MOVIE_DISCONTINUITY_SKIP,
    XG_RENDER_MOVIE_DISCONTINUITY_LOOP,
    XG_RENDER_MOVIE_DISCONTINUITY_STALL,
    XG_RENDER_MOVIE_DISCONTINUITY_DEPTH_CHANGE,
    XG_RENDER_MOVIE_DISCONTINUITY_RESTORE,
} XgRenderMovieDiscontinuity;

typedef struct XgRenderMovieFramePublication {
    XgRenderResourceHandle surface;
    XgRenderMovieOwnerKind owner_kind;
    uint64_t owner_receipt;
    uint64_t owner_capability;
    uint64_t frame_generation;
    uint64_t guest_cycle;
    uint32_t completed_strip_mask;
    uint32_t width;
    uint32_t height;
    bool depth24;
    bool odd_field;
    bool held;
    bool discontinuity;
    XgRenderMovieDiscontinuity discontinuity_reason;
} XgRenderMovieFramePublication;

typedef enum XgRenderMovieResult {
    XG_RENDER_MOVIE_OK = 0,
    XG_RENDER_MOVIE_INVALID_ARGUMENT,
    XG_RENDER_MOVIE_FRAME_ACTIVE,
    XG_RENDER_MOVIE_STALE_FRAME,
    XG_RENDER_MOVIE_DUPLICATE_STRIP,
    XG_RENDER_MOVIE_INCOMPLETE_FRAME,
    XG_RENDER_MOVIE_OUT_OF_MEMORY,
    XG_RENDER_MOVIE_RESOURCE_FAILED,
    XG_RENDER_MOVIE_NO_COMPLETE_FRAME,
    XG_RENDER_MOVIE_INVALID_SNAPSHOT,
} XgRenderMovieResult;

typedef struct XgRenderMovieDiagnostics {
    uint64_t complete_frames;
    uint64_t frame_generation;
    uint64_t partial_publish_attempts;
    uint64_t cancelled_frames;
    uint64_t held_frames;
    uint64_t discontinuities;
    uint32_t completed_strip_mask;
    bool frame_active;
    bool complete_frame_available;
    bool discontinuity_pending;
    XgRenderMovieDiscontinuity discontinuity_reason;
} XgRenderMovieDiagnostics;

#define XG_RENDER_MOVIE_PUBLISHER_SNAPSHOT_VERSION 5u
#define XG_RENDER_MOVIE_PUBLISHER_WIRE_VERSION 5u

/*
 * Variable-sized, pointer-free snapshot. Allocate the byte count returned by
 * xg_render_movie_publisher_snapshot_size() and serialize exactly that range.
 * This native size includes any padding needed to reserve at least sizeof the
 * snapshot object. Readers also accept the exact offsetof(payload) + payload
 * length, but typed storage must still reserve at least sizeof the object.
 * The canonical wire v5 format does not include native structure padding.
 */
typedef struct XgRenderMoviePublisherSnapshot {
    uint32_t version;
    uint32_t assembly_generation;
    uint64_t snapshot_size;
    uint64_t published_frame_generation;
    XgRenderMovieFrameDescription active_description;
    XgRenderMovieFrameDescription complete_description;
    XgRenderMovieFramePublication complete_publication;
    XgRenderMovieDiagnostics diagnostics;
    uint64_t active_byte_count;
    uint64_t active_written_byte_count;
    uint64_t complete_byte_count;
    uint32_t completed_strip_mask;
    bool active;
    bool complete_frame_available;
    bool discontinuity_pending;
    XgRenderMovieDiscontinuity discontinuity_reason;
    XgRenderResourceCapabilityCheckpoint active_authority;
    XgRenderResourceCapabilityCheckpoint complete_authority;
    uint8_t payload[];
} XgRenderMoviePublisherSnapshot;

typedef struct XgRenderMovieCheckpointRestore XgRenderMovieCheckpointRestore;
typedef struct XgRenderMovieFrameTransaction XgRenderMovieFrameTransaction;

void xg_render_movie_publisher_reset(void);
void xg_render_movie_publisher_scene_boundary(void);
XgRenderMovieResult xg_render_movie_frame_begin(
    const XgRenderMovieFrameDescription *description,
    XgRenderMovieFrameHandle *out_frame);
XgRenderMovieResult xg_render_movie_frame_write_strip(
    XgRenderMovieFrameHandle frame,
    uint32_t strip_index,
    size_t byte_offset,
    const void *bytes,
    size_t byte_count);
XgRenderMovieResult xg_render_movie_frame_publish(
    XgRenderMovieFrameHandle frame,
    XgRenderMovieFramePublication *out_publication);
/* Prepares an already-complete frame without changing publisher state. The
 * repository resource and publisher bytes remain private until commit. */
XgRenderMovieResult xg_render_movie_frame_transaction_prepare(
    const XgRenderMovieFrameDescription *description,
    const void *bytes, size_t byte_count,
    XgRenderMovieFrameTransaction **out_transaction,
    XgRenderMovieFramePublication *out_publication);
XgRenderMovieResult xg_render_movie_frame_transaction_commit(
    XgRenderMovieFrameTransaction *transaction);
void xg_render_movie_frame_transaction_cancel(
    XgRenderMovieFrameTransaction *transaction);
/* Restricted recovery path for a transaction that was committed so a
 * SourceCommit could validate the new current surface, but whose later seal
 * failed.  retained_old_surface is zero only when snapshot had no complete
 * frame.  Success restores snapshot without importing an alias generation. */
XgRenderMovieResult xg_render_movie_publisher_transaction_rollback(
    const XgRenderMoviePublisherSnapshot *snapshot,
    size_t snapshot_size,
    XgRenderResourceHandle new_surface,
    XgRenderResourceHandle retained_old_surface,
    bool new_surface_owned);
XgRenderMovieResult xg_render_movie_frame_hold(
    XgRenderMovieFramePublication *out_publication);
XgRenderMovieResult xg_render_movie_frame_cancel(
    XgRenderMovieFrameHandle frame);
XgRenderMovieResult xg_render_movie_publisher_note_discontinuity(
    XgRenderMovieDiscontinuity reason);
XgRenderMovieResult xg_render_movie_publisher_snapshot_size(
    size_t *out_size);
XgRenderMovieResult xg_render_movie_publisher_snapshot(
    XgRenderMoviePublisherSnapshot *out_snapshot,
    size_t snapshot_capacity);
XgRenderMovieResult xg_render_movie_publisher_restore(
    const XgRenderMoviePublisherSnapshot *snapshot,
    size_t snapshot_size,
    XgRenderMovieFrameHandle *out_active_frame);
XgRenderMovieResult xg_render_movie_publisher_wire_size(size_t *out_size);
XgRenderMovieResult xg_render_movie_publisher_wire_write(
    void *out_wire, size_t wire_capacity);
XgRenderMovieResult xg_render_movie_publisher_wire_validate(
    const void *wire, size_t wire_size, uint64_t restored_owner_generation);
XgRenderMovieResult xg_render_movie_publisher_wire_restore(
    const void *wire, size_t wire_size, uint64_t restored_owner_generation,
    XgRenderMovieFrameHandle *out_active_frame);
XgRenderMovieResult xg_render_movie_publisher_wire_prepare(
    const void *wire, size_t wire_size, uint64_t restored_owner_generation,
    XgRenderMovieCheckpointRestore **out_restore);
/* Returns the staged complete-frame resource, when present, so a composite
 * checkpoint transaction can share its identity with another subsystem. */
XgRenderMovieResult xg_render_movie_publisher_checkpoint_resource(
    const XgRenderMovieCheckpointRestore *restore,
    XgRenderResourceHandle *out_resource);
void xg_render_movie_publisher_wire_commit(
    XgRenderMovieCheckpointRestore *restore,
    XgRenderMovieFrameHandle *out_active_frame);
void xg_render_movie_publisher_wire_cancel(
    XgRenderMovieCheckpointRestore *restore);
void xg_render_movie_publisher_diagnostics(
    XgRenderMovieDiagnostics *out_diagnostics);

#ifdef __cplusplus
}
#endif

#endif
