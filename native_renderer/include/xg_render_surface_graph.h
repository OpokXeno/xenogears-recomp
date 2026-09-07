#ifndef XG_RENDER_SURFACE_GRAPH_H
#define XG_RENDER_SURFACE_GRAPH_H

#include "xg_render_resource_repository.h"
#include "xg_render_scene_snapshot.h"

#include <stddef.h>
#include <stdint.h>

#ifndef XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY
/* A 640-pixel depth-24 MDEC frame occupies 40 16-pixel strips. Keep both
 * display pages plus the assembled framebuffer and movie publication. */
#define XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY 128u
#endif

#ifndef XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY
#define XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY 64u
#endif

typedef enum XgRenderSurfaceFormat {
    XG_RENDER_SURFACE_VRAM16 = 0,
    XG_RENDER_SURFACE_RGBA8,
    XG_RENDER_SURFACE_DEPTH24,
} XgRenderSurfaceFormat;

typedef enum XgRenderSurfaceGraphResult {
    XG_RENDER_SURFACE_GRAPH_OK = 0,
    XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT,
    XG_RENDER_SURFACE_GRAPH_NOT_FOUND,
    XG_RENDER_SURFACE_GRAPH_STALE,
    XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED,
    XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED,
    XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT,
} XgRenderSurfaceGraphResult;

typedef enum XgRenderSurfaceGraphTransactionFault {
    XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_NONE = 0,
    XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_STALE,
    XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_CAPACITY,
    XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_RESOURCE,
} XgRenderSurfaceGraphTransactionFault;

typedef struct XgRenderSurfacePublicationDescription {
    XgRenderResourceIdentity identity;
    XgRenderResourceKind kind;
    XgRenderSurfaceFormat format;
    XgRenderResourceProvenance provenance;
    uint64_t owner_generation;
    uint32_t width;
    uint32_t height;
    XgRenderResourceDescriptor descriptor;
    const void *bytes;
    size_t byte_count;
} XgRenderSurfacePublicationDescription;

typedef struct XgRenderSurfacePublication {
    XgRenderResourceHandle handle;
    XgRenderResourceIdentity identity;
    XgRenderResourceKind kind;
    XgRenderResourceOwnerKind owner_kind;
    XgRenderSurfaceFormat format;
    XgRenderResourceProvenance provenance;
    uint64_t owner_generation;
    uint64_t content_digest;
    uint32_t width;
    uint32_t height;
    XgRenderResourceDescriptor descriptor;
    size_t byte_count;
} XgRenderSurfacePublication;

typedef struct XgRenderSurfaceAttachmentDescription {
    XgRenderResourceHandle handle;
    XgRenderResourceKind kind;
    XgRenderResourceOwnerKind owner_kind;
    XgRenderSurfaceFormat format;
    XgRenderResourceProvenance provenance;
    uint64_t owner_generation;
    uint64_t content_digest;
    uint32_t width;
    uint32_t height;
    size_t byte_count;
} XgRenderSurfaceAttachmentDescription;

typedef struct XgRenderSurfaceGraphSnapshot {
    uint32_t node_count;
    uint32_t edge_count;
    uint64_t publications;
    uint64_t restorations;
    uint64_t rejected_operations;
} XgRenderSurfaceGraphSnapshot;

typedef struct XgRenderSurfaceGraphCheckpointRestore
    XgRenderSurfaceGraphCheckpointRestore;
typedef struct XgRenderSurfaceGraphTransaction XgRenderSurfaceGraphTransaction;
typedef struct XgRenderSurfaceGraphPublicationRollback
    XgRenderSurfaceGraphPublicationRollback;

/* Guest-owner only, including reads and repository mutation across checkpoints.
 * Callers: auth_runtime scanout/source_boundary/checkpoint, runtime_composition
 * scene reset, and debug_server_poll on the guest thread. Workers use retained
 * SourceCommits, never this graph. */
void xg_render_surface_graph_reset(void);
XgRenderSurfaceGraphResult xg_render_surface_graph_publish(
    const XgRenderSurfacePublicationDescription *description,
    XgRenderSurfacePublication *out_publication);
