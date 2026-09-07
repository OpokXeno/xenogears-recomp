#include "xg_render_vram_resources.h"

#include "psx_sha256.h"
#include "xg_render_semantic_compositor.h"
#include "xg_render_vram_journal.h"

#include <stdlib.h>
#include <string.h>

enum {
    XG_RENDER_VRAM_MAX_PAYLOAD_SIZE = 1024u * 512u * 2u,
    XG_RENDER_VRAM_CHECKPOINT_MAGIC = 0x4f544758u,
    XG_RENDER_VRAM_CHECKPOINT_HEADER_SIZE = 40u,
    XG_RENDER_VRAM_CHECKPOINT_ENTRY_SIZE =
        108u + XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE,
};

typedef struct XgRenderVramResourcePending {
    uint64_t owner_generation;
    uint64_t sequence_start;
    uint32_t stream_address;
    uint32_t image_count;
    uint32_t clut_count;
    uint8_t *stream_bytes;
    size_t stream_byte_count;
    uint16_t stream_x;
    uint16_t stream_y;
    uint16_t stream_width;
    uint16_t stream_height;
    XgRenderResourceKind required_kind;
    XgRenderResourceProvenance provenance;
    bool active;
    bool blocked;
    bool streaming;
} XgRenderVramResourcePending;

typedef struct XgRenderVramResourcePublication {
    XgRenderResourceHandle handle;
    uint64_t content_digest;
    uint64_t sequence;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    XgRenderResourceKind kind;
    XgRenderResourceProvenance provenance;
    bool occupied;
} XgRenderVramResourcePublication;

typedef struct XgRenderVramResourceCachedPublication {
    XgRenderResourceIdentity identity;
    XgRenderResourceCapabilityMetadata authority;
    uint64_t content_digest;
    uint8_t *bytes;
    size_t byte_count;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    XgRenderResourceKind kind;
    bool occupied;
} XgRenderVramResourceCachedPublication;

typedef struct XgRenderVramResourceMutationUpdate {
    XgRenderResourceHandle previous_handle;
    XgRenderResourceHandle replacement_handle;
    XgRenderResourceIdentity previous_identity;
    XgRenderResourceIdentity replacement_identity;
    uint8_t *bytes;
    size_t byte_count;
    uint64_t content_digest;
    uint32_t publication_index;
    uint32_t cache_index;
} XgRenderVramResourceMutationUpdate;

typedef struct XgRenderVramResourceCheckpointEntry {
    XgRenderResourceIdentity identity;
    uint64_t content_digest;
    uint64_t sequence;
    uint64_t owner_generation;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    XgRenderResourceKind kind;
    XgRenderResourceOwnerKind owner_kind;
    XgRenderResourceState state;
    XgRenderResourceProvenance provenance;
    XgRenderResourceProvenance restored_provenance;
    XgRenderResourceCapabilityCheckpoint authority;
    XgRenderResourceCapabilityMetadata authority_metadata;
    const uint8_t *bytes;
    size_t byte_count;
} XgRenderVramResourceCheckpointEntry;

struct XgRenderVramResourceCheckpointRestore {
    XgRenderVramResourcePublication
        publications[XG_RENDER_VRAM_RESOURCE_CAPACITY];
    XgRenderVramResourceCachedPublication
        cached_publications[XG_RENDER_VRAM_RESOURCE_CAPACITY];
    XgRenderVramResourceSnapshot snapshot;
    uint64_t next_sequence;
    uint32_t publication_count;
};

static XgRenderVramResourcePending g_pending;
static XgRenderVramResourcePublication
    g_publications[XG_RENDER_VRAM_RESOURCE_CAPACITY];
static XgRenderVramResourcePublication
    g_staged_publications[XG_RENDER_VRAM_RESOURCE_CAPACITY];
static XgRenderVramResourceCachedPublication
    g_cached_publications[XG_RENDER_VRAM_RESOURCE_CAPACITY];
static uint32_t g_staged_publication_count;
static XgRenderVramResourceSnapshot g_snapshot;
static uint64_t g_next_sequence = 1u;
static XgRenderVramResourceRetirementSnapshot
    g_retirements[XG_RENDER_VRAM_RETIREMENT_CAPACITY];
static uint64_t g_retirement_total;

static bool rebind_cached_publications(uint64_t owner_generation);

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
    const uint16_t value = (uint16_t)(*cursor)[0] |
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

static bool valid_kind(XgRenderResourceKind kind) {
    return kind == XG_RENDER_RESOURCE_TEXTURE ||
        kind == XG_RENDER_RESOURCE_CLUT;
}

static bool valid_provenance(
        const XgRenderResourceProvenance *provenance,
        uint64_t owner_generation) {
    return provenance != NULL && !provenance->synthetic &&
        provenance->receipt != 0u && provenance->capability != 0u &&
        (provenance->kind == XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT ||
         provenance->kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE) &&
        xg_render_resource_capability_validate(
            provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
            owner_generation, NULL) == XG_RENDER_RESOURCE_CAPABILITY_OK;
}

static bool same_provenance(const XgRenderResourceProvenance *left,
                            const XgRenderResourceProvenance *right) {
    return left->kind == right->kind && left->receipt == right->receipt &&
        left->capability == right->capability &&
        left->synthetic == right->synthetic;
}

static bool bind_pending_provenance(
        const XgRenderVramResourceServices *services) {
    if (services == NULL || !valid_provenance(
            &services->provenance, g_pending.owner_generation))
        return false;
    if (g_pending.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_NONE) {
        g_pending.provenance = services->provenance;
        return true;
    }
    return same_provenance(&g_pending.provenance, &services->provenance);
}

static bool regions_overlap(uint16_t left_x, uint16_t left_y,
                            uint16_t left_width, uint16_t left_height,
                            uint16_t right_x, uint16_t right_y,
                            uint16_t right_width, uint16_t right_height) {
    return (uint32_t)left_x < (uint32_t)right_x + right_width &&
        (uint32_t)right_x < (uint32_t)left_x + left_width &&
        (uint32_t)left_y < (uint32_t)right_y + right_height &&
        (uint32_t)right_y < (uint32_t)left_y + left_height;
}

static void reject_pending(void) {
    if (g_pending.active) {
        g_pending.blocked = true;
        g_snapshot.loader_blocked = true;
    }
    g_snapshot.rejected_operations++;
}

static bool same_handle(XgRenderResourceHandle left,
                        XgRenderResourceHandle right) {
    return left.resource_id == right.resource_id &&
        left.generation == right.generation;
}

static void cancel_staged_imports(void) {
    for (uint32_t index = 0u; index < g_staged_publication_count; ++index) {
        const XgRenderVramResourcePublication *publication =
            &g_staged_publications[index];
        bool duplicate = false;

        if (!publication->occupied) continue;
        for (uint32_t previous = 0u; previous < index; ++previous) {
            if (g_staged_publications[previous].occupied &&
                same_handle(g_staged_publications[previous].handle,
                            publication->handle)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            (void)xg_render_resource_import_cancel(publication->handle);
    }
}

static void clear_pending(bool committed) {
    cancel_staged_imports();
    if (!committed && g_pending.active && g_pending.sequence_start != 0u)
        g_next_sequence = g_pending.sequence_start;
    free(g_pending.stream_bytes);
    memset(&g_pending, 0, sizeof(g_pending));
    memset(g_staged_publications, 0, sizeof(g_staged_publications));
    g_staged_publication_count = 0u;
    g_snapshot.loader_active = false;
    g_snapshot.loader_blocked = false;
}

static void retire_publication(XgRenderVramResourcePublication *publication) {
    if (publication == NULL || !publication->occupied) return;
    (void)xg_render_resource_retire_current(publication->handle);
    memset(publication, 0, sizeof(*publication));
}

static void record_publication_retirement(
        const XgRenderVramResourcePublication *publication,
        uint32_t reason) {
    if (publication == NULL || !publication->occupied) return;
    g_retirements[g_retirement_total % XG_RENDER_VRAM_RETIREMENT_CAPACITY] =
        (XgRenderVramResourceRetirementSnapshot){
            .sequence = g_retirement_total + 1u,
            .resource_id = publication->handle.resource_id,
            .resource_generation = publication->handle.generation,
            .content_digest = publication->content_digest,
            .vram_generation = g_snapshot.vram_generation,
            .x = publication->x,
            .y = publication->y,
            .width = publication->width,
            .height = publication->height,
            .kind = publication->kind,
            .reason = reason,
        };
    g_retirement_total++;
}

static void clear_cached_publications(void) {
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index)
        free(g_cached_publications[index].bytes);
    memset(g_cached_publications, 0, sizeof(g_cached_publications));
}

static void reset_publications(uint64_t owner_generation) {
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        record_publication_retirement(&g_publications[index], 3u);
        retire_publication(&g_publications[index]);
    }
    g_snapshot.publication_count = 0u;
    g_snapshot.owner_generation = owner_generation;
    g_next_sequence = 1u;
}

void xg_render_vram_resources_reset(void) {
    cancel_staged_imports();
    free(g_pending.stream_bytes);
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        record_publication_retirement(&g_publications[index], 4u);
        retire_publication(&g_publications[index]);
    }
    memset(&g_pending, 0, sizeof(g_pending));
    memset(g_staged_publications, 0, sizeof(g_staged_publications));
    clear_cached_publications();
    g_staged_publication_count = 0u;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    memset(g_retirements, 0, sizeof(g_retirements));
    g_retirement_total = 0u;
    g_next_sequence = 1u;
}

void xg_render_vram_resources_scene_boundary(uint64_t owner_generation) {
    const uint64_t vram_generation = g_snapshot.vram_generation;
    const uint32_t cached_publication_count =
        g_snapshot.cached_publication_count;

    cancel_staged_imports();
    free(g_pending.stream_bytes);
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        record_publication_retirement(&g_publications[index], 4u);
        retire_publication(&g_publications[index]);
    }
    memset(&g_pending, 0, sizeof(g_pending));
    memset(g_staged_publications, 0, sizeof(g_staged_publications));
    g_staged_publication_count = 0u;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.vram_generation = vram_generation;
    g_snapshot.cached_publication_count = cached_publication_count;
    g_next_sequence = 1u;
    if (owner_generation != 0u)
        (void)rebind_cached_publications(owner_generation);
}

static void sha_update_u16(psx_sha256_ctx *sha, uint16_t value) {
    const uint8_t bytes[] = {
        (uint8_t)value,
        (uint8_t)(value >> 8u),
    };
    psx_sha256_update(sha, bytes, sizeof(bytes));
}

