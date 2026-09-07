#include "xg_render_movie_publisher.h"

#include <stdlib.h>
#include <string.h>

typedef struct XgRenderMoviePublisher {
    XgRenderMovieFrameDescription description;
    XgRenderMovieFrameDescription complete_description;
    XgRenderMovieFramePublication complete_publication;
    uint8_t *bytes;
    uint8_t *written;
    uint8_t *complete_bytes;
    uint32_t completed_strip_mask;
    uint32_t generation;
    uint64_t published_frame_generation;
    XgRenderMovieDiagnostics diagnostics;
    XgRenderMovieDiscontinuity discontinuity_reason;
    bool active;
    bool complete_frame_available;
    bool discontinuity_pending;
} XgRenderMoviePublisher;

struct XgRenderMovieCheckpointRestore {
    XgRenderMoviePublisher publisher;
};

struct XgRenderMovieFrameTransaction {
    XgRenderMovieFrameDescription description;
    XgRenderMovieFramePublication publication;
    uint8_t *bytes;
    uint32_t publisher_generation;
    bool surface_staged;
};

enum {
    XG_RENDER_MOVIE_WIRE_MAGIC = 0x574d4758u,
    XG_RENDER_MOVIE_WIRE_FIXED_SIZE = 332u +
        2u * XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE,
};

static XgRenderMoviePublisher g_publisher;

static bool frame_valid(XgRenderMovieFrameHandle frame) {
    return g_publisher.active && frame.generation != 0u &&
        frame.generation == g_publisher.generation;
}

static uint32_t expected_strip_mask(uint32_t expected_strips) {
    return expected_strips == 32u
        ? UINT32_MAX
        : (UINT32_C(1) << expected_strips) - 1u;
}

static XgRenderResourceDescriptor movie_descriptor(
        const XgRenderMovieFrameDescription *description) {
    if (description == NULL || description->height == 0u ||
        description->byte_count % description->height != 0u ||
        description->byte_count / description->height > UINT32_MAX)
        return (XgRenderResourceDescriptor){0};
    return (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = description->depth24
            ? XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24
            : XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = description->width,
        .height = description->height,
        .row_pitch = (uint32_t)(description->byte_count /
                                description->height),
        .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
        .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
        .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
    };
}

static bool description_valid(
        const XgRenderMovieFrameDescription *description) {
    XgRenderResourceCapabilityMetadata metadata;
    const XgRenderResourceProvenance provenance = description != NULL
        ? (XgRenderResourceProvenance){
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = description->owner_receipt,
            .capability = description->owner_capability,
        }
        : (XgRenderResourceProvenance){0};

    return description != NULL && description->movie_id != 0u &&
        (description->owner_kind == XG_RENDER_MOVIE_OWNER_STANDALONE ||
         description->owner_kind == XG_RENDER_MOVIE_OWNER_FIELD) &&
        description->owner_receipt != 0u && description->owner_capability != 0u &&
        description->owner_generation != 0u && description->width != 0u &&
        description->height != 0u && description->expected_strips != 0u &&
        description->expected_strips <= 32u && description->byte_count != 0u &&
        description->byte_count % description->height == 0u &&
        description->byte_count / description->height <= UINT32_MAX &&
        xg_render_resource_capability_validate(
            &provenance, XG_RENDER_RESOURCE_OWNER_SOURCE,
            description->owner_generation, &metadata) ==
                XG_RENDER_RESOURCE_CAPABILITY_OK &&
        metadata.source.source_class == XG_RENDER_RESOURCE_SOURCE_MOVIE_OWNER;
}

static XgRenderResourceIdentity movie_identity(uint64_t movie_id) {
    XgRenderResourceIdentity identity = {0};

    for (uint32_t index = 0u; index < sizeof(movie_id); ++index)
        identity.bytes[index] = (uint8_t)(movie_id >> (index * 8u));
    return identity;
}

static void advance_generation(void) {
    g_publisher.generation++;
    if (g_publisher.generation == 0u) g_publisher.generation = 1u;
}

static void clear_active_frame(void) {
    free(g_publisher.bytes);
    free(g_publisher.written);
    g_publisher.bytes = NULL;
    g_publisher.written = NULL;
    g_publisher.active = false;
    g_publisher.completed_strip_mask = 0u;
    g_publisher.diagnostics.frame_active = false;
    g_publisher.diagnostics.completed_strip_mask = 0u;
    advance_generation();
}

static void update_diagnostic_state(void) {
    g_publisher.diagnostics.frame_generation =
        g_publisher.published_frame_generation;
    g_publisher.diagnostics.frame_active = g_publisher.active;
    g_publisher.diagnostics.completed_strip_mask =
        g_publisher.completed_strip_mask;
    g_publisher.diagnostics.complete_frame_available =
        g_publisher.complete_frame_available;
    g_publisher.diagnostics.discontinuity_pending =
        g_publisher.discontinuity_pending;
    g_publisher.diagnostics.discontinuity_reason =
        g_publisher.discontinuity_pending
            ? g_publisher.discontinuity_reason
            : XG_RENDER_MOVIE_DISCONTINUITY_NONE;
}

static XgRenderMovieResult import_complete_surface(
        XgRenderResourceHandle *out_surface) {
    XgRenderResourceImport import;

    if (!g_publisher.complete_frame_available ||
        g_publisher.complete_bytes == NULL)
        return XG_RENDER_MOVIE_NO_COMPLETE_FRAME;
    import = (XgRenderResourceImport){
        .resource_id = g_publisher.complete_description.movie_id,
        .kind = XG_RENDER_RESOURCE_MOVIE_FRAME,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
        .owner_generation =
            g_publisher.complete_description.owner_generation,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = g_publisher.complete_description.owner_receipt,
            .capability = g_publisher.complete_description.owner_capability,
            .synthetic = false,
        },
        .content_digest = xg_render_resource_digest(
            g_publisher.complete_bytes,
            g_publisher.complete_description.byte_count),
        .bytes = g_publisher.complete_bytes,
        .byte_count = g_publisher.complete_description.byte_count,
        .identity = movie_identity(g_publisher.complete_description.movie_id),
        .descriptor = movie_descriptor(&g_publisher.complete_description),
    };
    if (xg_render_resource_import_native(&import, out_surface) !=
        XG_RENDER_RESOURCE_OK)
        return XG_RENDER_MOVIE_RESOURCE_FAILED;
    return XG_RENDER_MOVIE_OK;
}

static bool size_add(size_t left, size_t right, size_t *out) {
    if (out == NULL || right > SIZE_MAX - left) return false;
    *out = left + right;
    return true;
}

void xg_render_movie_publisher_reset(void) {
    uint32_t generation = g_publisher.generation + 1u;
    free(g_publisher.bytes);
    free(g_publisher.written);
    free(g_publisher.complete_bytes);
    memset(&g_publisher, 0, sizeof(g_publisher));
    g_publisher.generation = generation == 0u ? 1u : generation;
}

void xg_render_movie_publisher_scene_boundary(void) {
    free(g_publisher.bytes);
    free(g_publisher.written);
    g_publisher.bytes = NULL;
    g_publisher.written = NULL;
    g_publisher.description = (XgRenderMovieFrameDescription){0};
    g_publisher.completed_strip_mask = 0u;
    g_publisher.active = false;
    advance_generation();
    update_diagnostic_state();
}

