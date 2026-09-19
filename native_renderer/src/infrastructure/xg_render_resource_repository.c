#include "xg_render_resource_repository.h"
#include "xg_render_array.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

typedef struct XgRenderResourceEntry {
    XgRenderResourceView view;
    void *storage;
    bool occupied;
    bool restore_staged;
} XgRenderResourceEntry;

typedef struct XgRenderResourceIdentityEntry {
    uint64_t resource_id;
    XgRenderResourceIdentity identity;
    bool has_identity;
    bool occupied;
} XgRenderResourceIdentityEntry;

typedef struct XgRenderResourceCapabilityEntry {
    XgRenderResourceCapabilityMetadata metadata;
    uint64_t capability;
    XgRenderResourceOwnerKind restored_owner_kind;
    uint64_t restored_owner_generation;
    uint64_t checkpoint_capability;
    bool occupied;
    bool live;
    bool retire_pending;
    bool restore_staged;
    bool restore_created;
} XgRenderResourceCapabilityEntry;

static XgRenderResourceEntry *g_entries;
static XgRenderResourceIdentityEntry *g_identities;
static XgRenderResourceCapabilityEntry *g_capabilities;
static uint32_t g_entry_capacity, g_identity_capacity, g_capability_capacity;
static XgRenderResourceDiagnostics g_diagnostics;
static uint64_t g_next_generation = 1u;
static uint64_t g_next_capability = 1u;
static uint64_t g_capability_session;
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static XgRenderResourceRestoreFault g_restore_fault;
static uint32_t g_restore_fail_after;
static bool g_capability_restore_active;

static XgRenderResourceCapabilityEntry *capability_by_id(uint64_t capability);
static void retire_pending_capability_if_unused_locked(uint64_t capability);

enum {
    XG_RENDER_CAPABILITY_CHECKPOINT_MAGIC = 0x41434758u,
    XG_RENDER_CAPABILITY_CHECKPOINT_VERSION = 1u,
    XG_RENDER_CAPABILITY_CHECKPOINT_PAYLOAD_SIZE = 192u,
};

static void repository_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {}
}