static void sha_update_u32(psx_sha256_ctx *sha, uint32_t value) {
    uint8_t bytes[4];

    for (uint32_t index = 0u; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    psx_sha256_update(sha, bytes, sizeof(bytes));
}

static XgRenderResourceDescriptor vram_descriptor(
        XgRenderResourceKind kind, uint16_t x, uint16_t y,
        uint16_t width, uint16_t height) {
    return (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        /* Upload payloads are canonical VRAM words.  Indexed interpretation
         * remains draw metadata; the immutable backing is therefore RGB555. */
        .pixel_format = kind == XG_RENDER_RESOURCE_CLUT
            ? XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555
            : XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = width,
        .height = height,
        .row_pitch = (uint32_t)width * sizeof(uint16_t),
        .vram_x = x,
        .vram_y = y,
        .vram_width = width,
        .vram_height = height,
        .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
        .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
        .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
        .flags = XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION,
    };
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

static void sha_update_u64(psx_sha256_ctx *sha, uint64_t value) {
    uint8_t bytes[8];

    for (uint32_t index = 0u; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    psx_sha256_update(sha, bytes, sizeof(bytes));
}

static bool content_identity(XgRenderResourceKind kind,
                             uint16_t x, uint16_t y,
                             uint16_t width, uint16_t height,
                             const uint8_t *bytes, size_t byte_count,
                             XgRenderResourceIdentity *out_identity) {
    static const uint8_t domain[] = "xg-scene-vram-content-v2";
    psx_sha256_ctx sha;
    const uint8_t encoded_kind = (uint8_t)kind;
    const XgRenderResourceDescriptor descriptor =
        vram_descriptor(kind, x, y, width, height);

    if (!valid_kind(kind) || bytes == NULL || byte_count == 0u ||
        out_identity == NULL)
        return false;
    psx_sha256_init(&sha);
    psx_sha256_update(&sha, domain, sizeof(domain) - 1u);
    psx_sha256_update(&sha, &encoded_kind, sizeof(encoded_kind));
    sha_update_u16(&sha, x);
    sha_update_u16(&sha, y);
    sha_update_u16(&sha, width);
    sha_update_u16(&sha, height);
    sha_update_u32(&sha, descriptor.version);
    sha_update_u32(&sha, (uint32_t)descriptor.pixel_format);
    sha_update_u32(&sha, descriptor.width);
    sha_update_u32(&sha, descriptor.height);
    sha_update_u32(&sha, descriptor.row_pitch);
    sha_update_u32(&sha, descriptor.vram_x);
    sha_update_u32(&sha, descriptor.vram_y);
    sha_update_u32(&sha, descriptor.vram_width);
    sha_update_u32(&sha, descriptor.vram_height);
    sha_update_u32(&sha, (uint32_t)descriptor.sampler);
    sha_update_u32(&sha, (uint32_t)descriptor.wrap_u);
    sha_update_u32(&sha, (uint32_t)descriptor.wrap_v);
    sha_update_u32(&sha, descriptor.flags);
    sha_update_u64(&sha, byte_count);
    psx_sha256_update(&sha, bytes, byte_count);
    psx_sha256_final(&sha, out_identity->bytes);
    return xg_render_resource_identity_id(out_identity) != 0u;
}

XgRenderVramResourceResult xg_render_vram_resources_begin(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        uint64_t owner_generation, const XgRenderVramResourceServices *services) {
    const uint32_t stream_address = cpu != NULL ? cpu->gpr[4] : 0u;

    if (cpu == NULL || services == NULL ||
        services->authorize_guest_range == NULL || owner_generation == 0u ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !valid_provenance(&services->provenance, owner_generation)) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    }
    if (g_pending.active) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    if (!services->authorize_guest_range(
            stream_address, 8u, 4u, false)) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_VRAM_RESOURCE_INVALID_RANGE;
    }
    if (g_snapshot.owner_generation != owner_generation)
        reset_publications(owner_generation);
    g_pending = (XgRenderVramResourcePending){
        .owner_generation = owner_generation,
        .sequence_start = g_next_sequence,
        .stream_address = stream_address,
        .required_kind = XG_RENDER_RESOURCE_TEXTURE,
        .provenance = services->provenance,
        .active = true,
    };
    memset(g_staged_publications, 0, sizeof(g_staged_publications));
    g_staged_publication_count = 0u;
    g_snapshot.loader_active = true;
    g_snapshot.loader_blocked = false;
    return XG_RENDER_VRAM_RESOURCE_OK;
}

XgRenderVramResourceResult xg_render_vram_image_begin(
        GuestRenderRenderMode render_mode, uint64_t owner_generation) {
    if (owner_generation == 0u ||
        render_mode != GUEST_RENDER_RENDER_NATIVE) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    }
    if (g_pending.active) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    if (g_snapshot.owner_generation != owner_generation)
        reset_publications(owner_generation);
    g_pending = (XgRenderVramResourcePending){
        .owner_generation = owner_generation,
        .sequence_start = g_next_sequence,
        .required_kind = XG_RENDER_RESOURCE_TEXTURE,
        .active = true,
    };
    memset(g_staged_publications, 0, sizeof(g_staged_publications));
    g_staged_publication_count = 0u;
    g_snapshot.loader_active = true;
    g_snapshot.loader_blocked = false;
    return XG_RENDER_VRAM_RESOURCE_OK;
}

XgRenderVramResourceResult xg_render_vram_clut_begin(
        GuestRenderRenderMode render_mode, uint64_t owner_generation) {
    XgRenderVramResourceResult result = xg_render_vram_image_begin(
        render_mode, owner_generation);

    if (result == XG_RENDER_VRAM_RESOURCE_OK)
        g_pending.required_kind = XG_RENDER_RESOURCE_CLUT;
    return result;
}

XgRenderVramResourceResult xg_render_vram_stream_begin(
        XgRenderResourceKind kind, GuestRenderRenderMode render_mode,
        uint64_t owner_generation) {
    XgRenderVramResourceResult result;

    if (!valid_kind(kind)) {
        g_snapshot.rejected_operations++;
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    }
    result = kind == XG_RENDER_RESOURCE_TEXTURE
        ? xg_render_vram_image_begin(render_mode, owner_generation)
        : xg_render_vram_clut_begin(render_mode, owner_generation);
    if (result == XG_RENDER_VRAM_RESOURCE_OK) g_pending.streaming = true;
    return result;
}

static bool read_region(CPUState *cpu,
                        const XgRenderVramResourceServices *services,
                        uint16_t x, uint16_t y,
                        uint16_t width, uint16_t height,
                        uint32_t payload_address,
                        uint8_t **out_bytes, size_t *out_byte_count) {
    uint64_t byte_count;
    uint8_t *bytes;

    if (cpu == NULL || cpu->read_byte == NULL || services == NULL ||
        services->authorize_guest_range == NULL || width == 0u || height == 0u ||
        (uint32_t)x + width > 1024u || (uint32_t)y + height > 512u)
        return false;
    byte_count = (uint64_t)width * height * 2u;
    if (byte_count == 0u || byte_count > XG_RENDER_VRAM_MAX_PAYLOAD_SIZE ||
        !services->authorize_guest_range(
            payload_address, (uint32_t)byte_count, 2u, false))
        return false;
    bytes = (uint8_t *)malloc((size_t)byte_count);
    if (bytes == NULL) return false;
    for (size_t index = 0u; index < (size_t)byte_count; ++index)
        bytes[index] = cpu->read_byte(payload_address + (uint32_t)index);
    *out_bytes = bytes;
    *out_byte_count = (size_t)byte_count;
    return true;
}

static bool read_upload(CPUState *cpu, const XgRenderVramResourceServices *services,
                         uint16_t *out_x, uint16_t *out_y,
                         uint16_t *out_width, uint16_t *out_height,
                         uint8_t **out_bytes, size_t *out_byte_count) {
    const uint32_t rect_address = cpu->gpr[4];
    const uint32_t payload_address = cpu->gpr[5];
    int16_t signed_x;
    int16_t signed_y;
    int16_t signed_width;
    int16_t signed_height;

    if (cpu->read_half == NULL ||
        !services->authorize_guest_range(rect_address, 8u, 2u, false))
        return false;
    signed_x = (int16_t)cpu->read_half(rect_address);
    signed_y = (int16_t)cpu->read_half(rect_address + 2u);
    signed_width = (int16_t)cpu->read_half(rect_address + 4u);
    signed_height = (int16_t)cpu->read_half(rect_address + 6u);
    if (signed_x < 0 || signed_y < 0 || signed_width <= 0 ||
        signed_height <= 0 ||
        !read_region(cpu, services,
                     (uint16_t)signed_x, (uint16_t)signed_y,
                     (uint16_t)signed_width, (uint16_t)signed_height,
                     payload_address, out_bytes, out_byte_count))
        return false;
    *out_x = (uint16_t)signed_x;
    *out_y = (uint16_t)signed_y;
    *out_width = (uint16_t)signed_width;
    *out_height = (uint16_t)signed_height;
    return true;
}

static XgRenderVramResourceResult stage_upload(
        XgRenderResourceKind kind, uint64_t owner_generation,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        const uint8_t *bytes, size_t byte_count) {
    XgRenderVramResourcePublication *publication = NULL;
    XgRenderResourceIdentity identity;
    XgRenderResourceImport import;
    XgRenderResourceHandle handle;
    bool reused = false;

    if (!valid_kind(kind) || bytes == NULL || byte_count == 0u) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    }
    if (!g_pending.active || g_pending.blocked || owner_generation == 0u ||
        owner_generation != g_pending.owner_generation) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    for (uint32_t index = 0u; index < g_staged_publication_count; ++index) {
        XgRenderVramResourcePublication *candidate = &g_staged_publications[index];

        if (candidate->kind == kind &&
            candidate->x == x && candidate->y == y &&
            candidate->width == width && candidate->height == height) {
            publication = candidate;
            break;
        }
    }
    if (publication == NULL &&
        g_staged_publication_count < XG_RENDER_VRAM_RESOURCE_CAPACITY)
        publication = &g_staged_publications[g_staged_publication_count++];
    if (publication == NULL) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_CAPACITY_EXCEEDED;
    }
    if (g_next_sequence == 0u) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_CAPACITY_EXCEEDED;
    }
    if (!content_identity(kind, x, y, width, height,
                          bytes, byte_count, &identity)) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
    }
    import = (XgRenderResourceImport){
        .resource_id = xg_render_resource_identity_id(&identity),
        .kind = kind,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = owner_generation,
        .state = XG_RENDER_RESOURCE_IMPORTING_NATIVE,
        .provenance = g_pending.provenance,
        .content_digest = xg_render_resource_digest(bytes, byte_count),
        .bytes = bytes,
        .byte_count = byte_count,
        .identity = identity,
        .descriptor = vram_descriptor(kind, x, y, width, height),
    };
    for (uint32_t index = 0u; index < g_staged_publication_count; ++index) {
        const XgRenderVramResourcePublication *candidate =
            &g_staged_publications[index];
        XgRenderResourceView view;

        if (!candidate->occupied ||
            candidate->handle.resource_id != import.resource_id ||
            xg_render_resource_view(candidate->handle, &view) !=
                XG_RENDER_RESOURCE_OK ||
            !view.has_identity ||
            memcmp(&view.identity, &identity, sizeof(identity)) != 0)
            continue;
        handle = candidate->handle;
        reused = true;
        break;
    }
    if (!reused && xg_render_resource_import_begin(&import, &handle) !=
            XG_RENDER_RESOURCE_OK) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
    }
    if (publication->occupied && !same_handle(publication->handle, handle)) {
        bool shared = false;

        for (uint32_t index = 0u; index < g_staged_publication_count; ++index) {
            if (&g_staged_publications[index] != publication &&
                g_staged_publications[index].occupied &&
                same_handle(g_staged_publications[index].handle,
                            publication->handle)) {
                shared = true;
                break;
            }
        }
        if (!shared)
            (void)xg_render_resource_import_cancel(publication->handle);
    }
    *publication = (XgRenderVramResourcePublication){
        .handle = handle,
        .content_digest = import.content_digest,
        .sequence = g_next_sequence++,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .kind = kind,
        .provenance = g_pending.provenance,
        .occupied = true,
    };
    if (kind == XG_RENDER_RESOURCE_TEXTURE) {
        g_pending.image_count++;
    } else {
        g_pending.clut_count++;
    }
    return XG_RENDER_VRAM_RESOURCE_OK;
}

