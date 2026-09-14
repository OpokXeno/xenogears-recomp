#include "xg_render_model_resources.h"
#include "xg_render_array.h"

#include <stdbool.h>
#include <string.h>

typedef struct XgRenderModelPendingPublication {
    XgRenderModelPublication publication;
    XgRenderModelAuthenticatedSourceReceipt source;
    const void *source_bytes;
    size_t source_byte_count;
    uint64_t payload_content_digest;
    bool occupied;
} XgRenderModelPendingPublication;

typedef struct XgRenderModelPublishedMetadata {
    XgRenderResourceHandle handle;
    XgRenderModelAuthenticatedSourceReceipt source;
    bool occupied;
} XgRenderModelPublishedMetadata;

static XgRenderModelPendingPublication *g_pending;
static XgRenderModelPublishedMetadata *g_published;
static uint32_t g_pending_capacity, g_published_capacity;
static uint64_t g_next_transaction = 1u;

static bool identity_present(const XgRenderResourceIdentity *identity) {
    for (size_t index = 0u; index < sizeof(identity->bytes); ++index)
        if (identity->bytes[index] != 0u) return true;
    return false;
}

static bool receipt_valid(
        const XgRenderModelAuthenticatedSourceReceipt *source) {
    return source != NULL && source->receipt != 0u &&
        source->capability != 0u &&
        (source->source_kind == XG_RENDER_MODEL_SOURCE_ARTIFACT ||
         source->source_kind == XG_RENDER_MODEL_SOURCE_RECEIPT) &&
        identity_present(&source->source_identity) &&
        source->range_size != 0u && source->range_content_digest != 0u &&
        source->range_offset <= UINT64_MAX - source->range_size;
}