XgRenderMovieResult xg_render_movie_frame_begin(
        const XgRenderMovieFrameDescription *description,
        XgRenderMovieFrameHandle *out_frame) {
    if (!description_valid(description) || out_frame == NULL)
        return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    if (g_publisher.active) return XG_RENDER_MOVIE_FRAME_ACTIVE;
    g_publisher.bytes = (uint8_t *)calloc(description->byte_count, 1u);
    g_publisher.written = (uint8_t *)calloc(description->byte_count, 1u);
    if (g_publisher.bytes == NULL || g_publisher.written == NULL) {
        free(g_publisher.bytes);
        free(g_publisher.written);
        g_publisher.bytes = NULL;
        g_publisher.written = NULL;
        return XG_RENDER_MOVIE_OUT_OF_MEMORY;
    }
    if (g_publisher.generation == 0u) g_publisher.generation = 1u;
    g_publisher.description = *description;
    g_publisher.completed_strip_mask = 0u;
    g_publisher.active = true;
    update_diagnostic_state();
    *out_frame = (XgRenderMovieFrameHandle){ g_publisher.generation };
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_frame_write_strip(
        XgRenderMovieFrameHandle frame, uint32_t strip_index,
        size_t byte_offset, const void *bytes, size_t byte_count) {
    size_t index;
    if (!frame_valid(frame)) return XG_RENDER_MOVIE_STALE_FRAME;
    if (bytes == NULL || byte_count == 0u ||
        strip_index >= g_publisher.description.expected_strips ||
        byte_offset > g_publisher.description.byte_count ||
        byte_count > g_publisher.description.byte_count - byte_offset)
        return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    if ((g_publisher.completed_strip_mask & (UINT32_C(1) << strip_index)) != 0u)
        return XG_RENDER_MOVIE_DUPLICATE_STRIP;
    for (index = 0; index < byte_count; index++) {
        if (g_publisher.written[byte_offset + index] != 0u)
            return XG_RENDER_MOVIE_DUPLICATE_STRIP;
    }
    memcpy(g_publisher.bytes + byte_offset, bytes, byte_count);
    memset(g_publisher.written + byte_offset, 1, byte_count);
    g_publisher.completed_strip_mask |= UINT32_C(1) << strip_index;
    update_diagnostic_state();
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_frame_publish(
        XgRenderMovieFrameHandle frame,
        XgRenderMovieFramePublication *out_publication) {
    uint32_t expected_mask;
    XgRenderResourceImport import;
    XgRenderResourceHandle surface;
    XgRenderMovieDiscontinuity discontinuity_reason;
    bool discontinuity;
    size_t index;
    if (!frame_valid(frame)) return XG_RENDER_MOVIE_STALE_FRAME;
    if (out_publication == NULL) return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    expected_mask = expected_strip_mask(
        g_publisher.description.expected_strips);
    if (g_publisher.completed_strip_mask != expected_mask) {
        g_publisher.diagnostics.partial_publish_attempts++;
        return XG_RENDER_MOVIE_INCOMPLETE_FRAME;
    }
    for (index = 0; index < g_publisher.description.byte_count; index++) {
        if (g_publisher.written[index] == 0u) {
            g_publisher.diagnostics.partial_publish_attempts++;
            return XG_RENDER_MOVIE_INCOMPLETE_FRAME;
        }
    }
    import = (XgRenderResourceImport){
        .resource_id = g_publisher.description.movie_id,
        .kind = XG_RENDER_RESOURCE_MOVIE_FRAME,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
        .owner_generation = g_publisher.description.owner_generation,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = g_publisher.description.owner_receipt,
            .capability = g_publisher.description.owner_capability,
            .synthetic = false,
        },
        .content_digest = xg_render_resource_digest(
            g_publisher.bytes, g_publisher.description.byte_count),
        .bytes = g_publisher.bytes,
        .byte_count = g_publisher.description.byte_count,
        .identity = movie_identity(g_publisher.description.movie_id),
        .descriptor = movie_descriptor(&g_publisher.description),
    };
    if (xg_render_resource_import_native(&import, &surface) !=
        XG_RENDER_RESOURCE_OK)
        return XG_RENDER_MOVIE_RESOURCE_FAILED;
    discontinuity = g_publisher.discontinuity_pending ||
        (g_publisher.complete_frame_available &&
         g_publisher.complete_description.depth24 !=
             g_publisher.description.depth24);
    discontinuity_reason = g_publisher.discontinuity_pending
        ? g_publisher.discontinuity_reason
        : discontinuity
            ? XG_RENDER_MOVIE_DISCONTINUITY_DEPTH_CHANGE
            : XG_RENDER_MOVIE_DISCONTINUITY_NONE;
    if (discontinuity_reason == XG_RENDER_MOVIE_DISCONTINUITY_DEPTH_CHANGE)
        g_publisher.diagnostics.discontinuities++;
    g_publisher.complete_description = g_publisher.description;
    g_publisher.complete_publication = (XgRenderMovieFramePublication){
        .surface = surface,
        .owner_kind = g_publisher.description.owner_kind,
        .owner_receipt = g_publisher.description.owner_receipt,
        .owner_capability = g_publisher.description.owner_capability,
        .frame_generation = g_publisher.published_frame_generation + 1u,
        .guest_cycle = g_publisher.description.guest_cycle,
        .completed_strip_mask = g_publisher.completed_strip_mask,
        .width = g_publisher.description.width,
        .height = g_publisher.description.height,
        .depth24 = g_publisher.description.depth24,
        .odd_field = g_publisher.description.odd_field,
        .discontinuity = discontinuity,
        .discontinuity_reason = discontinuity_reason,
    };
    if (g_publisher.complete_publication.frame_generation == 0u)
        g_publisher.complete_publication.frame_generation = 1u;
    g_publisher.published_frame_generation =
        g_publisher.complete_publication.frame_generation;
    free(g_publisher.complete_bytes);
    g_publisher.complete_bytes = g_publisher.bytes;
    g_publisher.bytes = NULL;
    free(g_publisher.written);
    g_publisher.written = NULL;
    g_publisher.active = false;
    g_publisher.completed_strip_mask = 0u;
    g_publisher.complete_frame_available = true;
    g_publisher.discontinuity_pending = false;
    g_publisher.discontinuity_reason = XG_RENDER_MOVIE_DISCONTINUITY_NONE;
    g_publisher.diagnostics.complete_frames++;
    g_publisher.diagnostics.frame_generation =
        g_publisher.published_frame_generation;
    advance_generation();
    update_diagnostic_state();
    *out_publication = g_publisher.complete_publication;
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_frame_transaction_prepare(
        const XgRenderMovieFrameDescription *description,
        const void *bytes, size_t byte_count,
        XgRenderMovieFrameTransaction **out_transaction,
        XgRenderMovieFramePublication *out_publication) {
    XgRenderMovieFrameTransaction *transaction;
    XgRenderResourceImport import;
    XgRenderResourceView view;
    XgRenderMovieDiscontinuity discontinuity_reason;
    bool discontinuity;

    if (out_transaction == NULL) return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    *out_transaction = NULL;
    if (!description_valid(description) || bytes == NULL ||
        byte_count != description->byte_count ||
        description->expected_strips != 1u || out_publication == NULL)
        return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    if (g_publisher.active) return XG_RENDER_MOVIE_FRAME_ACTIVE;
    transaction = (XgRenderMovieFrameTransaction *)calloc(
        1u, sizeof(*transaction));
    if (transaction == NULL) return XG_RENDER_MOVIE_OUT_OF_MEMORY;
    transaction->bytes = (uint8_t *)malloc(byte_count);
    if (transaction->bytes == NULL) {
        free(transaction);
        return XG_RENDER_MOVIE_OUT_OF_MEMORY;
    }
    memcpy(transaction->bytes, bytes, byte_count);
    transaction->description = *description;
    transaction->publisher_generation = g_publisher.generation;
    import = (XgRenderResourceImport){
        .resource_id = description->movie_id,
        .kind = XG_RENDER_RESOURCE_MOVIE_FRAME,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
        .owner_generation = description->owner_generation,
        .state = XG_RENDER_RESOURCE_IMPORTING_NATIVE,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = description->owner_receipt,
            .capability = description->owner_capability,
            .synthetic = false,
        },
        .content_digest = xg_render_resource_digest(bytes, byte_count),
        .bytes = transaction->bytes,
        .byte_count = byte_count,
        .identity = movie_identity(description->movie_id),
        .descriptor = movie_descriptor(description),
    };
    if (xg_render_resource_import_begin(
            &import, &transaction->publication.surface) !=
            XG_RENDER_RESOURCE_OK) {
        free(transaction->bytes);
        free(transaction);
        return XG_RENDER_MOVIE_RESOURCE_FAILED;
    }
    if (xg_render_resource_view(transaction->publication.surface, &view) !=
            XG_RENDER_RESOURCE_OK) {
        (void)xg_render_resource_import_cancel(
            transaction->publication.surface);
        free(transaction->bytes);
        free(transaction);
        return XG_RENDER_MOVIE_RESOURCE_FAILED;
    }
    transaction->surface_staged = !view.current &&
        view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE;
    discontinuity = g_publisher.discontinuity_pending ||
        (g_publisher.complete_frame_available &&
         g_publisher.complete_description.depth24 != description->depth24);
    discontinuity_reason = g_publisher.discontinuity_pending
        ? g_publisher.discontinuity_reason
        : discontinuity
            ? XG_RENDER_MOVIE_DISCONTINUITY_DEPTH_CHANGE
            : XG_RENDER_MOVIE_DISCONTINUITY_NONE;
    transaction->publication = (XgRenderMovieFramePublication){
        .surface = transaction->publication.surface,
        .owner_kind = description->owner_kind,
        .owner_receipt = description->owner_receipt,
        .owner_capability = description->owner_capability,
        .frame_generation = g_publisher.published_frame_generation + 1u,
        .guest_cycle = description->guest_cycle,
        .completed_strip_mask = 1u,
        .width = description->width,
        .height = description->height,
        .depth24 = description->depth24,
        .odd_field = description->odd_field,
        .discontinuity = discontinuity,
        .discontinuity_reason = discontinuity_reason,
    };
    if (transaction->publication.frame_generation == 0u)
        transaction->publication.frame_generation = 1u;
    *out_publication = transaction->publication;
    *out_transaction = transaction;
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_frame_transaction_commit(
        XgRenderMovieFrameTransaction *transaction) {
    XgRenderMovieResult result = XG_RENDER_MOVIE_OK;

    if (transaction == NULL) return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    if (g_publisher.active)
        result = XG_RENDER_MOVIE_FRAME_ACTIVE;
    else if (g_publisher.generation != transaction->publisher_generation)
        result = XG_RENDER_MOVIE_STALE_FRAME;
    else if (transaction->surface_staged &&
             xg_render_resource_import_commit(
                 transaction->publication.surface) != XG_RENDER_RESOURCE_OK)
        result = XG_RENDER_MOVIE_RESOURCE_FAILED;
    if (result != XG_RENDER_MOVIE_OK) {
        if (transaction->surface_staged)
            (void)xg_render_resource_import_cancel(
                transaction->publication.surface);
        free(transaction->bytes);
        free(transaction);
        return result;
    }
    if (transaction->publication.discontinuity_reason ==
            XG_RENDER_MOVIE_DISCONTINUITY_DEPTH_CHANGE)
        g_publisher.diagnostics.discontinuities++;
    g_publisher.complete_description = transaction->description;
    g_publisher.complete_publication = transaction->publication;
    g_publisher.published_frame_generation =
        transaction->publication.frame_generation;
    free(g_publisher.complete_bytes);
    g_publisher.complete_bytes = transaction->bytes;
    g_publisher.complete_frame_available = true;
    g_publisher.discontinuity_pending = false;
    g_publisher.discontinuity_reason = XG_RENDER_MOVIE_DISCONTINUITY_NONE;
    g_publisher.diagnostics.complete_frames++;
    g_publisher.diagnostics.frame_generation =
        g_publisher.published_frame_generation;
    advance_generation();
    update_diagnostic_state();
    free(transaction);
    return XG_RENDER_MOVIE_OK;
}

void xg_render_movie_frame_transaction_cancel(
        XgRenderMovieFrameTransaction *transaction) {
    if (transaction == NULL) return;
    if (transaction->surface_staged)
        (void)xg_render_resource_import_cancel(transaction->publication.surface);
    free(transaction->bytes);
    free(transaction);
}

XgRenderMovieResult xg_render_movie_frame_hold(
        XgRenderMovieFramePublication *out_publication) {
    XgRenderResourceHandle surface;
    XgRenderMovieResult result;

    if (out_publication == NULL) return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    result = import_complete_surface(&surface);
    if (result != XG_RENDER_MOVIE_OK) return result;
    *out_publication = g_publisher.complete_publication;
    out_publication->surface = surface;
    out_publication->held = true;
    if (g_publisher.discontinuity_pending) {
        out_publication->discontinuity = true;
        out_publication->discontinuity_reason =
            g_publisher.discontinuity_reason;
    }
    g_publisher.complete_publication.surface = surface;
    g_publisher.diagnostics.held_frames++;
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_frame_cancel(XgRenderMovieFrameHandle frame) {
    if (!frame_valid(frame)) return XG_RENDER_MOVIE_STALE_FRAME;
    g_publisher.diagnostics.cancelled_frames++;
    clear_active_frame();
    update_diagnostic_state();
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_publisher_note_discontinuity(
        XgRenderMovieDiscontinuity reason) {
    if (reason <= XG_RENDER_MOVIE_DISCONTINUITY_NONE ||
        reason > XG_RENDER_MOVIE_DISCONTINUITY_RESTORE)
        return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    if (g_publisher.active) {
        g_publisher.diagnostics.cancelled_frames++;
        clear_active_frame();
    }
    g_publisher.discontinuity_pending = true;
    g_publisher.discontinuity_reason = reason;
    g_publisher.diagnostics.discontinuities++;
    update_diagnostic_state();
    return XG_RENDER_MOVIE_OK;
}

static size_t snapshot_allocation_size(size_t logical_size) {
    /* A flexible array may begin inside the struct's trailing padding. */
    return logical_size < sizeof(XgRenderMoviePublisherSnapshot)
        ? sizeof(XgRenderMoviePublisherSnapshot) : logical_size;
}

XgRenderMovieResult xg_render_movie_publisher_snapshot_size(size_t *out_size) {
    size_t size = offsetof(XgRenderMoviePublisherSnapshot, payload);

    if (out_size == NULL) return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    if (g_publisher.active &&
        (!size_add(size, g_publisher.description.byte_count, &size) ||
         !size_add(size, g_publisher.description.byte_count, &size)))
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    if (g_publisher.complete_frame_available &&
        !size_add(size, g_publisher.complete_description.byte_count, &size))
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    *out_size = snapshot_allocation_size(size);
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_publisher_snapshot(
        XgRenderMoviePublisherSnapshot *out_snapshot,
        size_t snapshot_capacity) {
    uint8_t *payload;
    size_t required_size;

    if (out_snapshot == NULL) return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    if (xg_render_movie_publisher_snapshot_size(&required_size) !=
            XG_RENDER_MOVIE_OK ||
        snapshot_capacity < required_size)
        return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    memset(out_snapshot, 0, required_size);
    out_snapshot->version = XG_RENDER_MOVIE_PUBLISHER_SNAPSHOT_VERSION;
    out_snapshot->assembly_generation = g_publisher.generation;
    out_snapshot->snapshot_size = (uint64_t)required_size;
    out_snapshot->published_frame_generation =
        g_publisher.published_frame_generation;
    out_snapshot->active_description = g_publisher.description;
    out_snapshot->complete_description = g_publisher.complete_description;
    out_snapshot->complete_publication = g_publisher.complete_publication;
    out_snapshot->diagnostics = g_publisher.diagnostics;
    out_snapshot->active_byte_count = g_publisher.active
        ? (uint64_t)g_publisher.description.byte_count : 0u;
    out_snapshot->active_written_byte_count = out_snapshot->active_byte_count;
    out_snapshot->complete_byte_count = g_publisher.complete_frame_available
        ? (uint64_t)g_publisher.complete_description.byte_count : 0u;
    out_snapshot->completed_strip_mask = g_publisher.completed_strip_mask;
    out_snapshot->active = g_publisher.active;
    out_snapshot->complete_frame_available =
        g_publisher.complete_frame_available;
    out_snapshot->discontinuity_pending = g_publisher.discontinuity_pending;
    out_snapshot->discontinuity_reason = g_publisher.discontinuity_reason;
    if ((g_publisher.active &&
         xg_render_resource_capability_checkpoint(
             &(XgRenderResourceProvenance){
                 .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                 .receipt = g_publisher.description.owner_receipt,
                 .capability = g_publisher.description.owner_capability,
             }, XG_RENDER_RESOURCE_OWNER_SOURCE,
             g_publisher.description.owner_generation,
             &out_snapshot->active_authority) !=
                 XG_RENDER_RESOURCE_CAPABILITY_OK) ||
        (g_publisher.complete_frame_available &&
         xg_render_resource_capability_checkpoint(
             &(XgRenderResourceProvenance){
                 .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                 .receipt = g_publisher.complete_description.owner_receipt,
                 .capability = g_publisher.complete_description.owner_capability,
             }, XG_RENDER_RESOURCE_OWNER_SOURCE,
             g_publisher.complete_description.owner_generation,
             &out_snapshot->complete_authority) !=
                 XG_RENDER_RESOURCE_CAPABILITY_OK))
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    payload = out_snapshot->payload;
    if (g_publisher.active) {
        memcpy(payload, g_publisher.bytes, g_publisher.description.byte_count);
        payload += g_publisher.description.byte_count;
        memcpy(payload, g_publisher.written,
               g_publisher.description.byte_count);
        payload += g_publisher.description.byte_count;
    }
    if (g_publisher.complete_frame_available)
        memcpy(payload, g_publisher.complete_bytes,
               g_publisher.complete_description.byte_count);
    return XG_RENDER_MOVIE_OK;
}

static bool snapshot_valid(const XgRenderMoviePublisherSnapshot *snapshot,
                           size_t snapshot_size) {
    size_t required_size = offsetof(XgRenderMoviePublisherSnapshot, payload);
    uint32_t mask;
    size_t index;
    const uint8_t *written;

    if (snapshot == NULL ||
        snapshot_size < offsetof(XgRenderMoviePublisherSnapshot, payload) ||
        snapshot->version != XG_RENDER_MOVIE_PUBLISHER_SNAPSHOT_VERSION ||
        snapshot->assembly_generation == 0u ||
        snapshot->snapshot_size != (uint64_t)snapshot_size)
        return false;
    if (snapshot->active) {
        if (!description_valid(&snapshot->active_description) ||
            snapshot->active_byte_count !=
                (uint64_t)snapshot->active_description.byte_count ||
            snapshot->active_written_byte_count !=
                snapshot->active_byte_count ||
            !size_add(required_size,
                      snapshot->active_description.byte_count,
                      &required_size) ||
            !size_add(required_size,
                      snapshot->active_description.byte_count,
                      &required_size))
            return false;
        if (required_size > snapshot_size) return false;
        mask = expected_strip_mask(
            snapshot->active_description.expected_strips);
        if ((snapshot->completed_strip_mask & ~mask) != 0u) return false;
        written = snapshot->payload + snapshot->active_description.byte_count;
        for (index = 0u; index < snapshot->active_description.byte_count;
             ++index)
            if (written[index] > 1u) return false;
    } else if (snapshot->active_byte_count != 0u ||
               snapshot->active_written_byte_count != 0u ||
               snapshot->completed_strip_mask != 0u) {
        return false;
    }
    if (snapshot->complete_frame_available) {
        if (!description_valid(&snapshot->complete_description) ||
            snapshot->complete_byte_count !=
                (uint64_t)snapshot->complete_description.byte_count ||
            snapshot->complete_publication.surface.resource_id !=
                snapshot->complete_description.movie_id ||
            snapshot->complete_publication.owner_kind !=
                snapshot->complete_description.owner_kind ||
            snapshot->complete_publication.owner_receipt !=
                snapshot->complete_description.owner_receipt ||
            snapshot->complete_publication.owner_capability !=
                snapshot->complete_description.owner_capability ||
            snapshot->complete_publication.frame_generation == 0u ||
            snapshot->complete_publication.frame_generation !=
                snapshot->published_frame_generation ||
            snapshot->complete_publication.completed_strip_mask !=
                expected_strip_mask(
                    snapshot->complete_description.expected_strips) ||
            snapshot->complete_publication.width !=
                snapshot->complete_description.width ||
            snapshot->complete_publication.height !=
                snapshot->complete_description.height ||
            snapshot->complete_publication.depth24 !=
                snapshot->complete_description.depth24 ||
            !size_add(required_size,
                      snapshot->complete_description.byte_count,
                      &required_size))
            return false;
    } else if (snapshot->complete_byte_count != 0u ||
               snapshot->published_frame_generation != 0u) {
        return false;
    }
    if (snapshot->discontinuity_pending &&
        (snapshot->discontinuity_reason <=
             XG_RENDER_MOVIE_DISCONTINUITY_NONE ||
         snapshot->discontinuity_reason >
             XG_RENDER_MOVIE_DISCONTINUITY_RESTORE))
        return false;
    if (!snapshot->discontinuity_pending &&
        snapshot->discontinuity_reason !=
            XG_RENDER_MOVIE_DISCONTINUITY_NONE)
        return false;
    if (snapshot->diagnostics.frame_generation !=
            snapshot->published_frame_generation ||
        snapshot->diagnostics.completed_strip_mask !=
            snapshot->completed_strip_mask ||
        snapshot->diagnostics.frame_active != snapshot->active ||
        snapshot->diagnostics.complete_frame_available !=
            snapshot->complete_frame_available ||
        snapshot->diagnostics.discontinuity_pending !=
            snapshot->discontinuity_pending ||
        snapshot->diagnostics.discontinuity_reason !=
            (snapshot->discontinuity_pending
                ? snapshot->discontinuity_reason
                : XG_RENDER_MOVIE_DISCONTINUITY_NONE))
        return false;
    return required_size == snapshot_size ||
        snapshot_allocation_size(required_size) == snapshot_size;
}

static void free_publisher_storage(XgRenderMoviePublisher *publisher) {
    free(publisher->bytes);
    free(publisher->written);
    free(publisher->complete_bytes);
    publisher->bytes = NULL;
    publisher->written = NULL;
    publisher->complete_bytes = NULL;
}

XgRenderMovieResult xg_render_movie_publisher_transaction_rollback(
        const XgRenderMoviePublisherSnapshot *snapshot,
        size_t snapshot_size, XgRenderResourceHandle new_surface,
        XgRenderResourceHandle retained_old_surface,
        bool new_surface_owned) {
    XgRenderMoviePublisher restored = {0};
    const uint8_t *payload;
    XgRenderResourceResult resource_result;
    const bool had_complete = snapshot != NULL &&
        snapshot->complete_frame_available;

    if (!snapshot_valid(snapshot, snapshot_size) || snapshot->active ||
        new_surface.resource_id == 0u || new_surface.generation == 0u ||
        !g_publisher.complete_frame_available ||
        g_publisher.complete_publication.surface.resource_id !=
            new_surface.resource_id ||
        g_publisher.complete_publication.surface.generation !=
            new_surface.generation ||
        (had_complete &&
         (snapshot->complete_publication.surface.resource_id !=
              retained_old_surface.resource_id ||
          snapshot->complete_publication.surface.generation !=
              retained_old_surface.generation)) ||
        (!had_complete && (retained_old_surface.resource_id != 0u ||
                           retained_old_surface.generation != 0u)))
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;

    restored.generation = snapshot->assembly_generation;
    restored.published_frame_generation =
        snapshot->published_frame_generation;
    restored.description = snapshot->active_description;
    restored.complete_description = snapshot->complete_description;
    restored.complete_publication = snapshot->complete_publication;
    restored.completed_strip_mask = snapshot->completed_strip_mask;
    restored.diagnostics = snapshot->diagnostics;
    restored.active = snapshot->active;
    restored.complete_frame_available = snapshot->complete_frame_available;
    restored.discontinuity_pending = snapshot->discontinuity_pending;
    restored.discontinuity_reason = snapshot->discontinuity_reason;
    payload = snapshot->payload;
    if (restored.complete_frame_available) {
        restored.complete_bytes = (uint8_t *)malloc(
            restored.complete_description.byte_count);
        if (restored.complete_bytes == NULL)
            return XG_RENDER_MOVIE_OUT_OF_MEMORY;
        memcpy(restored.complete_bytes, payload,
               restored.complete_description.byte_count);
    }

    resource_result = had_complete
        ? xg_render_resource_rollback_current(
              new_surface, retained_old_surface)
        : new_surface_owned
            ? xg_render_resource_retire_current(new_surface)
            : XG_RENDER_RESOURCE_OK;
    if (resource_result != XG_RENDER_RESOURCE_OK) {
        free_publisher_storage(&restored);
        return XG_RENDER_MOVIE_RESOURCE_FAILED;
    }
    free_publisher_storage(&g_publisher);
    g_publisher = restored;
    update_diagnostic_state();
    return XG_RENDER_MOVIE_OK;
}

static XgRenderMovieResult movie_publisher_snapshot_prepare(
        const XgRenderMoviePublisherSnapshot *snapshot,
        size_t snapshot_size,
        bool authority_staged,
        XgRenderMovieCheckpointRestore **out_restore) {
    XgRenderMovieCheckpointRestore *restore;
    XgRenderMoviePublisher *restored;
    XgRenderResourceImport import;
    const uint8_t *payload;
    XgRenderMoviePublisherSnapshot *normalized;
    XgRenderResourceProvenance restored_provenance;
    const XgRenderResourceCapabilityCheckpoint empty_authority = {0};

    if (out_restore == NULL || snapshot == NULL ||
        snapshot_size < offsetof(XgRenderMoviePublisherSnapshot, payload) ||
        snapshot->version != XG_RENDER_MOVIE_PUBLISHER_SNAPSHOT_VERSION ||
        snapshot->snapshot_size != (uint64_t)snapshot_size)
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    normalized = (XgRenderMoviePublisherSnapshot *)malloc(
        snapshot_allocation_size(snapshot_size));
    if (normalized == NULL) return XG_RENDER_MOVIE_OUT_OF_MEMORY;
    memcpy(normalized, snapshot, snapshot_size);
    snapshot = normalized;
    if (!authority_staged && ((!snapshot->active &&
         memcmp(&snapshot->active_authority, &empty_authority,
                sizeof(empty_authority)) != 0) ||
        (!snapshot->complete_frame_available &&
         memcmp(&snapshot->complete_authority, &empty_authority,
                sizeof(empty_authority)) != 0) ||
        (snapshot->complete_frame_available &&
         snapshot->complete_publication.owner_capability !=
             snapshot->complete_description.owner_capability))) {
        free(normalized);
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    }
    if (!authority_staged && snapshot->active) {
        const XgRenderResourceProvenance checkpoint_provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = snapshot->active_description.owner_receipt,
            .capability = snapshot->active_description.owner_capability,
        };
        if (xg_render_resource_capability_checkpoint_restore_stage(
                &snapshot->active_authority, &checkpoint_provenance,
                XG_RENDER_RESOURCE_OWNER_SOURCE,
                snapshot->active_description.owner_generation,
                snapshot->active_description.owner_generation,
                &restored_provenance) != XG_RENDER_RESOURCE_CAPABILITY_OK) {
            free(normalized);
            return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
        }
        normalized->active_description.owner_capability =
            restored_provenance.capability;
    }
    if (!authority_staged && snapshot->complete_frame_available) {
        const XgRenderResourceProvenance checkpoint_provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = snapshot->complete_description.owner_receipt,
            .capability = snapshot->complete_description.owner_capability,
        };
        if (xg_render_resource_capability_checkpoint_restore_stage(
                &snapshot->complete_authority, &checkpoint_provenance,
                XG_RENDER_RESOURCE_OWNER_SOURCE,
                snapshot->complete_description.owner_generation,
                snapshot->complete_description.owner_generation,
                &restored_provenance) != XG_RENDER_RESOURCE_CAPABILITY_OK) {
            free(normalized);
            return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
        }
        normalized->complete_description.owner_capability =
            restored_provenance.capability;
        normalized->complete_publication.owner_capability =
            restored_provenance.capability;
    }
    if ((snapshot->active &&
         xg_render_resource_capability_restore_stage(
             (XgRenderResourceProvenance){
                 .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                 .receipt = snapshot->active_description.owner_receipt,
                 .capability = snapshot->active_description.owner_capability,
             }, XG_RENDER_RESOURCE_OWNER_SOURCE,
             snapshot->active_description.owner_generation) !=
                 XG_RENDER_RESOURCE_CAPABILITY_OK) ||
        (snapshot->complete_frame_available &&
         xg_render_resource_capability_restore_stage(
             (XgRenderResourceProvenance){
                 .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                 .receipt = snapshot->complete_description.owner_receipt,
                 .capability = snapshot->complete_description.owner_capability,
             }, XG_RENDER_RESOURCE_OWNER_SOURCE,
             snapshot->complete_description.owner_generation) !=
                 XG_RENDER_RESOURCE_CAPABILITY_OK))
        {
            free(normalized);
            return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
        }
    if (!snapshot_valid(snapshot, snapshot_size)) {
        free(normalized);
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    }
    restore = (XgRenderMovieCheckpointRestore *)calloc(1u, sizeof(*restore));
    if (restore == NULL) {
        free(normalized);
        return XG_RENDER_MOVIE_OUT_OF_MEMORY;
    }
    restored = &restore->publisher;
    payload = snapshot->payload;
    restored->generation = g_publisher.generation + 1u;
    if (restored->generation == 0u) restored->generation = 1u;
    restored->published_frame_generation =
        snapshot->published_frame_generation;
    restored->description = snapshot->active_description;
    restored->complete_description = snapshot->complete_description;
    restored->complete_publication = snapshot->complete_publication;
    restored->completed_strip_mask = snapshot->completed_strip_mask;
    restored->diagnostics = snapshot->diagnostics;
    restored->active = snapshot->active;
    restored->complete_frame_available = snapshot->complete_frame_available;
    if (restored->active) {
        restored->bytes = (uint8_t *)malloc(restored->description.byte_count);
        restored->written = (uint8_t *)malloc(restored->description.byte_count);
        if (restored->bytes == NULL || restored->written == NULL) {
            free(restored->bytes);
            free(restored->written);
            free(restore);
            free(normalized);
            return XG_RENDER_MOVIE_OUT_OF_MEMORY;
        }
        memcpy(restored->bytes, payload, restored->description.byte_count);
        payload += restored->description.byte_count;
        memcpy(restored->written, payload, restored->description.byte_count);
        payload += restored->description.byte_count;
    }
    if (restored->complete_frame_available) {
        restored->complete_bytes =
            (uint8_t *)malloc(restored->complete_description.byte_count);
        if (restored->complete_bytes == NULL) {
            free(restored->bytes);
            free(restored->written);
            free(restore);
            free(normalized);
            return XG_RENDER_MOVIE_OUT_OF_MEMORY;
        }
        memcpy(restored->complete_bytes, payload,
               restored->complete_description.byte_count);
        import = (XgRenderResourceImport){
            .resource_id = restored->complete_description.movie_id,
            .kind = XG_RENDER_RESOURCE_MOVIE_FRAME,
            .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
            .owner_generation = restored->complete_description.owner_generation,
            .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
            .provenance = {
                .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                .receipt = restored->complete_description.owner_receipt,
                .capability = restored->complete_description.owner_capability,
                .synthetic = false,
            },
            .content_digest = xg_render_resource_digest(
                restored->complete_bytes,
                restored->complete_description.byte_count),
            .bytes = restored->complete_bytes,
            .byte_count = restored->complete_description.byte_count,
            .identity = movie_identity(restored->complete_description.movie_id),
            .descriptor = movie_descriptor(
                &restored->complete_description),
        };
        if (xg_render_resource_restore_stage(
                &import, &restored->complete_publication.surface) !=
            XG_RENDER_RESOURCE_OK) {
            free(restored->bytes);
            free(restored->written);
            free(restored->complete_bytes);
            free(restore);
            free(normalized);
            return XG_RENDER_MOVIE_RESOURCE_FAILED;
        }
    }
    restored->discontinuity_pending = true;
    restored->discontinuity_reason = XG_RENDER_MOVIE_DISCONTINUITY_RESTORE;
    restored->diagnostics.discontinuity_pending = true;
    restored->diagnostics.discontinuity_reason =
        XG_RENDER_MOVIE_DISCONTINUITY_RESTORE;
    if (restored->diagnostics.discontinuities != UINT64_MAX)
        restored->diagnostics.discontinuities++;
    *out_restore = restore;
    free(normalized);
    return XG_RENDER_MOVIE_OK;
}

void xg_render_movie_publisher_wire_commit(
        XgRenderMovieCheckpointRestore *restore,
        XgRenderMovieFrameHandle *out_active_frame) {
    if (restore == NULL) return;
    if (restore->publisher.complete_frame_available)
        (void)xg_render_resource_restore_commit(
            restore->publisher.complete_publication.surface);
    free(g_publisher.bytes);
    free(g_publisher.written);
    free(g_publisher.complete_bytes);
    g_publisher = restore->publisher;
    free(restore);
    update_diagnostic_state();
    if (out_active_frame != NULL) {
        out_active_frame->generation = g_publisher.active
            ? g_publisher.generation : 0u;
    }
}

void xg_render_movie_publisher_wire_cancel(
        XgRenderMovieCheckpointRestore *restore) {
    if (restore == NULL) return;
    if (restore->publisher.complete_frame_available)
        xg_render_resource_restore_cancel(
            restore->publisher.complete_publication.surface);
    free(restore->publisher.bytes);
    free(restore->publisher.written);
    free(restore->publisher.complete_bytes);
    free(restore);
}

XgRenderMovieResult xg_render_movie_publisher_restore(
        const XgRenderMoviePublisherSnapshot *snapshot,
        size_t snapshot_size,
        XgRenderMovieFrameHandle *out_active_frame) {
    XgRenderMovieCheckpointRestore *restore;
    XgRenderMovieResult result;

    if (!xg_render_resource_capability_restore_begin())
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    result = movie_publisher_snapshot_prepare(
        snapshot, snapshot_size, false, &restore);
    if (result == XG_RENDER_MOVIE_OK) {
        xg_render_resource_capability_restore_commit();
        xg_render_movie_publisher_wire_commit(restore, out_active_frame);
    } else {
        xg_render_resource_capability_restore_cancel();
    }
    return result;
}

typedef struct XgRenderMovieWireWriter {
    uint8_t *cursor;
    uint8_t *end;
    bool valid;
} XgRenderMovieWireWriter;

typedef struct XgRenderMovieWireReader {
    const uint8_t *cursor;
    const uint8_t *end;
    bool valid;
} XgRenderMovieWireReader;

static void wire_write_u32(XgRenderMovieWireWriter *writer, uint32_t value) {
    if (!writer->valid || (size_t)(writer->end - writer->cursor) < 4u) {
        writer->valid = false;
        return;
    }
    for (uint32_t index = 0u; index < 4u; ++index)
        writer->cursor[index] = (uint8_t)(value >> (index * 8u));
    writer->cursor += 4u;
}

static void wire_write_u64(XgRenderMovieWireWriter *writer, uint64_t value) {
    if (!writer->valid || (size_t)(writer->end - writer->cursor) < 8u) {
        writer->valid = false;
        return;
    }
    for (uint32_t index = 0u; index < 8u; ++index)
        writer->cursor[index] = (uint8_t)(value >> (index * 8u));
    writer->cursor += 8u;
}

static uint32_t wire_read_u32(XgRenderMovieWireReader *reader) {
    uint32_t value = 0u;
    if (!reader->valid || (size_t)(reader->end - reader->cursor) < 4u) {
        reader->valid = false;
        return 0u;
    }
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)reader->cursor[index] << (index * 8u);
    reader->cursor += 4u;
    return value;
}

static uint64_t wire_read_u64(XgRenderMovieWireReader *reader) {
    uint64_t value = 0u;
    if (!reader->valid || (size_t)(reader->end - reader->cursor) < 8u) {
        reader->valid = false;
        return 0u;
    }
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)reader->cursor[index] << (index * 8u);
    reader->cursor += 8u;
    return value;
}

static void wire_write_description(
        XgRenderMovieWireWriter *writer,
        const XgRenderMovieFrameDescription *description) {
    wire_write_u64(writer, description->movie_id);
    wire_write_u32(writer, (uint32_t)description->owner_kind);
    wire_write_u64(writer, description->owner_receipt);
    wire_write_u64(writer, description->owner_capability);
    wire_write_u64(writer, description->owner_generation);
    wire_write_u64(writer, description->guest_cycle);
    wire_write_u32(writer, description->width);
    wire_write_u32(writer, description->height);
    wire_write_u32(writer, description->expected_strips);
    wire_write_u64(writer, description->byte_count);
    wire_write_u32(writer, (description->depth24 ? 1u : 0u) |
                           (description->odd_field ? 2u : 0u));
}

static void wire_read_description(
        XgRenderMovieWireReader *reader,
        XgRenderMovieFrameDescription *description) {
    uint64_t byte_count;
    uint32_t flags;

    description->movie_id = wire_read_u64(reader);
    description->owner_kind =
        (XgRenderMovieOwnerKind)wire_read_u32(reader);
    description->owner_receipt = wire_read_u64(reader);
    description->owner_capability = wire_read_u64(reader);
    description->owner_generation = wire_read_u64(reader);
    description->guest_cycle = wire_read_u64(reader);
    description->width = wire_read_u32(reader);
    description->height = wire_read_u32(reader);
    description->expected_strips = wire_read_u32(reader);
    byte_count = wire_read_u64(reader);
    flags = wire_read_u32(reader);
    if (byte_count > SIZE_MAX || (flags & ~3u) != 0u) {
        reader->valid = false;
        return;
    }
    description->byte_count = (size_t)byte_count;
    description->depth24 = (flags & 1u) != 0u;
    description->odd_field = (flags & 2u) != 0u;
}

XgRenderMovieResult xg_render_movie_publisher_wire_size(size_t *out_size) {
    size_t size = XG_RENDER_MOVIE_WIRE_FIXED_SIZE;

    if (out_size == NULL) return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    if (g_publisher.active &&
        (!size_add(size, g_publisher.description.byte_count, &size) ||
         !size_add(size, g_publisher.description.byte_count, &size)))
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    if (g_publisher.complete_frame_available &&
        !size_add(size, g_publisher.complete_description.byte_count, &size))
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    *out_size = size;
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_publisher_wire_write(
        void *out_wire, size_t wire_capacity) {
    XgRenderMovieWireWriter writer;
    size_t required_size;
    uint32_t flags;
    uint32_t publication_flags;
    uint64_t active_digest = 0u;
    uint64_t written_digest = 0u;
    uint64_t complete_digest = 0u;
    XgRenderResourceCapabilityCheckpoint active_authority = {0};
    XgRenderResourceCapabilityCheckpoint complete_authority = {0};

    if (out_wire == NULL ||
        xg_render_movie_publisher_wire_size(&required_size) !=
            XG_RENDER_MOVIE_OK || wire_capacity != required_size)
        return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    writer = (XgRenderMovieWireWriter){
        .cursor = (uint8_t *)out_wire,
        .end = (uint8_t *)out_wire + wire_capacity,
        .valid = true,
    };
    flags = (g_publisher.active ? 1u : 0u) |
        (g_publisher.complete_frame_available ? 2u : 0u) |
        (g_publisher.discontinuity_pending ? 4u : 0u);
    publication_flags = (g_publisher.complete_publication.depth24 ? 1u : 0u) |
        (g_publisher.complete_publication.odd_field ? 2u : 0u) |
        (g_publisher.complete_publication.held ? 4u : 0u) |
        (g_publisher.complete_publication.discontinuity ? 8u : 0u);
    if (g_publisher.active) {
        active_digest = xg_render_resource_digest(
            g_publisher.bytes, g_publisher.description.byte_count);
        written_digest = xg_render_resource_digest(
            g_publisher.written, g_publisher.description.byte_count);
    }
    if (g_publisher.complete_frame_available)
        complete_digest = xg_render_resource_digest(
            g_publisher.complete_bytes,
            g_publisher.complete_description.byte_count);
    if ((g_publisher.active &&
         xg_render_resource_capability_checkpoint(
             &(XgRenderResourceProvenance){
                 .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                 .receipt = g_publisher.description.owner_receipt,
                 .capability = g_publisher.description.owner_capability,
             }, XG_RENDER_RESOURCE_OWNER_SOURCE,
             g_publisher.description.owner_generation,
             &active_authority) != XG_RENDER_RESOURCE_CAPABILITY_OK) ||
        (g_publisher.complete_frame_available &&
         xg_render_resource_capability_checkpoint(
             &(XgRenderResourceProvenance){
                 .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                 .receipt = g_publisher.complete_description.owner_receipt,
                 .capability = g_publisher.complete_description.owner_capability,
             }, XG_RENDER_RESOURCE_OWNER_SOURCE,
             g_publisher.complete_description.owner_generation,
             &complete_authority) != XG_RENDER_RESOURCE_CAPABILITY_OK))
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;

    wire_write_u32(&writer, XG_RENDER_MOVIE_WIRE_MAGIC);
    wire_write_u32(&writer, XG_RENDER_MOVIE_PUBLISHER_WIRE_VERSION);
    wire_write_u64(&writer, required_size);
    wire_write_u32(&writer, g_publisher.generation);
    wire_write_u32(&writer, flags);
    wire_write_u64(&writer, g_publisher.published_frame_generation);
    wire_write_u32(&writer, g_publisher.completed_strip_mask);
    wire_write_u32(&writer, (uint32_t)g_publisher.discontinuity_reason);
    wire_write_description(&writer, &g_publisher.description);
    wire_write_description(&writer, &g_publisher.complete_description);
    wire_write_u32(&writer,
        (uint32_t)g_publisher.complete_publication.owner_kind);
    wire_write_u64(&writer, g_publisher.complete_publication.owner_receipt);
    wire_write_u64(&writer, g_publisher.complete_publication.owner_capability);
    wire_write_u64(&writer, g_publisher.complete_publication.frame_generation);
    wire_write_u64(&writer, g_publisher.complete_publication.guest_cycle);
    wire_write_u32(&writer, g_publisher.complete_publication.completed_strip_mask);
    wire_write_u32(&writer, g_publisher.complete_publication.width);
    wire_write_u32(&writer, g_publisher.complete_publication.height);
    wire_write_u32(&writer, publication_flags);
    wire_write_u32(&writer,
        (uint32_t)g_publisher.complete_publication.discontinuity_reason);
    wire_write_u64(&writer, g_publisher.active
        ? g_publisher.description.byte_count : 0u);
    wire_write_u64(&writer, g_publisher.complete_frame_available
        ? g_publisher.complete_description.byte_count : 0u);
    wire_write_u64(&writer, active_digest);
    wire_write_u64(&writer, written_digest);
    wire_write_u64(&writer, complete_digest);
    wire_write_u64(&writer, g_publisher.diagnostics.complete_frames);
    wire_write_u64(&writer, g_publisher.diagnostics.frame_generation);
    wire_write_u64(&writer,
                   g_publisher.diagnostics.partial_publish_attempts);
    wire_write_u64(&writer, g_publisher.diagnostics.cancelled_frames);
    wire_write_u64(&writer, g_publisher.diagnostics.held_frames);
    wire_write_u64(&writer, g_publisher.diagnostics.discontinuities);
    wire_write_u32(&writer, g_publisher.diagnostics.completed_strip_mask);
    wire_write_u32(&writer,
        (g_publisher.diagnostics.frame_active ? 1u : 0u) |
        (g_publisher.diagnostics.complete_frame_available ? 2u : 0u) |
        (g_publisher.diagnostics.discontinuity_pending ? 4u : 0u));
    wire_write_u32(&writer,
        (uint32_t)g_publisher.diagnostics.discontinuity_reason);
    memcpy(writer.cursor, active_authority.bytes, sizeof(active_authority.bytes));
    writer.cursor += sizeof(active_authority.bytes);
    memcpy(writer.cursor, complete_authority.bytes,
           sizeof(complete_authority.bytes));
    writer.cursor += sizeof(complete_authority.bytes);
    if (!writer.valid) return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    if (g_publisher.active) {
        memcpy(writer.cursor, g_publisher.bytes,
               g_publisher.description.byte_count);
        writer.cursor += g_publisher.description.byte_count;
        memcpy(writer.cursor, g_publisher.written,
               g_publisher.description.byte_count);
        writer.cursor += g_publisher.description.byte_count;
    }
    if (g_publisher.complete_frame_available) {
        memcpy(writer.cursor, g_publisher.complete_bytes,
               g_publisher.complete_description.byte_count);
        writer.cursor += g_publisher.complete_description.byte_count;
    }
    return writer.cursor == writer.end
        ? XG_RENDER_MOVIE_OK : XG_RENDER_MOVIE_INVALID_SNAPSHOT;
}

static XgRenderMovieResult movie_publisher_wire_process(
        const void *wire, size_t wire_size, uint64_t restored_owner_generation,
        XgRenderMovieCheckpointRestore **out_restore) {
    XgRenderMovieWireReader reader;
    XgRenderMoviePublisherSnapshot *snapshot;
    size_t snapshot_size;
    uint64_t declared_size;
    uint64_t active_byte_count;
    uint64_t complete_byte_count;
    uint64_t active_digest;
    uint64_t written_digest;
    uint64_t complete_digest;
    uint32_t flags;
    uint32_t publication_flags;
    uint32_t publication_reason;
    uint32_t diagnostic_flags;
    uint32_t diagnostic_reason;
    uint64_t active_original_owner_generation;
    uint64_t complete_original_owner_generation;
    XgRenderResourceCapabilityCheckpoint active_authority;
    XgRenderResourceCapabilityCheckpoint complete_authority;
    XgRenderMovieResult result;
    XgRenderMoviePublisherSnapshot *expanded;

    if (wire == NULL || wire_size < XG_RENDER_MOVIE_WIRE_FIXED_SIZE ||
        restored_owner_generation == 0u)
        return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    reader = (XgRenderMovieWireReader){
        .cursor = (const uint8_t *)wire,
        .end = (const uint8_t *)wire + wire_size,
        .valid = true,
    };
    if (wire_read_u32(&reader) != XG_RENDER_MOVIE_WIRE_MAGIC ||
        wire_read_u32(&reader) != XG_RENDER_MOVIE_PUBLISHER_WIRE_VERSION)
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    declared_size = wire_read_u64(&reader);
    if (declared_size != wire_size)
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    snapshot_size = offsetof(XgRenderMoviePublisherSnapshot, payload);
    snapshot = (XgRenderMoviePublisherSnapshot *)calloc(1u, sizeof(*snapshot));
    if (snapshot == NULL) return XG_RENDER_MOVIE_OUT_OF_MEMORY;
    snapshot->version = XG_RENDER_MOVIE_PUBLISHER_SNAPSHOT_VERSION;
    snapshot->assembly_generation = wire_read_u32(&reader);
    flags = wire_read_u32(&reader);
    snapshot->published_frame_generation = wire_read_u64(&reader);
    snapshot->completed_strip_mask = wire_read_u32(&reader);
    snapshot->discontinuity_reason =
        (XgRenderMovieDiscontinuity)wire_read_u32(&reader);
    wire_read_description(&reader, &snapshot->active_description);
    wire_read_description(&reader, &snapshot->complete_description);
    snapshot->complete_publication.owner_kind =
        (XgRenderMovieOwnerKind)wire_read_u32(&reader);
    snapshot->complete_publication.owner_receipt = wire_read_u64(&reader);
    snapshot->complete_publication.owner_capability = wire_read_u64(&reader);
    snapshot->complete_publication.frame_generation = wire_read_u64(&reader);
    snapshot->complete_publication.guest_cycle = wire_read_u64(&reader);
    snapshot->complete_publication.completed_strip_mask = wire_read_u32(&reader);
    snapshot->complete_publication.width = wire_read_u32(&reader);
    snapshot->complete_publication.height = wire_read_u32(&reader);
    publication_flags = wire_read_u32(&reader);
    publication_reason = wire_read_u32(&reader);
    active_byte_count = wire_read_u64(&reader);
    complete_byte_count = wire_read_u64(&reader);
    active_digest = wire_read_u64(&reader);
    written_digest = wire_read_u64(&reader);
    complete_digest = wire_read_u64(&reader);
    snapshot->diagnostics.complete_frames = wire_read_u64(&reader);
    snapshot->diagnostics.frame_generation = wire_read_u64(&reader);
    snapshot->diagnostics.partial_publish_attempts = wire_read_u64(&reader);
    snapshot->diagnostics.cancelled_frames = wire_read_u64(&reader);
    snapshot->diagnostics.held_frames = wire_read_u64(&reader);
    snapshot->diagnostics.discontinuities = wire_read_u64(&reader);
    snapshot->diagnostics.completed_strip_mask = wire_read_u32(&reader);
    diagnostic_flags = wire_read_u32(&reader);
    diagnostic_reason = wire_read_u32(&reader);
    if (!reader.valid || (size_t)(reader.end - reader.cursor) <
            sizeof(active_authority) + sizeof(complete_authority)) {
        free(snapshot);
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    }
    memcpy(active_authority.bytes, reader.cursor,
           sizeof(active_authority.bytes));
    reader.cursor += sizeof(active_authority.bytes);
    memcpy(complete_authority.bytes, reader.cursor,
           sizeof(complete_authority.bytes));
    reader.cursor += sizeof(complete_authority.bytes);
    if (!reader.valid || (flags & ~7u) != 0u ||
        (publication_flags & ~15u) != 0u || active_byte_count > SIZE_MAX ||
        complete_byte_count > SIZE_MAX ||
        (diagnostic_flags & ~7u) != 0u ||
        diagnostic_reason > XG_RENDER_MOVIE_DISCONTINUITY_RESTORE ||
        (((diagnostic_flags & 4u) == 0u) !=
         (diagnostic_reason == XG_RENDER_MOVIE_DISCONTINUITY_NONE)) ||
        publication_reason > XG_RENDER_MOVIE_DISCONTINUITY_RESTORE ||
        (((publication_flags & 8u) == 0u) !=
         (publication_reason == XG_RENDER_MOVIE_DISCONTINUITY_NONE)) ||
        (active_byte_count > SIZE_MAX - snapshot_size) ||
        (flags & 1u && active_byte_count >
            (SIZE_MAX - snapshot_size) / 2u)) {
        free(snapshot);
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    }
    if ((flags & 1u) != 0u) {
        snapshot_size += (size_t)active_byte_count * 2u;
    } else if (active_byte_count != 0u || active_digest != 0u ||
               written_digest != 0u) {
        free(snapshot);
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    }
    if ((flags & 2u) != 0u) {
        if (complete_byte_count > SIZE_MAX - snapshot_size) {
            free(snapshot);
            return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
        }
        snapshot_size += (size_t)complete_byte_count;
    } else if (complete_byte_count != 0u || complete_digest != 0u) {
        free(snapshot);
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    }
    if ((size_t)(reader.end - reader.cursor) !=
            snapshot_size - offsetof(XgRenderMoviePublisherSnapshot, payload)) {
        free(snapshot);
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    }
    expanded = (XgRenderMoviePublisherSnapshot *)realloc(
        snapshot, snapshot_allocation_size(snapshot_size));
    if (expanded == NULL) {
        free(snapshot);
        return XG_RENDER_MOVIE_OUT_OF_MEMORY;
    }
    snapshot = expanded;
    snapshot->snapshot_size = snapshot_size;
    snapshot->active = (flags & 1u) != 0u;
    snapshot->complete_frame_available = (flags & 2u) != 0u;
    snapshot->discontinuity_pending = (flags & 4u) != 0u;
    active_original_owner_generation =
        snapshot->active_description.owner_generation;
    complete_original_owner_generation =
        snapshot->complete_description.owner_generation;
    snapshot->active_description.owner_generation = restored_owner_generation;
    snapshot->complete_description.owner_generation = restored_owner_generation;
    snapshot->complete_publication.surface.resource_id =
        snapshot->complete_description.movie_id;
    snapshot->complete_publication.depth24 = (publication_flags & 1u) != 0u;
    snapshot->complete_publication.odd_field = (publication_flags & 2u) != 0u;
    snapshot->complete_publication.held = (publication_flags & 4u) != 0u;
    snapshot->complete_publication.discontinuity =
        (publication_flags & 8u) != 0u;
    snapshot->complete_publication.discontinuity_reason =
        (XgRenderMovieDiscontinuity)publication_reason;
    snapshot->diagnostics.frame_active = (diagnostic_flags & 1u) != 0u;
    snapshot->diagnostics.complete_frame_available =
        (diagnostic_flags & 2u) != 0u;
    snapshot->diagnostics.discontinuity_pending =
        (diagnostic_flags & 4u) != 0u;
    snapshot->diagnostics.discontinuity_reason =
        (XgRenderMovieDiscontinuity)diagnostic_reason;
    snapshot->active_byte_count = active_byte_count;
    snapshot->active_written_byte_count = active_byte_count;
    snapshot->complete_byte_count = complete_byte_count;
    memcpy(snapshot->payload, reader.cursor,
           snapshot_size - offsetof(XgRenderMoviePublisherSnapshot, payload));
    if ((snapshot->active &&
         (xg_render_resource_digest(snapshot->payload,
                                    (size_t)active_byte_count) != active_digest ||
          xg_render_resource_digest(snapshot->payload + active_byte_count,
                                    (size_t)active_byte_count) !=
              written_digest)) ||
        (snapshot->complete_frame_available &&
         xg_render_resource_digest(
             snapshot->payload + active_byte_count * 2u,
             (size_t)complete_byte_count) != complete_digest)) {
        free(snapshot);
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    }
    {
        const XgRenderResourceCapabilityCheckpoint empty_authority = {0};
        const bool local_restore = out_restore == NULL;
        XgRenderResourceProvenance restored_provenance;
        bool staged = !local_restore ||
            xg_render_resource_capability_restore_begin();

        if (snapshot->complete_frame_available &&
            snapshot->complete_publication.owner_capability !=
                snapshot->complete_description.owner_capability)
            staged = false;

        if (staged && snapshot->active) {
            const XgRenderResourceProvenance checkpoint_provenance = {
                .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                .receipt = snapshot->active_description.owner_receipt,
                .capability = snapshot->active_description.owner_capability,
            };
            staged = xg_render_resource_capability_checkpoint_restore_stage(
                &active_authority, &checkpoint_provenance,
                XG_RENDER_RESOURCE_OWNER_SOURCE,
                active_original_owner_generation, restored_owner_generation,
                &restored_provenance) == XG_RENDER_RESOURCE_CAPABILITY_OK;
            if (staged)
                snapshot->active_description.owner_capability =
                    restored_provenance.capability;
        } else if (staged && memcmp(&active_authority, &empty_authority,
                                    sizeof(empty_authority)) != 0) {
            staged = false;
        }
        if (staged && snapshot->complete_frame_available) {
            const XgRenderResourceProvenance checkpoint_provenance = {
                .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                .receipt = snapshot->complete_description.owner_receipt,
                .capability = snapshot->complete_description.owner_capability,
            };
            staged = xg_render_resource_capability_checkpoint_restore_stage(
                &complete_authority, &checkpoint_provenance,
                XG_RENDER_RESOURCE_OWNER_SOURCE,
                complete_original_owner_generation, restored_owner_generation,
                &restored_provenance) == XG_RENDER_RESOURCE_CAPABILITY_OK;
            if (staged) {
                snapshot->complete_description.owner_capability =
                    restored_provenance.capability;
                snapshot->complete_publication.owner_capability =
                    restored_provenance.capability;
            }
        } else if (staged && memcmp(&complete_authority, &empty_authority,
                                    sizeof(empty_authority)) != 0) {
            staged = false;
        }
        if (!staged) {
            if (local_restore)
                xg_render_resource_capability_restore_cancel();
            free(snapshot);
            return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
        }
        if (out_restore != NULL) {
        result = movie_publisher_snapshot_prepare(
            snapshot, snapshot_size, true, out_restore);
        } else {
            result = snapshot_valid(snapshot, snapshot_size)
            ? XG_RENDER_MOVIE_OK : XG_RENDER_MOVIE_INVALID_SNAPSHOT;
            xg_render_resource_capability_restore_cancel();
        }
    }
    free(snapshot);
    return result;
}

XgRenderMovieResult xg_render_movie_publisher_wire_validate(
        const void *wire, size_t wire_size,
        uint64_t restored_owner_generation) {
    return movie_publisher_wire_process(
        wire, wire_size, restored_owner_generation, NULL);
}

XgRenderMovieResult xg_render_movie_publisher_wire_prepare(
        const void *wire, size_t wire_size, uint64_t restored_owner_generation,
        XgRenderMovieCheckpointRestore **out_restore) {
    if (out_restore == NULL) return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    *out_restore = NULL;
    return movie_publisher_wire_process(
        wire, wire_size, restored_owner_generation, out_restore);
}

XgRenderMovieResult xg_render_movie_publisher_checkpoint_resource(
        const XgRenderMovieCheckpointRestore *restore,
        XgRenderResourceHandle *out_resource) {
    if (restore == NULL || out_resource == NULL)
        return XG_RENDER_MOVIE_INVALID_ARGUMENT;
    *out_resource = (XgRenderResourceHandle){0};
    if (!restore->publisher.complete_frame_available)
        return XG_RENDER_MOVIE_NO_COMPLETE_FRAME;
    *out_resource = restore->publisher.complete_publication.surface;
    return XG_RENDER_MOVIE_OK;
}

XgRenderMovieResult xg_render_movie_publisher_wire_restore(
        const void *wire, size_t wire_size, uint64_t restored_owner_generation,
        XgRenderMovieFrameHandle *out_active_frame) {
    XgRenderMovieCheckpointRestore *restore;
    XgRenderMovieResult result;

    if (!xg_render_resource_capability_restore_begin())
        return XG_RENDER_MOVIE_INVALID_SNAPSHOT;
    result = xg_render_movie_publisher_wire_prepare(
        wire, wire_size, restored_owner_generation, &restore);
    if (result == XG_RENDER_MOVIE_OK) {
        xg_render_resource_capability_restore_commit();
        xg_render_movie_publisher_wire_commit(restore, out_active_frame);
    } else {
        xg_render_resource_capability_restore_cancel();
    }
    return result;
}

void xg_render_movie_publisher_diagnostics(
        XgRenderMovieDiagnostics *out_diagnostics) {
    if (out_diagnostics != NULL) *out_diagnostics = g_publisher.diagnostics;
}
