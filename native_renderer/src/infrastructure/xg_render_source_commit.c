#include "xg_render_source_commit.h"
#include "xg_render_resource_repository.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct XgRenderSourceSlot {
    XgRenderSourceCommitHeader header;
    XgSemanticPassRecord passes[XG_RENDER_SCENE_PASS_CAPACITY];
    XgSemanticDrawRecord draws[XG_RENDER_SCENE_DRAW_CAPACITY];
    XgRenderNativeOperation native_operations[XG_RENDER_NATIVE_OPERATION_CAPACITY];
    XgSemanticResourceRef resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgRenderMotionRef motion_resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgSemanticResourceRef temporal_coverage[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgRenderTemporalPublication temporal_publications[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY];
    XgSemanticUiNodeRecord ui_nodes[XG_RENDER_SCENE_UI_NODE_CAPACITY];
    XgSemanticUiGlyphRunRecord
        ui_glyph_runs[XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY];
    XgSemanticUiGlyphPlacementRecord
        ui_glyph_placements[XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY];
    uint32_t generation;
    bool occupied;
} XgRenderSourceSlot;

/* The queue has bounded backpressure, but inactive queue slots do not need a
 * multi-megabyte geometry workspace. Allocate only slots actually used. */
static XgRenderSourceSlot *g_slots[XG_RENDER_SOURCE_COMMIT_CAPACITY];
static uint32_t g_slot_generations[XG_RENDER_SOURCE_COMMIT_CAPACITY];
static atomic_flag g_source_commit_lock = ATOMIC_FLAG_INIT;

static void source_commit_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_source_commit_lock,
                                              memory_order_acquire)) {
    }
}