static bool cache_publication(
        const XgRenderVramResourcePublication *publication) {
    XgRenderVramResourceCachedPublication *cached = NULL;
    XgRenderResourceCapabilityMetadata authority;
    XgRenderResourceView view;
    uint8_t *bytes;

    if (publication == NULL || !publication->occupied ||
        xg_render_resource_view(publication->handle, &view) !=
            XG_RENDER_RESOURCE_OK ||
        !view.current || !view.has_identity || view.bytes == NULL ||
        view.byte_count == 0u ||
        xg_render_resource_capability_validate(
            &view.provenance, view.owner_kind, view.owner_generation,
            &authority) != XG_RENDER_RESOURCE_CAPABILITY_OK)
        return false;
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        XgRenderVramResourceCachedPublication *candidate =
            &g_cached_publications[index];

        if (candidate->occupied && candidate->kind == publication->kind &&
            candidate->x == publication->x && candidate->y == publication->y &&
            candidate->width == publication->width &&
            candidate->height == publication->height) {
            cached = candidate;
            break;
        }
        if (cached == NULL && !candidate->occupied) cached = candidate;
    }
    if (cached == NULL) return false;
    bytes = (uint8_t *)malloc(view.byte_count);
    if (bytes == NULL) return false;
    memcpy(bytes, view.bytes, view.byte_count);
    if (cached->occupied &&
        authority.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE &&
        authority.source.source_class ==
            XG_RENDER_RESOURCE_SOURCE_VRAM_CARRY)
        authority = cached->authority;
    if (!cached->occupied) g_snapshot.cached_publication_count++;
    free(cached->bytes);
    *cached = (XgRenderVramResourceCachedPublication){
        .identity = view.identity,
        .authority = authority,
        .content_digest = publication->content_digest,
        .bytes = bytes,
        .byte_count = view.byte_count,
        .x = publication->x,
        .y = publication->y,
        .width = publication->width,
        .height = publication->height,
        .kind = publication->kind,
        .occupied = true,
    };
    return true;
}

XgRenderVramResourceResult xg_render_vram_resources_upload(
        CPUState *cpu, XgRenderResourceKind kind, uint64_t owner_generation,
        const XgRenderVramResourceServices *services) {
    XgRenderVramResourceResult result;
    uint8_t *bytes = NULL;
    size_t byte_count = 0u;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;

    if (cpu == NULL || services == NULL ||
        services->authorize_guest_range == NULL || !valid_kind(kind) ||
        !bind_pending_provenance(services)) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    }
    if (g_pending.streaming) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    if (!read_upload(cpu, services, &x, &y, &width, &height,
                     &bytes, &byte_count)) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_RANGE;
    }
    result = stage_upload(kind, owner_generation, x, y, width, height,
                          bytes, byte_count);
    free(bytes);
    return result;
}

XgRenderVramResourceResult xg_render_vram_resources_upload_region(
        CPUState *cpu, XgRenderResourceKind kind, uint64_t owner_generation,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        uint32_t payload_address, const XgRenderVramResourceServices *services) {
    XgRenderVramResourceResult result;
    uint8_t *bytes = NULL;
    size_t byte_count = 0u;

    if (cpu == NULL || services == NULL || !valid_kind(kind) ||
        !bind_pending_provenance(services)) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    }
    if (g_pending.streaming) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    if (!read_region(cpu, services, x, y, width, height, payload_address,
                     &bytes, &byte_count)) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_RANGE;
    }
    result = stage_upload(kind, owner_generation, x, y, width, height,
                          bytes, byte_count);
    free(bytes);
    return result;
}

XgRenderVramResourceResult xg_render_vram_stream_upload(
        CPUState *cpu, uint64_t owner_generation,
        const XgRenderVramResourceServices *services) {
    uint8_t *chunk = NULL;
    uint8_t *combined;
    size_t chunk_byte_count = 0u;
    size_t combined_byte_count;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;

    if (cpu == NULL || services == NULL ||
        services->authorize_guest_range == NULL ||
        !bind_pending_provenance(services)) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    }
    if (!g_pending.active || g_pending.blocked || !g_pending.streaming ||
        owner_generation == 0u ||
        owner_generation != g_pending.owner_generation) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    if (!read_upload(cpu, services, &x, &y, &width, &height,
                     &chunk, &chunk_byte_count)) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_RANGE;
    }
    combined_byte_count = g_pending.stream_byte_count + chunk_byte_count;
    if (combined_byte_count < g_pending.stream_byte_count ||
        combined_byte_count > XG_RENDER_VRAM_MAX_PAYLOAD_SIZE ||
        (g_pending.stream_byte_count != 0u &&
         (x != g_pending.stream_x || width != g_pending.stream_width ||
          y != (uint32_t)g_pending.stream_y + g_pending.stream_height)) ||
        (uint32_t)g_pending.stream_height + height > UINT16_MAX ||
        (uint32_t)g_pending.stream_y + g_pending.stream_height + height >
            512u) {
        free(chunk);
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_RANGE;
    }
    combined = (uint8_t *)realloc(
        g_pending.stream_bytes, combined_byte_count);
    if (combined == NULL) {
        free(chunk);
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
    }
    memcpy(combined + g_pending.stream_byte_count, chunk, chunk_byte_count);
    free(chunk);
    g_pending.stream_bytes = combined;
    g_pending.stream_byte_count = combined_byte_count;
    if (g_pending.stream_height == 0u) {
        g_pending.stream_x = x;
        g_pending.stream_y = y;
        g_pending.stream_width = width;
    }
    g_pending.stream_height =
        (uint16_t)(g_pending.stream_height + height);
    return XG_RENDER_VRAM_RESOURCE_OK;
}

XgRenderVramResourceResult xg_render_vram_stream_finish(
        uint64_t owner_generation) {
    XgRenderVramResourceResult result;

    if (!g_pending.active || g_pending.blocked || !g_pending.streaming ||
        owner_generation == 0u ||
        owner_generation != g_pending.owner_generation ||
        g_pending.stream_bytes == NULL ||
        g_pending.stream_byte_count == 0u ||
        g_pending.stream_height == 0u) {
        reject_pending();
        if (g_pending.active) clear_pending(false);
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    g_pending.streaming = false;
    result = stage_upload(
        g_pending.required_kind, owner_generation,
        g_pending.stream_x, g_pending.stream_y,
        g_pending.stream_width, g_pending.stream_height,
        g_pending.stream_bytes, g_pending.stream_byte_count);
    if (result != XG_RENDER_VRAM_RESOURCE_OK) {
        clear_pending(false);
        return result;
    }
    return xg_render_vram_resources_commit(owner_generation);
}

XgRenderVramResourceResult xg_render_vram_resources_commit(uint64_t owner_generation) {
    uint32_t targets[XG_RENDER_VRAM_RESOURCE_CAPACITY];
    XgRenderResourceHandle imports[XG_RENDER_VRAM_RESOURCE_CAPACITY];
    bool reserved[XG_RENDER_VRAM_RESOURCE_CAPACITY] = { false };
    size_t import_count = 0u;

    if (!g_pending.active || g_pending.streaming || owner_generation == 0u ||
        owner_generation != g_pending.owner_generation) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    if (g_pending.blocked ||
        (g_pending.required_kind == XG_RENDER_RESOURCE_TEXTURE &&
         g_pending.image_count == 0u) ||
        (g_pending.required_kind == XG_RENDER_RESOURCE_CLUT &&
         g_pending.clut_count == 0u)) {
        reject_pending();
        clear_pending(false);
        return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
    }
    for (uint32_t staged_index = 0u;
         staged_index < g_staged_publication_count; ++staged_index) {
        const XgRenderVramResourcePublication *staged =
            &g_staged_publications[staged_index];
        uint32_t target = XG_RENDER_VRAM_RESOURCE_CAPACITY;

        for (uint32_t index = 0u;
             index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
            const XgRenderVramResourcePublication *current = &g_publications[index];

            if (current->occupied && current->kind == staged->kind &&
                current->x == staged->x && current->y == staged->y &&
                current->width == staged->width &&
                current->height == staged->height) {
                target = index;
                break;
            }
            if (target == XG_RENDER_VRAM_RESOURCE_CAPACITY &&
                !current->occupied && !reserved[index])
                target = index;
        }
        if (target == XG_RENDER_VRAM_RESOURCE_CAPACITY) {
            reject_pending();
            clear_pending(false);
            return XG_RENDER_VRAM_RESOURCE_CAPACITY_EXCEEDED;
        }
        targets[staged_index] = target;
        reserved[target] = true;
    }
    for (uint32_t staged_index = 0u;
         staged_index < g_staged_publication_count; ++staged_index) {
        const XgRenderResourceHandle handle =
            g_staged_publications[staged_index].handle;
        bool duplicate = false;

        for (size_t index = 0u; index < import_count; ++index) {
            if (same_handle(imports[index], handle)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) imports[import_count++] = handle;
    }
    if (xg_render_resource_import_commit_many(imports, import_count) !=
            XG_RENDER_RESOURCE_OK) {
        reject_pending();
        clear_pending(false);
        return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
    }
    for (uint32_t staged_index = 0u;
         staged_index < g_staged_publication_count; ++staged_index) {
        XgRenderVramResourcePublication *target =
            &g_publications[targets[staged_index]];

        if (target->occupied && !same_handle(
                target->handle,
                g_staged_publications[staged_index].handle)) {
            record_publication_retirement(target, 5u);
            retire_publication(target);
        }
        if (!target->occupied) g_snapshot.publication_count++;
        *target = g_staged_publications[staged_index];
        g_snapshot.last_publication_x = target->x;
        g_snapshot.last_publication_y = target->y;
        g_snapshot.last_publication_width = target->width;
        g_snapshot.last_publication_height = target->height;
        g_snapshot.last_publication_kind = target->kind;
    }
    g_snapshot.published_images += g_pending.image_count;
    g_snapshot.published_cluts += g_pending.clut_count;
    g_snapshot.completed_loaders++;
    for (uint32_t index = 0u;
         index < g_staged_publication_count; ++index) {
        XgRenderResourceView view;

        (void)cache_publication(&g_staged_publications[index]);
        if (xg_render_resource_view(
                g_staged_publications[index].handle, &view) ==
                    XG_RENDER_RESOURCE_OK &&
            (view.descriptor.flags &
             XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u)
            (void)xg_render_vram_journal_authenticate_rect(
                view.descriptor.vram_x, view.descriptor.vram_y,
                view.descriptor.vram_width, view.descriptor.vram_height,
                view.bytes, view.byte_count);
    }
    clear_pending(true);
    return XG_RENDER_VRAM_RESOURCE_OK;
}

XgRenderVramResourceResult xg_render_vram_resources_commit_optional(
        uint64_t owner_generation) {
    if (!g_pending.active || g_pending.streaming || owner_generation == 0u ||
        owner_generation != g_pending.owner_generation) {
        reject_pending();
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    }
    if (!g_pending.blocked && g_staged_publication_count == 0u &&
        g_pending.image_count == 0u && g_pending.clut_count == 0u) {
        clear_pending(false);
        return XG_RENDER_VRAM_RESOURCE_OK;
    }
    return xg_render_vram_resources_commit(owner_generation);
}

static const XgRenderArtifactIdentity *cached_origin_artifact(
        const XgRenderVramResourceCachedPublication *cached) {
    if (cached->authority.kind == XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT)
        return &cached->authority.artifact;
    return &cached->authority.source.origin_artifact;
}

static bool artifact_identity_equal(const XgRenderArtifactIdentity *left,
                                    const XgRenderArtifactIdentity *right) {
    return left->base == right->base && left->size == right->size &&
        left->crc32 == right->crc32 &&
        memcmp(left->sha256, right->sha256, sizeof(left->sha256)) == 0;
}

static bool rebind_cached_publications(uint64_t owner_generation) {
    static const char domain[] = "xg-vram-carry-v1";
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = owner_generation,
        .source = {
            .source_class = XG_RENDER_RESOURCE_SOURCE_VRAM_CARRY,
        },
    };
    XgRenderResourceProvenance provenance = {0};
    XgRenderArtifactIdentity common_origin = {0};
    psx_sha256_ctx sha;
    uint64_t byte_count = 0u;
    bool capability_created = false;
    bool have_origin = false;
    uint32_t cache_count = 0u;

    if (owner_generation == 0u || g_pending.active) goto fail;
    psx_sha256_init(&sha);
    psx_sha256_update(&sha, (const uint8_t *)domain, sizeof(domain));
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        const XgRenderVramResourceCachedPublication *cached =
            &g_cached_publications[index];
        const XgRenderArtifactIdentity *origin;

        if (!cached->occupied) continue;
        origin = cached_origin_artifact(cached);
        if (!have_origin) {
            common_origin = *origin;
            have_origin = true;
        } else if (!artifact_identity_equal(&common_origin, origin)) {
            memset(&common_origin, 0, sizeof(common_origin));
        }
        sha_update_u32(&sha, (uint32_t)cached->kind);
        sha_update_u16(&sha, cached->x);
        sha_update_u16(&sha, cached->y);
        sha_update_u16(&sha, cached->width);
        sha_update_u16(&sha, cached->height);
        sha_update_u64(&sha, cached->content_digest);
        psx_sha256_update(&sha, cached->identity.bytes,
                          sizeof(cached->identity.bytes));
        sha_update_u32(&sha, origin->base);
        sha_update_u32(&sha, origin->size);
        sha_update_u32(&sha, origin->crc32);
        psx_sha256_update(&sha, origin->sha256, sizeof(origin->sha256));
        if (UINT64_MAX - byte_count < cached->byte_count) goto fail;
        byte_count += cached->byte_count;
        cache_count++;
    }
    if (cache_count == 0u) return true;
    psx_sha256_final(&sha, metadata.source.identity.bytes);
    metadata.receipt = xg_render_resource_identity_id(
        &metadata.source.identity);
    if (metadata.receipt == 0u) metadata.receipt = 1u;
    metadata.source.range_size = byte_count;
    metadata.source.range_content_digest = xg_render_resource_digest(
        metadata.source.identity.bytes, sizeof(metadata.source.identity.bytes));
    metadata.source.origin_artifact = common_origin;
    if (xg_render_resource_capability_register_tracked(
            &metadata, &provenance, &capability_created) !=
            XG_RENDER_RESOURCE_CAPABILITY_OK)
        goto fail;
    if (xg_render_vram_image_begin(
            GUEST_RENDER_RENDER_NATIVE, owner_generation) !=
            XG_RENDER_VRAM_RESOURCE_OK)
        goto fail_capability;
    g_pending.provenance = provenance;
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        const XgRenderVramResourceCachedPublication *cached =
            &g_cached_publications[index];

        if (!cached->occupied) continue;
        if (stage_upload(
                cached->kind, owner_generation, cached->x, cached->y,
                cached->width, cached->height, cached->bytes,
                cached->byte_count) != XG_RENDER_VRAM_RESOURCE_OK)
            goto fail_pending;
    }
    if (xg_render_vram_resources_commit(owner_generation) !=
            XG_RENDER_VRAM_RESOURCE_OK)
        goto fail_capability;
    g_snapshot.cache_rebinds++;
    return true;

fail_pending:
    clear_pending(false);
fail_capability:
    if (capability_created)
        (void)xg_render_resource_capability_revoke(provenance);
fail:
    g_snapshot.cache_rebind_failures++;
    return false;
}