static void repository_unlock(void) {
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

static void checkpoint_write_u32(uint8_t **cursor, uint32_t value) {
    for (uint32_t index = 0u; index < 4u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 4u;
}

static void checkpoint_write_u64(uint8_t **cursor, uint64_t value) {
    for (uint32_t index = 0u; index < 8u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 8u;
}

static uint32_t checkpoint_read_u32(const uint8_t **cursor) {
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)(*cursor)[index] << (index * 8u);
    *cursor += 4u;
    return value;
}

static uint64_t checkpoint_read_u64(const uint8_t **cursor) {
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)(*cursor)[index] << (index * 8u);
    *cursor += 8u;
    return value;
}

static uint64_t capability_hash(const uint8_t *bytes, size_t byte_count,
                                uint64_t seed) {
    uint64_t hash = seed;
    for (size_t index = 0u; index < byte_count; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void capability_checkpoint_seal(
        const uint8_t bytes[XG_RENDER_CAPABILITY_CHECKPOINT_PAYLOAD_SIZE],
        uint8_t seal[XG_RENDER_RESOURCE_IDENTITY_SIZE]) {
    static const uint64_t seeds[4] = {
        UINT64_C(1469598103934665603), UINT64_C(7809847782465536322),
        UINT64_C(9650029242287828579), UINT64_C(2870177450012600261),
    };
    for (uint32_t word = 0u; word < 4u; ++word) {
        const uint64_t hash = capability_hash(
            bytes, XG_RENDER_CAPABILITY_CHECKPOINT_PAYLOAD_SIZE, seeds[word]);
        for (uint32_t byte = 0u; byte < 8u; ++byte)
            seal[word * 8u + byte] = (uint8_t)(hash >> (byte * 8u));
    }
}

static void capability_session_advance(void) {
    struct timespec now = {0};
    uint64_t seed;

    (void)timespec_get(&now, TIME_UTC);
    seed = (uint64_t)now.tv_sec ^ ((uint64_t)now.tv_nsec << 32u) ^
        (uint64_t)(uintptr_t)&g_capability_session ^ g_capability_session;
    g_capability_session = capability_hash(
        (const uint8_t *)&seed, sizeof(seed),
        UINT64_C(1469598103934665603));
    if (g_capability_session == 0u) g_capability_session = 1u;
    g_next_capability = 1u;
}

static uint64_t next_capability_locked(void) {
    uint8_t input[16];

    if (g_capability_session == 0u) capability_session_advance();
    for (;;) {
        uint64_t capability;
        if (g_next_capability == 0u) return 0u;
        memcpy(input, &g_capability_session, sizeof(g_capability_session));
        memcpy(input + 8u, &g_next_capability, sizeof(g_next_capability));
        g_next_capability++;
        capability = capability_hash(
            input, sizeof(input), UINT64_C(1099511628211));
        if (capability != 0u && capability_by_id(capability) == NULL)
            return capability;
    }
}

uint64_t xg_render_resource_digest(const void *bytes, size_t byte_count) {
    const uint8_t *source = (const uint8_t *)bytes;
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    if (bytes == NULL && byte_count != 0u) return 0u;
    /* Poses and descriptors contain long zero-filled unused tails. Eight zero
     * bytes are exactly eight FNV multiplications, without the serial byte
     * dependency. memcpy keeps this valid for unaligned resource storage. */
    for (index = 0; byte_count - index >= sizeof(uint64_t); index += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, source + index, sizeof(word));
        if (word == 0u) {
            hash *= UINT64_C(0x1efac7090aef4a21);
        } else {
            for (size_t byte = 0; byte < sizeof(word); ++byte) {
                hash ^= source[index + byte];
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    for (; index < byte_count; index++) {
        hash ^= source[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t xg_render_resource_identity_id(
        const XgRenderResourceIdentity *identity) {
    uint64_t resource_id = 0u;
    size_t index;

    if (identity == NULL) return 0u;
    for (index = 0u; index < sizeof(resource_id); ++index)
        resource_id |= (uint64_t)identity->bytes[index] << (index * 8u);
    return resource_id;
}

static bool identity_present(const XgRenderResourceIdentity *identity) {
    size_t index;

    for (index = 0u; index < sizeof(identity->bytes); ++index)
        if (identity->bytes[index] != 0u) return true;
    return false;
}

static bool resource_kind_valid(XgRenderResourceKind kind) {
    return kind >= XG_RENDER_RESOURCE_TEXTURE &&
        kind <= XG_RENDER_RESOURCE_MOVIE_FRAME;
}

static bool descriptor_is_zero(
        const XgRenderResourceDescriptor *descriptor) {
    const XgRenderResourceDescriptor zero = {0};

    return descriptor != NULL &&
        memcmp(descriptor, &zero, sizeof(zero)) == 0;
}

bool xg_render_resource_descriptor_validate(
        const XgRenderResourceDescriptor *descriptor) {
    const bool has_vram = descriptor != NULL &&
        (descriptor->flags &
         XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u;

    if (descriptor_is_zero(descriptor)) return true;
    if (descriptor == NULL ||
        descriptor->version != XG_RENDER_RESOURCE_DESCRIPTOR_VERSION ||
        descriptor->pixel_format < XG_RENDER_RESOURCE_PIXEL_FORMAT_NON_PIXEL ||
        descriptor->pixel_format > XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555 ||
        (descriptor->flags &
         ~XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u ||
        descriptor->sampler < XG_RENDER_RESOURCE_SAMPLER_UNSPECIFIED ||
        descriptor->sampler > XG_RENDER_RESOURCE_SAMPLER_LINEAR ||
        descriptor->wrap_u < XG_RENDER_RESOURCE_WRAP_UNSPECIFIED ||
        descriptor->wrap_u > XG_RENDER_RESOURCE_WRAP_MIRROR ||
        descriptor->wrap_v < XG_RENDER_RESOURCE_WRAP_UNSPECIFIED ||
        descriptor->wrap_v > XG_RENDER_RESOURCE_WRAP_MIRROR)
        return false;
    if (descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_NON_PIXEL)
        return descriptor->width == 0u && descriptor->height == 0u &&
            descriptor->row_pitch == 0u && descriptor->vram_x == 0u &&
            descriptor->vram_y == 0u && descriptor->vram_width == 0u &&
            descriptor->vram_height == 0u &&
            descriptor->sampler == XG_RENDER_RESOURCE_SAMPLER_UNSPECIFIED &&
            descriptor->wrap_u == XG_RENDER_RESOURCE_WRAP_UNSPECIFIED &&
            descriptor->wrap_v == XG_RENDER_RESOURCE_WRAP_UNSPECIFIED &&
            !has_vram;
    if (descriptor->width == 0u || descriptor->height == 0u ||
        descriptor->row_pitch == 0u)
        return false;
    if ((descriptor->sampler == XG_RENDER_RESOURCE_SAMPLER_UNSPECIFIED) !=
            (descriptor->wrap_u == XG_RENDER_RESOURCE_WRAP_UNSPECIFIED &&
             descriptor->wrap_v == XG_RENDER_RESOURCE_WRAP_UNSPECIFIED))
        return false;
    if (!has_vram)
        return descriptor->vram_x == 0u && descriptor->vram_y == 0u &&
            descriptor->vram_width == 0u && descriptor->vram_height == 0u;
    return descriptor->vram_width != 0u && descriptor->vram_height != 0u &&
        descriptor->vram_x < 1024u && descriptor->vram_y < 512u &&
        descriptor->vram_width <= 1024u - descriptor->vram_x &&
        descriptor->vram_height <= 512u - descriptor->vram_y;
}

static bool descriptor_matches_storage(
        const XgRenderResourceDescriptor *descriptor, size_t byte_count) {
    if (!xg_render_resource_descriptor_validate(descriptor)) return false;
    if (descriptor->version == 0u ||
        descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_NON_PIXEL)
        return true;
    return descriptor->height <= byte_count &&
        descriptor->row_pitch <= byte_count / descriptor->height;
}

static bool descriptor_matches_import(
        XgRenderResourceKind kind,
        const XgRenderResourceProvenance *provenance,
        const XgRenderResourceDescriptor *descriptor, size_t byte_count) {
    if (!descriptor_matches_storage(descriptor, byte_count)) return false;
    if (descriptor->version == 0u)
        return provenance->synthetic ||
            (kind != XG_RENDER_RESOURCE_TEXTURE &&
             kind != XG_RENDER_RESOURCE_CLUT &&
             kind != XG_RENDER_RESOURCE_MOVIE_FRAME);
    switch (kind) {
    case XG_RENDER_RESOURCE_TEXTURE:
        return descriptor->pixel_format >=
                XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8 &&
            descriptor->pixel_format <= XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8;
    case XG_RENDER_RESOURCE_CLUT:
        return descriptor->pixel_format ==
            XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555;
    case XG_RENDER_RESOURCE_MOVIE_FRAME:
        return descriptor->pixel_format ==
                XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8 ||
            descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24 ||
            descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555;
    default:
        return true;
    }
}

static bool descriptor_equal(const XgRenderResourceDescriptor *left,
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

static bool owner_kind_valid(XgRenderResourceOwnerKind owner_kind) {
    return owner_kind >= XG_RENDER_RESOURCE_OWNER_STATIC &&
        owner_kind <= XG_RENDER_RESOURCE_OWNER_BATCH;
}

static bool provenance_shape_valid(
        const XgRenderResourceProvenance *provenance) {
    if (provenance == NULL) return false;
    switch (provenance->kind) {
    case XG_RENDER_RESOURCE_PROVENANCE_NONE:
        return provenance->synthetic && provenance->receipt == 0u &&
            provenance->capability == 0u;
    case XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT:
    case XG_RENDER_RESOURCE_PROVENANCE_SOURCE:
        return !provenance->synthetic && provenance->receipt != 0u &&
            provenance->capability != 0u;
    }
    return false;
}

static bool provenance_equal(const XgRenderResourceProvenance *left,
                             const XgRenderResourceProvenance *right) {
    return left->kind == right->kind && left->receipt == right->receipt &&
        left->capability == right->capability &&
        left->synthetic == right->synthetic;
}

static bool bytes_present(const uint8_t *bytes, size_t byte_count) {
    for (size_t index = 0u; index < byte_count; ++index)
        if (bytes[index] != 0u) return true;
    return false;
}

static bool capability_metadata_valid(
        const XgRenderResourceCapabilityMetadata *metadata) {
    if (metadata == NULL || metadata->receipt == 0u ||
        metadata->owner_generation == 0u ||
        !owner_kind_valid(metadata->owner_kind) ||
        metadata->lifetime < XG_RENDER_RESOURCE_CAPABILITY_STATIC ||
        metadata->lifetime > XG_RENDER_RESOURCE_CAPABILITY_TIMELINE)
        return false;
    if (metadata->kind == XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT)
        return (metadata->source.source_class == XG_RENDER_RESOURCE_SOURCE_NONE ||
                metadata->source.source_class ==
                    XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE) &&
            metadata->artifact.size != 0u &&
            bytes_present(metadata->artifact.sha256,
                          sizeof(metadata->artifact.sha256));
    if (metadata->kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE)
        return metadata->source.source_class > XG_RENDER_RESOURCE_SOURCE_NONE &&
            metadata->source.source_class <=
                XG_RENDER_RESOURCE_SOURCE_VRAM_TRANSFER;
    return false;
}

static XgRenderResourceCapabilityEntry *capability_by_id(uint64_t capability) {
    for (size_t index = 0u; index < g_capability_capacity;
         ++index)
        if (g_capabilities[index].occupied &&
            g_capabilities[index].capability == capability)
            return &g_capabilities[index];
    return NULL;
}

static XgRenderResourceCapabilityEntry *available_capability(void) {
    for (size_t index = 0u; index < g_capability_capacity;
         ++index)
        if (!g_capabilities[index].occupied) return &g_capabilities[index];
    const uint32_t index = g_capability_capacity;
    if (index == UINT32_MAX) return NULL;
    XgRenderResourceCapabilityEntry *grown = xg_render_array_reserve(g_capabilities,
        sizeof(*g_capabilities), &g_capability_capacity, index + 1u, UINT32_MAX);
    if (!grown) return NULL;
    g_capabilities = grown;
    return &g_capabilities[index];
}

static bool capability_key_matches(
        const XgRenderResourceCapabilityEntry *entry,
        const XgRenderResourceCapabilityMetadata *metadata) {
    const XgRenderResourceCapabilityMetadata *current = &entry->metadata;

    return current->kind == metadata->kind &&
        current->receipt == metadata->receipt &&
        current->lifetime == metadata->lifetime &&
        current->owner_kind == metadata->owner_kind &&
        current->owner_generation == metadata->owner_generation &&
        current->artifact.base == metadata->artifact.base &&
        current->artifact.size == metadata->artifact.size &&
        memcmp(current->artifact.sha256, metadata->artifact.sha256,
               sizeof(current->artifact.sha256)) == 0 &&
        current->source.source_class == metadata->source.source_class &&
        memcmp(&current->source.identity, &metadata->source.identity,
               sizeof(current->source.identity)) == 0 &&
        current->source.range_offset == metadata->source.range_offset &&
        current->source.range_size == metadata->source.range_size &&
        current->source.range_content_digest ==
            metadata->source.range_content_digest &&
        current->source.origin_artifact.base ==
            metadata->source.origin_artifact.base &&
        current->source.origin_artifact.size ==
            metadata->source.origin_artifact.size &&
        memcmp(current->source.origin_artifact.sha256,
               metadata->source.origin_artifact.sha256,
               sizeof(current->source.origin_artifact.sha256)) == 0;
}

static bool capability_matches_locked(
        const XgRenderResourceProvenance *provenance,
        XgRenderResourceOwnerKind owner_kind, uint64_t owner_generation,
        bool allow_restore,
        XgRenderResourceCapabilityMetadata *out_metadata) {
    XgRenderResourceCapabilityEntry *entry;

    if (!provenance_shape_valid(provenance)) return false;
    if (provenance->kind == XG_RENDER_RESOURCE_PROVENANCE_NONE) return true;
    entry = capability_by_id(provenance->capability);
    if (entry == NULL ||
        (!entry->live && !(allow_restore && entry->restore_staged)) ||
        entry->metadata.kind != provenance->kind ||
        entry->metadata.receipt != provenance->receipt)
        return false;
    if (entry->metadata.owner_kind != owner_kind ||
        entry->metadata.owner_generation != owner_generation) {
        if (!allow_restore || !entry->restore_staged ||
            entry->restored_owner_kind != owner_kind ||
            entry->restored_owner_generation != owner_generation)
            return false;
    }
    if (out_metadata != NULL) *out_metadata = entry->metadata;
    return true;
}

static XgRenderResourceCapabilityResult capability_register(
        const XgRenderResourceCapabilityMetadata *metadata,
        XgRenderResourceProvenance *out_provenance, bool *out_created) {
    XgRenderResourceCapabilityEntry *available = NULL;

    if (!capability_metadata_valid(metadata) || out_provenance == NULL)
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    if (out_created != NULL) *out_created = false;
    repository_lock();
    for (size_t index = 0u; index < g_capability_capacity;
         ++index) {
        XgRenderResourceCapabilityEntry *entry = &g_capabilities[index];

        if (!entry->occupied) {
            if (available == NULL) available = entry;
            continue;
        }
        if (!capability_key_matches(entry, metadata)) continue;
        if (!entry->live) continue;
        *out_provenance = (XgRenderResourceProvenance){
            .kind = metadata->kind,
            .receipt = metadata->receipt,
            .capability = entry->capability,
        };
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_OK;
    }
    if (available == NULL) available = available_capability();
    if (available == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_CAPACITY_EXCEEDED;
    }
    const uint64_t capability = next_capability_locked();
    if (capability == 0u) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_CAPACITY_EXCEEDED;
    }
    *available = (XgRenderResourceCapabilityEntry){
        .metadata = *metadata,
        .capability = capability,
        .occupied = true,
        .live = true,
    };
    *out_provenance = (XgRenderResourceProvenance){
        .kind = metadata->kind,
        .receipt = metadata->receipt,
        .capability = available->capability,
    };
    if (out_created != NULL) *out_created = true;
    repository_unlock();
    return XG_RENDER_RESOURCE_CAPABILITY_OK;
}

XgRenderResourceCapabilityResult xg_render_resource_capability_register(
        const XgRenderResourceCapabilityMetadata *metadata,
        XgRenderResourceProvenance *out_provenance) {
    return capability_register(metadata, out_provenance, NULL);
}

XgRenderResourceCapabilityResult xg_render_resource_capability_register_tracked(
        const XgRenderResourceCapabilityMetadata *metadata,
        XgRenderResourceProvenance *out_provenance, bool *out_created) {
    if (out_created == NULL)
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    return capability_register(metadata, out_provenance, out_created);
}

static void capability_checkpoint_encode(
        uint64_t capability,
        const XgRenderResourceCapabilityMetadata *metadata,
        XgRenderResourceCapabilityCheckpoint *out_checkpoint) {
    uint8_t *cursor = out_checkpoint->bytes;

    memset(out_checkpoint, 0, sizeof(*out_checkpoint));
    checkpoint_write_u32(&cursor, XG_RENDER_CAPABILITY_CHECKPOINT_MAGIC);
    checkpoint_write_u32(&cursor, XG_RENDER_CAPABILITY_CHECKPOINT_VERSION);
    checkpoint_write_u64(&cursor, capability);
    checkpoint_write_u32(&cursor, (uint32_t)metadata->kind);
    checkpoint_write_u64(&cursor, metadata->receipt);
    checkpoint_write_u32(&cursor, (uint32_t)metadata->lifetime);
    checkpoint_write_u32(&cursor, (uint32_t)metadata->owner_kind);
    checkpoint_write_u64(&cursor, metadata->owner_generation);
    checkpoint_write_u32(&cursor, metadata->artifact.base);
    checkpoint_write_u32(&cursor, metadata->artifact.size);
    checkpoint_write_u32(&cursor, metadata->artifact.crc32);
    memcpy(cursor, metadata->artifact.sha256, sizeof(metadata->artifact.sha256));
    cursor += sizeof(metadata->artifact.sha256);
    checkpoint_write_u32(&cursor, (uint32_t)metadata->source.source_class);
    memcpy(cursor, metadata->source.identity.bytes,
           sizeof(metadata->source.identity.bytes));
    cursor += sizeof(metadata->source.identity.bytes);
    checkpoint_write_u64(&cursor, metadata->source.range_offset);
    checkpoint_write_u64(&cursor, metadata->source.range_size);
    checkpoint_write_u64(&cursor, metadata->source.range_content_digest);
    checkpoint_write_u32(&cursor, metadata->source.origin_artifact.base);
    checkpoint_write_u32(&cursor, metadata->source.origin_artifact.size);
    checkpoint_write_u32(&cursor, metadata->source.origin_artifact.crc32);
    memcpy(cursor, metadata->source.origin_artifact.sha256,
           sizeof(metadata->source.origin_artifact.sha256));
    cursor += sizeof(metadata->source.origin_artifact.sha256);
    capability_checkpoint_seal(out_checkpoint->bytes, cursor);
}

static bool capability_checkpoint_decode(
        const XgRenderResourceCapabilityCheckpoint *checkpoint,
        uint64_t *out_capability,
        XgRenderResourceCapabilityMetadata *out_metadata) {
    const uint8_t *cursor;
    uint8_t seal[XG_RENDER_RESOURCE_IDENTITY_SIZE];

    if (checkpoint == NULL || out_capability == NULL || out_metadata == NULL)
        return false;
    capability_checkpoint_seal(checkpoint->bytes, seal);
    if (memcmp(seal,
               checkpoint->bytes + XG_RENDER_CAPABILITY_CHECKPOINT_PAYLOAD_SIZE,
               sizeof(seal)) != 0)
        return false;
    cursor = checkpoint->bytes;
    if (checkpoint_read_u32(&cursor) != XG_RENDER_CAPABILITY_CHECKPOINT_MAGIC ||
        checkpoint_read_u32(&cursor) != XG_RENDER_CAPABILITY_CHECKPOINT_VERSION)
        return false;
    *out_capability = checkpoint_read_u64(&cursor);
    memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->kind =
        (XgRenderResourceProvenanceKind)checkpoint_read_u32(&cursor);
    out_metadata->receipt = checkpoint_read_u64(&cursor);
    out_metadata->lifetime =
        (XgRenderResourceCapabilityLifetime)checkpoint_read_u32(&cursor);
    out_metadata->owner_kind =
        (XgRenderResourceOwnerKind)checkpoint_read_u32(&cursor);
    out_metadata->owner_generation = checkpoint_read_u64(&cursor);
    out_metadata->artifact.base = checkpoint_read_u32(&cursor);
    out_metadata->artifact.size = checkpoint_read_u32(&cursor);
    out_metadata->artifact.crc32 = checkpoint_read_u32(&cursor);
    memcpy(out_metadata->artifact.sha256, cursor,
           sizeof(out_metadata->artifact.sha256));
    cursor += sizeof(out_metadata->artifact.sha256);
    out_metadata->source.source_class =
        (XgRenderResourceSourceClass)checkpoint_read_u32(&cursor);
    memcpy(out_metadata->source.identity.bytes, cursor,
           sizeof(out_metadata->source.identity.bytes));
    cursor += sizeof(out_metadata->source.identity.bytes);
    out_metadata->source.range_offset = checkpoint_read_u64(&cursor);
    out_metadata->source.range_size = checkpoint_read_u64(&cursor);
    out_metadata->source.range_content_digest = checkpoint_read_u64(&cursor);
    out_metadata->source.origin_artifact.base = checkpoint_read_u32(&cursor);
    out_metadata->source.origin_artifact.size = checkpoint_read_u32(&cursor);
    out_metadata->source.origin_artifact.crc32 = checkpoint_read_u32(&cursor);
    memcpy(out_metadata->source.origin_artifact.sha256, cursor,
           sizeof(out_metadata->source.origin_artifact.sha256));
    cursor += sizeof(out_metadata->source.origin_artifact.sha256);
    return cursor == checkpoint->bytes +
            XG_RENDER_CAPABILITY_CHECKPOINT_PAYLOAD_SIZE &&
        *out_capability != 0u && capability_metadata_valid(out_metadata);
}

XgRenderResourceCapabilityResult xg_render_resource_capability_checkpoint(
        const XgRenderResourceProvenance *provenance,
        XgRenderResourceOwnerKind owner_kind, uint64_t owner_generation,
        XgRenderResourceCapabilityCheckpoint *out_checkpoint) {
    XgRenderResourceCapabilityEntry *entry;

    if (provenance == NULL || out_checkpoint == NULL ||
        !owner_kind_valid(owner_kind) || owner_generation == 0u)
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    repository_lock();
    entry = capability_by_id(provenance->capability);
    if (entry == NULL || !entry->live || entry->metadata.kind != provenance->kind ||
        entry->metadata.receipt != provenance->receipt ||
        entry->metadata.owner_kind != owner_kind ||
        entry->metadata.owner_generation != owner_generation) {
        repository_unlock();
        return entry == NULL ? XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND
                             : XG_RENDER_RESOURCE_CAPABILITY_REVOKED;
    }
    capability_checkpoint_encode(entry->capability, &entry->metadata,
                                 out_checkpoint);
    repository_unlock();
    return XG_RENDER_RESOURCE_CAPABILITY_OK;
}

XgRenderResourceCapabilityResult
xg_render_resource_capability_checkpoint_validate(
        const XgRenderResourceCapabilityCheckpoint *checkpoint,
        const XgRenderResourceProvenance *provenance,
        XgRenderResourceOwnerKind owner_kind, uint64_t owner_generation,
        XgRenderResourceCapabilityMetadata *out_metadata) {
    XgRenderResourceCapabilityMetadata metadata;
    uint64_t capability;

    if (!provenance_shape_valid(provenance) ||
        provenance->kind == XG_RENDER_RESOURCE_PROVENANCE_NONE ||
        !owner_kind_valid(owner_kind) || owner_generation == 0u ||
        !capability_checkpoint_decode(checkpoint, &capability, &metadata))
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    if (capability != provenance->capability || metadata.kind != provenance->kind ||
        metadata.receipt != provenance->receipt ||
        metadata.owner_kind != owner_kind ||
        metadata.owner_generation != owner_generation)
        return XG_RENDER_RESOURCE_CAPABILITY_CONFLICT;
    if (out_metadata != NULL) *out_metadata = metadata;
    return XG_RENDER_RESOURCE_CAPABILITY_OK;
}

XgRenderResourceCapabilityResult xg_render_resource_capability_validate(
        const XgRenderResourceProvenance *provenance,
        XgRenderResourceOwnerKind owner_kind, uint64_t owner_generation,
        XgRenderResourceCapabilityMetadata *out_metadata) {
    XgRenderResourceCapabilityResult result;

    if (provenance == NULL || !owner_kind_valid(owner_kind) ||
        owner_generation == 0u)
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    repository_lock();
    result = capability_matches_locked(provenance, owner_kind,
                                       owner_generation,
                                       g_capability_restore_active,
                                       out_metadata)
        ? XG_RENDER_RESOURCE_CAPABILITY_OK
        : capability_by_id(provenance->capability) != NULL
            ? XG_RENDER_RESOURCE_CAPABILITY_REVOKED
            : XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND;
    repository_unlock();
    return result;
}

static XgRenderResourceEntry *find_entry(XgRenderResourceHandle handle) {
    /* Hint only: every hit revalidates the occupied slot and complete handle
     * under the repository lock. Retirement/reuse cannot authorize a stale hit. */
    static size_t hints[1024];
    uint64_t hash=handle.resource_id^(handle.generation*UINT64_C(0x9e3779b97f4a7c15));
    hash^=hash>>32u;
    const size_t bucket=(size_t)hash&1023u;
    if(hints[bucket] && hints[bucket] <= g_entry_capacity) {
        XgRenderResourceEntry *entry=&g_entries[hints[bucket]-1u];
        if(entry->occupied&&entry->view.handle.resource_id==handle.resource_id&&
            entry->view.handle.generation==handle.generation)return entry;
    }
    size_t index;
    for (index = 0; index < g_entry_capacity; index++) {
        XgRenderResourceEntry *entry = &g_entries[index];
        if (entry->occupied &&
            entry->view.handle.resource_id == handle.resource_id &&
            entry->view.handle.generation == handle.generation) {
            hints[bucket]=index+1u;
            return entry;
        }
    }
    return NULL;
}

static void retire_entry(XgRenderResourceEntry *entry) {
    const uint64_t capability = entry->view.provenance.capability;

    free(entry->storage);
    memset(entry, 0, sizeof(*entry));
    g_diagnostics.retirements++;
    if (capability != 0u)
        retire_pending_capability_if_unused_locked(capability);
}

static void reclaim_identity_if_unused(uint64_t resource_id) {
    size_t index;

    for (index = 0u; index < g_entry_capacity; ++index)
        if (g_entries[index].occupied &&
            g_entries[index].view.handle.resource_id == resource_id)
            return;
    for (index = 0u; index < g_identity_capacity; ++index) {
        XgRenderResourceIdentityEntry *identity = &g_identities[index];

        if (!identity->occupied || identity->resource_id != resource_id)
            continue;
        memset(identity, 0, sizeof(*identity));
        return;
    }
}

static void invalidate_capability_resources_locked(uint64_t capability) {
    for (size_t index = 0u; index < g_entry_capacity;
         ++index) {
        XgRenderResourceEntry *entry = &g_entries[index];

        if (!entry->occupied ||
            entry->view.provenance.capability != capability ||
            entry->restore_staged)
            continue;
        entry->view.current = false;
        g_diagnostics.invalidations++;
        if (entry->view.retain_count == 0u) {
            const uint64_t resource_id = entry->view.handle.resource_id;

            retire_entry(entry);
            reclaim_identity_if_unused(resource_id);
        }
    }
}

XgRenderResourceCapabilityResult xg_render_resource_capability_revoke(
        XgRenderResourceProvenance provenance) {
    XgRenderResourceCapabilityEntry *entry;

    if (!provenance_shape_valid(&provenance) ||
        provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_NONE)
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    repository_lock();
    entry = capability_by_id(provenance.capability);
    if (entry == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND;
    }
    if (!entry->live || entry->metadata.kind != provenance.kind ||
        entry->metadata.receipt != provenance.receipt) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_REVOKED;
    }
    entry->live = false;
    entry->restore_staged = false;
    invalidate_capability_resources_locked(entry->capability);
    memset(entry, 0, sizeof(*entry));
    repository_unlock();
    return XG_RENDER_RESOURCE_CAPABILITY_OK;
}

static void retire_pending_capability_if_unused_locked(uint64_t capability) {
    XgRenderResourceCapabilityEntry *entry = capability_by_id(capability);

    if (entry == NULL || !entry->live || !entry->retire_pending ||
        entry->restore_staged)
        return;
    for (size_t index = 0u; index < g_entry_capacity;
         ++index)
        if (g_entries[index].occupied &&
            g_entries[index].view.provenance.capability == capability)
            return;
    memset(entry, 0, sizeof(*entry));
}

XgRenderResourceCapabilityResult xg_render_resource_capability_retire(
        XgRenderResourceProvenance provenance) {
    XgRenderResourceCapabilityEntry *entry;

    if (!provenance_shape_valid(&provenance) ||
        provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_NONE)
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    repository_lock();
    entry = capability_by_id(provenance.capability);
    if (entry == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND;
    }
    if (!entry->live || entry->metadata.kind != provenance.kind ||
        entry->metadata.receipt != provenance.receipt) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_REVOKED;
    }
    entry->retire_pending = true;
    retire_pending_capability_if_unused_locked(entry->capability);
    repository_unlock();
    return XG_RENDER_RESOURCE_CAPABILITY_OK;
}

bool xg_render_resource_capability_restore_begin(void) {
    bool success;

    repository_lock();
    success = !g_capability_restore_active;
    if (success) g_capability_restore_active = true;
    repository_unlock();
    return success;
}

XgRenderResourceCapabilityResult xg_render_resource_capability_restore_stage(
        XgRenderResourceProvenance provenance,
        XgRenderResourceOwnerKind owner_kind,
        uint64_t restored_owner_generation) {
    XgRenderResourceCapabilityEntry *entry;

    if (!provenance_shape_valid(&provenance) ||
        provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_NONE ||
        !owner_kind_valid(owner_kind) || restored_owner_generation == 0u)
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    repository_lock();
    if (!g_capability_restore_active) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_RESTORE_ACTIVE;
    }
    entry = capability_by_id(provenance.capability);
    if (entry == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND;
    }
    if (entry->metadata.kind != provenance.kind ||
        entry->metadata.receipt != provenance.receipt) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_REVOKED;
    }
    if (entry->restore_staged &&
        (entry->restored_owner_kind != owner_kind ||
         entry->restored_owner_generation != restored_owner_generation)) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_CONFLICT;
    }
    entry->restore_staged = true;
    entry->restored_owner_kind = owner_kind;
    entry->restored_owner_generation = restored_owner_generation;
    repository_unlock();
    return XG_RENDER_RESOURCE_CAPABILITY_OK;
}

XgRenderResourceCapabilityResult
xg_render_resource_capability_checkpoint_restore_stage(
        const XgRenderResourceCapabilityCheckpoint *checkpoint,
        const XgRenderResourceProvenance *checkpoint_provenance,
        XgRenderResourceOwnerKind owner_kind,
        uint64_t original_owner_generation,
        uint64_t restored_owner_generation,
        XgRenderResourceProvenance *out_restored_provenance) {
    XgRenderResourceCapabilityMetadata metadata;
    XgRenderResourceCapabilityEntry *entry;
    uint64_t checkpoint_capability;

    if (out_restored_provenance == NULL || restored_owner_generation == 0u ||
        xg_render_resource_capability_checkpoint_validate(
            checkpoint, checkpoint_provenance, owner_kind,
            original_owner_generation, &metadata) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK)
        return XG_RENDER_RESOURCE_CAPABILITY_INVALID_ARGUMENT;
    checkpoint_capability = checkpoint_provenance->capability;
    repository_lock();
    if (!g_capability_restore_active) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_RESTORE_ACTIVE;
    }
    entry = capability_by_id(checkpoint_capability);
    if (entry != NULL) {
        if (!entry->live || !capability_key_matches(entry, &metadata)) {
            repository_unlock();
            return XG_RENDER_RESOURCE_CAPABILITY_REVOKED;
        }
    } else {
        for (size_t index = 0u;
             index < g_capability_capacity; ++index) {
            XgRenderResourceCapabilityEntry *candidate = &g_capabilities[index];
            if (candidate->occupied && candidate->restore_staged &&
                candidate->checkpoint_capability == checkpoint_capability &&
                capability_key_matches(candidate, &metadata)) {
                entry = candidate;
                break;
            }
        }
        if (entry == NULL) {
            uint64_t capability;
            entry = available_capability();
            capability = entry != NULL ? next_capability_locked() : 0u;
            if (entry == NULL || capability == 0u) {
                repository_unlock();
                return XG_RENDER_RESOURCE_CAPABILITY_CAPACITY_EXCEEDED;
            }
            while (capability == checkpoint_capability) {
                capability = next_capability_locked();
                if (capability == 0u) {
                    repository_unlock();
                    return XG_RENDER_RESOURCE_CAPABILITY_CAPACITY_EXCEEDED;
                }
            }
            *entry = (XgRenderResourceCapabilityEntry){
                .metadata = metadata,
                .capability = capability,
                .checkpoint_capability = checkpoint_capability,
                .occupied = true,
                .live = false,
                .restore_created = true,
            };
        }
    }
    if (entry->restore_staged &&
        (entry->restored_owner_kind != owner_kind ||
         entry->restored_owner_generation != restored_owner_generation)) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPABILITY_CONFLICT;
    }
    entry->restore_staged = true;
    entry->restored_owner_kind = owner_kind;
    entry->restored_owner_generation = restored_owner_generation;
    *out_restored_provenance = (XgRenderResourceProvenance){
        .kind = metadata.kind,
        .receipt = metadata.receipt,
        .capability = entry->capability,
    };
    repository_unlock();
    return XG_RENDER_RESOURCE_CAPABILITY_OK;
}

void xg_render_resource_capability_restore_commit(void) {
    repository_lock();
    if (g_capability_restore_active) {
        for (size_t index = 0u;
              index < g_capability_capacity; ++index) {
            XgRenderResourceCapabilityEntry *entry = &g_capabilities[index];

            if (!entry->occupied || !entry->restore_staged)
                continue;
            entry->live = true;
            entry->metadata.owner_kind = entry->restored_owner_kind;
            entry->metadata.owner_generation =
                entry->restored_owner_generation;
            entry->restore_staged = false;
            entry->restore_created = false;
            entry->retire_pending = false;
            entry->checkpoint_capability = 0u;
            entry->restored_owner_generation = 0u;
        }
        g_capability_restore_active = false;
    }
    repository_unlock();
}

void xg_render_resource_capability_restore_cancel(void) {
    repository_lock();
    for (size_t index = 0u; index < g_capability_capacity;
         ++index) {
        if (g_capabilities[index].restore_created) {
            memset(&g_capabilities[index], 0, sizeof(g_capabilities[index]));
            continue;
        }
        g_capabilities[index].restore_staged = false;
        g_capabilities[index].restored_owner_generation = 0u;
        retire_pending_capability_if_unused_locked(
            g_capabilities[index].capability);
    }
    g_capability_restore_active = false;
    repository_unlock();
}

void xg_render_resource_repository_reset(void) {
    size_t index;
    repository_lock();
    for (index = 0; index < g_entry_capacity; index++)
        free(g_entries[index].storage);
    free(g_entries); g_entries = NULL; g_entry_capacity = 0u;
    free(g_identities); g_identities = NULL; g_identity_capacity = 0u;
    free(g_capabilities); g_capabilities = NULL; g_capability_capacity = 0u;
    memset(&g_diagnostics, 0, sizeof(g_diagnostics));
    capability_session_advance();
    g_restore_fault = XG_RENDER_RESOURCE_RESTORE_FAULT_NONE;
    g_restore_fail_after = 0u;
    g_capability_restore_active = false;
    repository_unlock();
}

XgRenderResourceResult xg_render_resource_import(
        const XgRenderResourceImport *import,
        XgRenderResourceHandle *out_handle) {
    XgRenderResourceEntry *available = NULL;
    XgRenderResourceEntry *current = NULL;
    XgRenderResourceIdentityEntry *available_identity = NULL;
    XgRenderResourceIdentityEntry *registered_identity = NULL;
    const bool has_identity = import != NULL &&
        identity_present(&import->identity);
    uint64_t digest;
    size_t index;
    void *storage;

    if (import == NULL || out_handle == NULL || import->resource_id == 0u ||
        !resource_kind_valid(import->kind) ||
        !owner_kind_valid(import->owner_kind) ||
        !provenance_shape_valid(&import->provenance) ||
        import->owner_generation == 0u || import->bytes == NULL ||
        import->byte_count == 0u ||
        !descriptor_matches_import(import->kind, &import->provenance,
                                   &import->descriptor, import->byte_count) ||
        (import->state != XG_RENDER_RESOURCE_NATIVE_OWNED &&
         import->state != XG_RENDER_RESOURCE_GUEST_GPU_CANONICAL &&
         import->state != XG_RENDER_RESOURCE_GUEST_VISIBLE_MIRROR))
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    if (has_identity &&
        xg_render_resource_identity_id(&import->identity) != import->resource_id)
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    digest = xg_render_resource_digest(import->bytes, import->byte_count);
    if (digest == 0u || digest != import->content_digest)
        return XG_RENDER_RESOURCE_DIGEST_MISMATCH;
    repository_lock();
    if (!capability_matches_locked(
            &import->provenance, import->owner_kind,
            import->owner_generation, false, NULL)) {
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    }
    for (index = 0; index < g_identity_capacity; index++) {
        XgRenderResourceIdentityEntry *entry = &g_identities[index];

        if (!entry->occupied) {
            if (available_identity == NULL) available_identity = entry;
            continue;
        }
        if (entry->resource_id != import->resource_id) continue;
        registered_identity = entry;
        break;
    }
    if (registered_identity != NULL &&
        (registered_identity->has_identity != has_identity ||
         (has_identity && memcmp(&registered_identity->identity,
                                 &import->identity,
                                 sizeof(import->identity)) != 0))) {
        g_diagnostics.identity_collisions++;
        repository_unlock();
        return XG_RENDER_RESOURCE_IDENTITY_COLLISION;
    }
    if (registered_identity == NULL && available_identity == NULL) {
        const uint32_t next = g_identity_capacity;
        XgRenderResourceIdentityEntry *grown = next == UINT32_MAX ? NULL :
            xg_render_array_reserve(g_identities, sizeof(*g_identities),
                &g_identity_capacity, next + 1u, UINT32_MAX);
        if (!grown) { repository_unlock(); return XG_RENDER_RESOURCE_OUT_OF_MEMORY; }
        g_identities = grown;
        available_identity = &g_identities[next];
    }
    for (index = 0; index < g_entry_capacity; index++) {
        XgRenderResourceEntry *entry = &g_entries[index];
        if (!entry->occupied) {
            if (available == NULL) available = entry;
            continue;
        }
        if (entry->view.handle.resource_id != import->resource_id) continue;
        if (entry->view.current) current = entry;
    }
    if (current != NULL && current->view.content_digest == digest &&
        current->view.kind == import->kind &&
        current->view.owner_kind == import->owner_kind &&
        current->view.owner_generation == import->owner_generation &&
        provenance_equal(&current->view.provenance, &import->provenance) &&
        descriptor_equal(&current->view.descriptor, &import->descriptor) &&
        current->view.state == import->state) {
        *out_handle = current->view.handle;
        repository_unlock();
        return XG_RENDER_RESOURCE_OK;
    }
    if (available == NULL && current != NULL &&
        current->view.retain_count == 0u)
        available = current;
    if (g_next_generation == 0u) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPACITY_EXCEEDED;
    }
    if (available == NULL) {
        const uint32_t next = g_entry_capacity;
        const size_t current_index = current ? (size_t)(current - g_entries) : SIZE_MAX;
        XgRenderResourceEntry *grown = next == UINT32_MAX ? NULL :
            xg_render_array_reserve(g_entries, sizeof(*g_entries), &g_entry_capacity,
                next + 1u, UINT32_MAX);
        if (!grown) { repository_unlock(); return XG_RENDER_RESOURCE_OUT_OF_MEMORY; }
        g_entries = grown;
        available = &g_entries[next];
        current = current_index != SIZE_MAX ? &g_entries[current_index] : NULL;
    }
    storage = malloc(import->byte_count);
    if (storage == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_OUT_OF_MEMORY;
    }
    memcpy(storage, import->bytes, import->byte_count);
    if (current != NULL) {
        current->view.current = false;
        if (current->view.retain_count == 0u) retire_entry(current);
    }
    available->storage = storage;
    available->occupied = true;
    available->view = (XgRenderResourceView){
        .handle = { import->resource_id, g_next_generation++ },
        .kind = import->kind,
        .owner_kind = import->owner_kind,
        .owner_generation = import->owner_generation,
        .state = import->state,
        .provenance = import->provenance,
        .content_digest = digest,
        .bytes = storage,
        .byte_count = import->byte_count,
        .current = true,
        .has_identity = has_identity,
        .identity = import->identity,
        .descriptor = import->descriptor,
    };
    if (registered_identity == NULL) {
        available_identity->resource_id = import->resource_id;
        available_identity->identity = import->identity;
        available_identity->has_identity = has_identity;
        available_identity->occupied = true;
        g_diagnostics.registered_resource_ids++;
    }
    *out_handle = available->view.handle;
    g_diagnostics.imports++;
    g_diagnostics.generations++;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

static XgRenderResourceResult resource_import_begin(
        const XgRenderResourceImport *import,
        XgRenderResourceHandle *out_handle,
        bool allow_restore) {
    XgRenderResourceEntry *available = NULL;
    XgRenderResourceEntry *current = NULL;
    XgRenderResourceEntry *pending = NULL;
    XgRenderResourceIdentityEntry *available_identity = NULL;
    XgRenderResourceIdentityEntry *registered_identity = NULL;
    const bool has_identity = import != NULL &&
        identity_present(&import->identity);
    uint64_t digest;
    void *storage;

    if (import == NULL || out_handle == NULL || import->resource_id == 0u ||
        !resource_kind_valid(import->kind) ||
        !owner_kind_valid(import->owner_kind) ||
        !provenance_shape_valid(&import->provenance) ||
        import->owner_generation == 0u || import->bytes == NULL ||
        import->byte_count == 0u ||
        !descriptor_matches_import(import->kind, &import->provenance,
                                   &import->descriptor, import->byte_count) ||
        import->state != XG_RENDER_RESOURCE_IMPORTING_NATIVE)
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    if (has_identity &&
        xg_render_resource_identity_id(&import->identity) != import->resource_id)
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    digest = xg_render_resource_digest(import->bytes, import->byte_count);
    if (digest == 0u || digest != import->content_digest)
        return XG_RENDER_RESOURCE_DIGEST_MISMATCH;

    repository_lock();
    if (!capability_matches_locked(
            &import->provenance, import->owner_kind,
            import->owner_generation, allow_restore, NULL)) {
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    }
    for (size_t index = 0u;
         index < g_identity_capacity; ++index) {
        XgRenderResourceIdentityEntry *entry = &g_identities[index];
        if (!entry->occupied) {
            if (available_identity == NULL) available_identity = entry;
        } else if (entry->resource_id == import->resource_id) {
            registered_identity = entry;
            break;
        }
    }
    if (registered_identity != NULL &&
        (registered_identity->has_identity != has_identity ||
         (has_identity && memcmp(&registered_identity->identity,
                                 &import->identity,
                                 sizeof(import->identity)) != 0))) {
        g_diagnostics.identity_collisions++;
        repository_unlock();
        return XG_RENDER_RESOURCE_IDENTITY_COLLISION;
    }
    if (registered_identity == NULL && available_identity == NULL) {
        const uint32_t next = g_identity_capacity;
        XgRenderResourceIdentityEntry *grown = next == UINT32_MAX ? NULL :
            xg_render_array_reserve(g_identities, sizeof(*g_identities),
                &g_identity_capacity, next + 1u, UINT32_MAX);
        if (!grown) { repository_unlock(); return XG_RENDER_RESOURCE_OUT_OF_MEMORY; }
        g_identities = grown;
        available_identity = &g_identities[next];
    }
    for (size_t index = 0u;
         index < g_entry_capacity; ++index) {
        XgRenderResourceEntry *entry = &g_entries[index];
        if (!entry->occupied) {
            if (available == NULL) available = entry;
            continue;
        }
        if (entry->view.handle.resource_id != import->resource_id) continue;
        if (entry->view.current) current = entry;
        if (!entry->view.current &&
            entry->view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE)
            pending = entry;
    }
    /* Restore needs a private reservation even when current bytes match. */
    if (!allow_restore && current != NULL &&
        current->view.content_digest == digest &&
        current->view.kind == import->kind &&
        current->view.owner_kind == import->owner_kind &&
        current->view.owner_generation == import->owner_generation &&
        provenance_equal(&current->view.provenance, &import->provenance) &&
        descriptor_equal(&current->view.descriptor, &import->descriptor) &&
        current->view.state == XG_RENDER_RESOURCE_NATIVE_OWNED) {
        *out_handle = current->view.handle;
        repository_unlock();
        return XG_RENDER_RESOURCE_OK;
    }
    if (pending != NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_STATE;
    }
    if (g_next_generation == 0u) {
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPACITY_EXCEEDED;
    }
    if (available == NULL) {
        const uint32_t next = g_entry_capacity;
        XgRenderResourceEntry *grown = next == UINT32_MAX ? NULL :
            xg_render_array_reserve(g_entries, sizeof(*g_entries), &g_entry_capacity,
                next + 1u, UINT32_MAX);
        if (!grown) { repository_unlock(); return XG_RENDER_RESOURCE_OUT_OF_MEMORY; }
        g_entries = grown;
        available = &g_entries[next];
    }
    storage = malloc(import->byte_count);
    if (storage == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_OUT_OF_MEMORY;
    }
    memcpy(storage, import->bytes, import->byte_count);
    available->storage = storage;
    available->occupied = true;
    /* Boundary invalidation must never observe an unprotected reservation. */
    available->restore_staged = allow_restore;
    available->view = (XgRenderResourceView){
        .handle = { import->resource_id, g_next_generation++ },
        .kind = import->kind,
        .owner_kind = import->owner_kind,
        .owner_generation = import->owner_generation,
        .state = XG_RENDER_RESOURCE_IMPORTING_NATIVE,
        .provenance = import->provenance,
        .content_digest = digest,
        .bytes = storage,
        .byte_count = import->byte_count,
        .current = false,
        .has_identity = has_identity,
        .identity = import->identity,
        .descriptor = import->descriptor,
    };
    if (registered_identity == NULL) {
        available_identity->resource_id = import->resource_id;
        available_identity->identity = import->identity;
        available_identity->has_identity = has_identity;
        available_identity->occupied = true;
        g_diagnostics.registered_resource_ids++;
    }
    *out_handle = available->view.handle;
    g_diagnostics.import_starts++;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_import_begin(
        const XgRenderResourceImport *import,
        XgRenderResourceHandle *out_handle) {
    return resource_import_begin(import, out_handle, false);
}

XgRenderResourceResult xg_render_resource_import_commit(
        XgRenderResourceHandle handle) {
    XgRenderResourceEntry *entry;
    XgRenderResourceEntry *current = NULL;

    repository_lock();
    entry = find_entry(handle);
    if (entry == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (entry->view.current &&
        entry->view.state == XG_RENDER_RESOURCE_NATIVE_OWNED) {
        repository_unlock();
        return XG_RENDER_RESOURCE_OK;
    }
    if (entry->view.current ||
        entry->view.state != XG_RENDER_RESOURCE_IMPORTING_NATIVE) {
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_STATE;
    }
    for (size_t index = 0u;
         index < g_entry_capacity; ++index) {
        XgRenderResourceEntry *candidate = &g_entries[index];
        if (candidate != entry && candidate->occupied &&
            candidate->view.current &&
            candidate->view.handle.resource_id == handle.resource_id) {
            current = candidate;
            break;
        }
    }
    if (current != NULL) {
        current->view.current = false;
        if (current->view.retain_count == 0u) retire_entry(current);
    }
    entry->view.state = XG_RENDER_RESOURCE_NATIVE_OWNED;
    entry->view.current = true;
    g_diagnostics.imports++;
    g_diagnostics.generations++;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_import_commit_many(
        const XgRenderResourceHandle *handles, size_t handle_count) {
    XgRenderResourceResult result = XG_RENDER_RESOURCE_OK;

    if (handles == NULL && handle_count != 0u)
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    repository_lock();
    for (size_t index = 0u; index < handle_count; ++index) {
        XgRenderResourceEntry *entry = find_entry(handles[index]);

        if (entry == NULL) {
            result = XG_RENDER_RESOURCE_NOT_FOUND;
            break;
        }
        for (size_t previous = 0u; previous < index; ++previous) {
            if (handles[previous].resource_id == handles[index].resource_id &&
                handles[previous].generation != handles[index].generation) {
                result = XG_RENDER_RESOURCE_INVALID_STATE;
                break;
            }
        }
        if (result != XG_RENDER_RESOURCE_OK) break;
        if (!capability_matches_locked(
                &entry->view.provenance, entry->view.owner_kind,
                entry->view.owner_generation, false, NULL)) {
            result = XG_RENDER_RESOURCE_INVALID_STATE;
            break;
        }
        if (entry->view.current &&
            entry->view.state == XG_RENDER_RESOURCE_NATIVE_OWNED)
            continue;
        if (entry->view.current ||
            entry->view.state != XG_RENDER_RESOURCE_IMPORTING_NATIVE) {
            result = XG_RENDER_RESOURCE_INVALID_STATE;
            break;
        }
    }
    if (result == XG_RENDER_RESOURCE_OK) {
        for (size_t index = 0u; index < handle_count; ++index) {
            XgRenderResourceEntry *entry = find_entry(handles[index]);
            XgRenderResourceEntry *current = NULL;

            if (entry->view.current) continue;
            for (size_t candidate_index = 0u;
                 candidate_index < g_entry_capacity;
                 ++candidate_index) {
                XgRenderResourceEntry *candidate =
                    &g_entries[candidate_index];

                if (candidate != entry && candidate->occupied &&
                    candidate->view.current &&
                    candidate->view.handle.resource_id ==
                        entry->view.handle.resource_id) {
                    current = candidate;
                    break;
                }
            }
            if (current != NULL) {
                current->view.current = false;
                if (current->view.retain_count == 0u)
                    retire_entry(current);
            }
            entry->view.state = XG_RENDER_RESOURCE_NATIVE_OWNED;
            entry->view.current = true;
            g_diagnostics.imports++;
            g_diagnostics.generations++;
        }
    }
    repository_unlock();
    return result;
}

XgRenderResourceResult xg_render_resource_import_cancel(
        XgRenderResourceHandle handle) {
    XgRenderResourceEntry *entry;

    repository_lock();
    entry = find_entry(handle);
    if (entry == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (entry->view.current ||
        entry->view.state != XG_RENDER_RESOURCE_IMPORTING_NATIVE ||
        entry->view.retain_count != 0u) {
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_STATE;
    }
    {
        const uint64_t resource_id = entry->view.handle.resource_id;

        retire_entry(entry);
        reclaim_identity_if_unused(resource_id);
    }
    g_diagnostics.import_cancellations++;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_import_native(
        const XgRenderResourceImport *import,
        XgRenderResourceHandle *out_handle) {
    XgRenderResourceImport pending;
    XgRenderResourceResult result;

    if (import == NULL || import->state != XG_RENDER_RESOURCE_NATIVE_OWNED)
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    pending = *import;
    pending.state = XG_RENDER_RESOURCE_IMPORTING_NATIVE;
    result = resource_import_begin(&pending, out_handle, false);
    if (result != XG_RENDER_RESOURCE_OK) return result;
    result = xg_render_resource_import_commit(*out_handle);
    if (result != XG_RENDER_RESOURCE_OK)
        (void)xg_render_resource_import_cancel(*out_handle);
    return result;
}

void xg_render_resource_restore_fault_inject(
        XgRenderResourceRestoreFault fault, uint32_t fail_after) {
    repository_lock();
    g_restore_fault = fault;
    g_restore_fail_after = fail_after;
    repository_unlock();
}

XgRenderResourceResult xg_render_resource_restore_stage(
        const XgRenderResourceImport *import,
        XgRenderResourceHandle *out_handle) {
    XgRenderResourceImport pending;

    if (import == NULL || import->state != XG_RENDER_RESOURCE_NATIVE_OWNED)
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    repository_lock();
    if (g_restore_fault != XG_RENDER_RESOURCE_RESTORE_FAULT_NONE) {
        if (g_restore_fail_after != 0u) {
            --g_restore_fail_after;
        } else {
            const XgRenderResourceRestoreFault fault = g_restore_fault;
            g_restore_fault = XG_RENDER_RESOURCE_RESTORE_FAULT_NONE;
            repository_unlock();
            return fault == XG_RENDER_RESOURCE_RESTORE_FAULT_ALLOCATION
                ? XG_RENDER_RESOURCE_OUT_OF_MEMORY
                : XG_RENDER_RESOURCE_CAPACITY_EXCEEDED;
        }
    }
    repository_unlock();
    pending = *import;
    pending.state = XG_RENDER_RESOURCE_IMPORTING_NATIVE;
    return resource_import_begin(&pending, out_handle, true);
}

XgRenderResourceResult xg_render_resource_restore_commit(
        XgRenderResourceHandle handle) {
    XgRenderResourceEntry *entry;
    XgRenderResourceEntry *current = NULL;

    repository_lock();
    entry = find_entry(handle);
    if (entry == NULL) {
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (entry->view.current &&
        entry->view.state == XG_RENDER_RESOURCE_NATIVE_OWNED) {
        repository_unlock();
        return XG_RENDER_RESOURCE_OK;
    }
    if (!entry->restore_staged || entry->view.current ||
        entry->view.state != XG_RENDER_RESOURCE_IMPORTING_NATIVE) {
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_STATE;
    }
    for (size_t index = 0u;
         index < g_entry_capacity; ++index) {
        XgRenderResourceEntry *candidate = &g_entries[index];
        if (candidate != entry && candidate->occupied &&
            candidate->view.current &&
            candidate->view.handle.resource_id == handle.resource_id) {
            current = candidate;
            break;
        }
    }
    if (current != NULL) {
        current->view.current = false;
        if (current->view.retain_count == 0u) retire_entry(current);
    }
    entry->restore_staged = false;
    entry->view.state = XG_RENDER_RESOURCE_NATIVE_OWNED;
    entry->view.current = true;
    g_diagnostics.imports++;
    g_diagnostics.generations++;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

void xg_render_resource_restore_cancel(XgRenderResourceHandle handle) {
    XgRenderResourceEntry *entry;

    repository_lock();
    entry = find_entry(handle);
    if (entry != NULL && entry->restore_staged && !entry->view.current &&
        entry->view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE &&
        entry->view.retain_count == 0u) {
        const uint64_t resource_id = entry->view.handle.resource_id;
        retire_entry(entry);
        reclaim_identity_if_unused(resource_id);
        g_diagnostics.import_cancellations++;
    }
    repository_unlock();
}

XgRenderResourceResult xg_render_resource_retire_current(
        XgRenderResourceHandle handle) {
    XgRenderResourceEntry *entry;

    if (handle.resource_id == 0u || handle.generation == 0u)
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    repository_lock();
    entry = find_entry(handle);
    if (entry == NULL) {
        g_diagnostics.stale_accesses++;
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (!entry->view.current) {
        g_diagnostics.stale_accesses++;
        repository_unlock();
        return XG_RENDER_RESOURCE_STALE;
    }
    entry->view.current = false;
    g_diagnostics.invalidations++;
    if (entry->view.retain_count == 0u) {
        const uint64_t resource_id = entry->view.handle.resource_id;

        retire_entry(entry);
        reclaim_identity_if_unused(resource_id);
    }
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_rollback_current(
        XgRenderResourceHandle new_handle,
        XgRenderResourceHandle retained_old_handle) {
    XgRenderResourceEntry *new_entry;
    XgRenderResourceEntry *old_entry;

    if (new_handle.resource_id == 0u || new_handle.generation == 0u ||
        retained_old_handle.resource_id == 0u ||
        retained_old_handle.generation == 0u ||
        new_handle.resource_id != retained_old_handle.resource_id)
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    repository_lock();
    new_entry = find_entry(new_handle);
    old_entry = find_entry(retained_old_handle);
    if (new_entry == NULL || old_entry == NULL) {
        g_diagnostics.stale_accesses++;
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (!new_entry->view.current) {
        g_diagnostics.stale_accesses++;
        repository_unlock();
        return XG_RENDER_RESOURCE_STALE;
    }
    /* import_begin may deduplicate against the predecessor.  It is still
     * important to compare currentness under this lock, but there is no new
     * generation to retire in that case. */
    if (new_entry == old_entry) {
        const bool valid = old_entry->view.retain_count != 0u &&
            old_entry->view.state == XG_RENDER_RESOURCE_NATIVE_OWNED &&
            !old_entry->restore_staged;
        repository_unlock();
        return valid ? XG_RENDER_RESOURCE_OK
                     : XG_RENDER_RESOURCE_INVALID_STATE;
    }
    if (old_entry->view.current || old_entry->view.retain_count == 0u ||
        new_entry->view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        old_entry->view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        new_entry->restore_staged || old_entry->restore_staged ||
        new_entry->view.has_identity != old_entry->view.has_identity ||
        (new_entry->view.has_identity &&
         memcmp(&new_entry->view.identity, &old_entry->view.identity,
                sizeof(new_entry->view.identity)) != 0)) {
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_STATE;
    }
    /* new_entry being current is the compare-and-swap guard: if anybody has
     * installed a third generation, no current state is changed here. */
    new_entry->view.current = false;
    old_entry->view.current = true;
    g_diagnostics.invalidations++;
    if (new_entry->view.retain_count == 0u) retire_entry(new_entry);
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_retain(XgRenderResourceHandle handle) {
    XgRenderResourceEntry *entry;
    repository_lock();
    entry = find_entry(handle);
    if (entry == NULL) {
        g_diagnostics.stale_accesses++;
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (entry->view.retain_count == UINT32_MAX) {
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPACITY_EXCEEDED;
    }
    entry->view.retain_count++;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_acquire_current(
        XgRenderResourceHandle handle, uint64_t content_digest) {
    XgRenderResourceEntry *entry;

    repository_lock();
    if (handle.resource_id == 0u || handle.generation == 0u ||
        content_digest == 0u) {
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    }
    entry = find_entry(handle);
    if (entry == NULL) {
        g_diagnostics.stale_accesses++;
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (!entry->view.current) {
        g_diagnostics.stale_accesses++;
        repository_unlock();
        return XG_RENDER_RESOURCE_STALE;
    }
    if (entry->view.content_digest != content_digest) {
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_DIGEST_MISMATCH;
    }
    if (entry->view.retain_count == UINT32_MAX) {
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPACITY_EXCEEDED;
    }
    entry->view.retain_count++;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_acquire_snapshot(
        XgRenderResourceHandle handle, uint64_t content_digest) {
    XgRenderResourceEntry *entry;

    repository_lock();
    if (handle.resource_id == 0u || handle.generation == 0u ||
        content_digest == 0u) {
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    }
    entry = find_entry(handle);
    if (entry == NULL) {
        g_diagnostics.stale_accesses++;
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (!entry->view.current && entry->view.retain_count == 0u) {
        g_diagnostics.stale_accesses++;
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_STALE;
    }
    if (entry->view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        entry->restore_staged) {
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_STATE;
    }
    if (entry->view.content_digest != content_digest) {
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_DIGEST_MISMATCH;
    }
    if (entry->view.retain_count == UINT32_MAX) {
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_CAPACITY_EXCEEDED;
    }
    entry->view.retain_count++;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_release(XgRenderResourceHandle handle) {
    XgRenderResourceEntry *entry;
    repository_lock();
    entry = find_entry(handle);
    if (entry == NULL) {
        g_diagnostics.stale_accesses++;
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    if (entry->view.retain_count == 0u) {
        g_diagnostics.ownership_violations++;
        g_diagnostics.reference_failures++;
        repository_unlock();
        return XG_RENDER_RESOURCE_INVALID_STATE;
    }
    entry->view.retain_count--;
    if (!entry->view.current && entry->view.retain_count == 0u) {
        const uint64_t resource_id = entry->view.handle.resource_id;

        retire_entry(entry);
        reclaim_identity_if_unused(resource_id);
    }
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

XgRenderResourceResult xg_render_resource_view(XgRenderResourceHandle handle,
        XgRenderResourceView *out_view) {
    XgRenderResourceEntry *entry;
    if (out_view == NULL) return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
    repository_lock();
    entry = find_entry(handle);
    if (entry == NULL) {
        g_diagnostics.stale_accesses++;
        repository_unlock();
        return XG_RENDER_RESOURCE_NOT_FOUND;
    }
    *out_view = entry->view;
    repository_unlock();
    return XG_RENDER_RESOURCE_OK;
}

void xg_render_resource_invalidate_owner(XgRenderResourceOwnerKind owner_kind,
        uint64_t owner_generation) {
    size_t index;
    if (!owner_kind_valid(owner_kind) || owner_generation == 0u) return;
    repository_lock();
    for (index = 0; index < g_entry_capacity; index++) {
        XgRenderResourceEntry *entry = &g_entries[index];
        if (!entry->occupied || entry->view.owner_kind != owner_kind ||
            entry->view.owner_generation != owner_generation ||
            entry->restore_staged)
            continue;
        entry->view.current = false;
        g_diagnostics.invalidations++;
        if (entry->view.retain_count == 0u) {
            const uint64_t resource_id = entry->view.handle.resource_id;

            retire_entry(entry);
            reclaim_identity_if_unused(resource_id);
        }
    }
    for (index = 0u; index < g_capability_capacity; ++index) {
        XgRenderResourceCapabilityEntry *capability = &g_capabilities[index];

        if (capability->occupied && capability->live &&
            !capability->restore_staged &&
            capability->metadata.owner_kind == owner_kind &&
            capability->metadata.owner_generation == owner_generation) {
            capability->retire_pending = true;
            retire_pending_capability_if_unused_locked(capability->capability);
        }
    }
    repository_unlock();
}

void xg_render_resource_invalidate_transient(void) {
    xg_render_resource_invalidate_from_owner(XG_RENDER_RESOURCE_OWNER_MODULE);
}

void xg_render_resource_invalidate_scene_boundary(void) {
    xg_render_resource_invalidate_from_owner(XG_RENDER_RESOURCE_OWNER_SCENE);
}

void xg_render_resource_invalidate_from_owner(
        XgRenderResourceOwnerKind first_owner_kind) {
    size_t index;
    if (!owner_kind_valid(first_owner_kind)) return;
    repository_lock();
    for (index = 0; index < g_entry_capacity; index++) {
        XgRenderResourceEntry *entry = &g_entries[index];
        if (!entry->occupied ||
            entry->view.owner_kind < first_owner_kind || entry->restore_staged)
            continue;
        entry->view.current = false;
        g_diagnostics.invalidations++;
        if (entry->view.retain_count == 0u) {
            const uint64_t resource_id = entry->view.handle.resource_id;

            retire_entry(entry);
            reclaim_identity_if_unused(resource_id);
        }
    }
    for (index = 0u; index < g_capability_capacity; ++index) {
        XgRenderResourceCapabilityEntry *capability = &g_capabilities[index];

        if (capability->occupied && capability->live &&
            !capability->restore_staged &&
            capability->metadata.owner_kind >= first_owner_kind) {
            capability->retire_pending = true;
            retire_pending_capability_if_unused_locked(capability->capability);
        }
    }
    repository_unlock();
}

void xg_render_resource_repository_diagnostics(
        XgRenderResourceDiagnostics *out_diagnostics) {
    size_t index;
    if (out_diagnostics == NULL) return;
    repository_lock();
    *out_diagnostics = g_diagnostics;
    for (index = 0; index < g_entry_capacity; index++) {
        if (!g_entries[index].occupied) continue;
        out_diagnostics->live_resources++;
        if (g_entries[index].view.retain_count != 0u)
            out_diagnostics->retained_resources++;
    }
    for (index = 0u; index < g_capability_capacity; ++index) {
        if (!g_capabilities[index].occupied) continue;
        out_diagnostics->capability_slots++;
        if (g_capabilities[index].live) out_diagnostics->live_capabilities++;
    }
    repository_unlock();
}
