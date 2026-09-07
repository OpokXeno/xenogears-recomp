#include "xg_render_surface_graph.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
    XG_RENDER_SURFACE_GRAPH_MAGIC = 0x47534758u,
    XG_RENDER_SURFACE_GRAPH_VERSION = 6u,
    XG_RENDER_SURFACE_GRAPH_HEADER_SIZE = 32u,
    XG_RENDER_SURFACE_GRAPH_NODE_SIZE =
        156u + XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE,
    XG_RENDER_SURFACE_GRAPH_EDGE_SIZE = 128u,
};

typedef struct XgRenderSurfaceNode {
    XgRenderSurfacePublication publication;
    bool occupied;
} XgRenderSurfaceNode;

typedef struct XgRenderSurfaceCheckpointNode {
    XgRenderResourceIdentity identity;
    uint64_t content_digest;
    uint64_t owner_generation;
    uint64_t byte_count;
    uint32_t width;
    uint32_t height;
    XgRenderSurfaceFormat format;
    XgRenderResourceKind kind;
    XgRenderResourceOwnerKind owner_kind;
    XgRenderResourceState state;
    XgRenderResourceDescriptor descriptor;
    XgRenderResourceProvenance provenance;
    XgRenderResourceProvenance restored_provenance;
    XgRenderResourceCapabilityCheckpoint authority;
    const uint8_t *bytes;
} XgRenderSurfaceCheckpointNode;

typedef struct XgRenderSurfaceCheckpointEdge {
    uint64_t source_id;
    uint64_t target_id;
    XgSemanticSurfaceEdgeKind kind;
    XgSemanticOrderKey order;
    XgSemanticSurfaceRect source;
    XgSemanticSurfaceRect destination;
    XgSemanticSurfaceSampleState sample;
    uint32_t effect_phase;
    uint32_t effect_phase_count;
} XgRenderSurfaceCheckpointEdge;