bool xg_render_vram_resources_lookup(
        XgRenderResourceKind kind, uint16_t x, uint16_t y,
        uint16_t width, uint16_t height,
        XgSemanticResourceRef *out_resource) {
    const XgRenderVramResourcePublication *best = NULL;
    const uint32_t right = (uint32_t)x + width;
    const uint32_t bottom = (uint32_t)y + height;
    XgRenderResourceView view;

    if (!valid_kind(kind) || width == 0u || height == 0u ||
        right > 1024u || bottom > 512u || out_resource == NULL)
        return false;
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        const XgRenderVramResourcePublication *candidate = &g_publications[index];

        if (!candidate->occupied || candidate->kind != kind ||
            x < candidate->x || y < candidate->y ||
            right > (uint32_t)candidate->x + candidate->width ||
            bottom > (uint32_t)candidate->y + candidate->height ||
            (best != NULL && best->sequence > candidate->sequence))
            continue;
        best = candidate;
    }
    if (best != NULL) {
        for (uint32_t index = 0u;
             index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
            const XgRenderVramResourcePublication *candidate =
                &g_publications[index];

            if (candidate->occupied && candidate->kind == best->kind &&
                candidate->sequence > best->sequence &&
                regions_overlap(x, y, width, height,
                                candidate->x, candidate->y,
                                candidate->width, candidate->height))
                return false;
        }
    }
    if (best == NULL ||
        xg_render_resource_view(best->handle, &view) != XG_RENDER_RESOURCE_OK ||
        !view.current || !view.has_identity || view.kind != kind ||
        view.content_digest != best->content_digest ||
        !same_provenance(&view.provenance, &best->provenance))
        return false;
    *out_resource = (XgSemanticResourceRef){
        .resource_id = best->handle.resource_id,
        .generation = best->handle.generation,
        .content_digest = best->content_digest,
    };
    return true;
}

static bool wrapped_axis_overlaps(uint16_t start, uint16_t length,
                                  uint16_t limit,
                                  uint16_t region_start,
                                  uint16_t region_length) {
    const uint32_t end = (uint32_t)start + length;
    const uint32_t region_end = (uint32_t)region_start + region_length;

    if (length >= limit) return true;
    if (end <= limit)
        return start < region_end && region_start < end;
    return start < region_end || region_start < end - limit;
}

bool xg_render_vram_move_command_matches(
        uint16_t source_x, uint16_t source_y,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        uint8_t command_opcode, const uint32_t *command_words,
        uint8_t command_word_count, bool command_context_valid) {
    const uint32_t encoded_width = width == 1024u ? 0u : width;
    const uint32_t encoded_height = height == 512u ? 0u : height;

    if (!command_context_valid || command_words == NULL ||
        command_word_count != 4u || command_opcode < 0x80u ||
        command_opcode > 0x9fu)
        return false;
    return command_words[0] == (uint32_t)command_opcode << 24u &&
        command_words[1] == ((uint32_t)source_x |
            (uint32_t)source_y << 16u) &&
        command_words[2] == ((uint32_t)x | (uint32_t)y << 16u) &&
        command_words[3] == (encoded_width | encoded_height << 16u);
}

static bool publication_matches_cache(
        const XgRenderVramResourcePublication *publication,
        const XgRenderVramResourceCachedPublication *cached,
        const XgRenderResourceView *view) {
    return publication != NULL && cached != NULL && view != NULL &&
        cached->occupied && cached->kind == publication->kind &&
        cached->x == publication->x && cached->y == publication->y &&
        cached->width == publication->width &&
        cached->height == publication->height &&
        cached->bytes != NULL &&
        cached->byte_count == view->byte_count &&
        cached->content_digest == view->content_digest &&
        memcmp(&cached->identity, &view->identity,
               sizeof(cached->identity)) == 0 &&
        memcmp(cached->bytes, view->bytes, view->byte_count) == 0;
}