static void source_commit_unlock(void) {
    atomic_flag_clear_explicit(&g_source_commit_lock, memory_order_release);
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;

    for (index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Vertices dominate commit digests. Pack every field in declaration order
 * (no padding bytes) and mix whole 64-bit words: the same fields, one multiply
 * per 8 bytes instead of per byte. */
static uint64_t hash_words(uint64_t hash, const uint8_t *bytes, size_t size) {
    size_t index = 0u;
    for (; index + 8u <= size; index += 8u) {
        uint64_t word;
        memcpy(&word, bytes + index, sizeof(word));
        hash ^= word;
        hash *= UINT64_C(1099511628211);
        hash ^= hash >> 32u;
    }
    return hash_bytes(hash, bytes + index, size - index);
}

#define XG_HASH_PACK(vertex, field)                                            \
    do {                                                                       \
        memcpy(packed + used, &(vertex)->field, sizeof((vertex)->field));      \
        used += sizeof((vertex)->field);                                       \
    } while (0)
#define XG_HASH_VERTEX_BODY(vertex)                                            \
    uint8_t packed[192];                                                       \
    size_t used = 0u;                                                          \
    XG_HASH_PACK(vertex, x); \
    XG_HASH_PACK(vertex, y); \
    XG_HASH_PACK(vertex, u); \
    XG_HASH_PACK(vertex, v); \
    XG_HASH_PACK(vertex, r); \
    XG_HASH_PACK(vertex, g); \
    XG_HASH_PACK(vertex, b); \
    XG_HASH_PACK(vertex, native_view_x); \
    XG_HASH_PACK(vertex, native_view_y); \
    XG_HASH_PACK(vertex, native_view_position); \
    XG_HASH_PACK(vertex, native_view_depth); \
    XG_HASH_PACK(vertex, projective_view_x); \
    XG_HASH_PACK(vertex, projective_view_y); \
    XG_HASH_PACK(vertex, projective_view_z); \
    XG_HASH_PACK(vertex, projective_offset_x); \
    XG_HASH_PACK(vertex, projective_offset_y); \
    XG_HASH_PACK(vertex, projective_native_offset_x); \
    XG_HASH_PACK(vertex, projective_native_offset_y); \
    XG_HASH_PACK(vertex, projective_distance); \
    XG_HASH_PACK(vertex, projective_position); \
    XG_HASH_PACK(vertex, temporal_depth); \
    XG_HASH_PACK(vertex, temporal_depth_valid); \
    XG_HASH_PACK(vertex, interpolation_group_id); \
    XG_HASH_PACK(vertex, interpolation_vertex_id); \
    XG_HASH_PACK(vertex, interpolation_vertex_identity_valid); \
    return hash_words(hash, packed, used)

static uint64_t hash_ir_vertex(uint64_t hash, const XgRenderIrVertex *vertex) {
    XG_HASH_VERTEX_BODY(vertex);
}

static uint64_t hash_primitive(uint64_t hash,
                               const XgRenderIrNativePrimitive *primitive) {
    const XgRenderIrMaterialState *material = &primitive->material;
    uint32_t triangle_index;

#define HASH_FIELD(value) hash = hash_bytes(hash, &(value), sizeof(value))
    HASH_FIELD(material->tpage);
    HASH_FIELD(material->texture_page_x);
    HASH_FIELD(material->texture_page_y);
    HASH_FIELD(material->clut_x);
    HASH_FIELD(material->clut_y);
    HASH_FIELD(material->draw_area_left);
    HASH_FIELD(material->draw_area_top);
    HASH_FIELD(material->draw_area_right);
    HASH_FIELD(material->draw_area_bottom);
    HASH_FIELD(material->draw_offset_x);
    HASH_FIELD(material->draw_offset_y);
    HASH_FIELD(material->texture_depth);
    HASH_FIELD(material->texture_window_mask_x);
    HASH_FIELD(material->texture_window_mask_y);
    HASH_FIELD(material->texture_window_offset_x);
    HASH_FIELD(material->texture_window_offset_y);
    HASH_FIELD(material->shading);
    HASH_FIELD(material->textured);
    HASH_FIELD(material->raw_texture);
    HASH_FIELD(material->semi_transparent);
    HASH_FIELD(material->blend_mode);
    HASH_FIELD(material->dither);
    HASH_FIELD(material->mask_set);
    HASH_FIELD(material->mask_check);
    HASH_FIELD(primitive->depth_policy);
    HASH_FIELD(primitive->depth_bias);
    HASH_FIELD(primitive->triangle_count);
    for (triangle_index = 0; triangle_index < primitive->triangle_count;
         triangle_index++) {
        const XgRenderIrTriangle *triangle =
            &primitive->triangles[triangle_index];
        uint32_t vertex_index;
        HASH_FIELD(triangle->split_index);
        HASH_FIELD(triangle->split_count);
        for (vertex_index = 0; vertex_index < 3u; vertex_index++) {
            const XgRenderIrVertex *vertex = &triangle->vertices[vertex_index];
            hash = hash_ir_vertex(hash, vertex);
        }
    }
#undef HASH_FIELD
    return hash;
}

static uint64_t hash_semantic_vertex(uint64_t hash,
                                     const GpuRenderSemanticVertex *vertex) {
    XG_HASH_VERTEX_BODY(vertex);
}

static uint64_t hash_semantic(uint64_t hash,
                              const GpuRenderSemantic *semantic) {
    const GpuRenderMaterial *material = &semantic->material;
    uint8_t packed[192];
    size_t used = 0u;
#define PACK_FIELD(value)                                                      \
    do {                                                                       \
        memcpy(packed + used, &(value), sizeof(value));                        \
        used += sizeof(value);                                                 \
    } while (0)
    PACK_FIELD(material->tpage);
    PACK_FIELD(material->texture_page_x);
    PACK_FIELD(material->texture_page_y);
    PACK_FIELD(material->clut_x);
    PACK_FIELD(material->clut_y);
    PACK_FIELD(material->draw_area_left);
    PACK_FIELD(material->draw_area_top);
    PACK_FIELD(material->draw_area_right);
    PACK_FIELD(material->draw_area_bottom);
    PACK_FIELD(material->draw_offset_x);
    PACK_FIELD(material->draw_offset_y);
    PACK_FIELD(material->texture_depth);
    PACK_FIELD(material->texture_window_mask_x);
    PACK_FIELD(material->texture_window_mask_y);
    PACK_FIELD(material->texture_window_offset_x);
    PACK_FIELD(material->texture_window_offset_y);
    PACK_FIELD(material->shading);
    PACK_FIELD(material->textured);
    PACK_FIELD(material->raw_texture);
    PACK_FIELD(material->semi_transparent);
    PACK_FIELD(material->blend_mode);
    PACK_FIELD(material->dither);
    PACK_FIELD(material->mask_set);
    PACK_FIELD(material->mask_check);
    PACK_FIELD(semantic->topology);
    PACK_FIELD(semantic->screen_space_2d);
    PACK_FIELD(semantic->native_view_effect);
    PACK_FIELD(semantic->native_view_effect_index);
    PACK_FIELD(semantic->depth_policy);
    PACK_FIELD(semantic->depth_bias);
    PACK_FIELD(semantic->triangle_count);
    PACK_FIELD(semantic->line_count);
#undef PACK_FIELD
    hash = hash_words(hash, packed, used);
#define HASH_FIELD(value) hash = hash_bytes(hash, &(value), sizeof(value))
    for (uint32_t index = 0u; index < semantic->triangle_count; ++index) {
        const GpuRenderSemanticTriangle *triangle = &semantic->triangles[index];
        HASH_FIELD(triangle->split_index);
        HASH_FIELD(triangle->split_count);
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
            hash = hash_semantic_vertex(hash, &triangle->vertices[vertex]);
    }
    for (uint32_t index = 0u; index < semantic->line_count; ++index)
        for (uint32_t vertex = 0u; vertex < 2u; ++vertex)
            hash = hash_semantic_vertex(
                hash, &semantic->lines[index].vertices[vertex]);
    HASH_FIELD(semantic->interpolation_identity.scene_id);
    HASH_FIELD(semantic->interpolation_identity.producer_id);
    HASH_FIELD(semantic->interpolation_identity.primitive_id);
    HASH_FIELD(semantic->interpolation_identity.valid);
    HASH_FIELD(semantic->submission_command_id);
#undef HASH_FIELD
    return hash;
}

static int temporal_component_compare(const void *left, const void *right) {
    const XgRenderTemporalComponent *a = left, *b = right;
    return (a->component_id > b->component_id) - (a->component_id < b->component_id);
}

static int temporal_sample_compare(const void *left, const void *right) {
    const XgRenderTemporalSample *a = left, *b = right;
#define COMPARE(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    COMPARE(component_id);
    COMPARE(vertex.interpolation_group_id);
    COMPARE(vertex.interpolation_vertex_id);
#undef COMPARE
    return 0;
}

static const XgRenderTemporalComponent *temporal_component_find(
    const XgRenderTemporalCoverageView *view, uint64_t id) {
    uint32_t lo = 0, hi = view->header->component_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (view->components[mid].component_id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < view->header->component_count && view->components[lo].component_id == id
        ? &view->components[lo] : NULL;
}

bool xg_render_temporal_coverage_view(
    XgSemanticResourceRef ref, XgRenderTemporalCoverageView *out) {
    XgRenderResourceView resource;
    if (!out || !ref.resource_id || !ref.generation || !ref.content_digest ||
        xg_render_resource_view((XgRenderResourceHandle){ref.resource_id, ref.generation},
                                &resource) != XG_RENDER_RESOURCE_OK ||
        resource.kind != XG_RENDER_RESOURCE_MODEL || resource.provenance.synthetic ||
        resource.owner_kind != XG_RENDER_RESOURCE_OWNER_SOURCE ||
        resource.content_digest != ref.content_digest || !resource.bytes ||
        resource.byte_count < sizeof(XgRenderTemporalCoverageHeader)) return false;
    const XgRenderTemporalCoverageHeader *h = resource.bytes;
    if (h->version != XG_RENDER_TEMPORAL_COVERAGE_VERSION || !h->producer_scope ||
        !h->identity.presentation_epoch || !h->identity.scene_generation || !h->source_update ||
        h->identity.scene_generation != resource.owner_generation ||
        h->component_count > XG_RENDER_TEMPORAL_COMPONENT_CAPACITY ||
        h->sample_count > XG_RENDER_TEMPORAL_SAMPLE_CAPACITY ||
        resource.byte_count != sizeof(*h) + h->component_count * sizeof(XgRenderTemporalComponent) +
            h->sample_count * sizeof(XgRenderTemporalSample)) return false;
    out->header = h;
    out->components = (const XgRenderTemporalComponent *)(h + 1);
    out->samples = (const XgRenderTemporalSample *)(out->components + h->component_count);
    return true;
}

bool xg_render_temporal_components_compatible(
    const XgRenderTemporalCoverageHeader *previous, const XgRenderTemporalComponent *a,
    const XgRenderTemporalCoverageHeader *current, const XgRenderTemporalComponent *b) {
    return previous && a && current && b &&
        previous->version == XG_RENDER_TEMPORAL_COVERAGE_VERSION &&
        current->version == previous->version &&
        previous->producer_scope == current->producer_scope &&
        previous->identity.presentation_epoch == current->identity.presentation_epoch &&
        previous->identity.scene_generation == current->identity.scene_generation &&
        previous->identity.guest_cycle < current->identity.guest_cycle &&
        previous->source_update < current->source_update &&
        a->component_id == b->component_id && a->geometry_id == b->geometry_id &&
        a->scene_id == b->scene_id && a->producer_id == b->producer_id;
}

bool xg_render_temporal_coverage_create(
    const XgPresentationIdentity *identity, uint64_t source_update, uint32_t scope,
    const XgRenderTemporalComponent *components, uint32_t component_count,
    const XgRenderTemporalSample *samples, uint32_t sample_count, XgSemanticResourceRef *out) {
    static uint64_t receipt;
    if (!identity || !identity->presentation_epoch || !identity->scene_generation ||
        !source_update || !scope || !out || (component_count && !components) ||
        (sample_count && !samples) || component_count > XG_RENDER_TEMPORAL_COMPONENT_CAPACITY ||
        sample_count > XG_RENDER_TEMPORAL_SAMPLE_CAPACITY || receipt == UINT64_MAX) return false;
    const size_t bytes = sizeof(XgRenderTemporalCoverageHeader) +
        component_count * sizeof(*components) + sample_count * sizeof(*samples);
    XgRenderTemporalCoverageHeader *h = calloc(1, bytes);
    if (!h) return false;
    h->version = XG_RENDER_TEMPORAL_COVERAGE_VERSION;
    h->producer_scope = scope;
    h->identity.presentation_epoch = identity->presentation_epoch;
    h->identity.source_sequence = identity->source_sequence;
    h->identity.guest_vblank_sequence = identity->guest_vblank_sequence;
    h->identity.guest_cycle = identity->guest_cycle;
    h->identity.scene_generation = identity->scene_generation;
    h->source_update = source_update;
    h->component_count = component_count;
    XgRenderTemporalComponent *cs = (XgRenderTemporalComponent *)(h + 1);
    XgRenderTemporalSample *vs = (XgRenderTemporalSample *)(cs + component_count);
    XgRenderTemporalCoverageView view = {h, cs, vs};
    bool ok = false;
    for (uint32_t i = 0; i < component_count; ++i) {
        const XgRenderTemporalComponent *c = &components[i];
        if (!c->component_id || !c->geometry_id || !c->scene_id || !c->producer_id) goto done;
        cs[i].component_id = c->component_id;
        cs[i].geometry_id = c->geometry_id;
        cs[i].scene_id = c->scene_id;
        cs[i].producer_id = c->producer_id;
    }
    qsort(cs, component_count, sizeof(*cs), temporal_component_compare);
    for (uint32_t i = 1; i < component_count; ++i)
        if (cs[i-1].component_id == cs[i].component_id) goto done;
    for (uint32_t i = 0; i < sample_count; ++i) {
        const GpuRenderSemanticVertex *s = &samples[i].vertex;
        GpuRenderSemanticVertex *v = &vs[i].vertex;
        if (!temporal_component_find(&view, samples[i].component_id) ||
            s->interpolation_vertex_identity_valid != 1 || !s->interpolation_group_id ||
            s->native_view_position > 1 || s->projective_position > 1) goto done;
        vs[i].component_id = samples[i].component_id;
#define COPY(field) v->field = s->field
        COPY(x); COPY(y);
        COPY(native_view_position);
        if (s->native_view_position) { COPY(native_view_x); COPY(native_view_y); }
        COPY(native_view_depth);
        COPY(projective_position);
        if (s->projective_position) {
            COPY(projective_view_x); COPY(projective_view_y); COPY(projective_view_z);
            COPY(projective_offset_x); COPY(projective_offset_y);
            COPY(projective_native_offset_x); COPY(projective_native_offset_y);
            COPY(projective_distance);
        }
        COPY(interpolation_group_id); COPY(interpolation_vertex_id);
        COPY(interpolation_vertex_identity_valid);
#undef COPY
    }
    qsort(vs, sample_count, sizeof(*vs), temporal_sample_compare);
    for (uint32_t i = 0; i < sample_count; ++i) {
        if (h->sample_count && !temporal_sample_compare(&vs[h->sample_count-1], &vs[i])) {
            if (memcmp(&vs[h->sample_count-1], &vs[i], sizeof(*vs))) goto done;
        } else vs[h->sample_count++] = vs[i];
    }
    XgRenderResourceImport import = {0};
    XgRenderResourceCapabilityMetadata metadata = {0};
    const uint64_t key[4] = {UINT64_C(0x584754454d504f52), identity->presentation_epoch,
                             identity->scene_generation, scope};
    memcpy(import.identity.bytes, key, sizeof(key));
    const uint64_t id = xg_render_resource_digest(key, sizeof(key));
    for (uint32_t i = 0; i < 8; ++i) import.identity.bytes[i] = (uint8_t)(id >> (i*8));
    import.resource_id = id;
    import.kind = XG_RENDER_RESOURCE_MODEL;
    import.owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE;
    import.owner_generation = identity->scene_generation;
    import.state = XG_RENDER_RESOURCE_NATIVE_OWNED;
    import.bytes = h;
    import.byte_count = sizeof(*h) + component_count * sizeof(*cs) + h->sample_count * sizeof(*vs);
    import.content_digest = xg_render_resource_digest(h, import.byte_count);
    metadata.kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE;
    metadata.receipt = ++receipt;
    metadata.lifetime = XG_RENDER_RESOURCE_CAPABILITY_TIMELINE;
    metadata.owner_kind = import.owner_kind;
    metadata.owner_generation = import.owner_generation;
    metadata.source.source_class = XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE;
    metadata.source.identity = import.identity;
    metadata.source.range_size = import.byte_count;
    metadata.source.range_content_digest = import.content_digest;
    if (xg_render_resource_capability_register(&metadata, &import.provenance) !=
        XG_RENDER_RESOURCE_CAPABILITY_OK) goto done;
    XgRenderResourceHandle handle;
    if (xg_render_resource_import_native(&import, &handle) == XG_RENDER_RESOURCE_OK) {
        ok = xg_render_resource_acquire_snapshot(handle, import.content_digest) == XG_RENDER_RESOURCE_OK;
        (void)xg_render_resource_retire_current(handle);
        if (ok) *out = (XgSemanticResourceRef){handle.resource_id, handle.generation, import.content_digest};
    }
    (void)xg_render_resource_capability_retire(import.provenance);
done:
    free(h);
    return ok;
}

static uint64_t commit_digest(const XgRenderSourceSlot *slot) {
    uint64_t hash = UINT64_C(1469598103934665603);
    uint32_t index;

#define HASH_FIELD(value) hash = hash_bytes(hash, &(value), sizeof(value))
    HASH_FIELD(slot->header.identity.presentation_epoch);
    HASH_FIELD(slot->header.identity.source_sequence);
    HASH_FIELD(slot->header.identity.guest_vblank_sequence);
    HASH_FIELD(slot->header.identity.guest_cycle);
    HASH_FIELD(slot->header.identity.scene_generation);
    HASH_FIELD(slot->header.scene.disc_id);
    HASH_FIELD(slot->header.scene.executable_identity);
    HASH_FIELD(slot->header.scene.primary_overlay_identity);
    HASH_FIELD(slot->header.scene.companion_set_identity);
    HASH_FIELD(slot->header.scene.authored_scene_id);
    HASH_FIELD(slot->header.scene.authored_submode);
    HASH_FIELD(slot->header.scene.module);
    HASH_FIELD(slot->header.display.width);
    HASH_FIELD(slot->header.display.height);
    HASH_FIELD(slot->header.display.display_x);
    HASH_FIELD(slot->header.display.display_y);
    HASH_FIELD(slot->header.display.aspect_num);
    HASH_FIELD(slot->header.display.aspect_den);
    HASH_FIELD(slot->header.display.depth24);
    HASH_FIELD(slot->header.display.interlaced);
    HASH_FIELD(slot->header.display.disabled);
    HASH_FIELD(slot->header.display.native_width);
    HASH_FIELD(slot->header.display.native_height);
    HASH_FIELD(slot->header.display.native_offset_x);
    HASH_FIELD(slot->header.display.temporal_hz);
    const uint16_t render_scale = slot->header.display.render_scale
        ? slot->header.display.render_scale : 1u;
    HASH_FIELD(render_scale);
    HASH_FIELD(slot->header.display.dithering_disabled);
    HASH_FIELD(slot->header.display.native_depth_test);
    HASH_FIELD(slot->header.source_interval_vblanks);
    HASH_FIELD(slot->header.discontinuity);
    HASH_FIELD(slot->header.temporally_eligible);
    HASH_FIELD(slot->header.native_work);
    HASH_FIELD(slot->header.display_boundary);
    HASH_FIELD(slot->header.native_operation_count);
    HASH_FIELD(slot->header.motion_resource_count);
    HASH_FIELD(slot->header.temporal_coverage_count);
    HASH_FIELD(slot->header.temporal_publication_count);
    for (index = 0; index < slot->header.temporal_publication_count; ++index) {
        HASH_FIELD(slot->temporal_publications[index].coverage.resource_id);
        HASH_FIELD(slot->temporal_publications[index].coverage.generation);
        HASH_FIELD(slot->temporal_publications[index].coverage.content_digest);
        HASH_FIELD(slot->temporal_publications[index].before_operation);
    }
    for (index = 0; index < slot->header.temporal_coverage_count; ++index) {
        HASH_FIELD(slot->temporal_coverage[index].resource_id);
        HASH_FIELD(slot->temporal_coverage[index].generation);
        HASH_FIELD(slot->temporal_coverage[index].content_digest);
    }
    for (index = 0u; index < slot->header.native_operation_count; ++index) {
        const XgRenderNativeOperation *operation =
            &slot->native_operations[index];
        HASH_FIELD(operation->kind);
        if (operation->kind == XG_RENDER_NATIVE_OPERATION_DRAW) {
            hash = hash_semantic(hash, &operation->semantic);
            HASH_FIELD(operation->temporal.coverage.resource_id);
            HASH_FIELD(operation->temporal.coverage.generation);
            HASH_FIELD(operation->temporal.coverage.content_digest);
            HASH_FIELD(operation->temporal.component_id);
            const XgRenderMotionDrawBinding *motion = &operation->motion;
            HASH_FIELD(motion->motion.handle.resource_id);
            HASH_FIELD(motion->motion.handle.generation);
            HASH_FIELD(motion->motion.digest);
            HASH_FIELD(motion->motion_part_index);
            HASH_FIELD(motion->triangle_count);
            for (uint32_t t = 0; t < motion->triangle_count; ++t)
                for (uint32_t v = 0; v < 3; ++v) {
                    HASH_FIELD(motion->local[t][v].x);
                    HASH_FIELD(motion->local[t][v].y);
                    HASH_FIELD(motion->local[t][v].z);
                    HASH_FIELD(motion->vertex_ids[t][v]);
                }
            continue;
        }
        HASH_FIELD(operation->dst_x);
        HASH_FIELD(operation->dst_y);
        HASH_FIELD(operation->width);
        HASH_FIELD(operation->height);
        if (operation->kind == XG_RENDER_NATIVE_OPERATION_TARGET) continue;
        HASH_FIELD(operation->mask_set);
        HASH_FIELD(operation->mask_check);
        if (operation->kind == XG_RENDER_NATIVE_OPERATION_COPY) {
            HASH_FIELD(operation->src_x);
            HASH_FIELD(operation->src_y);
        } else if (operation->kind == XG_RENDER_NATIVE_OPERATION_FILL) {
            HASH_FIELD(operation->fill_color);
        } else if (operation->kind == XG_RENDER_NATIVE_OPERATION_UPLOAD) {
            HASH_FIELD(operation->upload.resource_id);
            HASH_FIELD(operation->upload.generation);
            HASH_FIELD(operation->upload.content_digest);
        }
    }
    for (index = 0; index < slot->header.pass_count; index++) {
        const XgSemanticPassRecord *pass = &slot->passes[index];
        HASH_FIELD(pass->pass_id);
        HASH_FIELD(pass->target_surface_id);
        HASH_FIELD(pass->target_generation);
        HASH_FIELD(pass->dependency_mask);
        HASH_FIELD(pass->viewport_x);
        HASH_FIELD(pass->viewport_y);
        HASH_FIELD(pass->viewport_width);
        HASH_FIELD(pass->viewport_height);
        HASH_FIELD(pass->load_operation);
        HASH_FIELD(pass->store);
        HASH_FIELD(pass->presentation_output);
    }
    for (index = 0; index < slot->header.draw_count; index++) {
        const XgSemanticDrawRecord *draw = &slot->draws[index];
        HASH_FIELD(draw->order.pass_id);
        HASH_FIELD(draw->order.layer);
        HASH_FIELD(draw->order.authored_depth);
        HASH_FIELD(draw->order.insertion_ordinal);
        HASH_FIELD(draw->order.split_ordinal);
        HASH_FIELD(draw->provenance.state_id.scene_epoch);
        HASH_FIELD(draw->provenance.state_id.state_sequence);
        HASH_FIELD(draw->provenance.slot_index);
        HASH_FIELD(draw->source_primitive_index);
        HASH_FIELD(draw->texture_resource_id);
        HASH_FIELD(draw->texture_generation);
        HASH_FIELD(draw->clut_resource_id);
        HASH_FIELD(draw->clut_generation);
        HASH_FIELD(draw->has_provenance);
        HASH_FIELD(draw->interpolable);
        HASH_FIELD(draw->interpolation_id);
        hash = hash_primitive(hash, &draw->primitive);
        HASH_FIELD(draw->topology);
        HASH_FIELD(draw->line_count);
        HASH_FIELD(draw->screen_space_2d);
        HASH_FIELD(draw->native_view_effect);
        HASH_FIELD(draw->native_view_effect_index);
        for (uint32_t line = 0u; line < draw->line_count; ++line)
            for (uint32_t vertex = 0u; vertex < 2u; ++vertex)
                hash = hash_semantic_vertex(
                    hash, &draw->lines[line].vertices[vertex]);
    }
    for (index = 0; index < slot->header.resource_count; index++) {
        const XgSemanticResourceRef *resource = &slot->resources[index];
        HASH_FIELD(resource->resource_id);
        HASH_FIELD(resource->generation);
        HASH_FIELD(resource->content_digest);
    }
    for (index = 0; index < slot->header.surface_edge_count; index++) {
        const XgSemanticSurfaceEdge *edge = &slot->edges[index];
        HASH_FIELD(edge->source_surface_id);
        HASH_FIELD(edge->source_generation);
        HASH_FIELD(edge->target_surface_id);
        HASH_FIELD(edge->target_generation);
        HASH_FIELD(edge->kind);
        HASH_FIELD(edge->order.pass_id);
        HASH_FIELD(edge->order.layer);
        HASH_FIELD(edge->order.authored_depth);
        HASH_FIELD(edge->order.insertion_ordinal);
        HASH_FIELD(edge->order.split_ordinal);
        HASH_FIELD(edge->source.x);
        HASH_FIELD(edge->source.y);
        HASH_FIELD(edge->source.width);
        HASH_FIELD(edge->source.height);
        HASH_FIELD(edge->destination.x);
        HASH_FIELD(edge->destination.y);
        HASH_FIELD(edge->destination.width);
        HASH_FIELD(edge->destination.height);
        HASH_FIELD(edge->sample.sampler);
        HASH_FIELD(edge->sample.wrap_u);
        HASH_FIELD(edge->sample.wrap_v);
        HASH_FIELD(edge->sample.clut_resource_id);
        HASH_FIELD(edge->sample.clut_generation);
        HASH_FIELD(edge->sample.clut_x);
        HASH_FIELD(edge->sample.clut_y);
        HASH_FIELD(edge->sample.texture_window_mask_x);
        HASH_FIELD(edge->sample.texture_window_mask_y);
        HASH_FIELD(edge->sample.texture_window_offset_x);
        HASH_FIELD(edge->sample.texture_window_offset_y);
        HASH_FIELD(edge->sample.texture_depth);
        HASH_FIELD(edge->sample.blend_mode);
        HASH_FIELD(edge->sample.palette_enabled);
        HASH_FIELD(edge->sample.semi_transparent);
        HASH_FIELD(edge->sample.mask_set);
        HASH_FIELD(edge->sample.mask_check);
        HASH_FIELD(edge->effect_phase);
        HASH_FIELD(edge->effect_phase_count);
    }
    if (slot->header.ui_node_count != 0u ||
        slot->header.ui_glyph_run_count != 0u ||
        slot->header.ui_glyph_placement_count != 0u) {
        HASH_FIELD(slot->header.ui_node_count);
        HASH_FIELD(slot->header.ui_glyph_run_count);
        HASH_FIELD(slot->header.ui_glyph_placement_count);
    }
    for (index = 0u; index < slot->header.ui_node_count; ++index) {
        const XgSemanticUiNodeRecord *node = &slot->ui_nodes[index];
        HASH_FIELD(node->node_id);
        HASH_FIELD(node->parent_node_id);
        HASH_FIELD(node->owner_domain);
        HASH_FIELD(node->owner_root);
        HASH_FIELD(node->owner_receipt);
        HASH_FIELD(node->order.pass_id);
        HASH_FIELD(node->order.layer);
        HASH_FIELD(node->order.authored_depth);
        HASH_FIELD(node->order.insertion_ordinal);
        HASH_FIELD(node->order.split_ordinal);
        HASH_FIELD(node->kind);
        HASH_FIELD(node->bounds.x);
        HASH_FIELD(node->bounds.y);
        HASH_FIELD(node->bounds.width);
        HASH_FIELD(node->bounds.height);
        HASH_FIELD(node->clip.x);
        HASH_FIELD(node->clip.y);
        HASH_FIELD(node->clip.width);
        HASH_FIELD(node->clip.height);
        HASH_FIELD(node->resource.resource_id);
        HASH_FIELD(node->resource.generation);
        HASH_FIELD(node->clut.resource_id);
        HASH_FIELD(node->clut.generation);
        HASH_FIELD(node->model.resource_id);
        HASH_FIELD(node->model.generation);
        HASH_FIELD(node->semantic_id);
        HASH_FIELD(node->state);
        HASH_FIELD(node->value);
        HASH_FIELD(node->maximum);
        HASH_FIELD(node->flags);
        HASH_FIELD(node->color);
        HASH_FIELD(node->temporal_mode);
        HASH_FIELD(node->visible);
        HASH_FIELD(node->clip_enabled);
    }
    for (index = 0u; index < slot->header.ui_glyph_run_count; ++index) {
        const XgSemanticUiGlyphRunRecord *run = &slot->ui_glyph_runs[index];
        HASH_FIELD(run->glyph_run_id);
        HASH_FIELD(run->node_id);
        HASH_FIELD(run->owner_domain);
        HASH_FIELD(run->owner_root);
        HASH_FIELD(run->owner_receipt);
        HASH_FIELD(run->order.pass_id);
        HASH_FIELD(run->order.layer);
        HASH_FIELD(run->order.authored_depth);
        HASH_FIELD(run->order.insertion_ordinal);
        HASH_FIELD(run->order.split_ordinal);
        HASH_FIELD(run->atlas.resource_id);
        HASH_FIELD(run->atlas.generation);
        HASH_FIELD(run->bounds.x);
        HASH_FIELD(run->bounds.y);
        HASH_FIELD(run->bounds.width);
        HASH_FIELD(run->bounds.height);
        HASH_FIELD(run->clip.x);
        HASH_FIELD(run->clip.y);
        HASH_FIELD(run->clip.width);
        HASH_FIELD(run->clip.height);
        HASH_FIELD(run->placement_offset);
        HASH_FIELD(run->placement_count);
        HASH_FIELD(run->reveal_count);
        HASH_FIELD(run->total_count);
        HASH_FIELD(run->temporal_mode);
        HASH_FIELD(run->clip_enabled);
    }
    for (index = 0u; index < slot->header.ui_glyph_placement_count; ++index) {
        const XgSemanticUiGlyphPlacementRecord *placement =
            &slot->ui_glyph_placements[index];
        HASH_FIELD(placement->glyph_run_id);
        HASH_FIELD(placement->glyph_id);
        HASH_FIELD(placement->x);
        HASH_FIELD(placement->y);
        HASH_FIELD(placement->atlas_x);
        HASH_FIELD(placement->atlas_y);
        HASH_FIELD(placement->width);
        HASH_FIELD(placement->height);
        HASH_FIELD(placement->color);
        HASH_FIELD(placement->layer);
        HASH_FIELD(placement->source_uv.left_fp16);
        HASH_FIELD(placement->source_uv.top_fp16);
        HASH_FIELD(placement->source_uv.right_fp16);
        HASH_FIELD(placement->source_uv.bottom_fp16);
        HASH_FIELD(placement->glyph_index);
        HASH_FIELD(placement->palette_index);
    }
#undef HASH_FIELD
    return hash;
}

static XgRenderSourceSlot *builder_slot(XgRenderSourceBuilder builder) {
    XgRenderSourceSlot *slot;

    if (builder.slot >= XG_RENDER_SOURCE_COMMIT_CAPACITY)
        return NULL;
    slot = g_slots[builder.slot];
    if (!slot || !slot->occupied || slot->generation != builder.generation ||
        slot->header.state != XG_RENDER_SOURCE_BUILDING)
        return NULL;
    return slot;
}

static void release_resources(XgRenderSourceSlot *slot) {
    uint32_t index;
    for (index = 0; index < slot->header.resource_count; index++) {
        const XgSemanticResourceRef *resource = &slot->resources[index];
        (void)xg_render_resource_release((XgRenderResourceHandle){
            resource->resource_id, resource->generation });
    }
    slot->header.resource_count = 0u;
    for (index = 0; index < slot->header.motion_resource_count; ++index)
        (void)xg_render_resource_release(slot->motion_resources[index].handle);
    slot->header.motion_resource_count = 0u;
    for (index = 0; index < slot->header.temporal_coverage_count; ++index) {
        const XgSemanticResourceRef ref = slot->temporal_coverage[index];
        (void)xg_render_resource_release((XgRenderResourceHandle){ref.resource_id, ref.generation});
    }
    slot->header.temporal_coverage_count = 0;
    slot->header.temporal_publication_count = 0;
    for (index = 0u; index < slot->header.native_operation_count; ++index) {
        const XgRenderNativeOperation *operation = &slot->native_operations[index];
        if (operation->kind == XG_RENDER_NATIVE_OPERATION_UPLOAD)
            (void)xg_render_resource_release((XgRenderResourceHandle){
                operation->upload.resource_id, operation->upload.generation,
            });
    }
    slot->header.native_operation_count = 0u;
}

static void reject_slot(XgRenderSourceSlot *slot,
                        XgRenderSourceCommitState state) {
    release_resources(slot);
    slot->header.state = state;
    slot->occupied = false;
    slot->generation++;
    if (slot->generation == 0u) slot->generation = 1u;
}

static XgRenderSourceSlot *handle_slot(XgRenderSourceCommitHandle handle) {
    XgRenderSourceSlot *slot;

    if (handle.slot >= XG_RENDER_SOURCE_COMMIT_CAPACITY) return NULL;
    slot = g_slots[handle.slot];
    if (!slot || !slot->occupied || slot->generation != handle.generation) return NULL;
    return slot;
}

static bool order_less(const XgSemanticOrderKey *left,
                       const XgSemanticOrderKey *right) {
    if (left->pass_id != right->pass_id) return left->pass_id < right->pass_id;
    if (left->layer != right->layer) return left->layer < right->layer;
    if (left->authored_depth != right->authored_depth)
        return left->authored_depth < right->authored_depth;
    if (left->insertion_ordinal != right->insertion_ordinal)
        return left->insertion_ordinal < right->insertion_ordinal;
    return left->split_ordinal < right->split_ordinal;
}

static void sort_ui_records(XgRenderSourceSlot *slot) {
    uint32_t index;

    for (index = 1u; index < slot->header.ui_node_count; ++index) {
        XgSemanticUiNodeRecord node = slot->ui_nodes[index];
        uint32_t destination = index;
        while (destination != 0u &&
               order_less(&node.order,
                          &slot->ui_nodes[destination - 1u].order)) {
            slot->ui_nodes[destination] = slot->ui_nodes[destination - 1u];
            --destination;
        }
        slot->ui_nodes[destination] = node;
    }
    for (index = 1u; index < slot->header.ui_glyph_run_count; ++index) {
        XgSemanticUiGlyphRunRecord run = slot->ui_glyph_runs[index];
        uint32_t destination = index;
        while (destination != 0u &&
               order_less(&run.order,
                          &slot->ui_glyph_runs[destination - 1u].order)) {
            slot->ui_glyph_runs[destination] =
                slot->ui_glyph_runs[destination - 1u];
            --destination;
        }
        slot->ui_glyph_runs[destination] = run;
    }
}

static bool pass_exists(const XgRenderSourceSlot *slot, uint32_t pass_id) {
    uint32_t index;
    for (index = 0; index < slot->header.pass_count; index++) {
        if (slot->passes[index].pass_id == pass_id) return true;
    }
    return false;
}

static bool resource_exists(const XgRenderSourceSlot *slot,
                            uint64_t resource_id, uint64_t generation) {
    uint32_t index;
    if (resource_id == 0u) return generation == 0u;
    if (generation == 0u) return false;
    for (index = 0u; index < slot->header.resource_count; ++index) {
        if (slot->resources[index].resource_id == resource_id &&
            slot->resources[index].generation == generation)
            return true;
    }
    return false;
}

static bool resource_kind_matches(const XgRenderSourceSlot *slot,
                                  uint64_t resource_id, uint64_t generation,
                                  uint32_t allowed_kinds) {
    XgRenderResourceView view;

    if (resource_id == 0u) return generation == 0u;
    if (!resource_exists(slot, resource_id, generation) ||
        xg_render_resource_view((XgRenderResourceHandle){
            resource_id, generation }, &view) != XG_RENDER_RESOURCE_OK ||
        view.kind >= 32u)
        return false;
    return (allowed_kinds & (UINT32_C(1) << view.kind)) != 0u;
}

/* Both snapshot forms carry the same PSX material fields. Empty/outside draw
 * areas are legal GPU state (a no-op clip), not malformed work. */
#define DEFINE_MATERIAL_VALIDATOR(name, type)                                  \
    static bool name(const type *m) {                                          \
        return m->tpage <= 0x1ffu && (uint32_t)m->texture_depth <= 2u &&       \
               (uint32_t)m->texture_depth == ((m->tpage >> 7u) & 3u) &&        \
               m->texture_page_x == (m->tpage & 15u) &&                        \
               m->texture_page_y == ((m->tpage >> 4u) & 1u) &&                 \
               (uint32_t)m->blend_mode == ((m->tpage >> 5u) & 3u) &&           \
               (uint32_t)m->shading <= 1u && m->clut_x < 1024u &&              \
               (m->clut_x & 15u) == 0u && m->clut_y < 512u &&                  \
               m->draw_area_left < 1024u && m->draw_area_right < 1024u &&      \
               m->draw_area_top < 1024u && m->draw_area_bottom < 1024u &&      \
               m->draw_offset_x >= -1024 && m->draw_offset_x <= 1023 &&        \
               m->draw_offset_y >= -1024 && m->draw_offset_y <= 1023 &&        \
               m->texture_window_mask_x < 32u &&                               \
               m->texture_window_mask_y < 32u &&                               \
               m->texture_window_offset_x < 32u &&                             \
               m->texture_window_offset_y < 32u &&                             \
               (!m->raw_texture || m->textured);                               \
    }
DEFINE_MATERIAL_VALIDATOR(ir_material_valid, XgRenderIrMaterialState)
DEFINE_MATERIAL_VALIDATOR(gpu_material_valid, GpuRenderMaterial)
#undef DEFINE_MATERIAL_VALIDATOR

static bool geometry_counts_valid(GpuRenderSemanticTopology topology,
                                  uint32_t triangles, uint32_t lines,
                                  uint8_t screen_mode, uint8_t effect,
                                  uint16_t effect_index) {
    if (screen_mode > GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE ||
        effect > GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID ||
        (effect == GPU_RENDER_NATIVE_VIEW_EFFECT_NONE && effect_index != 0u) ||
        (effect == GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID &&
         effect_index >= 340u))
        return false;
    if (topology == GPU_RENDER_SEMANTIC_TRIANGLES)
        return triangles != 0u && triangles <= XG_RENDER_IR_TRIANGLE_CAPACITY &&
               triangles <= GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY &&
               lines == 0u;
    return topology == GPU_RENDER_SEMANTIC_LINES && triangles == 0u &&
           lines != 0u && lines <= GPU_RENDER_SEMANTIC_LINE_CAPACITY &&
           screen_mode == GPU_RENDER_SCREEN_SPACE_2D_NONE &&
           effect == GPU_RENDER_NATIVE_VIEW_EFFECT_NONE;
}

static bool gpu_vertex_valid(const GpuRenderSemanticVertex *vertex,
                             uint8_t screen_mode) {
    return vertex->native_view_position <= 1u &&
           vertex->projective_position <= 1u &&
           vertex->temporal_depth_valid <= 1u &&
           vertex->interpolation_vertex_identity_valid <= 1u &&
           !(screen_mode != GPU_RENDER_SCREEN_SPACE_2D_NONE &&
             vertex->native_view_position) &&
           (!vertex->projective_position || vertex->projective_distance != 0u);
}

static bool lines_valid(const GpuRenderSemanticLine *lines, uint32_t count,
                        bool flat) {
    for (uint32_t line = 0u; line < count; ++line) {
        const GpuRenderSemanticVertex *a = &lines[line].vertices[0];
        const GpuRenderSemanticVertex *b = &lines[line].vertices[1];
        if (!gpu_vertex_valid(a, GPU_RENDER_SCREEN_SPACE_2D_NONE) ||
            !gpu_vertex_valid(b, GPU_RENDER_SCREEN_SPACE_2D_NONE) ||
            (flat && (a->r != b->r || a->g != b->g || a->b != b->b)))
            return false;
    }
    return true;
}

static bool semantic_valid(const GpuRenderSemantic *semantic) {
    const GpuRenderMaterial *material = &semantic->material;
    if (!geometry_counts_valid(semantic->topology, semantic->triangle_count,
                               semantic->line_count, semantic->screen_space_2d,
                               semantic->native_view_effect,
                               semantic->native_view_effect_index) ||
        !gpu_material_valid(material) || material->textured > 1u ||
        material->raw_texture > 1u || material->semi_transparent > 1u ||
        material->dither > 1u || material->mask_set > 1u ||
        material->mask_check > 1u ||
        semantic->depth_policy > GPU_RENDER_DEPTH_TEST_WRITE ||
        semantic->depth_bias > 16u ||
        semantic->interpolation_identity.valid > 1u)
        return false;
    if (semantic->topology == GPU_RENDER_SEMANTIC_LINES)
        return !material->textured && !material->raw_texture &&
               lines_valid(semantic->lines, semantic->line_count,
                           material->shading == GPU_RENDER_SHADING_FLAT);
    for (uint32_t index = 0u; index < semantic->triangle_count; ++index) {
        const GpuRenderSemanticTriangle *triangle = &semantic->triangles[index];
        if (triangle->split_index != index ||
            triangle->split_count != semantic->triangle_count)
            return false;
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const GpuRenderSemanticVertex *a = &triangle->vertices[0];
            const GpuRenderSemanticVertex *b = &triangle->vertices[vertex];
            if (!gpu_vertex_valid(b, semantic->screen_space_2d) ||
                (material->shading == GPU_RENDER_SHADING_FLAT &&
                 (a->r != b->r || a->g != b->g || a->b != b->b)))
                return false;
        }
    }
    return true;
}

static bool draw_valid(const XgSemanticDrawRecord *draw) {
    const XgRenderIrMaterialState *material = &draw->primitive.material;
    if (!geometry_counts_valid(draw->topology, draw->primitive.triangle_count,
                               draw->line_count, draw->screen_space_2d,
                               draw->native_view_effect,
                               draw->native_view_effect_index) ||
        !ir_material_valid(material) ||
        (!draw->has_provenance &&
         (draw->provenance.state_id.scene_epoch != 0u ||
          draw->provenance.state_id.state_sequence != 0u ||
          draw->provenance.slot_index != 0u)))
        return false;
    if (draw->topology == GPU_RENDER_SEMANTIC_LINES)
        return !material->textured && !material->raw_texture &&
               lines_valid(draw->lines, draw->line_count,
                           material->shading == XG_RENDER_IR_SHADING_FLAT);
    for (uint32_t index = 0u; index < draw->primitive.triangle_count; ++index) {
        const XgRenderIrTriangle *triangle = &draw->primitive.triangles[index];
        if (triangle->split_index != index ||
            triangle->split_count != draw->primitive.triangle_count)
            return false;
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const XgRenderIrVertex *a = &triangle->vertices[0];
            const XgRenderIrVertex *b = &triangle->vertices[vertex];
            if ((draw->screen_space_2d != GPU_RENDER_SCREEN_SPACE_2D_NONE &&
                 b->native_view_position) ||
                (b->projective_position && b->projective_distance == 0u) ||
                (material->shading == XG_RENDER_IR_SHADING_FLAT &&
                 (a->r != b->r || a->g != b->g || a->b != b->b)))
                return false;
        }
    }
    return true;
}

static bool native_operation_valid(const XgRenderNativeOperation *operation) {
    XgRenderResourceView view;
    if (operation == NULL ||
        (uint32_t)operation->kind > XG_RENDER_NATIVE_OPERATION_TARGET)
        return false;
    if (operation->kind == XG_RENDER_NATIVE_OPERATION_DRAW) {
        const XgRenderTemporalBinding *binding = &operation->temporal;
        if (binding->coverage.resource_id) {
            XgRenderTemporalCoverageView coverage;
            if (!xg_render_temporal_coverage_view(binding->coverage, &coverage)) return false;
            const XgRenderTemporalComponent *c = temporal_component_find(&coverage, binding->component_id);
            if (!c || c->producer_id != operation->semantic.interpolation_identity.producer_id ||
                c->scene_id != operation->semantic.interpolation_identity.scene_id) return false;
        } else if (binding->component_id || binding->coverage.generation ||
                   binding->coverage.content_digest) return false;
        return semantic_valid(&operation->semantic) &&
            xg_render_motion_binding_valid(&operation->motion) &&
            (!operation->motion.motion.handle.resource_id ||
             (operation->semantic.topology == GPU_RENDER_SEMANTIC_TRIANGLES &&
               operation->semantic.triangle_count == operation->motion.triangle_count));
    }
    if (operation->dst_x >= 1024u || operation->dst_y >= 512u ||
        operation->width == 0u || operation->width > 1024u ||
        operation->height == 0u || operation->height > 512u)
        return false;
    if (operation->kind == XG_RENDER_NATIVE_OPERATION_COPY)
        return operation->src_x < 1024u && operation->src_y < 512u;
    if (operation->kind == XG_RENDER_NATIVE_OPERATION_TARGET)
        return operation->width <= 1024u - operation->dst_x &&
            operation->height <= 512u - operation->dst_y;
    if (operation->kind == XG_RENDER_NATIVE_OPERATION_FILL) return true;
    if (operation->upload.resource_id == 0u ||
        operation->upload.generation == 0u ||
        operation->upload.content_digest == 0u ||
        xg_render_resource_view(
            (XgRenderResourceHandle){operation->upload.resource_id,
                                     operation->upload.generation},
            &view) != XG_RENDER_RESOURCE_OK)
        return false;
    return view.bytes != NULL &&
           view.content_digest == operation->upload.content_digest &&
           view.descriptor.version == XG_RENDER_RESOURCE_DESCRIPTOR_VERSION &&
           xg_render_resource_descriptor_validate(&view.descriptor) &&
           (view.descriptor.pixel_format ==
                XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555 ||
            view.descriptor.pixel_format ==
                XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555) &&
           view.descriptor.width == operation->width &&
           view.descriptor.height == operation->height &&
           view.descriptor.row_pitch >= (uint32_t)operation->width * 2u &&
           view.byte_count >=
               (uint64_t)(operation->height - 1u) * view.descriptor.row_pitch +
                   (uint32_t)operation->width * 2u;
}

static bool has_scene_records(const XgRenderSourceSlot *slot) {
    return slot->header.pass_count != 0u || slot->header.draw_count != 0u ||
           slot->header.surface_edge_count != 0u ||
           slot->header.ui_node_count != 0u ||
           slot->header.ui_glyph_run_count != 0u ||
           slot->header.ui_glyph_placement_count != 0u;
}

static bool ui_owner_valid(XgRenderUiOwnerDomain owner_domain,
                           uint32_t owner_root, uint64_t owner_receipt) {
    return owner_domain > XG_RENDER_UI_OWNER_NONE &&
           owner_domain < XG_RENDER_UI_OWNER_COUNT && owner_root != 0u &&
           owner_receipt != 0u;
}

static bool ui_rect_valid(const XgSemanticUiRect *rect) {
    return rect->width >= 0 && rect->height >= 0;
}

static bool ui_clip_valid(const XgSemanticUiRect *clip, bool enabled) {
    return ui_rect_valid(clip) &&
           (!enabled || (clip->width > 0 && clip->height > 0));
}

static int32_t ui_node_index(const XgRenderSourceSlot *slot,
                             uint64_t node_id, uint32_t limit) {
    uint32_t index;

    for (index = 0u; index < limit; ++index)
        if (slot->ui_nodes[index].node_id == node_id) return (int32_t)index;
    return -1;
}

static XgRenderSourceCommitResult validate_ui(
        const XgRenderSourceSlot *slot, uint32_t sampled_image_kinds,
        uint32_t clut_kinds) {
    bool placement_used[XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY] = {false};
    uint32_t index;

    for (index = 0u; index < slot->header.ui_node_count; ++index) {
        const XgSemanticUiNodeRecord *node = &slot->ui_nodes[index];
        const uint32_t model_kinds = UINT32_C(1) << XG_RENDER_RESOURCE_MODEL;
        uint32_t duplicate;

        if (node->node_id == 0u ||
            !ui_owner_valid(node->owner_domain, node->owner_root,
                            node->owner_receipt) ||
            (uint32_t)node->kind >= XG_SEMANTIC_UI_NODE_KIND_COUNT ||
            node->temporal_mode != XG_SEMANTIC_UI_TEMPORAL_DISCRETE ||
            !ui_rect_valid(&node->bounds) ||
            !ui_clip_valid(&node->clip, node->clip_enabled) ||
            !resource_kind_matches(slot, node->resource.resource_id,
                                   node->resource.generation,
                                   sampled_image_kinds) ||
            !resource_kind_matches(slot, node->clut.resource_id,
                                   node->clut.generation, clut_kinds) ||
            !resource_kind_matches(slot, node->model.resource_id,
                                   node->model.generation, model_kinds) ||
            (node->kind == XG_SEMANTIC_UI_NODE_MODEL_PREVIEW) !=
                (node->model.resource_id != 0u))
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        if (!pass_exists(slot, node->order.pass_id))
            return XG_RENDER_SOURCE_COMMIT_INVALID_ORDER;
        for (duplicate = 0u; duplicate < index; ++duplicate)
            if (slot->ui_nodes[duplicate].node_id == node->node_id)
                return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        if (node->parent_node_id != 0u &&
            ui_node_index(slot, node->parent_node_id, index) < 0)
            return XG_RENDER_SOURCE_COMMIT_INVALID_ORDER;
    }
    for (index = 0u; index < slot->header.ui_glyph_run_count; ++index) {
        const XgSemanticUiGlyphRunRecord *run = &slot->ui_glyph_runs[index];
        const int32_t node_index = ui_node_index(
            slot, run->node_id, slot->header.ui_node_count);
        const XgSemanticUiNodeRecord *node;
        XgRenderResourceView atlas;
        uint64_t minimum_pitch;
        uint64_t palette_bank_count = 0u;
        bool indexed;
        uint32_t placement_index;
        uint32_t duplicate;

        if (run->glyph_run_id == 0u || node_index < 0 ||
            !ui_owner_valid(run->owner_domain, run->owner_root,
                            run->owner_receipt) ||
            run->temporal_mode != XG_SEMANTIC_UI_TEMPORAL_DISCRETE ||
            !ui_rect_valid(&run->bounds) ||
            !ui_clip_valid(&run->clip, run->clip_enabled) ||
            run->reveal_count > run->total_count ||
            run->placement_offset > slot->header.ui_glyph_placement_count ||
            run->placement_count > slot->header.ui_glyph_placement_count -
                                       run->placement_offset ||
            !resource_kind_matches(
                slot, run->atlas.resource_id, run->atlas.generation,
                UINT32_C(1) << XG_RENDER_RESOURCE_GLYPH_ATLAS) ||
            xg_render_resource_view(run->atlas, &atlas) !=
                XG_RENDER_RESOURCE_OK ||
            atlas.descriptor.version !=
                XG_RENDER_RESOURCE_DESCRIPTOR_VERSION ||
            !xg_render_resource_descriptor_validate(&atlas.descriptor))
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        node = &slot->ui_nodes[node_index];
        if (node->owner_domain != run->owner_domain ||
            node->owner_root != run->owner_root ||
            node->owner_receipt != run->owner_receipt)
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        indexed = atlas.descriptor.pixel_format ==
                      XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4 ||
                  atlas.descriptor.pixel_format ==
                      XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8;
        switch (atlas.descriptor.pixel_format) {
        case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8:
            minimum_pitch = (uint64_t)atlas.descriptor.width * 4u; break;
        case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24:
            minimum_pitch = (uint64_t)atlas.descriptor.width * 3u; break;
        case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555:
            minimum_pitch = (uint64_t)atlas.descriptor.width * 2u; break;
        case XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4:
            minimum_pitch = ((uint64_t)atlas.descriptor.width + 1u) / 2u; break;
        case XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8:
            minimum_pitch = atlas.descriptor.width; break;
        default:
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        }
        if (atlas.descriptor.row_pitch < minimum_pitch ||
            atlas.descriptor.row_pitch == 0u ||
            atlas.descriptor.height >
                atlas.byte_count / atlas.descriptor.row_pitch ||
            (indexed && node->clut.resource_id == 0u))
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        /* The run samples the CLUT retained by its referenced node. */
        if (node->clut.resource_id != 0u) {
            XgRenderResourceView clut;
            if (xg_render_resource_view(node->clut, &clut) !=
                    XG_RENDER_RESOURCE_OK ||
                clut.descriptor.version !=
                    XG_RENDER_RESOURCE_DESCRIPTOR_VERSION ||
                (clut.descriptor.pixel_format !=
                     XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555 &&
                 clut.descriptor.pixel_format !=
                     XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555) ||
                !xg_render_resource_descriptor_validate(&clut.descriptor) ||
                clut.descriptor.row_pitch <
                    (uint64_t)clut.descriptor.width * 2u ||
                clut.descriptor.row_pitch == 0u ||
                clut.descriptor.height >
                    clut.byte_count / clut.descriptor.row_pitch)
                return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
            /* Logical row-major texels exclude row padding; count only full
             * banks so a selector cannot address a partial trailing bank. */
            if (indexed)
                palette_bank_count =
                    (uint64_t)clut.descriptor.width * clut.descriptor.height /
                    (atlas.descriptor.pixel_format ==
                         XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4 ? 16u : 256u);
        }
        if (!pass_exists(slot, run->order.pass_id))
            return XG_RENDER_SOURCE_COMMIT_INVALID_ORDER;
        for (duplicate = 0u; duplicate < index; ++duplicate)
            if (slot->ui_glyph_runs[duplicate].glyph_run_id ==
                    run->glyph_run_id)
                return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        for (placement_index = run->placement_offset;
             placement_index < run->placement_offset + run->placement_count;
             ++placement_index) {
            const XgSemanticUiGlyphPlacementRecord *placement =
                &slot->ui_glyph_placements[placement_index];
            const XgRenderUiLayoutUvRect *uv = &placement->source_uv;
            if (placement_used[placement_index] ||
                placement->glyph_run_id != run->glyph_run_id ||
                placement->width == 0u || placement->height == 0u ||
                (int64_t)placement->x + placement->width > INT32_MAX ||
                (int64_t)placement->y + placement->height > INT32_MAX ||
                (uint32_t)placement->layer >=
                    XG_SEMANTIC_UI_GLYPH_LAYER_COUNT ||
                placement->glyph_index >= run->total_count ||
                uv->left_fp16 >= uv->right_fp16 ||
                uv->top_fp16 >= uv->bottom_fp16 ||
                uv->right_fp16 > ((uint64_t)atlas.descriptor.width << 16u) ||
                uv->bottom_fp16 > ((uint64_t)atlas.descriptor.height << 16u) ||
                uv->right_fp16 > (UINT64_C(1) << 32u) ||
                uv->bottom_fp16 > (UINT64_C(1) << 32u) ||
                placement->atlas_x != (uv->left_fp16 >> 16u) ||
                placement->atlas_y != (uv->top_fp16 >> 16u) ||
                (indexed && placement->palette_index >= palette_bank_count) ||
                (!indexed && placement->palette_index != 0u))
                return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
            /* Reveal is indexed in the decoded run, not in this clipped list.
             * Either layer, or both, may be absent for any logical glyph. */
            for (duplicate = run->placement_offset;
                 duplicate < placement_index; ++duplicate) {
                const XgSemanticUiGlyphPlacementRecord *prior =
                    &slot->ui_glyph_placements[duplicate];
                if (prior->glyph_index == placement->glyph_index &&
                    (prior->layer == placement->layer ||
                     prior->glyph_id != placement->glyph_id))
                    return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
            }
            placement_used[placement_index] = true;
        }
    }
    for (index = 0u; index < slot->header.ui_glyph_placement_count; ++index)
        if (!placement_used[index])
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static void source_commit_reset_unlocked(void) {
    uint32_t index;
    for (index = 0; index < XG_RENDER_SOURCE_COMMIT_CAPACITY; index++) {
        XgRenderSourceSlot *slot = g_slots[index];
        uint32_t generation = (slot ? slot->generation : g_slot_generations[index]) + 1u;
        if (generation == 0u) generation = 1u;
        if (slot && slot->occupied) release_resources(slot);
        free(slot);
        g_slots[index] = NULL;
        g_slot_generations[index] = generation;
    }
}

static void source_commit_cancel_builders_unlocked(void) {
    uint32_t index;
    for (index = 0; index < XG_RENDER_SOURCE_COMMIT_CAPACITY; index++) {
        XgRenderSourceSlot *slot = g_slots[index];
        if (!slot || !slot->occupied || slot->header.state != XG_RENDER_SOURCE_BUILDING)
            continue;
        reject_slot(slot, XG_RENDER_SOURCE_CANCELLED);
    }
}

static XgRenderSourceCommitResult source_commit_begin_unlocked(
        const XgPresentationIdentity *identity,
        const XgSemanticSceneIdentity *scene,
        const XgSemanticDisplayState *display,
        uint32_t source_interval_vblanks,
        bool discontinuity,
        bool temporally_eligible,
        XgRenderSourceBuilder *out_builder) {
    uint32_t index;

    if (identity == NULL || scene == NULL || display == NULL ||
        out_builder == NULL || identity->presentation_epoch == 0u ||
        identity->source_sequence == 0u || display->render_scale > XG_SEMANTIC_RENDER_SCALE_MAX)
        return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    for (index = 0; index < XG_RENDER_SOURCE_COMMIT_CAPACITY; index++) {
        XgRenderSourceSlot *slot = g_slots[index];
        if (slot && slot->occupied) continue;
        if (!slot) {
            slot = malloc(sizeof(*slot));
            if (!slot) return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
            slot->generation = g_slot_generations[index];
            g_slots[index] = slot;
        }
        if (slot->generation == 0u) slot->generation = 1u;
        memset(&slot->header, 0, sizeof(slot->header));
        slot->header.identity = *identity;
        slot->header.scene = *scene;
        slot->header.display = *display;
        slot->header.source_interval_vblanks = source_interval_vblanks;
        slot->header.discontinuity = discontinuity;
        slot->header.temporally_eligible = temporally_eligible && !discontinuity;
        slot->header.display_boundary = true;
        slot->header.state = XG_RENDER_SOURCE_BUILDING;
        slot->occupied = true;
        *out_builder = (XgRenderSourceBuilder){ index, slot->generation };
        return XG_RENDER_SOURCE_COMMIT_OK;
    }
    return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
}

static XgRenderSourceCommitResult source_commit_append_pass_unlocked(
        XgRenderSourceBuilder builder, const XgSemanticPassRecord *pass) {
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (slot->header.native_work) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (pass == NULL || pass->pass_id >= 32u || pass_exists(slot, pass->pass_id))
        return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    if (pass->viewport_width == 0u || pass->viewport_height == 0u)
        return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    if (slot->header.pass_count >= XG_RENDER_SCENE_PASS_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    slot->passes[slot->header.pass_count++] = *pass;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult source_commit_append_draw_unlocked(
        XgRenderSourceBuilder builder, const XgSemanticDrawRecord *draw) {
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (slot->header.native_work) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (draw == NULL || !draw_valid(draw) ||
        !pass_exists(slot, draw->order.pass_id))
        return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    if (slot->header.draw_count >= XG_RENDER_SCENE_DRAW_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    slot->draws[slot->header.draw_count++] = *draw;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult source_commit_append_resource_unlocked(
        XgRenderSourceBuilder builder, const XgSemanticResourceRef *resource) {
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (resource == NULL || resource->resource_id == 0u ||
        resource->generation == 0u || resource->content_digest == 0u)
        return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    if (slot->header.resource_count >= XG_RENDER_SCENE_RESOURCE_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    if (xg_render_resource_acquire_snapshot((XgRenderResourceHandle){
            resource->resource_id, resource->generation },
            resource->content_digest) != XG_RENDER_RESOURCE_OK)
        return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    slot->resources[slot->header.resource_count++] = *resource;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult source_commit_append_surface_edge_unlocked(
        XgRenderSourceBuilder builder, const XgSemanticSurfaceEdge *edge) {
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (slot->header.native_work) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (edge == NULL || edge->source_surface_id == 0u ||
        edge->source_generation == 0u || edge->target_surface_id == 0u ||
        edge->target_generation == 0u || edge->source.x < 0 ||
        edge->source.y < 0 || edge->source.width == 0u ||
        edge->source.height == 0u || edge->destination.width == 0u ||
        edge->destination.height == 0u || edge->order.pass_id >= 32u ||
        edge->kind > XG_SEMANTIC_SURFACE_MOVIE ||
        edge->sample.sampler < XG_RENDER_RESOURCE_SAMPLER_NEAREST ||
        edge->sample.sampler > XG_RENDER_RESOURCE_SAMPLER_LINEAR ||
        edge->sample.wrap_u < XG_RENDER_RESOURCE_WRAP_CLAMP ||
        edge->sample.wrap_u > XG_RENDER_RESOURCE_WRAP_MIRROR ||
        edge->sample.wrap_v < XG_RENDER_RESOURCE_WRAP_CLAMP ||
        edge->sample.wrap_v > XG_RENDER_RESOURCE_WRAP_MIRROR ||
        edge->sample.texture_depth > XG_RENDER_IR_TEXTURE_15_BIT ||
        edge->sample.blend_mode > XG_RENDER_IR_BLEND_ADD_QUARTER ||
        (edge->effect_phase_count == 0u && edge->effect_phase != 0u) ||
        (edge->effect_phase_count != 0u &&
         edge->effect_phase >= edge->effect_phase_count))
        return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    if (edge->kind == XG_SEMANTIC_SURFACE_FEEDBACK &&
        edge->source_surface_id == edge->target_surface_id &&
        edge->source_generation >= edge->target_generation)
        return XG_RENDER_SOURCE_COMMIT_INVALID_ORDER;
    if (slot->header.surface_edge_count >= XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    slot->edges[slot->header.surface_edge_count++] = *edge;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult source_commit_append_ui_node_unlocked(
        XgRenderSourceBuilder builder, const XgSemanticUiNodeRecord *node) {
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (slot->header.native_work) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (node == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    if (slot->header.ui_node_count >= XG_RENDER_SCENE_UI_NODE_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    slot->ui_nodes[slot->header.ui_node_count++] = *node;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult source_commit_append_ui_glyph_run_unlocked(
        XgRenderSourceBuilder builder,
        const XgSemanticUiGlyphRunRecord *glyph_run) {
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (slot->header.native_work) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (glyph_run == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    if (slot->header.ui_glyph_run_count >=
            XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    slot->ui_glyph_runs[slot->header.ui_glyph_run_count++] = *glyph_run;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult
source_commit_append_ui_glyph_placement_unlocked(
        XgRenderSourceBuilder builder,
        const XgSemanticUiGlyphPlacementRecord *placement) {
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (slot->header.native_work) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (placement == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    if (slot->header.ui_glyph_placement_count >=
            XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    slot->ui_glyph_placements[
        slot->header.ui_glyph_placement_count++] = *placement;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult source_commit_seal_unlocked(
        XgRenderSourceBuilder builder, XgRenderSourceCommitHandle *out_commit) {
    const uint32_t sampled_image_kinds =
        (UINT32_C(1) << XG_RENDER_RESOURCE_TEXTURE) |
        (UINT32_C(1) << XG_RENDER_RESOURCE_GLYPH_ATLAS) |
        (UINT32_C(1) << XG_RENDER_RESOURCE_GENERATED_SURFACE) |
        (UINT32_C(1) << XG_RENDER_RESOURCE_FRAMEBUFFER) |
        (UINT32_C(1) << XG_RENDER_RESOURCE_MOVIE_FRAME);
    const uint32_t clut_kinds = UINT32_C(1) << XG_RENDER_RESOURCE_CLUT;
    const uint32_t surface_kinds =
        (UINT32_C(1) << XG_RENDER_RESOURCE_GENERATED_SURFACE) |
        (UINT32_C(1) << XG_RENDER_RESOURCE_FRAMEBUFFER) |
        (UINT32_C(1) << XG_RENDER_RESOURCE_MOVIE_FRAME);
    const uint32_t edge_source_kinds = surface_kinds |
        (UINT32_C(1) << XG_RENDER_RESOURCE_TEXTURE);
    XgRenderSourceSlot *slot = builder_slot(builder);
    XgRenderResourceView resource_view;
    uint32_t presentation_output_count = 0u;
    uint32_t index;

    if (slot == NULL) return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    if (out_commit == NULL ||
        (!slot->header.native_work && (slot->header.identity.scene_generation == 0u ||
                                      slot->header.pass_count == 0u ||
                                      slot->header.source_interval_vblanks == 0u)) ||
        (slot->header.native_work && has_scene_records(slot))) {
        reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
        return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    }
    sort_ui_records(slot);
    for (index = 0u; index < slot->header.resource_count; ++index) {
        const XgSemanticResourceRef *resource = &slot->resources[index];
        if (xg_render_resource_view((XgRenderResourceHandle){
                resource->resource_id, resource->generation },
                &resource_view) != XG_RENDER_RESOURCE_OK ||
            (!resource_view.current && resource_view.retain_count == 0u) ||
            resource_view.content_digest != resource->content_digest) {
            reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        }
    }
    if (slot->header.native_work) {
        for (index = 0; index < slot->header.temporal_publication_count; ++index) {
            const uint32_t gap = slot->temporal_publications[index].before_operation;
            if (gap > slot->header.native_operation_count ||
                (index && gap < slot->temporal_publications[index-1].before_operation)) {
                reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
                return XG_RENDER_SOURCE_COMMIT_INVALID_ORDER;
            }
        }
        for (index = 0; index < slot->header.temporal_coverage_count; ++index) {
            XgRenderTemporalCoverageView coverage;
            if (!xg_render_temporal_coverage_view(slot->temporal_coverage[index], &coverage) ||
                coverage.header->identity.presentation_epoch != slot->header.identity.presentation_epoch ||
                coverage.header->identity.scene_generation != slot->header.identity.scene_generation ||
                coverage.header->identity.guest_cycle > slot->header.identity.guest_cycle) {
                reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
                return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
            }
        }
        for (index = 0u; index < slot->header.native_operation_count; ++index) {
            if (!native_operation_valid(&slot->native_operations[index])) {
                reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
                return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
            }
        }
        /* A work chunk is an ordered mutation stream. Display state (including
         * disabled or not-yet-sized scanout) does not constrain VRAM writes. */
        goto sealed;
    }
    for (index = 0; index < slot->header.pass_count; index++) {
        const XgSemanticPassRecord *pass = &slot->passes[index];
        XgRenderResourceView target_view;
        uint32_t allowed_dependencies = 0u;
        uint32_t prior;
        for (prior = 0; prior < index; prior++)
            allowed_dependencies |= UINT32_C(1) << slot->passes[prior].pass_id;
        if ((pass->dependency_mask & ~allowed_dependencies) != 0u) {
            reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
            return XG_RENDER_SOURCE_COMMIT_INVALID_ORDER;
        }
        if (pass->viewport_width == 0u || pass->viewport_height == 0u ||
            pass->viewport_x > UINT32_MAX - pass->viewport_width ||
            pass->viewport_y > UINT32_MAX - pass->viewport_height ||
            !resource_kind_matches(slot, pass->target_surface_id,
                                    pass->target_generation,
                                    surface_kinds) ||
            xg_render_resource_view((XgRenderResourceHandle){
                pass->target_surface_id, pass->target_generation},
                &target_view) != XG_RENDER_RESOURCE_OK ||
            pass->viewport_x + pass->viewport_width >
                target_view.descriptor.width ||
            pass->viewport_y + pass->viewport_height >
                target_view.descriptor.height ||
            (pass->presentation_output && !pass->store)) {
            reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        }
        if (pass->presentation_output) ++presentation_output_count;
    }
    if (presentation_output_count != 1u) {
        reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
        return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    }
    for (index = 1; index < slot->header.draw_count; index++) {
        if (order_less(&slot->draws[index].order,
                       &slot->draws[index - 1u].order)) {
            reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
            return XG_RENDER_SOURCE_COMMIT_INVALID_ORDER;
        }
    }
    for (index = 0u; index < slot->header.draw_count; ++index) {
        const XgSemanticDrawRecord *draw = &slot->draws[index];
        if (!draw_valid(draw) ||
            (draw->primitive.material.textured &&
             (draw->texture_resource_id == 0u ||
              (draw->primitive.material.texture_depth != XG_RENDER_IR_TEXTURE_15_BIT &&
               draw->clut_resource_id == 0u))) ||
            !resource_kind_matches(slot, draw->texture_resource_id,
                                   draw->texture_generation,
                                   sampled_image_kinds) ||
            !resource_kind_matches(slot, draw->clut_resource_id,
                                   draw->clut_generation, clut_kinds)) {
            reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        }
    }
    for (index = 0u; index < slot->header.surface_edge_count; ++index) {
        const XgSemanticSurfaceEdge *edge = &slot->edges[index];
        XgRenderResourceView source_view;
        XgRenderResourceView target_view;
        const int64_t destination_right =
            (int64_t)edge->destination.x + edge->destination.width;
        const int64_t destination_bottom =
            (int64_t)edge->destination.y + edge->destination.height;
        if (!pass_exists(slot, edge->order.pass_id) ||
            !resource_kind_matches(slot, edge->source_surface_id,
                                    edge->source_generation,
                                    edge->kind == XG_SEMANTIC_SURFACE_COPY ||
                                            edge->kind ==
                                                XG_SEMANTIC_SURFACE_SAMPLE
                                        ? edge_source_kinds : surface_kinds) ||
            !resource_kind_matches(slot, edge->target_surface_id,
                                    edge->target_generation, surface_kinds) ||
            !resource_kind_matches(slot, edge->sample.clut_resource_id,
                                    edge->sample.clut_generation,
                                    clut_kinds) ||
            xg_render_resource_view((XgRenderResourceHandle){
                edge->source_surface_id, edge->source_generation},
                &source_view) != XG_RENDER_RESOURCE_OK ||
            xg_render_resource_view((XgRenderResourceHandle){
                edge->target_surface_id, edge->target_generation},
                &target_view) != XG_RENDER_RESOURCE_OK ||
            (uint64_t)(uint32_t)edge->source.x + edge->source.width >
                source_view.descriptor.width ||
            (uint64_t)(uint32_t)edge->source.y + edge->source.height >
                source_view.descriptor.height ||
            edge->destination.x >= (int64_t)target_view.descriptor.width ||
            edge->destination.y >= (int64_t)target_view.descriptor.height ||
            destination_right <= 0 || destination_bottom <= 0) {
            reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
            return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
        }
    }
    {
        XgRenderSourceCommitResult ui_result = validate_ui(
            slot, sampled_image_kinds, clut_kinds);
        if (ui_result != XG_RENDER_SOURCE_COMMIT_OK) {
            reject_slot(slot, XG_RENDER_SOURCE_REJECTED);
            return ui_result;
        }
    }
sealed:
    slot->header.digest = commit_digest(slot);
    slot->header.state = XG_RENDER_SOURCE_SEALED;
    *out_commit = (XgRenderSourceCommitHandle){ builder.slot,
                                                 builder.generation };
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static void source_commit_cancel_unlocked(XgRenderSourceBuilder builder) {
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL) return;
    reject_slot(slot, XG_RENDER_SOURCE_CANCELLED);
}

static XgRenderSourceCommitResult source_commit_mark_queued_unlocked(
        XgRenderSourceCommitHandle commit) {
    XgRenderSourceSlot *slot = handle_slot(commit);
    if (slot == NULL || slot->header.state != XG_RENDER_SOURCE_SEALED)
        return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    slot->header.state = XG_RENDER_SOURCE_QUEUED;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult source_commit_retire_unlocked(
        XgRenderSourceCommitHandle commit) {
    XgRenderSourceSlot *slot = handle_slot(commit);
    if (slot == NULL || (slot->header.state != XG_RENDER_SOURCE_SEALED &&
                         slot->header.state != XG_RENDER_SOURCE_QUEUED))
        return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    reject_slot(slot, XG_RENDER_SOURCE_RETIRED);
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult source_commit_header_copy_unlocked(
        XgRenderSourceCommitHandle commit, XgRenderSourceCommitHeader *out_header) {
    XgRenderSourceSlot *slot = handle_slot(commit);
    if (slot == NULL || out_header == NULL)
        return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    if (slot->header.state != XG_RENDER_SOURCE_SEALED &&
        slot->header.state != XG_RENDER_SOURCE_QUEUED)
        return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    *out_header = slot->header;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

#define DEFINE_COPY_FUNCTION(name, type, field, count_field)                    \
static XgRenderSourceCommitResult name(XgRenderSourceCommitHandle commit,       \
                                        size_t index, type *out_value) {         \
    XgRenderSourceSlot *slot = handle_slot(commit);                             \
    if (slot == NULL || out_value == NULL)                                     \
        return XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;                        \
    if (slot->header.state != XG_RENDER_SOURCE_SEALED &&                        \
        slot->header.state != XG_RENDER_SOURCE_QUEUED)                          \
        return XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;                      \
    if (index >= slot->header.count_field)                                     \
        return XG_RENDER_SOURCE_COMMIT_OUT_OF_RANGE;                            \
    *out_value = slot->field[index];                                            \
    return XG_RENDER_SOURCE_COMMIT_OK;                                          \
}

DEFINE_COPY_FUNCTION(source_commit_pass_copy_unlocked,
                     XgSemanticPassRecord, passes, pass_count)
DEFINE_COPY_FUNCTION(source_commit_draw_copy_unlocked,
                     XgSemanticDrawRecord, draws, draw_count)
DEFINE_COPY_FUNCTION(source_commit_copy_native_operation_unlocked,
                     XgRenderNativeOperation, native_operations, native_operation_count)
DEFINE_COPY_FUNCTION(source_commit_motion_resource_copy_unlocked,
                     XgRenderMotionRef, motion_resources, motion_resource_count)
DEFINE_COPY_FUNCTION(source_commit_temporal_coverage_copy_unlocked,
                     XgSemanticResourceRef, temporal_coverage, temporal_coverage_count)
DEFINE_COPY_FUNCTION(source_commit_temporal_publication_copy_unlocked,
                     XgRenderTemporalPublication, temporal_publications, temporal_publication_count)
DEFINE_COPY_FUNCTION(source_commit_resource_copy_unlocked,
                            XgSemanticResourceRef, resources, resource_count)
DEFINE_COPY_FUNCTION(source_commit_surface_edge_copy_unlocked,
                     XgSemanticSurfaceEdge, edges, surface_edge_count)
DEFINE_COPY_FUNCTION(source_commit_ui_node_copy_unlocked,
                     XgSemanticUiNodeRecord, ui_nodes, ui_node_count)
DEFINE_COPY_FUNCTION(source_commit_ui_glyph_run_copy_unlocked,
                     XgSemanticUiGlyphRunRecord, ui_glyph_runs,
                     ui_glyph_run_count)
DEFINE_COPY_FUNCTION(source_commit_ui_glyph_placement_copy_unlocked,
                     XgSemanticUiGlyphPlacementRecord, ui_glyph_placements,
                     ui_glyph_placement_count)

void xg_render_source_commit_reset(void) {
    source_commit_lock();
    source_commit_reset_unlocked();
    source_commit_unlock();
}

void xg_render_source_commit_cancel_builders(void) {
    source_commit_lock();
    source_commit_cancel_builders_unlocked();
    source_commit_unlock();
}

XgRenderSourceCommitResult xg_render_source_commit_begin(
        const XgPresentationIdentity *identity,
        const XgSemanticSceneIdentity *scene,
        const XgSemanticDisplayState *display,
        uint32_t source_interval_vblanks,
        bool discontinuity,
        bool temporally_eligible,
        XgRenderSourceBuilder *out_builder) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_begin_unlocked(identity, scene, display,
                                          source_interval_vblanks,
                                          discontinuity, temporally_eligible,
                                          out_builder);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_append_pass(
        XgRenderSourceBuilder builder, const XgSemanticPassRecord *pass) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_append_pass_unlocked(builder, pass);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult
xg_render_source_commit_set_native_work(XgRenderSourceBuilder builder,
                                        bool display_boundary) {
    XgRenderSourceCommitResult result = XG_RENDER_SOURCE_COMMIT_OK;
    source_commit_lock();
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL || has_scene_records(slot)) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    } else {
        slot->header.native_work = true;
        slot->header.display_boundary = display_boundary;
        slot->header.temporally_eligible = false;
    }
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_set_native_display(
    XgRenderSourceBuilder builder, const XgSemanticDisplayState *display) {
    XgRenderSourceCommitResult result = XG_RENDER_SOURCE_COMMIT_OK;
    source_commit_lock();
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL || !slot->header.native_work) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    } else if (display == NULL || display->render_scale > XG_SEMANTIC_RENDER_SCALE_MAX) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    } else {
        slot->header.display = *display;
    }
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_set_native_identity(
    XgRenderSourceBuilder builder, const XgPresentationIdentity *identity) {
    XgRenderSourceCommitResult result = XG_RENDER_SOURCE_COMMIT_OK;
    source_commit_lock();
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL || !slot->header.native_work) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    } else if (identity == NULL) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    } else if (identity->presentation_epoch !=
               slot->header.identity.presentation_epoch) {
        result = XG_RENDER_SOURCE_COMMIT_STALE_EPOCH;
    } else if (identity->scene_generation !=
               slot->header.identity.scene_generation) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    } else if (identity->source_sequence != slot->header.identity.source_sequence ||
               identity->guest_vblank_sequence <
                   slot->header.identity.guest_vblank_sequence ||
               identity->guest_cycle < slot->header.identity.guest_cycle) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_ORDER;
    } else {
        slot->header.identity = *identity;
    }
    source_commit_unlock();
    return result;
}

static XgRenderSourceCommitResult retain_motion(
    XgRenderSourceSlot *slot, const XgRenderMotionDrawBinding *binding) {
    const XgRenderMotionRef ref = binding->motion;
    const XgRenderMotionPose *pose;
    if (!ref.handle.resource_id) return XG_RENDER_SOURCE_COMMIT_OK;
    if (!xg_render_motion_view(ref, &pose) ||
        pose->presentation_epoch != slot->header.identity.presentation_epoch ||
        pose->scene_generation != slot->header.identity.scene_generation)
        return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    for (uint32_t i = 0; i < slot->header.motion_resource_count; ++i) {
        const XgRenderMotionRef prior = slot->motion_resources[i];
        if (prior.handle.resource_id == ref.handle.resource_id &&
            prior.handle.generation == ref.handle.generation)
            return prior.digest == ref.digest ? XG_RENDER_SOURCE_COMMIT_OK :
                XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    }
    if (slot->header.motion_resource_count == XG_RENDER_SCENE_RESOURCE_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    if (xg_render_resource_acquire_snapshot(ref.handle, ref.digest) != XG_RENDER_RESOURCE_OK)
        return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    slot->motion_resources[slot->header.motion_resource_count++] = ref;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

static XgRenderSourceCommitResult retain_coverage(
    XgRenderSourceSlot *slot, XgSemanticResourceRef ref) {
    XgRenderTemporalCoverageView view;
    if (!ref.resource_id) return XG_RENDER_SOURCE_COMMIT_OK;
    if (!xg_render_temporal_coverage_view(ref, &view) ||
        view.header->identity.presentation_epoch != slot->header.identity.presentation_epoch ||
        view.header->identity.scene_generation != slot->header.identity.scene_generation)
        return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    for (uint32_t i = 0; i < slot->header.temporal_coverage_count; ++i) {
        const XgSemanticResourceRef prior = slot->temporal_coverage[i];
        if (prior.resource_id == ref.resource_id && prior.generation == ref.generation)
            return prior.content_digest == ref.content_digest ? XG_RENDER_SOURCE_COMMIT_OK :
                XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    }
    if (slot->header.temporal_coverage_count == XG_RENDER_SCENE_RESOURCE_CAPACITY)
        return XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    if (xg_render_resource_acquire_snapshot((XgRenderResourceHandle){ref.resource_id, ref.generation},
                                           ref.content_digest) != XG_RENDER_RESOURCE_OK)
        return XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    slot->temporal_coverage[slot->header.temporal_coverage_count++] = ref;
    return XG_RENDER_SOURCE_COMMIT_OK;
}

XgRenderSourceCommitResult xg_render_source_commit_append_temporal_coverage(
    XgRenderSourceBuilder builder, XgSemanticResourceRef ref) {
    source_commit_lock();
    XgRenderSourceSlot *slot = builder_slot(builder);
    XgRenderSourceCommitResult result = !slot || !slot->header.native_work ?
        XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION : !ref.resource_id ?
        XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT :
        slot->header.temporal_publication_count == XG_RENDER_SCENE_RESOURCE_CAPACITY ?
        XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED : retain_coverage(slot, ref);
    if (result == XG_RENDER_SOURCE_COMMIT_OK)
        slot->temporal_publications[slot->header.temporal_publication_count++] =
            (XgRenderTemporalPublication){ref, slot->header.native_operation_count};
    source_commit_unlock();
    return result;
}

static XgRenderSourceCommitResult retain_draw_metadata(
    XgRenderSourceSlot *slot, const XgRenderNativeOperation *operation) {
    const uint32_t before = slot->header.temporal_coverage_count;
    XgRenderSourceCommitResult result = retain_coverage(slot, operation->temporal.coverage);
    if (result != XG_RENDER_SOURCE_COMMIT_OK) return result;
    result = retain_motion(slot, &operation->motion);
    if (result != XG_RENDER_SOURCE_COMMIT_OK && slot->header.temporal_coverage_count != before) {
        const XgSemanticResourceRef ref = slot->temporal_coverage[--slot->header.temporal_coverage_count];
        (void)xg_render_resource_release((XgRenderResourceHandle){ref.resource_id, ref.generation});
    }
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_append_native_operation(
    XgRenderSourceBuilder builder, const XgRenderNativeOperation *operation) {
    XgRenderSourceCommitResult result = XG_RENDER_SOURCE_COMMIT_OK;
    source_commit_lock();
    XgRenderSourceSlot *slot = builder_slot(builder);
    if (slot == NULL || !slot->header.native_work) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION;
    } else if (operation == NULL) {
        result = XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT;
    } else if (slot->header.native_operation_count >=
               XG_RENDER_NATIVE_OPERATION_CAPACITY) {
        result = XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED;
    } else if (!native_operation_valid(operation)) {
        result = XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    } else if (operation->kind == XG_RENDER_NATIVE_OPERATION_DRAW &&
               (result = retain_draw_metadata(slot, operation)) !=
                   XG_RENDER_SOURCE_COMMIT_OK) {
        /* Failed retain leaves the operation and resource arrays unchanged. */
    } else if (operation->kind == XG_RENDER_NATIVE_OPERATION_UPLOAD &&
               xg_render_resource_acquire_snapshot(
                   (XgRenderResourceHandle){operation->upload.resource_id,
                                            operation->upload.generation},
                   operation->upload.content_digest) != XG_RENDER_RESOURCE_OK) {
        result = XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED;
    } else {
        /* Only defined fields of the selected operation enter the immutable
         * stream. The upload retain above and append are one transaction. */
        XgRenderNativeOperation copy = {.kind = operation->kind};
        if (operation->kind == XG_RENDER_NATIVE_OPERATION_DRAW) {
            copy.semantic = operation->semantic;
            copy.motion = operation->motion;
            copy.temporal = operation->temporal;
        } else {
            copy.dst_x = operation->dst_x;
            copy.dst_y = operation->dst_y;
            copy.width = operation->width;
            copy.height = operation->height;
            if (operation->kind != XG_RENDER_NATIVE_OPERATION_TARGET) {
                copy.mask_set = operation->mask_set;
                copy.mask_check = operation->mask_check;
            }
            if (operation->kind == XG_RENDER_NATIVE_OPERATION_UPLOAD)
                copy.upload = operation->upload;
            else if (operation->kind == XG_RENDER_NATIVE_OPERATION_COPY) {
                copy.src_x = operation->src_x;
                copy.src_y = operation->src_y;
            } else if (operation->kind == XG_RENDER_NATIVE_OPERATION_FILL) {
                copy.fill_color = operation->fill_color;
            }
        }
        slot->native_operations[slot->header.native_operation_count++] = copy;
    }
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_append_draw(
        XgRenderSourceBuilder builder, const XgSemanticDrawRecord *draw) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_append_draw_unlocked(builder, draw);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_append_resource(
        XgRenderSourceBuilder builder, const XgSemanticResourceRef *resource) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_append_resource_unlocked(builder, resource);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_append_surface_edge(
        XgRenderSourceBuilder builder, const XgSemanticSurfaceEdge *edge) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_append_surface_edge_unlocked(builder, edge);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_append_ui_node(
        XgRenderSourceBuilder builder, const XgSemanticUiNodeRecord *node) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_append_ui_node_unlocked(builder, node);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_append_ui_glyph_run(
        XgRenderSourceBuilder builder,
        const XgSemanticUiGlyphRunRecord *glyph_run) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_append_ui_glyph_run_unlocked(builder, glyph_run);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult
xg_render_source_commit_append_ui_glyph_placement(
        XgRenderSourceBuilder builder,
        const XgSemanticUiGlyphPlacementRecord *placement) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_append_ui_glyph_placement_unlocked(builder,
                                                               placement);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_seal(
        XgRenderSourceBuilder builder, XgRenderSourceCommitHandle *out_commit) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_seal_unlocked(builder, out_commit);
    source_commit_unlock();
    return result;
}

void xg_render_source_commit_cancel(XgRenderSourceBuilder builder) {
    source_commit_lock();
    source_commit_cancel_unlocked(builder);
    source_commit_unlock();
}

XgRenderSourceCommitResult xg_render_source_commit_mark_queued(
        XgRenderSourceCommitHandle commit) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_mark_queued_unlocked(commit);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_retire(
        XgRenderSourceCommitHandle commit) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_retire_unlocked(commit);
    source_commit_unlock();
    return result;
}

XgRenderSourceCommitResult xg_render_source_commit_header_copy(
        XgRenderSourceCommitHandle commit,
        XgRenderSourceCommitHeader *out_header) {
    XgRenderSourceCommitResult result;
    source_commit_lock();
    result = source_commit_header_copy_unlocked(commit, out_header);
    source_commit_unlock();
    return result;
}

#define DEFINE_LOCKED_COPY_FUNCTION(name, unlocked_name, type)                  \
XgRenderSourceCommitResult name(XgRenderSourceCommitHandle commit,             \
                                 size_t index, type *out_value) {                \
    XgRenderSourceCommitResult result;                                          \
    source_commit_lock();                                                       \
    result = unlocked_name(commit, index, out_value);                            \
    source_commit_unlock();                                                     \
    return result;                                                              \
}

DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_pass_copy,
                            source_commit_pass_copy_unlocked,
                            XgSemanticPassRecord)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_draw_copy,
                            source_commit_draw_copy_unlocked,
                            XgSemanticDrawRecord)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_copy_native_operation,
                            source_commit_copy_native_operation_unlocked,
                            XgRenderNativeOperation)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_motion_resource_copy,
                            source_commit_motion_resource_copy_unlocked,
                           XgRenderMotionRef)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_temporal_coverage_copy,
                           source_commit_temporal_coverage_copy_unlocked,
                           XgSemanticResourceRef)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_temporal_publication_copy,
                           source_commit_temporal_publication_copy_unlocked,
                           XgRenderTemporalPublication)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_resource_copy,
                            source_commit_resource_copy_unlocked,
                            XgSemanticResourceRef)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_surface_edge_copy,
                            source_commit_surface_edge_copy_unlocked,
                            XgSemanticSurfaceEdge)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_ui_node_copy,
                            source_commit_ui_node_copy_unlocked,
                            XgSemanticUiNodeRecord)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_ui_glyph_run_copy,
                            source_commit_ui_glyph_run_copy_unlocked,
                            XgSemanticUiGlyphRunRecord)
DEFINE_LOCKED_COPY_FUNCTION(xg_render_source_commit_ui_glyph_placement_copy,
                            source_commit_ui_glyph_placement_copy_unlocked,
                            XgSemanticUiGlyphPlacementRecord)