static uint64_t identity_hash(const uint8_t *bytes, size_t byte_count,
                              uint64_t seed) {
    uint64_t hash = seed;

    for (size_t index = 0u; index < byte_count; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void encode_u64(uint8_t bytes[8], uint64_t value) {
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

XgRenderModelResourceResult xg_render_model_resource_identity(
        const XgRenderModelAuthenticatedSourceReceipt *source,
        XgRenderResourceIdentity *out_identity) {
    static const uint8_t domain[] = "xg-model-source-range-v1";
    static const uint64_t seeds[4] = {
        UINT64_C(1469598103934665603), UINT64_C(1099511628211),
        UINT64_C(7809847782465536322), UINT64_C(9650029242287828579),
    };
    uint8_t encoded[sizeof(domain) - 1u +
        XG_RENDER_RESOURCE_IDENTITY_SIZE + 16u];
    size_t offset = 0u;

    if (!receipt_valid(source) || out_identity == NULL)
        return XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT;
    memcpy(encoded + offset, domain, sizeof(domain) - 1u);
    offset += sizeof(domain) - 1u;
    memcpy(encoded + offset, source->source_identity.bytes,
           sizeof(source->source_identity.bytes));
    offset += sizeof(source->source_identity.bytes);
    encode_u64(encoded + offset, source->range_offset);
    offset += 8u;
    encode_u64(encoded + offset, source->range_size);
    offset += 8u;
    for (uint32_t word = 0u; word < 4u; ++word) {
        const uint64_t hash = identity_hash(encoded, offset, seeds[word]);
        encode_u64(out_identity->bytes + word * 8u, hash);
    }
    if (xg_render_resource_identity_id(out_identity) == 0u)
        return XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT;
    return XG_RENDER_MODEL_RESOURCE_OK;
}

static XgRenderModelResourceResult map_repository_result(
        XgRenderResourceResult result) {
    switch (result) {
    case XG_RENDER_RESOURCE_OK:
        return XG_RENDER_MODEL_RESOURCE_OK;
    case XG_RENDER_RESOURCE_INVALID_ARGUMENT:
        return XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT;
    case XG_RENDER_RESOURCE_CAPACITY_EXCEEDED:
    case XG_RENDER_RESOURCE_OUT_OF_MEMORY:
        return XG_RENDER_MODEL_RESOURCE_CAPACITY_EXCEEDED;
    case XG_RENDER_RESOURCE_NOT_FOUND:
    case XG_RENDER_RESOURCE_STALE:
        return XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION;
    case XG_RENDER_RESOURCE_INVALID_STATE:
        return XG_RENDER_MODEL_RESOURCE_INVALID_STATE;
    case XG_RENDER_RESOURCE_DIGEST_MISMATCH:
        return XG_RENDER_MODEL_RESOURCE_PAYLOAD_DIGEST_MISMATCH;
    default:
        return XG_RENDER_MODEL_RESOURCE_REPOSITORY_FAILED;
    }
}

static XgRenderResourceProvenanceKind provenance_kind(
        XgRenderModelSourceKind source_kind) {
    return source_kind == XG_RENDER_MODEL_SOURCE_ARTIFACT
        ? XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT
        : XG_RENDER_RESOURCE_PROVENANCE_SOURCE;
}

static bool handles_equal(XgRenderResourceHandle left,
                          XgRenderResourceHandle right) {
    return left.resource_id == right.resource_id &&
        left.generation == right.generation;
}

static XgRenderModelPendingPublication *find_pending(
        XgRenderModelPublication publication) {
    for (size_t index = 0u; index < g_pending_capacity;
         ++index) {
        XgRenderModelPendingPublication *pending = &g_pending[index];

        if (pending->occupied &&
            pending->publication.transaction == publication.transaction &&
            handles_equal(pending->publication.resource,
                          publication.resource))
            return pending;
    }
    return NULL;
}

static XgRenderModelPendingPublication *available_pending(void) {
    for (size_t index = 0u; index < g_pending_capacity;
         ++index)
        if (!g_pending[index].occupied) return &g_pending[index];
    const uint32_t next = g_pending_capacity;
    if (next == UINT32_MAX) return NULL;
    XgRenderModelPendingPublication *grown = xg_render_array_reserve(g_pending,
        sizeof(*grown), &g_pending_capacity, next + 1u, UINT32_MAX);
    if (!grown) return NULL;
    g_pending = grown;
    return &g_pending[next];
}

static bool pending_resource_id(uint64_t resource_id) {
    for (size_t index = 0u; index < g_pending_capacity;
         ++index)
        if (g_pending[index].occupied &&
            g_pending[index].publication.resource.resource_id == resource_id)
            return true;
    return false;
}

static void cancel_pending_publications(void) {
    for (size_t index = 0u; index < g_pending_capacity;
         ++index) {
        XgRenderModelPendingPublication *pending = &g_pending[index];
        XgRenderResourceView view;

        if (!pending->occupied) continue;
        if (xg_render_resource_view(pending->publication.resource, &view) ==
                XG_RENDER_RESOURCE_OK &&
            !view.current &&
            view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE)
            (void)xg_render_resource_import_cancel(
                pending->publication.resource);
        memset(pending, 0, sizeof(*pending));
    }
}

void xg_render_model_resources_reset(void) {
    cancel_pending_publications();
    free(g_pending); g_pending = NULL; g_pending_capacity = 0u;
    free(g_published); g_published = NULL; g_published_capacity = 0u;
}

void xg_render_model_resources_scene_boundary(void) {
    cancel_pending_publications();
}

static XgRenderModelPublishedMetadata *published_metadata(
        XgRenderResourceHandle handle) {
    for (size_t index = 0u; index < g_published_capacity;
         ++index) {
        XgRenderModelPublishedMetadata *metadata = &g_published[index];

        if (metadata->occupied && handles_equal(metadata->handle, handle))
            return metadata;
    }
    return NULL;
}

static XgRenderModelPublishedMetadata *available_metadata(void) {
    XgRenderResourceView view;

    for (size_t index = 0u; index < g_published_capacity;
         ++index) {
        XgRenderModelPublishedMetadata *metadata = &g_published[index];

        if (!metadata->occupied) return metadata;
        if (xg_render_resource_view(metadata->handle, &view) ==
                XG_RENDER_RESOURCE_NOT_FOUND) {
            memset(metadata, 0, sizeof(*metadata));
            return metadata;
        }
    }
    const uint32_t next = g_published_capacity;
    if (next == UINT32_MAX) return NULL;
    XgRenderModelPublishedMetadata *grown = xg_render_array_reserve(g_published,
        sizeof(*grown), &g_published_capacity, next + 1u, UINT32_MAX);
    if (!grown) return NULL;
    g_published = grown;
    return &g_published[next];
}

XgRenderModelResourceResult xg_render_model_resource_publish_begin(
        const XgRenderModelPublicationDescription *description,
        XgRenderModelPublication *out_publication) {
    const XgRenderModelAuthenticatedSourceReceipt *source =
        description != NULL ? description->authenticated_source : NULL;
    XgRenderModelPendingPublication *pending;
    XgRenderResourceIdentity identity;
    XgRenderResourceImport import;
    XgRenderResourceHandle handle;
    XgRenderResourceResult repository_result;
    XgRenderResourceCapabilityMetadata capability_metadata;
    XgRenderResourceProvenance provenance;

    if (description == NULL || out_publication == NULL ||
        source == NULL || source->receipt == 0u)
        return XG_RENDER_MODEL_RESOURCE_AUTHENTICATION_REQUIRED;
    if (!receipt_valid(source) || description->source_bytes == NULL ||
        description->source_byte_count == 0u ||
        source->range_size != description->source_byte_count ||
        description->payload_bytes == NULL ||
        description->payload_byte_count == 0u ||
        description->payload_content_digest == 0u ||
        description->owner_generation == 0u ||
        description->owner_kind < XG_RENDER_RESOURCE_OWNER_STATIC ||
        description->owner_kind > XG_RENDER_RESOURCE_OWNER_BATCH)
        return XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT;
    if (description->provenance_receipt == 0u)
        return XG_RENDER_MODEL_RESOURCE_AUTHENTICATION_REQUIRED;
    if (description->provenance_receipt != source->receipt)
        return XG_RENDER_MODEL_RESOURCE_RECEIPT_MISMATCH;
    provenance = (XgRenderResourceProvenance){
        .kind = provenance_kind(source->source_kind),
        .receipt = source->receipt,
        .capability = source->capability,
    };
    if (xg_render_resource_capability_validate(
            &provenance, description->owner_kind,
            description->owner_generation, &capability_metadata) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK ||
        capability_metadata.source.source_class !=
            XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE ||
        memcmp(&capability_metadata.source.identity,
               &source->source_identity,
               sizeof(source->source_identity)) != 0 ||
        capability_metadata.source.range_offset != source->range_offset ||
        capability_metadata.source.range_size != source->range_size ||
        capability_metadata.source.range_content_digest !=
            source->range_content_digest)
        return XG_RENDER_MODEL_RESOURCE_AUTHENTICATION_REQUIRED;
    if (xg_render_resource_digest(description->source_bytes,
            description->source_byte_count) != source->range_content_digest)
        return XG_RENDER_MODEL_RESOURCE_SOURCE_DIGEST_MISMATCH;
    if (xg_render_resource_digest(description->payload_bytes,
            description->payload_byte_count) !=
            description->payload_content_digest)
        return XG_RENDER_MODEL_RESOURCE_PAYLOAD_DIGEST_MISMATCH;
    if (xg_render_model_resource_identity(source, &identity) !=
            XG_RENDER_MODEL_RESOURCE_OK)
        return XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT;
    if (pending_resource_id(xg_render_resource_identity_id(&identity)))
        return XG_RENDER_MODEL_RESOURCE_INVALID_STATE;
    pending = available_pending();
    if (pending == NULL)
        return XG_RENDER_MODEL_RESOURCE_CAPACITY_EXCEEDED;
    import = (XgRenderResourceImport){
        .resource_id = xg_render_resource_identity_id(&identity),
        .kind = XG_RENDER_RESOURCE_MODEL,
        .owner_kind = description->owner_kind,
        .owner_generation = description->owner_generation,
        .state = XG_RENDER_RESOURCE_IMPORTING_NATIVE,
        .provenance = {
            .kind = provenance_kind(source->source_kind),
            .receipt = source->receipt,
            .capability = source->capability,
            .synthetic = false,
        },
        .content_digest = description->payload_content_digest,
        .bytes = description->payload_bytes,
        .byte_count = description->payload_byte_count,
        .identity = identity,
    };
    repository_result = xg_render_resource_import_begin(&import, &handle);
    if (repository_result != XG_RENDER_RESOURCE_OK)
        return map_repository_result(repository_result);
    if (g_next_transaction == 0u) {
        (void)xg_render_resource_import_cancel(handle);
        return XG_RENDER_MODEL_RESOURCE_CAPACITY_EXCEEDED;
    }
    *pending = (XgRenderModelPendingPublication){
        .publication = { handle, g_next_transaction++ },
        .source = *source,
        .source_bytes = description->source_bytes,
        .source_byte_count = description->source_byte_count,
        .payload_content_digest = description->payload_content_digest,
        .occupied = true,
    };
    *out_publication = pending->publication;
    return XG_RENDER_MODEL_RESOURCE_OK;
}

XgRenderModelResourceResult xg_render_model_resource_publish_commit(
        XgRenderModelPublication publication,
        XgRenderModelResource *out_resource) {
    XgRenderModelPendingPublication *pending = find_pending(publication);
    XgRenderModelPublishedMetadata *metadata;
    XgRenderResourceResult repository_result;

    if (out_resource == NULL)
        return XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT;
    if (pending == NULL)
        return XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION;
    if (xg_render_resource_digest(pending->source_bytes,
            pending->source_byte_count) !=
            pending->source.range_content_digest) {
        (void)xg_render_resource_import_cancel(pending->publication.resource);
        memset(pending, 0, sizeof(*pending));
        return XG_RENDER_MODEL_RESOURCE_SOURCE_DIGEST_MISMATCH;
    }
    metadata = published_metadata(pending->publication.resource);
    if (metadata == NULL) metadata = available_metadata();
    if (metadata == NULL) {
        (void)xg_render_resource_import_cancel(pending->publication.resource);
        memset(pending, 0, sizeof(*pending));
        return XG_RENDER_MODEL_RESOURCE_CAPACITY_EXCEEDED;
    }
    repository_result = xg_render_resource_import_commit(
        pending->publication.resource);
    if (repository_result != XG_RENDER_RESOURCE_OK) {
        (void)xg_render_resource_import_cancel(pending->publication.resource);
        memset(pending, 0, sizeof(*pending));
        return map_repository_result(repository_result);
    }
    *metadata = (XgRenderModelPublishedMetadata){
        .handle = pending->publication.resource,
        .source = pending->source,
        .occupied = true,
    };
    *out_resource = (XgRenderModelResource){
        .handle = pending->publication.resource,
        .content_digest = pending->payload_content_digest,
        .provenance_receipt = pending->source.receipt,
    };
    memset(pending, 0, sizeof(*pending));
    return XG_RENDER_MODEL_RESOURCE_OK;
}

XgRenderModelResourceResult xg_render_model_resource_publish_cancel(
        XgRenderModelPublication publication) {
    XgRenderModelPendingPublication *pending = find_pending(publication);
    XgRenderResourceView view;
    XgRenderResourceResult repository_result;

    if (pending == NULL)
        return XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION;
    repository_result = xg_render_resource_view(
        pending->publication.resource, &view);
    if (repository_result == XG_RENDER_RESOURCE_OK &&
        view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE)
        repository_result = xg_render_resource_import_cancel(
            pending->publication.resource);
    memset(pending, 0, sizeof(*pending));
    return map_repository_result(repository_result);
}

XgRenderModelResourceResult xg_render_model_resource_retire_current(
        XgRenderResourceHandle handle) {
    if (handle.resource_id == 0u || handle.generation == 0u)
        return XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT;
    if (published_metadata(handle) == NULL)
        return XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION;
    return map_repository_result(xg_render_resource_retire_current(handle));
}

XgRenderModelResourceResult xg_render_model_resource_view(
        XgRenderResourceHandle handle,
        XgRenderModelResourceView *out_view) {
    XgRenderModelPublishedMetadata *metadata;
    XgRenderResourceResult repository_result;

    if (out_view == NULL)
        return XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT;
    repository_result = xg_render_resource_view(handle, &out_view->resource);
    if (repository_result != XG_RENDER_RESOURCE_OK)
        return map_repository_result(repository_result);
    metadata = published_metadata(handle);
    if (metadata == NULL || out_view->resource.kind != XG_RENDER_RESOURCE_MODEL ||
        out_view->resource.provenance.synthetic ||
        out_view->resource.provenance.kind !=
            provenance_kind(metadata->source.source_kind) ||
        out_view->resource.provenance.receipt != metadata->source.receipt)
        return XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION;
    out_view->source_kind = metadata->source.source_kind;
    out_view->source_identity = metadata->source.source_identity;
    out_view->source_range_offset = metadata->source.range_offset;
    out_view->source_range_size = metadata->source.range_size;
    out_view->provenance_receipt = out_view->resource.provenance.receipt;
    return XG_RENDER_MODEL_RESOURCE_OK;
}