static void cancel_mutation_updates(
        XgRenderVramResourceMutationUpdate *updates, uint32_t update_count) {
    for (uint32_t index = 0u; index < update_count; ++index) {
        bool duplicate = false;

        free(updates[index].bytes);
        updates[index].bytes = NULL;
        if (updates[index].replacement_handle.resource_id == 0u) continue;
        for (uint32_t previous = 0u; previous < index; ++previous) {
            if (same_handle(updates[previous].replacement_handle,
                            updates[index].replacement_handle)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            (void)xg_render_resource_import_cancel(
                updates[index].replacement_handle);
    }
}

static bool apply_authenticated_vram_move(
        uint16_t source_x, uint16_t source_y,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        uint64_t source_vram_generation, uint64_t vram_generation,
        uint64_t content_digest, uint32_t operation, uint32_t direction,
        uint32_t command_source_address, uint32_t command_pc,
        uint32_t command_function, uint32_t command_return_address,
        uint32_t command_source_kind, uint8_t command_opcode,
        const uint32_t *command_words, uint8_t command_word_count,
        bool command_context_valid,
        const XgRenderResourceIdentity *mutation_source_identity,
        const XgRenderResourceProvenance *mutation_source_provenance,
        const uint16_t *mutation_pixels, size_t mutation_pixel_count,
        bool *out_applicable) {
    static const uint8_t domain[] = "xg-vram-transfer-v1";
    XgRenderVramResourceMutationUpdate
        updates[XG_RENDER_VRAM_RESOURCE_CAPACITY] = {0};
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = vram_generation,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = g_snapshot.owner_generation,
        .source = {
            .source_class = XG_RENDER_RESOURCE_SOURCE_VRAM_TRANSFER,
        },
    };
    XgRenderResourceProvenance provenance = {0};
    XgRenderResourceCapabilityMetadata source_authority;
    XgRenderResourceHandle imports[XG_RENDER_VRAM_RESOURCE_CAPACITY];
    uint8_t *payload_bytes = NULL;
    psx_sha256_ctx sha;
    size_t payload_size;
    size_t import_count = 0u;
    uint32_t update_count = 0u;
    bool capability_created = false;
    bool overlaps = false;

    if (out_applicable == NULL) return false;
    *out_applicable = false;
    if (operation != (uint32_t)XG_RENDER_VRAM_MOVE ||
        direction != (uint32_t)XG_RENDER_VRAM_VRAM_TO_VRAM)
        return false;
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        const XgRenderVramResourcePublication *publication =
            &g_publications[index];
        const XgRenderVramResourceCachedPublication *cached =
            &g_cached_publications[index];

        if ((publication->occupied &&
             wrapped_axis_overlaps(
                 x, width, 1024u, publication->x, publication->width) &&
             wrapped_axis_overlaps(
                 y, height, 512u, publication->y, publication->height)) ||
            (cached->occupied &&
             wrapped_axis_overlaps(
                 x, width, 1024u, cached->x, cached->width) &&
             wrapped_axis_overlaps(
                 y, height, 512u, cached->y, cached->height))) {
            overlaps = true;
            break;
        }
    }
    if (!overlaps) return false;
    *out_applicable = true;
    if (g_snapshot.owner_generation == 0u || vram_generation == 0u ||
        vram_generation != (source_vram_generation == UINT64_MAX
            ? 1u : source_vram_generation + 1u) ||
        mutation_pixels == NULL || mutation_pixel_count == 0u ||
        mutation_pixel_count != (size_t)width * height ||
        mutation_pixel_count > SIZE_MAX / sizeof(uint16_t) ||
        mutation_source_identity == NULL ||
        mutation_source_provenance == NULL ||
        xg_render_resource_capability_validate(
            mutation_source_provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
            g_snapshot.owner_generation, &source_authority) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK ||
        source_authority.kind != XG_RENDER_RESOURCE_PROVENANCE_SOURCE ||
        memcmp(source_authority.source.identity.bytes,
               mutation_source_identity->bytes,
               sizeof(mutation_source_identity->bytes)) != 0 ||
        !xg_render_vram_move_command_matches(
            source_x, source_y, x, y, width, height,
            command_opcode, command_words,
            command_word_count, command_context_valid))
        return false;
    payload_size = mutation_pixel_count * sizeof(uint16_t);
    payload_bytes = (uint8_t *)malloc(payload_size);
    if (payload_bytes == NULL) return false;
    for (size_t index = 0u; index < mutation_pixel_count; ++index) {
        payload_bytes[index * 2u] = (uint8_t)mutation_pixels[index];
        payload_bytes[index * 2u + 1u] =
            (uint8_t)(mutation_pixels[index] >> 8u);
    }
    if (xg_render_resource_digest(payload_bytes, payload_size) != content_digest)
        goto fail;

    for (uint32_t publication_index = 0u;
         publication_index < XG_RENDER_VRAM_RESOURCE_CAPACITY;
         ++publication_index) {
        const XgRenderVramResourcePublication *publication =
            &g_publications[publication_index];
        XgRenderVramResourceMutationUpdate *update;
        XgRenderResourceIdentity verified_identity;
        XgRenderResourceCapabilityMetadata authority;
        XgRenderResourceView view;
        uint32_t cache_index = XG_RENDER_VRAM_RESOURCE_CAPACITY;
        uint32_t free_cache_index = XG_RENDER_VRAM_RESOURCE_CAPACITY;

        if (!publication->occupied ||
            !wrapped_axis_overlaps(
                x, width, 1024u, publication->x, publication->width) ||
            !wrapped_axis_overlaps(
                y, height, 512u, publication->y, publication->height))
            continue;
        if (update_count == XG_RENDER_VRAM_RESOURCE_CAPACITY ||
            xg_render_resource_view(publication->handle, &view) !=
                XG_RENDER_RESOURCE_OK ||
            !view.current || !view.has_identity ||
            view.kind != publication->kind ||
            view.owner_kind != XG_RENDER_RESOURCE_OWNER_SCENE ||
            view.owner_generation != g_snapshot.owner_generation ||
            view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
            view.content_digest != publication->content_digest ||
            !same_provenance(&view.provenance, &publication->provenance) ||
            view.bytes == NULL ||
            view.byte_count !=
                (size_t)publication->width * publication->height * 2u ||
            xg_render_resource_capability_validate(
                &view.provenance, view.owner_kind, view.owner_generation,
                &authority) != XG_RENDER_RESOURCE_CAPABILITY_OK ||
            !content_identity(
                publication->kind, publication->x, publication->y,
                publication->width, publication->height,
                (const uint8_t *)view.bytes, view.byte_count,
                &verified_identity) ||
            memcmp(&verified_identity, &view.identity,
                   sizeof(verified_identity)) != 0)
            goto fail;
        for (uint32_t index = 0u;
             index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
            const XgRenderVramResourceCachedPublication *cached =
                &g_cached_publications[index];

            if (!cached->occupied) {
                bool reserved = false;

                for (uint32_t update_index = 0u;
                     update_index < update_count; ++update_index)
                    if (updates[update_index].cache_index == index) {
                        reserved = true;
                        break;
                    }
                if (!reserved &&
                    free_cache_index == XG_RENDER_VRAM_RESOURCE_CAPACITY)
                    free_cache_index = index;
                continue;
            }
            if (cached->kind == publication->kind &&
                cached->x == publication->x && cached->y == publication->y &&
                cached->width == publication->width &&
                cached->height == publication->height) {
                if (cache_index != XG_RENDER_VRAM_RESOURCE_CAPACITY ||
                    !publication_matches_cache(publication, cached, &view))
                    goto fail;
                cache_index = index;
            }
        }
        if (cache_index == XG_RENDER_VRAM_RESOURCE_CAPACITY)
            cache_index = free_cache_index;
        if (cache_index == XG_RENDER_VRAM_RESOURCE_CAPACITY) goto fail;
        for (uint32_t index = 0u; index < update_count; ++index)
            if (updates[index].cache_index == cache_index) goto fail;

        update = &updates[update_count++];
        update->publication_index = publication_index;
        update->cache_index = cache_index;
        update->previous_handle = publication->handle;
        update->previous_identity = view.identity;
        update->byte_count = view.byte_count;
        update->bytes = (uint8_t *)malloc(view.byte_count);
        if (update->bytes == NULL) goto fail;
        memcpy(update->bytes, view.bytes, view.byte_count);
        for (uint32_t row = 0u; row < height; ++row) {
            const uint16_t destination_y =
                (uint16_t)(((uint32_t)y + row) % 512u);

            if (destination_y < publication->y ||
                destination_y >= (uint32_t)publication->y + publication->height)
                continue;
            for (uint32_t column = 0u; column < width; ++column) {
                const uint16_t destination_x =
                    (uint16_t)(((uint32_t)x + column) % 1024u);
                size_t destination_offset;
                const uint16_t pixel =
                    mutation_pixels[(size_t)row * width + column];

                if (destination_x < publication->x ||
                    destination_x >=
                        (uint32_t)publication->x + publication->width)
                    continue;
                destination_offset =
                    ((size_t)(destination_y - publication->y) *
                         publication->width +
                     (destination_x - publication->x)) * 2u;
                update->bytes[destination_offset] = (uint8_t)pixel;
                update->bytes[destination_offset + 1u] =
                    (uint8_t)(pixel >> 8u);
            }
        }
        update->content_digest = xg_render_resource_digest(
            update->bytes, update->byte_count);
        if (update->content_digest == 0u ||
            !content_identity(
                publication->kind, publication->x, publication->y,
                publication->width, publication->height,
                update->bytes, update->byte_count,
                &update->replacement_identity))
            goto fail;
    }
    if (update_count == 0u) goto fail;
    for (uint32_t cache_index = 0u;
         cache_index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++cache_index) {
        const XgRenderVramResourceCachedPublication *cached =
            &g_cached_publications[cache_index];
        bool covered = false;

        if (!cached->occupied ||
            !wrapped_axis_overlaps(
                x, width, 1024u, cached->x, cached->width) ||
            !wrapped_axis_overlaps(
                y, height, 512u, cached->y, cached->height))
            continue;
        for (uint32_t index = 0u; index < update_count; ++index)
            if (updates[index].cache_index == cache_index) {
                covered = true;
                break;
            }
        if (!covered) goto fail;
    }

    psx_sha256_init(&sha);
    psx_sha256_update(&sha, domain, sizeof(domain) - 1u);
    sha_update_u64(&sha, source_vram_generation);
    sha_update_u64(&sha, vram_generation);
    sha_update_u32(&sha, operation);
    sha_update_u32(&sha, direction);
    sha_update_u16(&sha, source_x);
    sha_update_u16(&sha, source_y);
    sha_update_u16(&sha, x);
    sha_update_u16(&sha, y);
    sha_update_u16(&sha, width);
    sha_update_u16(&sha, height);
    sha_update_u64(&sha, content_digest);
    psx_sha256_update(&sha, mutation_source_identity->bytes,
                      sizeof(mutation_source_identity->bytes));
    sha_update_u32(&sha, command_source_address);
    sha_update_u32(&sha, command_pc);
    sha_update_u32(&sha, command_function);
    sha_update_u32(&sha, command_return_address);
    sha_update_u32(&sha, command_source_kind);
    psx_sha256_update(&sha, &command_opcode, sizeof(command_opcode));
    psx_sha256_update(&sha, &command_word_count, sizeof(command_word_count));
    for (uint32_t index = 0u; index < command_word_count; ++index)
        sha_update_u32(&sha, command_words[index]);
    psx_sha256_update(&sha, payload_bytes, payload_size);
    for (uint32_t index = 0u; index < update_count; ++index) {
        const XgRenderVramResourcePublication *publication =
            &g_publications[updates[index].publication_index];

        sha_update_u32(&sha, (uint32_t)publication->kind);
        sha_update_u16(&sha, publication->x);
        sha_update_u16(&sha, publication->y);
        sha_update_u16(&sha, publication->width);
        sha_update_u16(&sha, publication->height);
        sha_update_u64(&sha, publication->content_digest);
        psx_sha256_update(&sha, updates[index].previous_identity.bytes,
                          sizeof(updates[index].previous_identity.bytes));
    }
    psx_sha256_final(&sha, metadata.source.identity.bytes);
    metadata.source.range_offset =
        ((uint64_t)source_y << 10u) | source_x;
    metadata.source.range_size = payload_size;
    metadata.source.range_content_digest = content_digest;
    metadata.source.origin_artifact = source_authority.source.origin_artifact;
    if (xg_render_resource_capability_register_tracked(
            &metadata, &provenance, &capability_created) !=
            XG_RENDER_RESOURCE_CAPABILITY_OK)
        goto fail;

    for (uint32_t index = 0u; index < update_count; ++index) {
        const XgRenderVramResourcePublication *publication =
            &g_publications[updates[index].publication_index];
        const XgRenderResourceImport import = {
            .resource_id = xg_render_resource_identity_id(
                &updates[index].replacement_identity),
            .kind = publication->kind,
            .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
            .owner_generation = g_snapshot.owner_generation,
            .state = XG_RENDER_RESOURCE_IMPORTING_NATIVE,
            .provenance = provenance,
            .content_digest = updates[index].content_digest,
            .bytes = updates[index].bytes,
            .byte_count = updates[index].byte_count,
            .identity = updates[index].replacement_identity,
            .descriptor = vram_descriptor(
                publication->kind, publication->x, publication->y,
                publication->width, publication->height),
        };

        if (xg_render_resource_import_begin(
                &import, &updates[index].replacement_handle) !=
                XG_RENDER_RESOURCE_OK)
            goto fail_capability;
        imports[import_count++] = updates[index].replacement_handle;
    }
    if (xg_render_resource_import_commit_many(imports, import_count) !=
            XG_RENDER_RESOURCE_OK)
        goto fail_capability;

    for (uint32_t index = 0u; index < update_count; ++index) {
        XgRenderVramResourcePublication *publication =
            &g_publications[updates[index].publication_index];
        XgRenderVramResourceCachedPublication *cached =
            &g_cached_publications[updates[index].cache_index];
        const XgSemanticResourceRef previous_resource = {
            .resource_id = updates[index].previous_handle.resource_id,
            .generation = updates[index].previous_handle.generation,
            .content_digest = publication->content_digest,
        };
        const XgSemanticResourceRef replacement_resource = {
            .resource_id = updates[index].replacement_handle.resource_id,
            .generation = updates[index].replacement_handle.generation,
            .content_digest = updates[index].content_digest,
        };
        const XgRenderSemanticCompositorResult rebind_result =
            xg_render_semantic_compositor_rebind_mutated_resource(
                &previous_resource, &replacement_resource);

        if (rebind_result != XG_RENDER_SEMANTIC_COMPOSITOR_OK)
            g_snapshot.mutation_republication_failures++;
        if (updates[index].previous_handle.resource_id !=
            updates[index].replacement_handle.resource_id)
            (void)xg_render_resource_retire_current(
                updates[index].previous_handle);
        *publication = (XgRenderVramResourcePublication){
            .handle = updates[index].replacement_handle,
            .content_digest = updates[index].content_digest,
            .sequence = publication->sequence,
            .x = publication->x,
            .y = publication->y,
            .width = publication->width,
            .height = publication->height,
            .kind = publication->kind,
            .provenance = provenance,
            .occupied = true,
        };
        if (!cached->occupied) g_snapshot.cached_publication_count++;
        free(cached->bytes);
        *cached = (XgRenderVramResourceCachedPublication){
            .identity = updates[index].replacement_identity,
            .authority = metadata,
            .content_digest = updates[index].content_digest,
            .bytes = updates[index].bytes,
            .byte_count = updates[index].byte_count,
            .x = publication->x,
            .y = publication->y,
            .width = publication->width,
            .height = publication->height,
            .kind = publication->kind,
            .occupied = true,
        };
        updates[index].bytes = NULL;
    }
    free(payload_bytes);
    return true;

fail_capability:
    cancel_mutation_updates(updates, update_count);
    if (capability_created)
        (void)xg_render_resource_capability_revoke(provenance);
    free(payload_bytes);
    return false;
fail:
    cancel_mutation_updates(updates, update_count);
    free(payload_bytes);
    return false;
}