XgRenderSurfaceGraphResult xg_render_surface_graph_publish_reversible(
    const XgRenderSurfacePublicationDescription *description,
    XgRenderSurfacePublication *out_publication,
    XgRenderSurfaceGraphPublicationRollback **out_rollback);
void xg_render_surface_graph_publication_accept(
    XgRenderSurfaceGraphPublicationRollback *rollback);
XgRenderSurfaceGraphResult xg_render_surface_graph_publication_rollback(
    XgRenderSurfaceGraphPublicationRollback *rollback);
XgRenderSurfaceGraphResult xg_render_surface_graph_attach_resource_with_edge(
    const XgRenderSurfaceAttachmentDescription *description,
    const XgSemanticSurfaceEdge *edge,
    XgRenderSurfacePublication *out_publication);
XgRenderSurfaceGraphResult xg_render_surface_graph_lookup(
    uint64_t resource_id, XgRenderSurfacePublication *out_publication);
XgRenderSurfaceGraphResult xg_render_surface_graph_copy_publications(
    XgRenderSurfacePublication *out_publications, size_t publication_capacity,
    size_t *out_publication_count);
XgRenderSurfaceGraphResult xg_render_surface_graph_append_edge(
    const XgSemanticSurfaceEdge *edge);
/* Builds a private future graph and stages the publication's repository
 * resource. Commit consumes the transaction and either publishes the complete
 * future graph or leaves the graph unchanged and cancels its staged import. */
XgRenderSurfaceGraphResult xg_render_surface_graph_transaction_begin(
    const XgRenderSurfacePublicationDescription *description,
    XgRenderSurfaceGraphTransaction **out_transaction,
    XgRenderSurfacePublication *out_publication);
XgRenderSurfaceGraphResult xg_render_surface_graph_transaction_append_edge(
    XgRenderSurfaceGraphTransaction *transaction,
    const XgSemanticSurfaceEdge *edge);
XgRenderSurfaceGraphResult
xg_render_surface_graph_transaction_attach_resource_with_edge(
    XgRenderSurfaceGraphTransaction *transaction,
    const XgRenderSurfaceAttachmentDescription *description,
    const XgSemanticSurfaceEdge *edge,
    XgRenderSurfacePublication *out_publication);
XgRenderSurfaceGraphResult xg_render_surface_graph_transaction_commit(
    XgRenderSurfaceGraphTransaction *transaction);
void xg_render_surface_graph_transaction_cancel(
    XgRenderSurfaceGraphTransaction *transaction);
/* One-shot deterministic commit fault used by synthetic transaction tests. */
void xg_render_surface_graph_transaction_fault_inject(
    XgRenderSurfaceGraphTransactionFault fault);
XgRenderSurfaceGraphResult xg_render_surface_graph_copy_edges(
    XgSemanticSurfaceEdge *out_edges, size_t edge_capacity,
    size_t *out_edge_count);
size_t xg_render_surface_graph_checkpoint_size(void);
XgRenderSurfaceGraphResult xg_render_surface_graph_checkpoint_write(
    void *out_checkpoint, size_t checkpoint_size);
XgRenderSurfaceGraphResult xg_render_surface_graph_checkpoint_validate(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t restored_owner_generation);
XgRenderSurfaceGraphResult xg_render_surface_graph_checkpoint_restore(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t restored_owner_generation);
XgRenderSurfaceGraphResult xg_render_surface_graph_checkpoint_prepare(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t restored_owner_generation,
    XgRenderSurfaceGraphCheckpointRestore **out_restore);
/* Reuses exact identity matches already staged by the same composite restore.
 * Shared resources remain owned by their original restore participant. */
XgRenderSurfaceGraphResult
xg_render_surface_graph_checkpoint_prepare_with_shared_resources(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t restored_owner_generation,
    const XgRenderResourceHandle *shared_resources,
    size_t shared_resource_count,
    XgRenderSurfaceGraphCheckpointRestore **out_restore);
void xg_render_surface_graph_checkpoint_commit(
    XgRenderSurfaceGraphCheckpointRestore *restore);
void xg_render_surface_graph_checkpoint_cancel(
    XgRenderSurfaceGraphCheckpointRestore *restore);
void xg_render_surface_graph_snapshot(XgRenderSurfaceGraphSnapshot *out_snapshot);

#endif