struct XgRenderSurfaceGraphCheckpointRestore {
    XgRenderSurfaceNode nodes[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY];
    bool resource_owned[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    uint32_t node_count;
    uint32_t edge_count;
};

struct XgRenderSurfaceGraphTransaction {
    XgRenderSurfaceNode nodes[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY];
    XgRenderSurfaceGraphSnapshot snapshot;
    XgRenderResourceHandle staged_publication;
    uint64_t base_revision;
    bool publication_staged;
};

struct XgRenderSurfaceGraphPublicationRollback {
    XgRenderSurfaceNode nodes[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY];
    XgRenderSurfaceGraphSnapshot snapshot;
    XgRenderResourceHandle previous;
    XgRenderResourceHandle publication;
    uint64_t committed_revision;
    bool previous_retained;
    bool publication_owned;
};

static XgRenderSurfaceNode g_nodes[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
static XgSemanticSurfaceEdge g_edges[XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY];
static XgRenderSurfaceGraphSnapshot g_snapshot;
static uint64_t g_revision;
static XgRenderSurfaceGraphTransactionFault g_transaction_fault;

static void graph_revision_advance(void) {
    g_revision++;
    if (g_revision == 0u) g_revision = 1u;
}

static bool valid_published_kind(XgRenderResourceKind kind) {
    return kind == XG_RENDER_RESOURCE_GENERATED_SURFACE ||
        kind == XG_RENDER_RESOURCE_FRAMEBUFFER;
}

static bool valid_graph_kind(XgRenderResourceKind kind) {
    return valid_published_kind(kind) ||
        kind == XG_RENDER_RESOURCE_MOVIE_FRAME;
}

static bool valid_owner(XgRenderResourceKind kind,
                        XgRenderResourceOwnerKind owner_kind) {
    return owner_kind == (kind == XG_RENDER_RESOURCE_MOVIE_FRAME
        ? XG_RENDER_RESOURCE_OWNER_SOURCE
        : XG_RENDER_RESOURCE_OWNER_SCENE);
}

static bool valid_provenance(
        const XgRenderResourceProvenance *provenance,
        XgRenderResourceOwnerKind owner_kind,
        uint64_t owner_generation) {
    if (provenance == NULL) return false;
    if (provenance->kind == XG_RENDER_RESOURCE_PROVENANCE_NONE)
        return provenance->synthetic && provenance->receipt == 0u &&
            provenance->capability == 0u;
    return !provenance->synthetic && provenance->receipt != 0u &&
        provenance->capability != 0u &&
        (provenance->kind == XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT ||
         provenance->kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE) &&
        xg_render_resource_capability_validate(
            provenance, owner_kind, owner_generation, NULL) ==
                XG_RENDER_RESOURCE_CAPABILITY_OK;
}

static bool same_provenance(const XgRenderResourceProvenance *left,
                            const XgRenderResourceProvenance *right) {
    return left->kind == right->kind && left->receipt == right->receipt &&
        left->capability == right->capability &&
        left->synthetic == right->synthetic;
}

static bool same_edge(const XgSemanticSurfaceEdge *left,
                      const XgSemanticSurfaceEdge *right) {
    return left->source_surface_id == right->source_surface_id &&
        left->source_generation == right->source_generation &&
        left->target_surface_id == right->target_surface_id &&
        left->target_generation == right->target_generation &&
        left->kind == right->kind &&
        left->order.pass_id == right->order.pass_id &&
        left->order.layer == right->order.layer &&
        left->order.authored_depth == right->order.authored_depth &&
        left->order.insertion_ordinal == right->order.insertion_ordinal &&
        left->order.split_ordinal == right->order.split_ordinal &&
        left->source.x == right->source.x &&
        left->source.y == right->source.y &&
        left->source.width == right->source.width &&
        left->source.height == right->source.height &&
        left->destination.x == right->destination.x &&
        left->destination.y == right->destination.y &&
        left->destination.width == right->destination.width &&
        left->destination.height == right->destination.height &&
        left->sample.sampler == right->sample.sampler &&
        left->sample.wrap_u == right->sample.wrap_u &&
        left->sample.wrap_v == right->sample.wrap_v &&
        left->sample.clut_resource_id == right->sample.clut_resource_id &&
        left->sample.clut_generation == right->sample.clut_generation &&
        left->sample.clut_x == right->sample.clut_x &&
        left->sample.clut_y == right->sample.clut_y &&
        left->sample.texture_window_mask_x ==
            right->sample.texture_window_mask_x &&
        left->sample.texture_window_mask_y ==
            right->sample.texture_window_mask_y &&
        left->sample.texture_window_offset_x ==
            right->sample.texture_window_offset_x &&
        left->sample.texture_window_offset_y ==
            right->sample.texture_window_offset_y &&
        left->sample.texture_depth == right->sample.texture_depth &&
        left->sample.blend_mode == right->sample.blend_mode &&
        left->sample.palette_enabled == right->sample.palette_enabled &&
        left->sample.semi_transparent == right->sample.semi_transparent &&
        left->sample.mask_set == right->sample.mask_set &&
        left->sample.mask_check == right->sample.mask_check &&
        left->effect_phase == right->effect_phase &&
        left->effect_phase_count == right->effect_phase_count;
}

static bool edge_contract_valid(const XgSemanticSurfaceEdge *edge) {
    return edge != NULL && edge->source_surface_id != 0u &&
        edge->source_generation != 0u && edge->target_surface_id != 0u &&
        edge->target_generation != 0u &&
        edge->kind <= XG_SEMANTIC_SURFACE_MOVIE && edge->source.x >= 0 &&
        edge->source.y >= 0 && edge->source.width != 0u &&
        edge->source.height != 0u && edge->destination.width != 0u &&
        edge->destination.height != 0u &&
        edge->sample.sampler >= XG_RENDER_RESOURCE_SAMPLER_NEAREST &&
        edge->sample.sampler <= XG_RENDER_RESOURCE_SAMPLER_LINEAR &&
        edge->sample.wrap_u >= XG_RENDER_RESOURCE_WRAP_CLAMP &&
        edge->sample.wrap_u <= XG_RENDER_RESOURCE_WRAP_MIRROR &&
        edge->sample.wrap_v >= XG_RENDER_RESOURCE_WRAP_CLAMP &&
        edge->sample.wrap_v <= XG_RENDER_RESOURCE_WRAP_MIRROR &&
        edge->sample.texture_depth <= XG_RENDER_IR_TEXTURE_15_BIT &&
        edge->sample.blend_mode <= XG_RENDER_IR_BLEND_ADD_QUARTER &&
        (edge->effect_phase_count != 0u
             ? edge->effect_phase < edge->effect_phase_count
             : edge->effect_phase == 0u) &&
        (edge->sample.clut_resource_id != 0u
             ? edge->sample.clut_generation != 0u
             : edge->sample.clut_generation == 0u);
}

static bool edge_rects_valid(const XgSemanticSurfaceEdge *edge,
                             uint32_t source_width, uint32_t source_height,
                             uint32_t target_width, uint32_t target_height) {
    const int64_t destination_right =
        (int64_t)edge->destination.x + edge->destination.width;
    const int64_t destination_bottom =
        (int64_t)edge->destination.y + edge->destination.height;

    return (uint64_t)(uint32_t)edge->source.x + edge->source.width <=
            source_width &&
        (uint64_t)(uint32_t)edge->source.y + edge->source.height <=
            source_height &&
        edge->destination.x < (int64_t)target_width &&
        edge->destination.y < (int64_t)target_height &&
        destination_right > 0 && destination_bottom > 0;
}

static bool checkpoint_edge_contract_valid(
        const XgRenderSurfaceCheckpointEdge *edge) {
    const XgSemanticSurfaceEdge semantic = {
        .source_surface_id = edge->source_id,
        .source_generation = 1u,
        .target_surface_id = edge->target_id,
        .target_generation = 1u,
        .kind = edge->kind,
        .order = edge->order,
        .source = edge->source,
        .destination = edge->destination,
        .sample = edge->sample,
        .effect_phase = edge->effect_phase,
        .effect_phase_count = edge->effect_phase_count,
    };
    return edge_contract_valid(&semantic) && edge->source_id != edge->target_id;
}

static size_t bytes_per_pixel(XgRenderSurfaceFormat format) {
    switch (format) {
        case XG_RENDER_SURFACE_VRAM16: return 2u;
        case XG_RENDER_SURFACE_RGBA8: return 4u;
        case XG_RENDER_SURFACE_DEPTH24: return 3u;
    }
    return 0u;
}

static bool same_descriptor(const XgRenderResourceDescriptor *left,
                            const XgRenderResourceDescriptor *right) {
    return left->version == right->version &&
        left->pixel_format == right->pixel_format &&
        left->width == right->width && left->height == right->height &&
        left->row_pitch == right->row_pitch &&
        left->vram_x == right->vram_x && left->vram_y == right->vram_y &&
        left->vram_width == right->vram_width &&
        left->vram_height == right->vram_height &&
        left->sampler == right->sampler && left->wrap_u == right->wrap_u &&
        left->wrap_v == right->wrap_v && left->flags == right->flags;
}

static bool valid_descriptor(XgRenderSurfaceFormat format,
                             uint32_t width, uint32_t height,
                             size_t byte_count,
                             const XgRenderResourceDescriptor *descriptor) {
    const size_t pixel_size = bytes_per_pixel(format);
    XgRenderResourcePixelFormat pixel_format;

    switch (format) {
        case XG_RENDER_SURFACE_VRAM16:
            pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555;
            break;
        case XG_RENDER_SURFACE_RGBA8:
            pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8;
            break;
        case XG_RENDER_SURFACE_DEPTH24:
            pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24;
            break;
        default:
            return false;
    }
    return descriptor != NULL && pixel_size != 0u &&
        xg_render_resource_descriptor_validate(descriptor) &&
        descriptor->version == XG_RENDER_RESOURCE_DESCRIPTOR_VERSION &&
        descriptor->pixel_format == pixel_format &&
        descriptor->width == width && descriptor->height == height &&
        descriptor->row_pitch == (size_t)width * pixel_size &&
        (uint64_t)descriptor->row_pitch * height == byte_count &&
        ((descriptor->flags &
          XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) == 0u ||
         (descriptor->vram_width == width &&
          descriptor->vram_height == height));
}

static bool valid_payload(XgRenderSurfaceFormat format,
                          uint32_t width, uint32_t height,
                          const XgRenderResourceDescriptor *descriptor,
                          const void *bytes, size_t byte_count) {
    const size_t pixel_size = bytes_per_pixel(format);
    const uint64_t required = (uint64_t)width * height * pixel_size;

    return pixel_size != 0u && width != 0u && height != 0u && bytes != NULL &&
        required != 0u && required <= SIZE_MAX &&
        byte_count == (size_t)required &&
        valid_descriptor(format, width, height, byte_count, descriptor);
}

static XgRenderSurfaceNode *node_by_id(uint64_t resource_id) {
    for (uint32_t index = 0u; index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY;
         ++index)
        if (g_nodes[index].occupied &&
            g_nodes[index].publication.handle.resource_id == resource_id)
            return &g_nodes[index];
    return NULL;
}

static XgRenderSurfaceNode *node_by_handle(XgRenderResourceHandle handle) {
    XgRenderSurfaceNode *node = node_by_id(handle.resource_id);
    return node != NULL && node->publication.handle.generation == handle.generation
        ? node : NULL;
}

static XgRenderSurfaceNode *transaction_node_by_id(
        XgRenderSurfaceGraphTransaction *transaction, uint64_t resource_id) {
    for (uint32_t index = 0u; index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY;
         ++index)
        if (transaction->nodes[index].occupied &&
            transaction->nodes[index].publication.handle.resource_id ==
                resource_id)
            return &transaction->nodes[index];
    return NULL;
}

static XgRenderSurfaceNode *transaction_node_by_handle(
        XgRenderSurfaceGraphTransaction *transaction,
        XgRenderResourceHandle handle) {
    XgRenderSurfaceNode *node =
        transaction_node_by_id(transaction, handle.resource_id);
    return node != NULL &&
            node->publication.handle.generation == handle.generation
        ? node : NULL;
}

static void transaction_remove_edges_for_resource(
        XgRenderSurfaceGraphTransaction *transaction, uint64_t resource_id) {
    uint32_t target = 0u;

    for (uint32_t index = 0u; index < transaction->snapshot.edge_count;
         ++index) {
        const XgSemanticSurfaceEdge *edge = &transaction->edges[index];
        if (edge->source_surface_id == resource_id ||
            edge->target_surface_id == resource_id)
            continue;
        if (target != index) transaction->edges[target] = *edge;
        target++;
    }
    memset(transaction->edges + target, 0,
           (transaction->snapshot.edge_count - target) *
               sizeof(*transaction->edges));
    transaction->snapshot.edge_count = target;
}

static void remove_edges_for_resource(uint64_t resource_id) {
    uint32_t target = 0u;
    for (uint32_t index = 0u; index < g_snapshot.edge_count; ++index) {
        const XgSemanticSurfaceEdge *edge = &g_edges[index];
        if (edge->source_surface_id == resource_id ||
            edge->target_surface_id == resource_id)
            continue;
        if (target != index) g_edges[target] = *edge;
        target++;
    }
    memset(g_edges + target, 0,
           (g_snapshot.edge_count - target) * sizeof(*g_edges));
    g_snapshot.edge_count = target;
}

void xg_render_surface_graph_reset(void) {
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_edges, 0, sizeof(g_edges));
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_transaction_fault = XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_NONE;
    graph_revision_advance();
}

XgRenderSurfaceGraphResult xg_render_surface_graph_publish(
        const XgRenderSurfacePublicationDescription *description,
        XgRenderSurfacePublication *out_publication) {
    XgRenderSurfaceNode *node;
    XgRenderResourceImport import;
    XgRenderResourceHandle handle;
    const uint64_t resource_id = description != NULL
        ? xg_render_resource_identity_id(&description->identity) : 0u;

    if (description == NULL || out_publication == NULL || resource_id == 0u ||
        !valid_published_kind(description->kind) ||
        description->owner_generation == 0u ||
        !valid_provenance(&description->provenance,
                          XG_RENDER_RESOURCE_OWNER_SCENE,
                          description->owner_generation) ||
        !valid_payload(description->format, description->width,
                       description->height, &description->descriptor,
                       description->bytes,
                       description->byte_count)) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    }
    node = node_by_id(resource_id);
    if (node == NULL) {
        for (uint32_t index = 0u;
             index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY; ++index) {
            if (!g_nodes[index].occupied) {
                node = &g_nodes[index];
                break;
            }
        }
    }
    if (node == NULL) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    }
    import = (XgRenderResourceImport){
        .resource_id = resource_id,
        .kind = description->kind,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = description->owner_generation,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = description->provenance,
        .content_digest = xg_render_resource_digest(
            description->bytes, description->byte_count),
        .bytes = description->bytes,
        .byte_count = description->byte_count,
        .identity = description->identity,
        .descriptor = description->descriptor,
    };
    if (xg_render_resource_import_native(&import, &handle) !=
        XG_RENDER_RESOURCE_OK) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
    }
    if (node->occupied && node->publication.handle.generation != handle.generation)
        remove_edges_for_resource(resource_id);
    if (!node->occupied) g_snapshot.node_count++;
    node->publication = (XgRenderSurfacePublication){
        .handle = handle,
        .identity = description->identity,
        .kind = description->kind,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .format = description->format,
        .provenance = description->provenance,
        .owner_generation = description->owner_generation,
        .content_digest = import.content_digest,
        .width = description->width,
        .height = description->height,
        .descriptor = description->descriptor,
        .byte_count = description->byte_count,
    };
    node->occupied = true;
    g_snapshot.publications++;
    graph_revision_advance();
    *out_publication = node->publication;
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_publish_reversible(
        const XgRenderSurfacePublicationDescription *description,
        XgRenderSurfacePublication *out_publication,
        XgRenderSurfaceGraphPublicationRollback **out_rollback) {
    XgRenderSurfaceGraphPublicationRollback *rollback;
    XgRenderSurfaceGraphTransaction *transaction = NULL;
    XgRenderSurfaceNode *previous_node;
    XgRenderResourceView publication_view;
    XgRenderSurfaceGraphResult result;
    const uint64_t resource_id = description != NULL
        ? xg_render_resource_identity_id(&description->identity) : 0u;

    if (out_rollback == NULL)
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    *out_rollback = NULL;
    rollback = (XgRenderSurfaceGraphPublicationRollback *)calloc(
        1u, sizeof(*rollback));
    if (rollback == NULL) return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
    memcpy(rollback->nodes, g_nodes, sizeof(g_nodes));
    memcpy(rollback->edges, g_edges, sizeof(g_edges));
    rollback->snapshot = g_snapshot;
    previous_node = resource_id != 0u ? node_by_id(resource_id) : NULL;
    if (previous_node != NULL)
        rollback->previous = previous_node->publication.handle;
    result = xg_render_surface_graph_transaction_begin(
        description, &transaction, out_publication);
    if (result != XG_RENDER_SURFACE_GRAPH_OK) {
        free(rollback);
        return result;
    }
    if (xg_render_resource_view(
            out_publication->handle, &publication_view) !=
            XG_RENDER_RESOURCE_OK) {
        xg_render_surface_graph_transaction_cancel(transaction);
        free(rollback);
        return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
    }
    rollback->publication = out_publication->handle;
    rollback->publication_owned = !publication_view.current &&
        publication_view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE;
    if (rollback->previous.resource_id != 0u &&
        (rollback->previous.resource_id != rollback->publication.resource_id ||
         rollback->previous.generation != rollback->publication.generation)) {
        XgRenderResourceView previous_view;

        if (xg_render_resource_view(rollback->previous, &previous_view) !=
                XG_RENDER_RESOURCE_OK || !previous_view.current ||
            xg_render_resource_acquire_current(
                rollback->previous, previous_view.content_digest) !=
                    XG_RENDER_RESOURCE_OK) {
            xg_render_surface_graph_transaction_cancel(transaction);
            free(rollback);
            return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
        }
        rollback->previous_retained = true;
    }
    result = xg_render_surface_graph_transaction_commit(transaction);
    if (result != XG_RENDER_SURFACE_GRAPH_OK) {
        if (rollback->previous_retained)
            (void)xg_render_resource_release(rollback->previous);
        free(rollback);
        return result;
    }
    rollback->committed_revision = g_revision;
    *out_rollback = rollback;
    return XG_RENDER_SURFACE_GRAPH_OK;
}