void xg_render_vram_resources_note_vram_mutation_context(
        uint16_t source_x, uint16_t source_y,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        uint64_t source_vram_generation, uint64_t vram_generation,
        uint64_t content_digest,
        uint32_t operation, uint32_t direction,
        uint32_t command_source_address, uint32_t command_pc,
        uint32_t command_function, uint32_t command_return_address,
        uint32_t command_source_kind, uint8_t command_opcode,
        const uint32_t *command_words, uint8_t command_word_count,
        bool command_context_valid,
        const XgRenderResourceIdentity *mutation_source_identity,
        const XgRenderResourceProvenance *mutation_source_provenance,
        const uint16_t *mutation_pixels, size_t mutation_pixel_count,
        bool preserve_exact_content) {
    uint32_t captured_command_words[4] = {0};
    bool mutation_update_applicable = false;

    if (x >= 1024u || y >= 512u || width == 0u || height == 0u ||
        width > 1024u || height > 512u || vram_generation == 0u)
        return;
    if (command_word_count > 4u) command_word_count = 4u;
    if (command_words != NULL)
        memcpy(captured_command_words, command_words,
               (size_t)command_word_count * sizeof(*command_words));
    else
        command_word_count = 0u;
    if (apply_authenticated_vram_move(
            source_x, source_y, x, y, width, height,
            source_vram_generation, vram_generation, content_digest,
            operation, direction, command_source_address, command_pc,
            command_function, command_return_address, command_source_kind,
            command_opcode, captured_command_words, command_word_count,
            command_context_valid, mutation_source_identity,
            mutation_source_provenance, mutation_pixels, mutation_pixel_count,
            &mutation_update_applicable)) {
        g_snapshot.mutation_republications++;
        g_snapshot.vram_generation = vram_generation;
        return;
    }
    if (mutation_update_applicable)
        g_snapshot.mutation_republication_failures++;
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        XgRenderVramResourcePublication *publication = &g_publications[index];

        if (!publication->occupied ||
            !wrapped_axis_overlaps(
                x, width, 1024u, publication->x, publication->width) ||
            !wrapped_axis_overlaps(
                y, height, 512u, publication->y, publication->height))
            continue;
        if (preserve_exact_content && publication->x == x &&
            publication->y == y && publication->width == width &&
            publication->height == height &&
            publication->content_digest == content_digest)
            continue;
        g_retirements[g_retirement_total % XG_RENDER_VRAM_RETIREMENT_CAPACITY] =
            (XgRenderVramResourceRetirementSnapshot){
                .sequence = g_retirement_total + 1u,
                .resource_id = publication->handle.resource_id,
                .resource_generation = publication->handle.generation,
                .content_digest = publication->content_digest,
                .vram_generation = vram_generation,
                .mutation_digest = content_digest,
                .x = publication->x,
                .y = publication->y,
                .width = publication->width,
                .height = publication->height,
                .mutation_x = x,
                .mutation_y = y,
                .mutation_width = width,
                .mutation_height = height,
                .mutation_source_x = source_x,
                .mutation_source_y = source_y,
                .kind = publication->kind,
                .mutation_operation = operation,
                .mutation_direction = direction,
                .mutation_command_source_address = command_source_address,
                .mutation_command_pc = command_pc,
                .mutation_command_function = command_function,
                .mutation_command_return_address = command_return_address,
                .mutation_command_source_kind = command_source_kind,
                .mutation_command_opcode = command_opcode,
                .mutation_command_word_count = command_word_count,
                .mutation_command_context_valid = command_context_valid,
                .reason = 1u,
                .preserve_exact_content = preserve_exact_content,
                .cached = false,
            };
        memcpy(g_retirements[
                   g_retirement_total % XG_RENDER_VRAM_RETIREMENT_CAPACITY]
                   .mutation_command_words,
               captured_command_words, sizeof(captured_command_words));
        g_retirement_total++;
        retire_publication(publication);
        if (g_snapshot.publication_count != 0u)
            g_snapshot.publication_count--;
    }
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        XgRenderVramResourceCachedPublication *cached =
            &g_cached_publications[index];

        if (!cached->occupied ||
            !wrapped_axis_overlaps(
                x, width, 1024u, cached->x, cached->width) ||
            !wrapped_axis_overlaps(
                y, height, 512u, cached->y, cached->height))
            continue;
        if (preserve_exact_content && cached->x == x && cached->y == y &&
            cached->width == width && cached->height == height &&
            cached->content_digest == content_digest)
            continue;
        g_retirements[g_retirement_total % XG_RENDER_VRAM_RETIREMENT_CAPACITY] =
            (XgRenderVramResourceRetirementSnapshot){
                .sequence = g_retirement_total + 1u,
                .content_digest = cached->content_digest,
                .vram_generation = vram_generation,
                .mutation_digest = content_digest,
                .x = cached->x,
                .y = cached->y,
                .width = cached->width,
                .height = cached->height,
                .mutation_x = x,
                .mutation_y = y,
                .mutation_width = width,
                .mutation_height = height,
                .mutation_source_x = source_x,
                .mutation_source_y = source_y,
                .kind = cached->kind,
                .mutation_operation = operation,
                .mutation_direction = direction,
                .mutation_command_source_address = command_source_address,
                .mutation_command_pc = command_pc,
                .mutation_command_function = command_function,
                .mutation_command_return_address = command_return_address,
                .mutation_command_source_kind = command_source_kind,
                .mutation_command_opcode = command_opcode,
                .mutation_command_word_count = command_word_count,
                .mutation_command_context_valid = command_context_valid,
                .reason = 2u,
                .preserve_exact_content = preserve_exact_content,
                .cached = true,
            };
        memcpy(g_retirements[
                   g_retirement_total % XG_RENDER_VRAM_RETIREMENT_CAPACITY]
                   .mutation_command_words,
               captured_command_words, sizeof(captured_command_words));
        g_retirement_total++;
        free(cached->bytes);
        memset(cached, 0, sizeof(*cached));
        if (g_snapshot.cached_publication_count != 0u)
            g_snapshot.cached_publication_count--;
    }
    g_snapshot.vram_generation = vram_generation;
}

void xg_render_vram_resources_note_vram_mutation(
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        uint64_t vram_generation, uint64_t content_digest,
        bool preserve_exact_content) {
    xg_render_vram_resources_note_vram_mutation_context(
        0u, 0u, x, y, width, height, 0u, vram_generation, content_digest,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, 0u, 0u, 0u, UINT32_MAX, 0u,
        NULL, 0u, false, NULL, NULL, NULL, 0u, preserve_exact_content);
}
static uint16_t texture_window_coordinate(uint16_t coordinate,
                                          uint8_t mask,
                                          uint8_t offset) {
    const uint16_t expanded_mask = (uint16_t)mask << 3u;
    const uint16_t expanded_offset = (uint16_t)(offset & mask) << 3u;

    return (coordinate & (uint16_t)~expanded_mask) | expanded_offset;
}

bool xg_render_vram_resources_resolve_draw(
        const XgRenderIrNativePrimitive *primitive,
        XgRenderVramResourceResolvedResources *out_resources) {
    const XgRenderIrMaterialState *material;
    uint16_t min_u = UINT16_MAX;
    uint16_t min_v = UINT16_MAX;
    uint16_t max_u = 0u;
    uint16_t max_v = 0u;
    uint16_t sampled_min_u = UINT16_MAX;
    uint16_t sampled_min_v = UINT16_MAX;
    uint16_t sampled_max_u = 0u;
    uint16_t sampled_max_v = 0u;
    uint16_t words_per_texel;
    uint16_t texture_x;
    uint16_t texture_y;
    uint16_t texture_width;
    uint16_t texture_height;

    if (out_resources == NULL) return false;
    *out_resources = (XgRenderVramResourceResolvedResources){0};
    if (primitive == NULL || primitive->triangle_count == 0u ||
        primitive->triangle_count > XG_RENDER_IR_TRIANGLE_CAPACITY ||
        !primitive->material.textured)
        return false;
    material = &primitive->material;
    for (uint32_t triangle = 0u; triangle < primitive->triangle_count;
         ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const XgRenderIrVertex *source =
                &primitive->triangles[triangle].vertices[vertex];
            uint16_t u;
            uint16_t v;

            if (source->u < 0 || source->v < 0 ||
                ((uint32_t)source->u >> 16u) > UINT8_MAX ||
                ((uint32_t)source->v >> 16u) > UINT8_MAX)
                return false;
            u = (uint16_t)((uint32_t)source->u >> 16u);
            v = (uint16_t)((uint32_t)source->v >> 16u);
            if (u < min_u) min_u = u;
            if (u > max_u) max_u = u;
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
        }
    }
    for (uint16_t u = min_u; u <= max_u; ++u) {
        const uint16_t sampled = texture_window_coordinate(
            u, material->texture_window_mask_x,
            material->texture_window_offset_x);

        if (sampled < sampled_min_u) sampled_min_u = sampled;
        if (sampled > sampled_max_u) sampled_max_u = sampled;
    }
    for (uint16_t v = min_v; v <= max_v; ++v) {
        const uint16_t sampled = texture_window_coordinate(
            v, material->texture_window_mask_y,
            material->texture_window_offset_y);

        if (sampled < sampled_min_v) sampled_min_v = sampled;
        if (sampled > sampled_max_v) sampled_max_v = sampled;
    }
    switch (material->texture_depth) {
    case XG_RENDER_IR_TEXTURE_4_BIT:
        words_per_texel = 4u;
        break;
    case XG_RENDER_IR_TEXTURE_8_BIT:
        words_per_texel = 2u;
        break;
    case XG_RENDER_IR_TEXTURE_15_BIT:
        words_per_texel = 1u;
        break;
    default:
        return false;
    }
    texture_x = material->texture_page_x * 64u +
        sampled_min_u / words_per_texel;
    texture_y = material->texture_page_y * 256u + sampled_min_v;
    texture_width = sampled_max_u / words_per_texel -
        sampled_min_u / words_per_texel + 1u;
    texture_height = sampled_max_v - sampled_min_v + 1u;
    out_resources->has_texture = xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, texture_x, texture_y,
        texture_width, texture_height, &out_resources->texture);
    if (!out_resources->has_texture &&
        (texture_width > 1u || texture_height > 1u)) {
        /* Rasterized triangles sample the half-open span between UV edge
         * endpoints rather than both endpoint texels. */
        out_resources->has_texture = xg_render_vram_resources_lookup(
            XG_RENDER_RESOURCE_TEXTURE, texture_x, texture_y,
            texture_width > 1u ? (uint16_t)(texture_width - 1u) : 1u,
            texture_height > 1u ? (uint16_t)(texture_height - 1u) : 1u,
            &out_resources->texture);
    }
    if (material->texture_depth != XG_RENDER_IR_TEXTURE_15_BIT) {
        const uint16_t clut_width = material->texture_depth ==
                XG_RENDER_IR_TEXTURE_4_BIT
            ? 16u : 256u;

        out_resources->has_clut = xg_render_vram_resources_lookup(
            XG_RENDER_RESOURCE_CLUT, material->clut_x, material->clut_y,
            clut_width, 1u, &out_resources->clut);
    }
    return out_resources->has_texture || out_resources->has_clut;
}

void xg_render_vram_resources_snapshot(XgRenderVramResourceSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = g_snapshot;
}

size_t xg_render_vram_resources_publications(
        XgRenderVramResourcePublicationSnapshot *out_publications,
        size_t capacity) {
    size_t count = 0u;

    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        const XgRenderVramResourcePublication *publication =
            &g_publications[index];

        if (!publication->occupied) continue;
        if (out_publications != NULL && count < capacity) {
            XgRenderResourceView view = {0};
            XgRenderVramResourcePublicationSnapshot snapshot = {
                    .resource_id = publication->handle.resource_id,
                    .generation = publication->handle.generation,
                    .sequence = publication->sequence,
                    .content_digest = publication->content_digest,
                    .x = publication->x,
                    .y = publication->y,
                    .width = publication->width,
                    .height = publication->height,
                    .kind = publication->kind,
            };

            if (xg_render_resource_view(publication->handle, &view) ==
                    XG_RENDER_RESOURCE_OK && view.bytes != NULL) {
                const uint8_t *bytes = (const uint8_t *)view.bytes;
                const size_t word_count = view.byte_count / 2u;
                XgRenderResourceCapabilityMetadata authority = {0};

                snapshot.owner_generation = view.owner_generation;
                snapshot.provenance_kind = view.provenance.kind;
                snapshot.provenance_receipt = view.provenance.receipt;
                snapshot.provenance_capability = view.provenance.capability;
                if (xg_render_resource_capability_validate(
                        &view.provenance, view.owner_kind,
                        view.owner_generation, &authority) ==
                        XG_RENDER_RESOURCE_CAPABILITY_OK &&
                    authority.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE)
                    snapshot.provenance_source_class =
                        authority.source.source_class;

                for (size_t word = 0u; word < word_count; ++word) {
                    const uint16_t value = (uint16_t)bytes[word * 2u] |
                        (uint16_t)bytes[word * 2u + 1u] << 8u;

                    if (value == 0u) snapshot.zero_word_count++;
                    if ((value & UINT16_C(0x7fff)) != 0u)
                        snapshot.visible_word_count++;
                    if ((value & UINT16_C(0x8000)) != 0u)
                        snapshot.mask_word_count++;
                }
            }
            out_publications[count] = snapshot;
        }
        ++count;
    }
    return count;
}

uint64_t xg_render_vram_resources_retirement_total(void) {
    return g_retirement_total;
}

size_t xg_render_vram_resources_retirements(
        XgRenderVramResourceRetirementSnapshot *out_retirements,
        size_t capacity) {
    const size_t count = g_retirement_total < XG_RENDER_VRAM_RETIREMENT_CAPACITY
        ? (size_t)g_retirement_total : XG_RENDER_VRAM_RETIREMENT_CAPACITY;
    const uint64_t first_sequence = g_retirement_total - count;

    if (out_retirements == NULL) return count;
    if (capacity < count) return count;
    for (size_t index = 0u; index < count; ++index) {
        const uint64_t sequence = first_sequence + index;

        out_retirements[index] =
            g_retirements[sequence % XG_RENDER_VRAM_RETIREMENT_CAPACITY];
    }
    return count;
}

size_t xg_render_vram_resources_checkpoint_size(void) {
    size_t size = XG_RENDER_VRAM_CHECKPOINT_HEADER_SIZE;
    uint32_t count = 0u;

    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        const XgRenderVramResourcePublication *publication = &g_publications[index];
        XgRenderResourceView view;

        if (!publication->occupied) continue;
        if (xg_render_resource_view(publication->handle, &view) !=
                XG_RENDER_RESOURCE_OK || !view.current ||
            size > SIZE_MAX - XG_RENDER_VRAM_CHECKPOINT_ENTRY_SIZE ||
            view.byte_count > SIZE_MAX - size -
                XG_RENDER_VRAM_CHECKPOINT_ENTRY_SIZE)
            return 0u;
        size += XG_RENDER_VRAM_CHECKPOINT_ENTRY_SIZE + view.byte_count;
        count++;
    }
    return count == g_snapshot.publication_count ? size : 0u;
}

bool xg_render_vram_resources_checkpoint_source_generation(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t *out_source_vram_generation) {
    const uint8_t *cursor = (const uint8_t *)checkpoint;

    if (checkpoint == NULL || out_source_vram_generation == NULL ||
        checkpoint_size < XG_RENDER_VRAM_CHECKPOINT_HEADER_SIZE ||
        read_u32(&cursor) != XG_RENDER_VRAM_CHECKPOINT_MAGIC ||
        read_u32(&cursor) != XG_RENDER_VRAM_CHECKPOINT_VERSION)
        return false;
    (void)read_u32(&cursor);
    if (read_u32(&cursor) != 0u) return false;
    (void)read_u64(&cursor);
    *out_source_vram_generation = read_u64(&cursor);
    return true;
}

XgRenderVramResourceResult xg_render_vram_resources_checkpoint_write(
        uint64_t source_vram_generation, void *out_checkpoint,
        size_t checkpoint_size) {
    uint8_t *cursor = (uint8_t *)out_checkpoint;
    const size_t required_size = xg_render_vram_resources_checkpoint_size();
    uint32_t written = 0u;

    if (required_size == 0u || out_checkpoint == NULL ||
        checkpoint_size != required_size)
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    if (g_pending.active || g_staged_publication_count != 0u)
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    write_u32(&cursor, XG_RENDER_VRAM_CHECKPOINT_MAGIC);
    write_u32(&cursor, XG_RENDER_VRAM_CHECKPOINT_VERSION);
    write_u32(&cursor, g_snapshot.publication_count);
    write_u32(&cursor, 0u);
    write_u64(&cursor, g_snapshot.owner_generation);
    write_u64(&cursor, source_vram_generation);
    write_u64(&cursor, g_next_sequence);
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_RESOURCE_CAPACITY; ++index) {
        const XgRenderVramResourcePublication *publication = &g_publications[index];
        XgRenderResourceView view;

        if (!publication->occupied) continue;
        if (xg_render_resource_view(publication->handle, &view) !=
                XG_RENDER_RESOURCE_OK || !view.current || !view.has_identity ||
            view.kind != publication->kind ||
            view.owner_kind != XG_RENDER_RESOURCE_OWNER_SCENE ||
            view.owner_generation != g_snapshot.owner_generation ||
            view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
            !same_provenance(&view.provenance, &publication->provenance) ||
            view.content_digest != publication->content_digest ||
            !same_descriptor(&view.descriptor,
                &(XgRenderResourceDescriptor){
                    .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
                    .pixel_format = publication->kind == XG_RENDER_RESOURCE_CLUT
                        ? XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555
                        : XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
                    .width = publication->width,
                    .height = publication->height,
                    .row_pitch = (uint32_t)publication->width *
                        sizeof(uint16_t),
                    .vram_x = publication->x,
                    .vram_y = publication->y,
                    .vram_width = publication->width,
                    .vram_height = publication->height,
                    .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
                    .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
                    .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
                    .flags = XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION,
                }) ||
            view.byte_count != (size_t)publication->width *
                publication->height * sizeof(uint16_t))
            return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
        memcpy(cursor, view.identity.bytes, sizeof(view.identity.bytes));
        cursor += sizeof(view.identity.bytes);
        write_u64(&cursor, publication->content_digest);
        write_u64(&cursor, publication->sequence);
        write_u16(&cursor, publication->x);
        write_u16(&cursor, publication->y);
        write_u16(&cursor, publication->width);
        write_u16(&cursor, publication->height);
        write_u32(&cursor, (uint32_t)publication->kind);
        write_u32(&cursor, (uint32_t)view.owner_kind);
        write_u64(&cursor, view.owner_generation);
        write_u32(&cursor, (uint32_t)view.state);
        write_u32(&cursor, (uint32_t)view.provenance.kind);
        write_u64(&cursor, view.provenance.receipt);
        write_u64(&cursor, view.provenance.capability);
        write_u32(&cursor, view.provenance.synthetic ? 1u : 0u);
        write_u64(&cursor, view.byte_count);
        {
            XgRenderResourceCapabilityCheckpoint authority;
            if (xg_render_resource_capability_checkpoint(
                    &view.provenance, view.owner_kind, view.owner_generation,
                    &authority) != XG_RENDER_RESOURCE_CAPABILITY_OK)
                return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
            memcpy(cursor, authority.bytes, sizeof(authority.bytes));
            cursor += sizeof(authority.bytes);
        }
        memcpy(cursor, view.bytes, view.byte_count);
        cursor += view.byte_count;
        written++;
    }
    return written == g_snapshot.publication_count
        ? XG_RENDER_VRAM_RESOURCE_OK : XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
}