void xg_render_surface_graph_publication_accept(
        XgRenderSurfaceGraphPublicationRollback *rollback) {
    if (rollback == NULL) return;
    if (rollback->previous_retained)
        (void)xg_render_resource_release(rollback->previous);
    free(rollback);
}

XgRenderSurfaceGraphResult xg_render_surface_graph_publication_rollback(
        XgRenderSurfaceGraphPublicationRollback *rollback) {
    XgRenderResourceResult resource_result = XG_RENDER_RESOURCE_OK;

    if (rollback == NULL) return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    if (g_revision != rollback->committed_revision) {
        xg_render_surface_graph_publication_accept(rollback);
        return XG_RENDER_SURFACE_GRAPH_STALE;
    }
    if (rollback->publication_owned) {
        resource_result = rollback->previous.resource_id != 0u
            ? xg_render_resource_rollback_current(
                  rollback->publication, rollback->previous)
            : xg_render_resource_retire_current(rollback->publication);
    }
    if (resource_result != XG_RENDER_RESOURCE_OK) {
        xg_render_surface_graph_publication_accept(rollback);
        return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
    }
    memcpy(g_nodes, rollback->nodes, sizeof(g_nodes));
    memcpy(g_edges, rollback->edges, sizeof(g_edges));
    g_snapshot = rollback->snapshot;
    graph_revision_advance();
    xg_render_surface_graph_publication_accept(rollback);
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_attach_resource_with_edge(
        const XgRenderSurfaceAttachmentDescription *description,
        const XgSemanticSurfaceEdge *edge,
        XgRenderSurfacePublication *out_publication) {
    XgRenderSurfaceNode *node;
    XgRenderSurfaceNode *source;
    XgRenderSurfacePublication current_source;
    XgRenderResourceView view;
    uint32_t retained_edge_count;

    if (description == NULL || edge == NULL || out_publication == NULL ||
        description->handle.resource_id == 0u ||
        description->handle.generation == 0u ||
        description->kind != XG_RENDER_RESOURCE_MOVIE_FRAME ||
        description->owner_kind != XG_RENDER_RESOURCE_OWNER_SOURCE ||
        description->owner_generation == 0u ||
        description->content_digest == 0u ||
        !valid_provenance(&description->provenance,
                          description->owner_kind,
                          description->owner_generation) ||
        !edge_contract_valid(edge) ||
        edge->kind != XG_SEMANTIC_SURFACE_MOVIE ||
        edge->target_surface_id != description->handle.resource_id ||
        edge->target_generation != description->handle.generation ||
        edge->source_surface_id == edge->target_surface_id) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    }
    source = node_by_handle((XgRenderResourceHandle){
        edge->source_surface_id, edge->source_generation});
    if (source == NULL || source->publication.kind !=
            XG_RENDER_RESOURCE_FRAMEBUFFER ||
        xg_render_surface_graph_lookup(edge->source_surface_id,
            &current_source) != XG_RENDER_SURFACE_GRAPH_OK ||
        current_source.handle.generation != edge->source_generation ||
        xg_render_resource_view(description->handle, &view) !=
            XG_RENDER_RESOURCE_OK || !view.current || !view.has_identity ||
        xg_render_resource_identity_id(&view.identity) !=
            description->handle.resource_id) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_STALE;
    }
    if (view.kind != description->kind ||
        view.owner_kind != description->owner_kind ||
        view.owner_generation != description->owner_generation ||
        view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        !same_provenance(&view.provenance, &description->provenance) ||
        view.content_digest != description->content_digest ||
        view.byte_count != description->byte_count ||
        !valid_payload(description->format, description->width,
                       description->height, &view.descriptor,
                       view.bytes, view.byte_count)) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_STALE;
    }
    if (!edge_rects_valid(edge, source->publication.width,
                          source->publication.height, description->width,
                          description->height)) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    }
    node = node_by_id(description->handle.resource_id);
    if (node == NULL) {
        for (uint32_t index = 0u;
             index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY; ++index) {
            if (!g_nodes[index].occupied) {
                node = &g_nodes[index];
                break;
            }
        }
        if (node == NULL) {
            g_snapshot.rejected_operations++;
            return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
        }
    }
    retained_edge_count = g_snapshot.edge_count;
    if (node->occupied &&
        node->publication.handle.generation != description->handle.generation) {
        retained_edge_count = 0u;
        for (uint32_t index = 0u; index < g_snapshot.edge_count; ++index)
            if (g_edges[index].source_surface_id !=
                    description->handle.resource_id &&
                g_edges[index].target_surface_id !=
                    description->handle.resource_id)
                retained_edge_count++;
    } else if (node->occupied) {
        for (uint32_t index = 0u; index < g_snapshot.edge_count; ++index)
            if (same_edge(&g_edges[index], edge)) {
                g_snapshot.rejected_operations++;
                return XG_RENDER_SURFACE_GRAPH_STALE;
            }
    }
    if (retained_edge_count >= XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    }

    if (node->occupied &&
        node->publication.handle.generation != description->handle.generation)
        remove_edges_for_resource(description->handle.resource_id);
    if (!node->occupied) g_snapshot.node_count++;
    node->publication = (XgRenderSurfacePublication){
        .handle = description->handle,
        .identity = view.identity,
        .kind = description->kind,
        .owner_kind = description->owner_kind,
        .format = description->format,
        .provenance = description->provenance,
        .owner_generation = description->owner_generation,
        .content_digest = description->content_digest,
        .width = description->width,
        .height = description->height,
        .descriptor = view.descriptor,
        .byte_count = description->byte_count,
    };
    node->occupied = true;
    g_edges[g_snapshot.edge_count++] = *edge;
    g_snapshot.publications++;
    graph_revision_advance();
    *out_publication = node->publication;
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_lookup(
        uint64_t resource_id, XgRenderSurfacePublication *out_publication) {
    XgRenderSurfaceNode *node = node_by_id(resource_id);
    XgRenderResourceView view;

    if (resource_id == 0u || out_publication == NULL)
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    if (node == NULL) return XG_RENDER_SURFACE_GRAPH_NOT_FOUND;
    if (xg_render_resource_view(node->publication.handle, &view) !=
            XG_RENDER_RESOURCE_OK || !view.current ||
        !view.has_identity || view.kind != node->publication.kind ||
        view.owner_kind != node->publication.owner_kind ||
        view.owner_generation != node->publication.owner_generation ||
        view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        !same_provenance(&view.provenance, &node->publication.provenance) ||
        view.content_digest != node->publication.content_digest ||
        view.byte_count != node->publication.byte_count ||
        memcmp(view.identity.bytes, node->publication.identity.bytes,
               sizeof(view.identity.bytes)) != 0)
        return XG_RENDER_SURFACE_GRAPH_STALE;
    *out_publication = node->publication;
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_copy_publications(
        XgRenderSurfacePublication *out_publications,
        size_t publication_capacity, size_t *out_publication_count) {
    uint32_t copied = 0u;

    if (out_publication_count == NULL ||
        (g_snapshot.node_count != 0u && out_publications == NULL))
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    *out_publication_count = g_snapshot.node_count;
    if (publication_capacity < g_snapshot.node_count)
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    for (uint32_t index = 0u;
         index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY; ++index) {
        XgRenderSurfacePublication publication;
        if (!g_nodes[index].occupied) continue;
        if (xg_render_surface_graph_lookup(
                g_nodes[index].publication.handle.resource_id,
                &publication) != XG_RENDER_SURFACE_GRAPH_OK)
            return XG_RENDER_SURFACE_GRAPH_STALE;
        out_publications[copied++] = publication;
    }
    return copied == g_snapshot.node_count
        ? XG_RENDER_SURFACE_GRAPH_OK : XG_RENDER_SURFACE_GRAPH_STALE;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_append_edge(
        const XgSemanticSurfaceEdge *edge) {
    XgRenderSurfaceNode *source;
    XgRenderSurfaceNode *target;

    if (!edge_contract_valid(edge)) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    }
    source = node_by_handle((XgRenderResourceHandle){
        edge->source_surface_id, edge->source_generation});
    target = node_by_handle((XgRenderResourceHandle){
        edge->target_surface_id, edge->target_generation});
    if (source == NULL || target == NULL) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_STALE;
    }
    if (!edge_rects_valid(edge, source->publication.width,
                          source->publication.height,
                          target->publication.width,
                          target->publication.height) ||
        edge->source_surface_id == edge->target_surface_id) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    }
    if (g_snapshot.edge_count >= XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    }
    g_edges[g_snapshot.edge_count++] = *edge;
    graph_revision_advance();
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_transaction_begin(
        const XgRenderSurfacePublicationDescription *description,
        XgRenderSurfaceGraphTransaction **out_transaction,
        XgRenderSurfacePublication *out_publication) {
    XgRenderSurfaceGraphTransaction *transaction;
    XgRenderSurfaceNode *node;
    XgRenderResourceImport import;
    XgRenderResourceView view;
    XgRenderResourceHandle handle;
    const uint64_t resource_id = description != NULL
        ? xg_render_resource_identity_id(&description->identity) : 0u;

    if (out_transaction == NULL) return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    *out_transaction = NULL;
    if (description == NULL || out_publication == NULL || resource_id == 0u ||
        !valid_published_kind(description->kind) ||
        description->owner_generation == 0u ||
        !valid_provenance(&description->provenance,
                          XG_RENDER_RESOURCE_OWNER_SCENE,
                          description->owner_generation) ||
        !valid_payload(description->format, description->width,
                       description->height, &description->descriptor,
                       description->bytes,
                       description->byte_count))
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;

    transaction = (XgRenderSurfaceGraphTransaction *)calloc(
        1u, sizeof(*transaction));
    if (transaction == NULL) return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
    memcpy(transaction->nodes, g_nodes, sizeof(g_nodes));
    memcpy(transaction->edges, g_edges, sizeof(g_edges));
    transaction->snapshot = g_snapshot;
    transaction->base_revision = g_revision;
    node = transaction_node_by_id(transaction, resource_id);
    if (node == NULL) {
        for (uint32_t index = 0u;
             index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY; ++index)
            if (!transaction->nodes[index].occupied) {
                node = &transaction->nodes[index];
                break;
            }
    }
    if (node == NULL) {
        free(transaction);
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    }
    import = (XgRenderResourceImport){
        .resource_id = resource_id,
        .kind = description->kind,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = description->owner_generation,
        .state = XG_RENDER_RESOURCE_IMPORTING_NATIVE,
        .provenance = description->provenance,
        .content_digest = xg_render_resource_digest(
            description->bytes, description->byte_count),
        .bytes = description->bytes,
        .byte_count = description->byte_count,
        .identity = description->identity,
        .descriptor = description->descriptor,
    };
    if (xg_render_resource_import_begin(&import, &handle) !=
            XG_RENDER_RESOURCE_OK) {
        free(transaction);
        return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
    }
    if (xg_render_resource_view(handle, &view) != XG_RENDER_RESOURCE_OK) {
        (void)xg_render_resource_import_cancel(handle);
        free(transaction);
        return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
    }
    transaction->staged_publication = handle;
    transaction->publication_staged = !view.current &&
        view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE;
    if (node->occupied && node->publication.handle.generation !=
            handle.generation)
        transaction_remove_edges_for_resource(transaction, resource_id);
    if (!node->occupied) transaction->snapshot.node_count++;
    node->publication = (XgRenderSurfacePublication){
        .handle = handle,
        .identity = description->identity,
        .kind = description->kind,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .format = description->format,
        .provenance = description->provenance,
        .owner_generation = description->owner_generation,
        .content_digest = import.content_digest,
        .width = description->width,
        .height = description->height,
        .descriptor = description->descriptor,
        .byte_count = description->byte_count,
    };
    node->occupied = true;
    transaction->snapshot.publications++;
    *out_publication = node->publication;
    *out_transaction = transaction;
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_transaction_append_edge(
        XgRenderSurfaceGraphTransaction *transaction,
        const XgSemanticSurfaceEdge *edge) {
    XgRenderSurfaceNode *source;
    XgRenderSurfaceNode *target;

    if (transaction == NULL || !edge_contract_valid(edge))
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    source = transaction_node_by_handle(transaction, (XgRenderResourceHandle){
        edge->source_surface_id, edge->source_generation});
    target = transaction_node_by_handle(transaction, (XgRenderResourceHandle){
        edge->target_surface_id, edge->target_generation});
    if (source == NULL || target == NULL)
        return XG_RENDER_SURFACE_GRAPH_STALE;
    if (!edge_rects_valid(edge, source->publication.width,
                          source->publication.height,
                          target->publication.width,
                          target->publication.height) ||
        edge->source_surface_id == edge->target_surface_id)
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    if (transaction->snapshot.edge_count >=
            XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY)
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    transaction->edges[transaction->snapshot.edge_count++] = *edge;
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult
xg_render_surface_graph_transaction_attach_resource_with_edge(
        XgRenderSurfaceGraphTransaction *transaction,
        const XgRenderSurfaceAttachmentDescription *description,
        const XgSemanticSurfaceEdge *edge,
        XgRenderSurfacePublication *out_publication) {
    XgRenderSurfaceNode *node;
    XgRenderSurfaceNode *source;
    XgRenderResourceView view;

    if (transaction == NULL || description == NULL || edge == NULL ||
        out_publication == NULL || description->handle.resource_id == 0u ||
        description->handle.generation == 0u ||
        description->kind != XG_RENDER_RESOURCE_MOVIE_FRAME ||
        description->owner_kind != XG_RENDER_RESOURCE_OWNER_SOURCE ||
        description->owner_generation == 0u ||
        description->content_digest == 0u ||
        !valid_provenance(&description->provenance,
                          description->owner_kind,
                          description->owner_generation) ||
        !edge_contract_valid(edge) ||
        edge->kind != XG_SEMANTIC_SURFACE_MOVIE ||
        edge->target_surface_id != description->handle.resource_id ||
        edge->target_generation != description->handle.generation ||
        edge->source_surface_id == edge->target_surface_id)
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    source = transaction_node_by_handle(transaction, (XgRenderResourceHandle){
        edge->source_surface_id, edge->source_generation});
    if (source == NULL ||
        source->publication.kind != XG_RENDER_RESOURCE_FRAMEBUFFER ||
        xg_render_resource_view(description->handle, &view) !=
            XG_RENDER_RESOURCE_OK ||
        (!view.current && view.state != XG_RENDER_RESOURCE_IMPORTING_NATIVE) ||
        !view.has_identity ||
        xg_render_resource_identity_id(&view.identity) !=
            description->handle.resource_id ||
        view.kind != description->kind ||
        view.owner_kind != description->owner_kind ||
        view.owner_generation != description->owner_generation ||
        !same_provenance(&view.provenance, &description->provenance) ||
        view.content_digest != description->content_digest ||
        view.byte_count != description->byte_count ||
        !valid_payload(description->format, description->width,
                       description->height, &view.descriptor,
                       view.bytes, view.byte_count))
        return XG_RENDER_SURFACE_GRAPH_STALE;
    if (!edge_rects_valid(edge, source->publication.width,
                          source->publication.height, description->width,
                          description->height))
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;

    node = transaction_node_by_id(transaction,
                                  description->handle.resource_id);
    if (node == NULL) {
        for (uint32_t index = 0u;
             index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY; ++index)
            if (!transaction->nodes[index].occupied) {
                node = &transaction->nodes[index];
                break;
            }
        if (node == NULL) return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    } else if (node->publication.handle.generation ==
                   description->handle.generation) {
        for (uint32_t index = 0u;
             index < transaction->snapshot.edge_count; ++index)
            if (same_edge(&transaction->edges[index], edge))
                return XG_RENDER_SURFACE_GRAPH_STALE;
    } else {
        transaction_remove_edges_for_resource(
            transaction, description->handle.resource_id);
    }
    if (transaction->snapshot.edge_count >=
            XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY)
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    if (!node->occupied) transaction->snapshot.node_count++;
    node->publication = (XgRenderSurfacePublication){
        .handle = description->handle,
        .identity = view.identity,
        .kind = description->kind,
        .owner_kind = description->owner_kind,
        .format = description->format,
        .provenance = description->provenance,
        .owner_generation = description->owner_generation,
        .content_digest = description->content_digest,
        .width = description->width,
        .height = description->height,
        .descriptor = view.descriptor,
        .byte_count = description->byte_count,
    };
    node->occupied = true;
    transaction->edges[transaction->snapshot.edge_count++] = *edge;
    transaction->snapshot.publications++;
    *out_publication = node->publication;
    return XG_RENDER_SURFACE_GRAPH_OK;
}

static void transaction_cancel_staged_import(
        XgRenderSurfaceGraphTransaction *transaction) {
    if (transaction->publication_staged)
        (void)xg_render_resource_import_cancel(
            transaction->staged_publication);
}

static XgRenderSurfaceGraphResult transaction_validate_future(
        const XgRenderSurfaceGraphTransaction *transaction) {
    uint32_t node_count = 0u;

    if (transaction->snapshot.node_count >
            XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY ||
        transaction->snapshot.edge_count >
            XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY)
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    for (uint32_t index = 0u;
         index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY; ++index) {
        const XgRenderSurfaceNode *node = &transaction->nodes[index];
        XgRenderResourceView view;
        bool staged;

        if (!node->occupied) continue;
        node_count++;
        if (xg_render_resource_view(node->publication.handle, &view) !=
                XG_RENDER_RESOURCE_OK || !view.has_identity ||
            view.kind != node->publication.kind ||
            view.owner_kind != node->publication.owner_kind ||
            view.owner_generation != node->publication.owner_generation ||
            !same_provenance(&view.provenance,
                             &node->publication.provenance) ||
            view.content_digest != node->publication.content_digest ||
            view.byte_count != node->publication.byte_count ||
            !same_descriptor(&view.descriptor,
                             &node->publication.descriptor) ||
            memcmp(view.identity.bytes, node->publication.identity.bytes,
                   sizeof(view.identity.bytes)) != 0)
            return XG_RENDER_SURFACE_GRAPH_STALE;
        staged = transaction->publication_staged &&
            node->publication.handle.resource_id ==
                transaction->staged_publication.resource_id &&
            node->publication.handle.generation ==
                transaction->staged_publication.generation;
        if ((!view.current || view.state != XG_RENDER_RESOURCE_NATIVE_OWNED) &&
            !(staged && !view.current &&
              view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE))
            return XG_RENDER_SURFACE_GRAPH_STALE;
    }
    if (node_count != transaction->snapshot.node_count)
        return XG_RENDER_SURFACE_GRAPH_STALE;
    for (uint32_t index = 0u; index < transaction->snapshot.edge_count;
         ++index) {
        const XgSemanticSurfaceEdge *edge = &transaction->edges[index];
        const XgRenderSurfaceNode *source = NULL;
        const XgRenderSurfaceNode *target = NULL;

        for (uint32_t node_index = 0u;
             node_index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY; ++node_index) {
            const XgRenderSurfaceNode *candidate =
                &transaction->nodes[node_index];
            if (!candidate->occupied) continue;
            if (candidate->publication.handle.resource_id ==
                    edge->source_surface_id &&
                candidate->publication.handle.generation ==
                    edge->source_generation)
                source = candidate;
            if (candidate->publication.handle.resource_id ==
                    edge->target_surface_id &&
                candidate->publication.handle.generation ==
                    edge->target_generation)
                target = candidate;
        }
        if (source == NULL || target == NULL || !edge_contract_valid(edge) ||
            edge->source_surface_id == edge->target_surface_id ||
            !edge_rects_valid(edge, source->publication.width,
                              source->publication.height,
                              target->publication.width,
                              target->publication.height))
            return XG_RENDER_SURFACE_GRAPH_STALE;
    }
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_transaction_commit(
        XgRenderSurfaceGraphTransaction *transaction) {
    XgRenderSurfaceGraphResult result;
    XgRenderResourceResult resource_result;

    if (transaction == NULL)
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    switch (g_transaction_fault) {
    case XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_STALE:
        result = XG_RENDER_SURFACE_GRAPH_STALE;
        break;
    case XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_CAPACITY:
        result = XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
        break;
    case XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_RESOURCE:
        result = XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
        break;
    default:
        result = XG_RENDER_SURFACE_GRAPH_OK;
        break;
    }
    g_transaction_fault = XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_NONE;
    if (result == XG_RENDER_SURFACE_GRAPH_OK &&
        transaction->base_revision != g_revision)
        result = XG_RENDER_SURFACE_GRAPH_STALE;
    if (result == XG_RENDER_SURFACE_GRAPH_OK)
        result = transaction_validate_future(transaction);
    if (result != XG_RENDER_SURFACE_GRAPH_OK) {
        transaction_cancel_staged_import(transaction);
        free(transaction);
        return result;
    }
    resource_result = xg_render_resource_import_commit_many(
        &transaction->staged_publication, 1u);
    if (resource_result != XG_RENDER_RESOURCE_OK) {
        transaction_cancel_staged_import(transaction);
        free(transaction);
        if (resource_result == XG_RENDER_RESOURCE_CAPACITY_EXCEEDED)
            return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
        if (resource_result == XG_RENDER_RESOURCE_NOT_FOUND ||
            resource_result == XG_RENDER_RESOURCE_STALE ||
            resource_result == XG_RENDER_RESOURCE_INVALID_STATE)
            return XG_RENDER_SURFACE_GRAPH_STALE;
        return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
    }
    memcpy(g_nodes, transaction->nodes, sizeof(g_nodes));
    memcpy(g_edges, transaction->edges, sizeof(g_edges));
    transaction->snapshot.rejected_operations =
        g_snapshot.rejected_operations;
    g_snapshot = transaction->snapshot;
    graph_revision_advance();
    free(transaction);
    return XG_RENDER_SURFACE_GRAPH_OK;
}

void xg_render_surface_graph_transaction_cancel(
        XgRenderSurfaceGraphTransaction *transaction) {
    if (transaction == NULL) return;
    transaction_cancel_staged_import(transaction);
    free(transaction);
}

void xg_render_surface_graph_transaction_fault_inject(
        XgRenderSurfaceGraphTransactionFault fault) {
    if (fault < XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_NONE ||
        fault > XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_RESOURCE)
        fault = XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_RESOURCE;
    g_transaction_fault = fault;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_copy_edges(
        XgSemanticSurfaceEdge *out_edges, size_t edge_capacity,
        size_t *out_edge_count) {
    if (out_edge_count == NULL ||
        (g_snapshot.edge_count != 0u && out_edges == NULL))
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    *out_edge_count = g_snapshot.edge_count;
    if (edge_capacity < g_snapshot.edge_count)
        return XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED;
    if (g_snapshot.edge_count != 0u)
        memcpy(out_edges, g_edges, g_snapshot.edge_count * sizeof(*g_edges));
    return XG_RENDER_SURFACE_GRAPH_OK;
}

static void write_u16(uint8_t **cursor, uint16_t value) {
    (*cursor)[0] = (uint8_t)value;
    (*cursor)[1] = (uint8_t)(value >> 8u);
    *cursor += 2u;
}

static void write_u32(uint8_t **cursor, uint32_t value) {
    for (uint32_t index = 0u; index < 4u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 4u;
}

static void write_u64(uint8_t **cursor, uint64_t value) {
    for (uint32_t index = 0u; index < 8u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 8u;
}

static uint16_t read_u16(const uint8_t **cursor) {
    uint16_t value = (uint16_t)(*cursor)[0] |
        (uint16_t)(*cursor)[1] << 8u;
    *cursor += 2u;
    return value;
}

static uint32_t read_u32(const uint8_t **cursor) {
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)(*cursor)[index] << (index * 8u);
    *cursor += 4u;
    return value;
}

static uint64_t read_u64(const uint8_t **cursor) {
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)(*cursor)[index] << (index * 8u);
    *cursor += 8u;
    return value;
}

size_t xg_render_surface_graph_checkpoint_size(void) {
    size_t size = XG_RENDER_SURFACE_GRAPH_HEADER_SIZE +
        (size_t)g_snapshot.edge_count * XG_RENDER_SURFACE_GRAPH_EDGE_SIZE;
    uint32_t count = 0u;

    for (uint32_t index = 0u; index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY;
         ++index) {
        XgRenderResourceView view;
        if (!g_nodes[index].occupied) continue;
        if (xg_render_resource_view(g_nodes[index].publication.handle, &view) !=
                XG_RENDER_RESOURCE_OK || !view.current ||
            size > SIZE_MAX - XG_RENDER_SURFACE_GRAPH_NODE_SIZE ||
            view.byte_count > SIZE_MAX - size - XG_RENDER_SURFACE_GRAPH_NODE_SIZE)
            return 0u;
        size += XG_RENDER_SURFACE_GRAPH_NODE_SIZE + view.byte_count;
        count++;
    }
    return count == g_snapshot.node_count ? size : 0u;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_checkpoint_write(
        void *out_checkpoint, size_t checkpoint_size) {
    const size_t required = xg_render_surface_graph_checkpoint_size();
    uint8_t *cursor = (uint8_t *)out_checkpoint;
    uint32_t count = 0u;

    if (required == 0u || out_checkpoint == NULL || checkpoint_size != required)
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    write_u32(&cursor, XG_RENDER_SURFACE_GRAPH_MAGIC);
    write_u32(&cursor, XG_RENDER_SURFACE_GRAPH_VERSION);
    write_u64(&cursor, required);
    write_u32(&cursor, g_snapshot.node_count);
    write_u32(&cursor, g_snapshot.edge_count);
    write_u64(&cursor, 0u);
    for (uint32_t index = 0u; index < XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY;
         ++index) {
        const XgRenderSurfaceNode *node = &g_nodes[index];
        XgRenderResourceView view;
        if (!node->occupied) continue;
        if (xg_render_resource_view(node->publication.handle, &view) !=
                XG_RENDER_RESOURCE_OK || !view.current || !view.has_identity ||
            view.kind != node->publication.kind ||
            view.owner_kind != node->publication.owner_kind ||
            view.owner_generation != node->publication.owner_generation ||
            view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
            !same_provenance(&view.provenance,
                             &node->publication.provenance) ||
            view.content_digest != node->publication.content_digest ||
            view.byte_count != node->publication.byte_count ||
            memcmp(view.identity.bytes, node->publication.identity.bytes,
                   sizeof(view.identity.bytes)) != 0)
            return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
        memcpy(cursor, view.identity.bytes, sizeof(view.identity.bytes));
        cursor += sizeof(view.identity.bytes);
        write_u64(&cursor, view.content_digest);
        write_u32(&cursor, node->publication.width);
        write_u32(&cursor, node->publication.height);
        write_u32(&cursor, (uint32_t)node->publication.format);
        write_u32(&cursor, view.descriptor.version);
        write_u32(&cursor, (uint32_t)view.descriptor.pixel_format);
        write_u32(&cursor, view.descriptor.width);
        write_u32(&cursor, view.descriptor.height);
        write_u32(&cursor, view.descriptor.row_pitch);
        write_u32(&cursor, view.descriptor.vram_x);
        write_u32(&cursor, view.descriptor.vram_y);
        write_u32(&cursor, view.descriptor.vram_width);
        write_u32(&cursor, view.descriptor.vram_height);
        write_u32(&cursor, (uint32_t)view.descriptor.sampler);
        write_u32(&cursor, (uint32_t)view.descriptor.wrap_u);
        write_u32(&cursor, (uint32_t)view.descriptor.wrap_v);
        write_u32(&cursor, view.descriptor.flags);
        write_u32(&cursor, (uint32_t)view.kind);
        write_u32(&cursor, (uint32_t)view.owner_kind);
        write_u32(&cursor, (uint32_t)view.state);
        write_u64(&cursor, view.owner_generation);
        write_u64(&cursor, view.byte_count);
        write_u32(&cursor, (uint32_t)view.provenance.kind);
        write_u32(&cursor, view.provenance.synthetic ? 1u : 0u);
        write_u64(&cursor, view.provenance.receipt);
        write_u64(&cursor, view.provenance.capability);
        if (view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_NONE) {
            memset(cursor, 0, XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE);
        } else {
            XgRenderResourceCapabilityCheckpoint authority;
            if (xg_render_resource_capability_checkpoint(
                    &view.provenance, view.owner_kind, view.owner_generation,
                    &authority) != XG_RENDER_RESOURCE_CAPABILITY_OK)
                return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
            memcpy(cursor, authority.bytes, sizeof(authority.bytes));
        }
        cursor += XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE;
        memcpy(cursor, view.bytes, view.byte_count);
        cursor += view.byte_count;
        count++;
    }
    for (uint32_t index = 0u; index < g_snapshot.edge_count; ++index) {
        const XgSemanticSurfaceEdge *edge = &g_edges[index];
        const uint32_t texture_window =
            (uint32_t)edge->sample.texture_window_mask_x |
            ((uint32_t)edge->sample.texture_window_mask_y << 8u) |
            ((uint32_t)edge->sample.texture_window_offset_x << 16u) |
            ((uint32_t)edge->sample.texture_window_offset_y << 24u);
        const uint32_t flags =
            (edge->sample.palette_enabled ? UINT32_C(1) : 0u) |
            (edge->sample.semi_transparent ? UINT32_C(2) : 0u) |
            (edge->sample.mask_set ? UINT32_C(4) : 0u) |
            (edge->sample.mask_check ? UINT32_C(8) : 0u);
        write_u64(&cursor, edge->source_surface_id);
        write_u64(&cursor, edge->target_surface_id);
        write_u32(&cursor, (uint32_t)edge->kind);
        write_u32(&cursor, edge->order.pass_id);
        write_u32(&cursor, (uint32_t)edge->order.layer);
        write_u32(&cursor, (uint32_t)edge->order.authored_depth);
        write_u32(&cursor, edge->order.insertion_ordinal);
        write_u16(&cursor, edge->order.split_ordinal);
        write_u16(&cursor, 0u);
        write_u32(&cursor, (uint32_t)edge->source.x);
        write_u32(&cursor, (uint32_t)edge->source.y);
        write_u32(&cursor, edge->source.width);
        write_u32(&cursor, edge->source.height);
        write_u32(&cursor, (uint32_t)edge->destination.x);
        write_u32(&cursor, (uint32_t)edge->destination.y);
        write_u32(&cursor, edge->destination.width);
        write_u32(&cursor, edge->destination.height);
        write_u32(&cursor, (uint32_t)edge->sample.sampler);
        write_u32(&cursor, (uint32_t)edge->sample.wrap_u);
        write_u32(&cursor, (uint32_t)edge->sample.wrap_v);
        write_u64(&cursor, edge->sample.clut_resource_id);
        write_u64(&cursor, edge->sample.clut_generation);
        write_u16(&cursor, edge->sample.clut_x);
        write_u16(&cursor, edge->sample.clut_y);
        write_u32(&cursor, texture_window);
        write_u32(&cursor, (uint32_t)edge->sample.texture_depth);
        write_u32(&cursor, (uint32_t)edge->sample.blend_mode);
        write_u32(&cursor, flags);
        write_u32(&cursor, edge->effect_phase);
        write_u32(&cursor, edge->effect_phase_count);
    }
    return count == g_snapshot.node_count &&
        cursor == (uint8_t *)out_checkpoint + checkpoint_size
        ? XG_RENDER_SURFACE_GRAPH_OK
        : XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
}

static XgRenderSurfaceGraphResult surface_graph_checkpoint_process(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_owner_generation,
        const XgRenderResourceHandle *shared_resources,
        size_t shared_resource_count,
        XgRenderSurfaceGraphCheckpointRestore **out_restore) {
    XgRenderSurfaceCheckpointNode nodes[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    XgRenderSurfaceCheckpointEdge edges[XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY];
    const uint8_t *cursor = (const uint8_t *)checkpoint;
    const uint8_t *end = cursor + checkpoint_size;
    uint32_t node_count;
    uint32_t edge_count;

    if (checkpoint == NULL || restored_owner_generation == 0u ||
        (shared_resources == NULL && shared_resource_count != 0u) ||
        checkpoint_size < XG_RENDER_SURFACE_GRAPH_HEADER_SIZE)
        return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    if (read_u32(&cursor) != XG_RENDER_SURFACE_GRAPH_MAGIC ||
        read_u32(&cursor) != XG_RENDER_SURFACE_GRAPH_VERSION ||
        read_u64(&cursor) != checkpoint_size)
        return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
    node_count = read_u32(&cursor);
    edge_count = read_u32(&cursor);
    if (read_u64(&cursor) != 0u ||
        node_count > XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY ||
        edge_count > XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY)
        return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
    for (uint32_t index = 0u; index < node_count; ++index) {
        XgRenderSurfaceCheckpointNode *node = &nodes[index];
        XgRenderResourceIdentity identity;
        uint64_t byte_count;
        uint32_t synthetic;
        if ((size_t)(end - cursor) < XG_RENDER_SURFACE_GRAPH_NODE_SIZE)
            return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
        memcpy(node->identity.bytes, cursor, sizeof(node->identity.bytes));
        cursor += sizeof(node->identity.bytes);
        node->content_digest = read_u64(&cursor);
        node->width = read_u32(&cursor);
        node->height = read_u32(&cursor);
        node->format = (XgRenderSurfaceFormat)read_u32(&cursor);
        node->descriptor.version = read_u32(&cursor);
        node->descriptor.pixel_format =
            (XgRenderResourcePixelFormat)read_u32(&cursor);
        node->descriptor.width = read_u32(&cursor);
        node->descriptor.height = read_u32(&cursor);
        node->descriptor.row_pitch = read_u32(&cursor);
        node->descriptor.vram_x = read_u32(&cursor);
        node->descriptor.vram_y = read_u32(&cursor);
        node->descriptor.vram_width = read_u32(&cursor);
        node->descriptor.vram_height = read_u32(&cursor);
        node->descriptor.sampler = (XgRenderResourceSampler)read_u32(&cursor);
        node->descriptor.wrap_u = (XgRenderResourceWrap)read_u32(&cursor);
        node->descriptor.wrap_v = (XgRenderResourceWrap)read_u32(&cursor);
        node->descriptor.flags = read_u32(&cursor);
        node->kind = (XgRenderResourceKind)read_u32(&cursor);
        node->owner_kind = (XgRenderResourceOwnerKind)read_u32(&cursor);
        node->state = (XgRenderResourceState)read_u32(&cursor);
        node->owner_generation = read_u64(&cursor);
        byte_count = read_u64(&cursor);
        node->provenance.kind =
            (XgRenderResourceProvenanceKind)read_u32(&cursor);
        synthetic = read_u32(&cursor);
        node->provenance.synthetic = synthetic != 0u;
        node->provenance.receipt = read_u64(&cursor);
        node->provenance.capability = read_u64(&cursor);
        memcpy(node->authority.bytes, cursor, sizeof(node->authority.bytes));
        cursor += sizeof(node->authority.bytes);
        if (byte_count > SIZE_MAX || byte_count > (uint64_t)(end - cursor))
            return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
        node->byte_count = byte_count;
        node->bytes = cursor;
        cursor += node->byte_count;
        if (!valid_graph_kind(node->kind) ||
            !valid_owner(node->kind, node->owner_kind) ||
            node->state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
            node->owner_generation == 0u ||
            synthetic > 1u ||
            !valid_payload(node->format, node->width, node->height,
                           &node->descriptor,
                           node->bytes, node->byte_count) ||
            xg_render_resource_digest(node->bytes, node->byte_count) !=
                node->content_digest ||
            xg_render_resource_identity_id(&node->identity) == 0u)
            return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
        node->restored_provenance = node->provenance;
        if (node->provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_NONE) {
            XgRenderResourceCapabilityCheckpoint empty = {0};
            if (!valid_provenance(&node->provenance, node->owner_kind,
                                  node->owner_generation) ||
                memcmp(&node->authority, &empty, sizeof(empty)) != 0)
                return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
        } else if (out_restore != NULL) {
            if (xg_render_resource_capability_checkpoint_restore_stage(
                    &node->authority, &node->provenance, node->owner_kind,
                    node->owner_generation, restored_owner_generation,
                    &node->restored_provenance) !=
                        XG_RENDER_RESOURCE_CAPABILITY_OK ||
                !valid_provenance(&node->restored_provenance,
                                  node->owner_kind,
                                  restored_owner_generation))
                return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
        } else if (xg_render_resource_capability_checkpoint_validate(
                       &node->authority, &node->provenance, node->owner_kind,
                       node->owner_generation, NULL) !=
                           XG_RENDER_RESOURCE_CAPABILITY_OK) {
            return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
        }
        identity = node->identity;
        for (uint32_t previous = 0u; previous < index; ++previous)
            if (xg_render_resource_identity_id(&nodes[previous].identity) ==
                xg_render_resource_identity_id(&identity))
                return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
    }
    for (uint32_t index = 0u; index < edge_count; ++index) {
        XgRenderSurfaceCheckpointEdge *edge = &edges[index];
        uint32_t texture_window;
        uint32_t flags;
        uint32_t reserved;
        if ((size_t)(end - cursor) < XG_RENDER_SURFACE_GRAPH_EDGE_SIZE)
            return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
        edge->source_id = read_u64(&cursor);
        edge->target_id = read_u64(&cursor);
        edge->kind = (XgSemanticSurfaceEdgeKind)read_u32(&cursor);
        edge->order.pass_id = read_u32(&cursor);
        edge->order.layer = (int32_t)read_u32(&cursor);
        edge->order.authored_depth = (int32_t)read_u32(&cursor);
        edge->order.insertion_ordinal = read_u32(&cursor);
        edge->order.split_ordinal = read_u16(&cursor);
        reserved = read_u16(&cursor);
        edge->source.x = (int32_t)read_u32(&cursor);
        edge->source.y = (int32_t)read_u32(&cursor);
        edge->source.width = read_u32(&cursor);
        edge->source.height = read_u32(&cursor);
        edge->destination.x = (int32_t)read_u32(&cursor);
        edge->destination.y = (int32_t)read_u32(&cursor);
        edge->destination.width = read_u32(&cursor);
        edge->destination.height = read_u32(&cursor);
        edge->sample.sampler = (XgRenderResourceSampler)read_u32(&cursor);
        edge->sample.wrap_u = (XgRenderResourceWrap)read_u32(&cursor);
        edge->sample.wrap_v = (XgRenderResourceWrap)read_u32(&cursor);
        edge->sample.clut_resource_id = read_u64(&cursor);
        edge->sample.clut_generation = read_u64(&cursor);
        edge->sample.clut_x = read_u16(&cursor);
        edge->sample.clut_y = read_u16(&cursor);
        texture_window = read_u32(&cursor);
        edge->sample.texture_window_mask_x = (uint8_t)texture_window;
        edge->sample.texture_window_mask_y = (uint8_t)(texture_window >> 8u);
        edge->sample.texture_window_offset_x =
            (uint8_t)(texture_window >> 16u);
        edge->sample.texture_window_offset_y =
            (uint8_t)(texture_window >> 24u);
        edge->sample.texture_depth =
            (XgRenderIrTextureDepth)read_u32(&cursor);
        edge->sample.blend_mode = (XgRenderIrBlendMode)read_u32(&cursor);
        flags = read_u32(&cursor);
        edge->sample.palette_enabled = (flags & UINT32_C(1)) != 0u;
        edge->sample.semi_transparent = (flags & UINT32_C(2)) != 0u;
        edge->sample.mask_set = (flags & UINT32_C(4)) != 0u;
        edge->sample.mask_check = (flags & UINT32_C(8)) != 0u;
        edge->effect_phase = read_u32(&cursor);
        edge->effect_phase_count = read_u32(&cursor);
        if (reserved != 0u || (flags & ~UINT32_C(15)) != 0u ||
            !checkpoint_edge_contract_valid(edge))
            return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
    }
    if (cursor != end) return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;

    for (uint32_t index = 0u; index < edge_count; ++index) {
        const XgRenderSurfaceCheckpointEdge *edge = &edges[index];
        const XgRenderSurfaceCheckpointNode *source = NULL;
        const XgRenderSurfaceCheckpointNode *target = NULL;
        for (uint32_t node_index = 0u; node_index < node_count; ++node_index) {
            const uint64_t node_id =
                xg_render_resource_identity_id(&nodes[node_index].identity);
            if (node_id == edge->source_id) source = &nodes[node_index];
            if (node_id == edge->target_id) target = &nodes[node_index];
        }
        const XgSemanticSurfaceEdge semantic = {
            .source = edge->source,
            .destination = edge->destination,
        };
        if (source == NULL || target == NULL ||
            !edge_rects_valid(&semantic, source->width, source->height,
                              target->width, target->height))
            return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
    }
    if (out_restore == NULL) return XG_RENDER_SURFACE_GRAPH_OK;

    XgRenderSurfaceGraphCheckpointRestore *restore =
        (XgRenderSurfaceGraphCheckpointRestore *)calloc(1u, sizeof(*restore));
    if (restore == NULL) return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;

    for (uint32_t index = 0u; index < node_count; ++index) {
        const XgRenderSurfaceCheckpointNode *node = &nodes[index];
        XgRenderResourceImport import;
        XgRenderResourceHandle handle = {0};
        bool shared = false;
        if (node->restored_provenance.kind !=
                XG_RENDER_RESOURCE_PROVENANCE_NONE &&
            xg_render_resource_capability_restore_stage(
                node->restored_provenance, node->owner_kind,
                restored_owner_generation) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK) {
            for (uint32_t staged = 0u; staged < index; ++staged)
                if (restore->resource_owned[staged])
                    xg_render_resource_restore_cancel(
                        restore->nodes[staged].publication.handle);
            free(restore);
            return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
        }
        import = (XgRenderResourceImport){
            .resource_id = xg_render_resource_identity_id(&node->identity),
            .kind = node->kind,
            .owner_kind = node->owner_kind,
            .owner_generation = restored_owner_generation,
            .state = node->state,
            .provenance = node->restored_provenance,
            .content_digest = node->content_digest,
            .bytes = node->bytes,
            .byte_count = node->byte_count,
            .identity = node->identity,
            .descriptor = node->descriptor,
        };
        for (size_t shared_index = 0u;
             shared_index < shared_resource_count; ++shared_index) {
            XgRenderResourceView view;

            if (xg_render_resource_view(shared_resources[shared_index], &view) !=
                    XG_RENDER_RESOURCE_OK || view.current ||
                view.state != XG_RENDER_RESOURCE_IMPORTING_NATIVE ||
                !view.has_identity) {
                for (uint32_t staged = 0u; staged < index; ++staged)
                    if (restore->resource_owned[staged])
                        xg_render_resource_restore_cancel(
                            restore->nodes[staged].publication.handle);
                free(restore);
                return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
            }
            if (memcmp(view.identity.bytes, node->identity.bytes,
                       sizeof(view.identity.bytes)) != 0)
                continue;
            if (shared || view.handle.resource_id != import.resource_id ||
                view.kind != import.kind || view.owner_kind != import.owner_kind ||
                view.owner_generation != import.owner_generation ||
                !same_provenance(&view.provenance, &import.provenance) ||
                view.content_digest != import.content_digest ||
                view.byte_count != import.byte_count ||
                !same_descriptor(&view.descriptor, &import.descriptor) ||
                memcmp(view.bytes, import.bytes, import.byte_count) != 0) {
                for (uint32_t staged = 0u; staged < index; ++staged)
                    if (restore->resource_owned[staged])
                        xg_render_resource_restore_cancel(
                            restore->nodes[staged].publication.handle);
                free(restore);
                return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
            }
            handle = view.handle;
            shared = true;
        }
        if (!shared && xg_render_resource_restore_stage(&import, &handle) !=
                XG_RENDER_RESOURCE_OK) {
            for (uint32_t staged = 0u; staged < index; ++staged)
                if (restore->resource_owned[staged])
                    xg_render_resource_restore_cancel(
                        restore->nodes[staged].publication.handle);
            free(restore);
            return XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED;
        }
        restore->resource_owned[index] = !shared;
        restore->nodes[index] = (XgRenderSurfaceNode){
            .publication = {
                .handle = handle,
                .identity = node->identity,
                .kind = node->kind,
                .owner_kind = node->owner_kind,
                .format = node->format,
                .provenance = node->restored_provenance,
                .owner_generation = restored_owner_generation,
                .content_digest = node->content_digest,
                .width = node->width,
                .height = node->height,
                .descriptor = node->descriptor,
                .byte_count = node->byte_count,
            },
            .occupied = true,
        };
    }
    for (uint32_t index = 0u; index < edge_count; ++index) {
        const XgRenderSurfaceCheckpointEdge *edge = &edges[index];
        XgRenderSurfaceNode *source = NULL;
        XgRenderSurfaceNode *target = NULL;
        for (uint32_t node_index = 0u; node_index < node_count; ++node_index) {
            if (restore->nodes[node_index].publication.handle.resource_id ==
                edge->source_id) source = &restore->nodes[node_index];
            if (restore->nodes[node_index].publication.handle.resource_id ==
                edge->target_id) target = &restore->nodes[node_index];
        }
        if (source == NULL || target == NULL) {
            for (uint32_t staged = 0u; staged < node_count; ++staged)
                if (restore->resource_owned[staged])
                    xg_render_resource_restore_cancel(
                        restore->nodes[staged].publication.handle);
            free(restore);
            return XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT;
        }
        restore->edges[index] = (XgSemanticSurfaceEdge){
            .source_surface_id = source->publication.handle.resource_id,
            .source_generation = source->publication.handle.generation,
            .target_surface_id = target->publication.handle.resource_id,
            .target_generation = target->publication.handle.generation,
            .kind = edge->kind,
            .order = edge->order,
            .source = edge->source,
            .destination = edge->destination,
            .sample = edge->sample,
            .effect_phase = edge->effect_phase,
            .effect_phase_count = edge->effect_phase_count,
        };
    }
    restore->node_count = node_count;
    restore->edge_count = edge_count;
    *out_restore = restore;
    return XG_RENDER_SURFACE_GRAPH_OK;
}

XgRenderSurfaceGraphResult xg_render_surface_graph_checkpoint_validate(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_owner_generation) {
    return surface_graph_checkpoint_process(
        checkpoint, checkpoint_size, restored_owner_generation,
        NULL, 0u, NULL);
}

XgRenderSurfaceGraphResult xg_render_surface_graph_checkpoint_prepare(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_owner_generation,
        XgRenderSurfaceGraphCheckpointRestore **out_restore) {
    if (out_restore == NULL) return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    *out_restore = NULL;
    return surface_graph_checkpoint_process(
        checkpoint, checkpoint_size, restored_owner_generation,
        NULL, 0u, out_restore);
}

XgRenderSurfaceGraphResult
xg_render_surface_graph_checkpoint_prepare_with_shared_resources(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_owner_generation,
        const XgRenderResourceHandle *shared_resources,
        size_t shared_resource_count,
        XgRenderSurfaceGraphCheckpointRestore **out_restore) {
    if (out_restore == NULL) return XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT;
    *out_restore = NULL;
    return surface_graph_checkpoint_process(
        checkpoint, checkpoint_size, restored_owner_generation,
        shared_resources, shared_resource_count, out_restore);
}

void xg_render_surface_graph_checkpoint_commit(
        XgRenderSurfaceGraphCheckpointRestore *restore) {
    if (restore == NULL) return;
    for (uint32_t index = 0u; index < restore->node_count; ++index)
        if (restore->resource_owned[index])
            (void)xg_render_resource_restore_commit(
                restore->nodes[index].publication.handle);
    memcpy(g_nodes, restore->nodes, sizeof(g_nodes));
    memcpy(g_edges, restore->edges, sizeof(g_edges));
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.node_count = restore->node_count;
    g_snapshot.edge_count = restore->edge_count;
    g_snapshot.restorations = 1u;
    graph_revision_advance();
    free(restore);
}

void xg_render_surface_graph_checkpoint_cancel(
        XgRenderSurfaceGraphCheckpointRestore *restore) {
    if (restore == NULL) return;
    for (uint32_t index = 0u; index < restore->node_count; ++index)
        if (restore->resource_owned[index])
            xg_render_resource_restore_cancel(
                restore->nodes[index].publication.handle);
    free(restore);
}

XgRenderSurfaceGraphResult xg_render_surface_graph_checkpoint_restore(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_owner_generation) {
    XgRenderSurfaceGraphCheckpointRestore *restore;
    XgRenderSurfaceGraphResult result;

    if (!xg_render_resource_capability_restore_begin())
        return XG_RENDER_SURFACE_GRAPH_STALE;
    result =
        xg_render_surface_graph_checkpoint_prepare(
            checkpoint, checkpoint_size, restored_owner_generation, &restore);
    if (result == XG_RENDER_SURFACE_GRAPH_OK) {
        xg_render_resource_capability_restore_commit();
        xg_render_surface_graph_checkpoint_commit(restore);
    } else {
        xg_render_resource_capability_restore_cancel();
    }
    return result;
}

void xg_render_surface_graph_snapshot(XgRenderSurfaceGraphSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = g_snapshot;
}