static XgRenderVramResourceResult vram_resource_checkpoint_process(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_vram_generation,
        uint64_t restored_owner_generation,
        XgRenderVramResourceCheckpointRestore **out_restore) {
    XgRenderVramResourceCheckpointEntry
        entries[XG_RENDER_VRAM_RESOURCE_CAPACITY];
    const uint8_t *cursor = (const uint8_t *)checkpoint;
    const uint8_t *end = cursor + checkpoint_size;
    uint64_t owner_generation;
    uint64_t source_vram_generation;
    uint64_t next_sequence;
    uint64_t maximum_sequence = 0u;
    uint32_t count;

    if (checkpoint == NULL || restored_vram_generation == 0u ||
        restored_owner_generation == 0u ||
        checkpoint_size < XG_RENDER_VRAM_CHECKPOINT_HEADER_SIZE)
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    if (read_u32(&cursor) != XG_RENDER_VRAM_CHECKPOINT_MAGIC ||
        read_u32(&cursor) != XG_RENDER_VRAM_CHECKPOINT_VERSION)
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    count = read_u32(&cursor);
    if (read_u32(&cursor) != 0u ||
        count > XG_RENDER_VRAM_RESOURCE_CAPACITY)
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    owner_generation = read_u64(&cursor);
    source_vram_generation = read_u64(&cursor);
    next_sequence = read_u64(&cursor);
    (void)source_vram_generation;
    if (next_sequence == 0u || (count != 0u && owner_generation == 0u))
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;

    for (uint32_t index = 0u; index < count; ++index) {
        XgRenderVramResourceCheckpointEntry *entry = &entries[index];
        uint64_t wire_byte_count;
        uint32_t synthetic;
        XgRenderResourceIdentity identity;

        if ((size_t)(end - cursor) <
                XG_RENDER_VRAM_CHECKPOINT_ENTRY_SIZE)
            return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
        memcpy(entry->identity.bytes, cursor, sizeof(entry->identity.bytes));
        cursor += sizeof(entry->identity.bytes);
        entry->content_digest = read_u64(&cursor);
        entry->sequence = read_u64(&cursor);
        entry->x = read_u16(&cursor);
        entry->y = read_u16(&cursor);
        entry->width = read_u16(&cursor);
        entry->height = read_u16(&cursor);
        entry->kind = (XgRenderResourceKind)read_u32(&cursor);
        entry->owner_kind = (XgRenderResourceOwnerKind)read_u32(&cursor);
        entry->owner_generation = read_u64(&cursor);
        entry->state = (XgRenderResourceState)read_u32(&cursor);
        entry->provenance.kind =
            (XgRenderResourceProvenanceKind)read_u32(&cursor);
        entry->provenance.receipt = read_u64(&cursor);
        entry->provenance.capability = read_u64(&cursor);
        synthetic = read_u32(&cursor);
        entry->provenance.synthetic = synthetic != 0u;
        wire_byte_count = read_u64(&cursor);
        memcpy(entry->authority.bytes, cursor, sizeof(entry->authority.bytes));
        cursor += sizeof(entry->authority.bytes);
        if (wire_byte_count > SIZE_MAX)
            return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
        entry->byte_count = (size_t)wire_byte_count;
        if (!valid_kind(entry->kind) || entry->width == 0u ||
            entry->height == 0u ||
            (uint32_t)entry->x + entry->width > 1024u ||
            (uint32_t)entry->y + entry->height > 512u ||
            entry->sequence == 0u || synthetic > 1u ||
            entry->owner_kind != XG_RENDER_RESOURCE_OWNER_SCENE ||
            entry->owner_generation != owner_generation ||
            entry->state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
            entry->byte_count != (size_t)entry->width * entry->height *
                sizeof(uint16_t) ||
            entry->byte_count > (size_t)(end - cursor))
            return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
        entry->restored_provenance = entry->provenance;
        if (xg_render_resource_capability_checkpoint_validate(
                &entry->authority, &entry->provenance,
                entry->owner_kind, entry->owner_generation,
                &entry->authority_metadata) !=
                    XG_RENDER_RESOURCE_CAPABILITY_OK)
            return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
        if (out_restore != NULL) {
            if (xg_render_resource_capability_checkpoint_restore_stage(
                    &entry->authority, &entry->provenance,
                    entry->owner_kind, entry->owner_generation,
                    restored_owner_generation,
                    &entry->restored_provenance) !=
                        XG_RENDER_RESOURCE_CAPABILITY_OK ||
                !valid_provenance(&entry->restored_provenance,
                                  restored_owner_generation))
                return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
        }
        entry->bytes = cursor;
        cursor += entry->byte_count;
        for (uint32_t previous = 0u; previous < index; ++previous) {
            if (entries[previous].sequence == entry->sequence ||
                (entries[previous].kind == entry->kind &&
                 entries[previous].x == entry->x &&
                 entries[previous].y == entry->y &&
                 entries[previous].width == entry->width &&
                 entries[previous].height == entry->height))
                return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
        }
        if (entry->sequence > maximum_sequence)
            maximum_sequence = entry->sequence;
        if (xg_render_resource_digest(entry->bytes, entry->byte_count) !=
                entry->content_digest ||
            !content_identity(entry->kind, entry->x, entry->y,
                              entry->width, entry->height,
                               entry->bytes, entry->byte_count, &identity) ||
            memcmp(identity.bytes, entry->identity.bytes,
                   sizeof(identity.bytes)) != 0)
            return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
    }
    if (cursor != end || next_sequence <= maximum_sequence)
        return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    if (out_restore == NULL) return XG_RENDER_VRAM_RESOURCE_OK;

    XgRenderVramResourceCheckpointRestore *restore =
        (XgRenderVramResourceCheckpointRestore *)calloc(1u, sizeof(*restore));
    if (restore == NULL) return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;

    for (uint32_t index = 0u; index < count; ++index) {
        const XgRenderVramResourceCheckpointEntry *entry = &entries[index];
        XgRenderResourceImport import;
        XgRenderResourceHandle handle;

        if (xg_render_resource_capability_restore_stage(
                entry->restored_provenance, entry->owner_kind,
                restored_owner_generation) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK) {
            for (uint32_t staged = 0u; staged < index; ++staged) {
                xg_render_resource_restore_cancel(
                    restore->publications[staged].handle);
                free(restore->cached_publications[staged].bytes);
            }
            free(restore);
            return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
        }
        import = (XgRenderResourceImport){
            .resource_id = xg_render_resource_identity_id(&entry->identity),
            .kind = entry->kind,
            .owner_kind = entry->owner_kind,
            .owner_generation = restored_owner_generation,
            .state = entry->state,
            .provenance = entry->restored_provenance,
            .content_digest = entry->content_digest,
            .bytes = entry->bytes,
            .byte_count = entry->byte_count,
            .identity = entry->identity,
            .descriptor = vram_descriptor(
                entry->kind, entry->x, entry->y,
                entry->width, entry->height),
        };
        if (xg_render_resource_restore_stage(&import, &handle) !=
                XG_RENDER_RESOURCE_OK) {
            for (uint32_t staged = 0u; staged < index; ++staged) {
                xg_render_resource_restore_cancel(
                    restore->publications[staged].handle);
                free(restore->cached_publications[staged].bytes);
            }
            free(restore);
            return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
        }
        restore->publications[index] = (XgRenderVramResourcePublication){
            .handle = handle,
            .content_digest = entry->content_digest,
            .sequence = entry->sequence,
            .x = entry->x,
            .y = entry->y,
            .width = entry->width,
            .height = entry->height,
            .kind = entry->kind,
            .provenance = entry->restored_provenance,
            .occupied = true,
        };
        restore->cached_publications[index].bytes =
            (uint8_t *)malloc(entry->byte_count);
        if (restore->cached_publications[index].bytes == NULL) {
            for (uint32_t staged = 0u; staged <= index; ++staged) {
                xg_render_resource_restore_cancel(
                    restore->publications[staged].handle);
                free(restore->cached_publications[staged].bytes);
            }
            free(restore);
            return XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED;
        }
        memcpy(restore->cached_publications[index].bytes,
               entry->bytes, entry->byte_count);
        restore->cached_publications[index] =
            (XgRenderVramResourceCachedPublication){
                .identity = entry->identity,
                .authority = entry->authority_metadata,
                .content_digest = entry->content_digest,
                .bytes = restore->cached_publications[index].bytes,
                .byte_count = entry->byte_count,
                .x = entry->x,
                .y = entry->y,
                .width = entry->width,
                .height = entry->height,
                .kind = entry->kind,
                .occupied = true,
            };
    }

    restore->snapshot.owner_generation = restored_owner_generation;
    restore->snapshot.vram_generation = restored_vram_generation;
    restore->snapshot.publication_count = count;
    restore->next_sequence = next_sequence;
    restore->publication_count = count;
    *out_restore = restore;
    return XG_RENDER_VRAM_RESOURCE_OK;
}

XgRenderVramResourceResult xg_render_vram_resources_checkpoint_validate(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_vram_generation,
        uint64_t restored_owner_generation) {
    return vram_resource_checkpoint_process(
        checkpoint, checkpoint_size, restored_vram_generation,
        restored_owner_generation, NULL);
}

XgRenderVramResourceResult xg_render_vram_resources_checkpoint_prepare(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_vram_generation,
        uint64_t restored_owner_generation,
        XgRenderVramResourceCheckpointRestore **out_restore) {
    if (out_restore == NULL) return XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT;
    *out_restore = NULL;
    return vram_resource_checkpoint_process(
        checkpoint, checkpoint_size, restored_vram_generation,
        restored_owner_generation, out_restore);
}

void xg_render_vram_resources_checkpoint_commit(
        XgRenderVramResourceCheckpointRestore *restore) {
    if (restore == NULL) return;
    for (uint32_t index = 0u; index < restore->publication_count; ++index)
        (void)xg_render_resource_restore_commit(
            restore->publications[index].handle);
    clear_pending(false);
    memcpy(g_publications, restore->publications, sizeof(g_publications));
    g_snapshot = restore->snapshot;
    g_next_sequence = restore->next_sequence;
    clear_cached_publications();
    memcpy(g_cached_publications, restore->cached_publications,
           sizeof(g_cached_publications));
    memset(restore->cached_publications, 0,
           sizeof(restore->cached_publications));
    g_snapshot.cached_publication_count = restore->publication_count;
    free(restore);
}

void xg_render_vram_resources_checkpoint_cancel(
        XgRenderVramResourceCheckpointRestore *restore) {
    if (restore == NULL) return;
    for (uint32_t index = 0u; index < restore->publication_count; ++index) {
        xg_render_resource_restore_cancel(restore->publications[index].handle);
        free(restore->cached_publications[index].bytes);
    }
    free(restore);
}

XgRenderVramResourceResult xg_render_vram_resources_checkpoint_restore(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_vram_generation,
        uint64_t restored_owner_generation) {
    XgRenderVramResourceCheckpointRestore *restore;
    XgRenderVramResourceResult result;

    if (!xg_render_resource_capability_restore_begin())
        return XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION;
    result = xg_render_vram_resources_checkpoint_prepare(
        checkpoint, checkpoint_size, restored_vram_generation,
        restored_owner_generation, &restore);
    if (result == XG_RENDER_VRAM_RESOURCE_OK) {
        xg_render_resource_capability_restore_commit();
        xg_render_vram_resources_checkpoint_commit(restore);
    } else {
        xg_render_resource_capability_restore_cancel();
    }
    return result;
}
