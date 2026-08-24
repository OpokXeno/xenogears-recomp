#include "xg_render_world_simple_producers.h"

#include "gpu.h"
#include "psx_cyc.h"
#include "xg_field_render_services.h"
#include "xg_render_backend.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_world_sky_producer.h"
#include "xg_world_decorations_source_capture.h"
#include "xg_world_decorations_shadow.h"
#include "xg_world_effects.h"
#include "xg_world_effects_source_capture.h"
#include "xg_world_entity_shadows_source_capture.h"
#include "xg_world_entity_shadows_shadow.h"
#include "xg_world_horizon.h"
#include "xg_world_horizon_source_capture.h"
#include "xg_world_minimap.h"
#include "xg_world_minimap_source_capture.h"
#include "xg_world_minimap_shadow.h"
#include "xg_world_terrain_water_source_capture.h"
#include "xg_world_terrain_water_shadow.h"

#include <limits.h>
#include <string.h>

typedef struct XgRenderWorldTerrainWaterState {
    struct {
        uint32_t group_id;
        int32_t canonical_x;
        int32_t canonical_y;
        int32_t native_x;
        int32_t native_y;
        uint32_t generation;
    } mesh_vertices[145u * 145u];
    uint32_t mesh_generation;
    XgWorldTerrainWaterSource source;
    XgWorldTerrainWaterRecord records[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    XgWorldTerrainWaterRecord
        unculled_records[XG_WORLD_TERRAIN_WATER_UNCULLED_CAPACITY];
    XgWorldTerrainWaterAnchor anchors[XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY];
    XgWorldTerrainWaterTileSource temporal_tiles[81];
    uint32_t packet_uv2_high[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    uint32_t scratch_words[XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT];
    uint32_t ot_heads[XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT];
    uint32_t simulated_ot_heads[XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT];
    uint64_t temporal_scene;
    uint32_t last_caller;
    uint32_t blocker_detail;
    uint32_t duplicate_vertices;
    uint32_t cross_tile_duplicate_vertices;
    uint32_t canonical_raster_conflicts;
    uint32_t native_raster_conflicts;
    uint32_t cross_tile_native_raster_conflicts;
} XgRenderWorldTerrainWaterState;

typedef struct XgRenderWorldEntityShadowsState {
    XgWorldEntityShadowRecord records[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
    uint32_t packet_tags[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
    uint32_t ot_heads[XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT];
} XgRenderWorldEntityShadowsState;

typedef struct XgRenderWorldDecorationsState {
    XgWorldDecorationsRecord records[XG_WORLD_DECORATIONS_PACKET_CAPACITY];
    XgWorldDecorationsRecord temporal_records[
        XG_WORLD_DECORATIONS_TEMPORAL_CAPACITY];
    uint32_t ot_heads[XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT];
    uint32_t simulated_ot_heads[XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT];
} XgRenderWorldDecorationsState;

typedef struct XgRenderWorldHorizonState {
    XgWorldHorizonCapture capture;
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT];
    PsxXgRenderWorldHorizonShadowSnapshot snapshot;
    uint32_t packet_addresses[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t initial_packet_tags[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t initial_texture_window_tags[2];
    uint32_t entry_stack_pointer;
    uint32_t ot_address;
    uint32_t initial_ot_word;
} XgRenderWorldHorizonState;

typedef struct XgRenderWorldEffectsState {
    XgWorldEffectsCapture capture;
    XgWorldEffectsRecord records[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    PsxXgRenderWorldEffectsShadowSnapshot snapshot;
    uint32_t packet_addresses[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t expected_tags[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t initial_ot_words[0xc0];
    uint32_t final_ot_packets[0xc0];
    bool ot_touched[0xc0];
    uint32_t entry_stack_pointer;
    uint32_t initial_packet_cursor;
    uint32_t ot_base;
    uint32_t count;
} XgRenderWorldEffectsState;

static XgRenderWorldTerrainWaterState terrain_water;
static XgRenderWorldEntityShadowsState entity_shadows;
static XgRenderWorldDecorationsState decorations;
static XgRenderWorldHorizonState horizon;
static XgRenderWorldEffectsState effects;

typedef struct XgRenderWorldCodeRange {
    uint32_t start;
    uint32_t size;
} XgRenderWorldCodeRange;

static const XgRenderWorldCodeRange horizon_code_ranges[] = {
    { UINT32_C(0x80071b50), 0x18u },
    { UINT32_C(0x800739b8), 0x14cu },
    { UINT32_C(0x80073b04), 0x32cu },
    { UINT32_C(0x8009a300), 0x40u },
    { UINT32_C(0x80043a1c), 0x54u },
    { UINT32_C(0x80043bfc), 0x28u },
    { UINT32_C(0x800453ac), 0x3cu },
    { UINT32_C(0x8004931c), 0x160u },
    { UINT32_C(0x80049efc), 0x30u },
    { UINT32_C(0x80049f8c), 0x20u },
    { UINT32_C(0x8004a73c), 0x78u },
    { UINT32_C(0x8004a92c), 0x68u },
};

static const XgRenderWorldCodeRange effects_code_ranges[] = {
    { UINT32_C(0x80071a90), 0x20u },
    { UINT32_C(0x8008901c), 0x10cu },
    { UINT32_C(0x80089748), 0x530u },
    { UINT32_C(0x80089c78), 0x650u },
    { UINT32_C(0x80093534), 0xa8u },
    { UINT32_C(0x80049dcc), 0x124u },
    { UINT32_C(0x8004b18c), 0x198u },
    { UINT32_C(0x8009aff0), 0x50u },
    { UINT32_C(0x8009b040), 0x140u },
};

static bool write_overlaps_code_ranges(
        const XgRenderWorldCodeRange *ranges, uint32_t range_count,
        uint32_t address, uint32_t size) {
    const uint64_t begin = address & UINT32_C(0x1fffffff);
    const uint64_t end = begin + size;

    if (size == 0u) return false;
    for (uint32_t index = 0u; index < range_count; ++index) {
        const uint64_t range_begin =
            ranges[index].start & UINT32_C(0x1fffffff);
        if (range_begin < end && begin < range_begin + ranges[index].size)
            return true;
    }
    return false;
}

void xg_render_world_simple_classify_code_write(
        uint32_t address, uint32_t size,
        XgRenderMutationClassification *out_classification) {
    uint32_t mask = 0u;

    if (write_overlaps_code_ranges(
            horizon_code_ranges,
            (uint32_t)(sizeof(horizon_code_ranges) /
                       sizeof(horizon_code_ranges[0])), address, size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON;
    if (write_overlaps_code_ranges(
            effects_code_ranges,
            (uint32_t)(sizeof(effects_code_ranges) /
                       sizeof(effects_code_ranges[0])), address, size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS;
    if (out_classification == NULL) return;
    *out_classification = (XgRenderMutationClassification){
        .properties = {
            .watched_range_mutation = mask != 0u,
            .runtime_variant_mutation = mask != 0u,
            .executable_mutation = mask != 0u,
            .authentication_mutation = mask != 0u,
            .authority_loss = mask != 0u,
            .interpolation_reset = mask != 0u,
            .reset_runtime_variant = mask != 0u,
        },
        .code_write_mask = mask,
    };
}

static void register_code_ranges(
        const XgRenderWorldCodeRange *ranges, uint32_t range_count,
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    for (uint32_t index = 0u; index < range_count; ++index)
        set_range(ranges[index].start & UINT32_C(0x1fffffff),
                  ranges[index].size);
}

void xg_render_world_simple_register_code_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    if (set_range == NULL) return;
    register_code_ranges(horizon_code_ranges,
        (uint32_t)(sizeof(horizon_code_ranges) /
                   sizeof(horizon_code_ranges[0])), set_range);
    register_code_ranges(effects_code_ranges,
        (uint32_t)(sizeof(effects_code_ranges) /
                   sizeof(effects_code_ranges[0])), set_range);
}

typedef struct XgRenderWorldReaderContext {
    CPUState *cpu;
    const XgRenderWorldSimpleServices *services;
} XgRenderWorldReaderContext;

static bool services_valid(const XgRenderWorldSimpleServices *services) {
    return services != NULL &&
        services->coordinated_dispatch_blocker != NULL &&
        services->authorize_direct_dispatch != NULL &&
        services->authentication_generation != NULL &&
        services->interpolation_scene_generation != NULL &&
        services->screen_x_cull_margin != NULL && services->native_view != NULL &&
        services->authorize_guest_range != NULL &&
        services->stage_native != NULL && services->stage_temporal != NULL &&
        services->abort_submission != NULL;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool read_native_u8(void *context, uint32_t address,
                           uint8_t *out_value) {
    XgRenderWorldReaderContext *reader = context;
    CPUState *cpu = reader != NULL ? reader->cpu : NULL;

    if (cpu == NULL || cpu->read_byte == NULL || out_value == NULL ||
        !reader->services->authorize_guest_range(address, 1u, 1u, true))
        return false;
    *out_value = cpu->read_byte(address);
    return true;
}

static bool read_native_u16(void *context, uint32_t address,
                            uint16_t *out_value) {
    XgRenderWorldReaderContext *reader = context;
    CPUState *cpu = reader != NULL ? reader->cpu : NULL;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL ||
        !reader->services->authorize_guest_range(address, 2u, 2u, true))
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_native_u32(void *context, uint32_t address,
                            uint32_t *out_value) {
    XgRenderWorldReaderContext *reader = context;
    CPUState *cpu = reader != NULL ? reader->cpu : NULL;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL ||
        !reader->services->authorize_guest_range(address, 4u, 4u, true))
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static int32_t fixed_floor(int32_t value) {
    const int64_t wide = value;

    return wide >= 0 ? (int32_t)(wide / INT64_C(65536))
        : -(int32_t)((-wide + INT64_C(65535)) / INT64_C(65536));
}

static void diagnose_terrain_mesh(uint32_t record_count) {
    uint32_t mesh_generation = ++terrain_water.mesh_generation;

    if (mesh_generation == 0u) {
        memset(terrain_water.mesh_vertices, 0,
               sizeof(terrain_water.mesh_vertices));
        mesh_generation = ++terrain_water.mesh_generation;
    }
    terrain_water.duplicate_vertices = 0u;
    terrain_water.cross_tile_duplicate_vertices = 0u;
    terrain_water.canonical_raster_conflicts = 0u;
    terrain_water.native_raster_conflicts = 0u;
    terrain_water.cross_tile_native_raster_conflicts = 0u;
    for (uint32_t record_index = 0u; record_index < record_count;
         ++record_index) {
        const XgRenderIrTriangle *triangle =
            &terrain_water.records[record_index].primitive.triangles[0];

        for (uint32_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const XgRenderIrVertex *vertex = &triangle->vertices[vertex_index];
            const uint32_t mesh_index = vertex->interpolation_vertex_id;
            int32_t canonical_x;
            int32_t canonical_y;
            int32_t native_x;
            int32_t native_y;

            if (vertex->interpolation_group_id != UINT32_C(0x63000000) ||
                mesh_index >= 145u * 145u)
                continue;
            canonical_x = fixed_floor(vertex->x);
            canonical_y = fixed_floor(vertex->y);
            native_x = fixed_floor(vertex->native_view_position
                ? vertex->native_view_x : vertex->x);
            native_y = fixed_floor(vertex->native_view_position
                ? vertex->native_view_y : vertex->y);
            if (terrain_water.mesh_vertices[mesh_index].generation !=
                    mesh_generation) {
                terrain_water.mesh_vertices[mesh_index].group_id =
                    vertex->interpolation_group_id;
                terrain_water.mesh_vertices[mesh_index].canonical_x = canonical_x;
                terrain_water.mesh_vertices[mesh_index].canonical_y = canonical_y;
                terrain_water.mesh_vertices[mesh_index].native_x = native_x;
                terrain_water.mesh_vertices[mesh_index].native_y = native_y;
                terrain_water.mesh_vertices[mesh_index].generation =
                    mesh_generation;
                continue;
            }
            ++terrain_water.duplicate_vertices;
            if (terrain_water.mesh_vertices[mesh_index].canonical_x !=
                    canonical_x ||
                terrain_water.mesh_vertices[mesh_index].canonical_y !=
                    canonical_y)
                ++terrain_water.canonical_raster_conflicts;
            if (terrain_water.mesh_vertices[mesh_index].native_x != native_x ||
                terrain_water.mesh_vertices[mesh_index].native_y != native_y)
                ++terrain_water.native_raster_conflicts;
        }
    }
}

void xg_render_world_simple_set_terrain_temporal_coverage(bool enabled) {
    xg_world_terrain_water_set_temporal_coverage(enabled);
}

void xg_render_world_terrain_water_note_entry(
        CPUState *cpu, bool native_enabled) {
    terrain_water.last_caller = cpu != NULL ? cpu->gpr[31] : 0u;
    terrain_water.blocker_detail = native_enabled ? 5u : 6u;
}

static bool authorize_entity_range(
        void *context, XgWorldEntityShadowsSourceRangeKind kind,
        uint32_t address, uint32_t size) {
    XgRenderWorldReaderContext *reader = context;

    if (kind != XG_WORLD_ENTITY_SHADOWS_SOURCE_PENDING_LIST &&
        kind != XG_WORLD_ENTITY_SHADOWS_SOURCE_TERRAIN_CHUNK)
        return false;
    return reader != NULL && reader->services->authorize_guest_range(
        address, size, 4u, false);
}

bool xg_render_world_terrain_water_cutover(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    XgWorldTerrainWaterNativePreparation preparation;
    XgWorldTerrainWaterNativeRequest request = {0};
    XgWorldTerrainWaterAuthenticatedReader reader;
    XgRenderWorldReaderContext reader_context = {cpu, services};
    XgWorldTerrainWaterShadowSnapshot shadow_snapshot;
    GpuDrawState draw = {0};
    uint64_t generation;
    uint64_t scene_id;
    uint32_t context;
    uint32_t unculled_record_count = 0u;

    terrain_water.last_caller = cpu != NULL ? cpu->gpr[31] : 0u;
    terrain_water.blocker_detail = 1u;
    if (!services_valid(services)) return false;
    {
        const uint32_t readiness_blocker =
            services->coordinated_dispatch_blocker();
        if (readiness_blocker != 0u) {
            terrain_water.blocker_detail = 10u + readiness_blocker;
            return false;
        }
    }
    if (cpu == NULL) {
        terrain_water.blocker_detail = 12u;
        return false;
    }
    if (cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL) {
        terrain_water.blocker_detail = 13u;
        return false;
    }
    if (!xg_world_terrain_water_caller_is_valid(cpu->gpr[31])) {
        terrain_water.blocker_detail = 14u;
        return false;
    }
    if (!services->authentication_generation(&generation)) {
        terrain_water.blocker_detail = 15u;
        return false;
    }
    terrain_water.blocker_detail = 2u;
    context = cpu->read_word(XG_WORLD_TERRAIN_WATER_NATIVE_CONTEXT_ADDRESS);
    if (!services->authorize_guest_range(context, 0x78u, 4u, false))
        return false;
    gpu_get_draw_state(&draw);
    request.capture = (XgWorldTerrainWaterCaptureRequest){
        .authentication_generation = generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = services->screen_x_cull_margin(),
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .texture_window_mask_x = draw.texture_window_mask_x,
            .texture_window_mask_y = draw.texture_window_mask_y,
            .texture_window_offset_x = draw.texture_window_offset_x,
            .texture_window_offset_y = draw.texture_window_offset_y,
            .dither = draw.dither,
            .mask_set = draw.mask_set,
            .mask_check = draw.mask_check,
        },
        .projection_state_authenticated = true,
    };
    request.entry_pc = XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC;
    request.ot_base = cpu->gpr[4];
    request.packet_base = cpu->gpr[5];
    request.position_address = cpu->gpr[6];
    reader = (XgWorldTerrainWaterAuthenticatedReader){
        .context = &reader_context,
        .read_u16 = read_native_u16,
        .read_u32 = read_native_u32,
        .authentication_generation = generation,
        .authenticated = true,
    };
    memset(terrain_water.ot_touched, 0, sizeof(terrain_water.ot_touched));
    terrain_water.blocker_detail = 3u;
    scene_id = services->interpolation_scene_generation();
    if (xg_world_terrain_water_native_prepare(
            &request, &reader, terrain_water.records,
            XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, terrain_water.anchors,
            XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY, &terrain_water.source,
            &preparation) != XG_WORLD_TERRAIN_WATER_NATIVE_OK ||
        !preparation.authenticated || !preparation.sealed ||
        preparation.authentication_generation != generation ||
        preparation.anchor_count > XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY ||
        !physical_address_equals(preparation.continuation_pc, cpu->gpr[31]) ||
        preparation.record_count > XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY ||
        !services->authorize_guest_range(
            XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_ADDRESS,
            XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_BYTES, 4u, true) ||
        !services->authorize_guest_range(
            XG_WORLD_TERRAIN_WATER_NATIVE_FINAL_COUNT_ADDRESS, 4u, 4u, false))
        return false;

    if (terrain_water.temporal_scene != scene_id) {
        memset(terrain_water.temporal_tiles, 0,
               sizeof(terrain_water.temporal_tiles));
        terrain_water.temporal_scene = scene_id;
    }
    if (xg_world_terrain_water_append_temporal_tile_anchors(
            terrain_water.temporal_tiles,
            (uint32_t)(sizeof(terrain_water.temporal_tiles) /
                       sizeof(terrain_water.temporal_tiles[0])),
            &terrain_water.source, terrain_water.anchors,
            XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY,
            &preparation.anchor_count) != XG_WORLD_TERRAIN_WATER_OK ||
        xg_world_terrain_water_build_unculled(
            &terrain_water.source, terrain_water.unculled_records,
            XG_WORLD_TERRAIN_WATER_UNCULLED_CAPACITY,
            &unculled_record_count) != XG_WORLD_TERRAIN_WATER_OK)
        return false;
    diagnose_terrain_mesh(preparation.record_count);

    for (uint32_t index = 0u; index < preparation.anchor_count;) {
        GpuRenderInterpolationVertexAnchor anchors[256];
        const uint32_t count = preparation.anchor_count - index < 256u
            ? preparation.anchor_count - index : 256u;

        for (uint32_t anchor_index = 0u; anchor_index < count; ++anchor_index) {
            const XgWorldTerrainWaterAnchor *source =
                &terrain_water.anchors[index + anchor_index];

            if (xg_render_backend_translate_anchor(
                    &source->material, &source->vertex,
                    services->interpolation_scene_generation(),
                    XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
                    &anchors[anchor_index]) != XG_RENDER_BACKEND_OK) {
                services->abort_submission();
                return false;
            }
        }
        if (gr_record_interpolation_anchors(anchors, count) !=
                GPU_RENDER_TRANSACTION_OK) {
            services->abort_submission();
            return false;
        }
        index += count;
    }

    terrain_water.blocker_detail = 4u;
    for (uint32_t index = 0u;
         index < XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT; ++index) {
        terrain_water.scratch_words[index] = cpu->read_word(
            XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_ADDRESS + index * 4u);
    }
    xg_world_terrain_water_shadow_snapshot(&shadow_snapshot);
    if (shadow_snapshot.native_cutover_count == UINT64_MAX ||
        shadow_snapshot.native_primitive_count >
            UINT64_MAX - preparation.record_count)
        return false;
    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldTerrainWaterRecord *record = &terrain_water.records[index];
        const XgRenderIrTriangle *triangle = &record->primitive.triangles[0];
        const uint32_t packet = preparation.packet_base +
            index * XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE;
        const uint32_t ot_address = preparation.ot_base +
            record->ordering_bucket * 4u;

        if (record->allocation_ordinal != index ||
            record->ordering_bucket >=
                XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT ||
            !services->authorize_guest_range(
                packet, XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE, 4u,
                false) ||
            !services->authorize_guest_range(ot_address, 4u, 4u, false) ||
            cpu->read_word(packet + 4u) !=
                XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_MATERIAL_WORD)
            return false;
        terrain_water.packet_uv2_high[index] =
            cpu->read_word(packet + 28u) & UINT32_C(0xffff0000);
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
            if ((xg_render_ir_fixed_uv(&triangle->vertices[vertex]) &
                 UINT32_C(0xffff0000)) != 0u)
                return false;
        if (!terrain_water.ot_touched[record->ordering_bucket]) {
            terrain_water.ot_heads[record->ordering_bucket] =
                cpu->read_word(ot_address);
            terrain_water.simulated_ot_heads[record->ordering_bucket] =
                terrain_water.ot_heads[record->ordering_bucket];
            terrain_water.ot_touched[record->ordering_bucket] = true;
        }
        if (((terrain_water.simulated_ot_heads[record->ordering_bucket] |
              UINT32_C(0x07000000)) >> 24u) !=
                XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_TAG_WORD_COUNT)
            return false;
        terrain_water.simulated_ot_heads[record->ordering_bucket] =
            packet & UINT32_C(0x00ffffff);
    }
    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldTerrainWaterRecord *record = &terrain_water.records[index];
        const uint32_t packet = preparation.packet_base +
            index * XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE;

        if (!services->stage_native(
                &record->primitive, packet,
                UINT32_C(0x63000000) | record->source_primitive_index,
                XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
                record->interpolation_primitive_id)) {
            services->abort_submission();
            return false;
        }
    }
    for (uint32_t index = 0u; index < unculled_record_count; ++index) {
        const XgWorldTerrainWaterRecord *record =
            &terrain_water.unculled_records[index];
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_DEPTH | GPU_RENDER_TEMPORAL_FORCE_PHASES,
            .depth_min_inclusive = 0,
            .depth_max_exclusive = 0x0f00,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = 4u,
        };

        if (!record->temporal_only ||
            record->temporal_cull_reasons != XG_WORLD_TERRAIN_WATER_CULL_DEPTH ||
            record->max_depth >= UINT16_C(0x0f80))
            continue;
        if (!services->stage_temporal(
                &record->primitive, XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
                record->interpolation_primitive_id, &policy)) {
            services->abort_submission();
            return false;
        }
    }
    if (!xg_world_terrain_water_shadow_record_native_cutover(
            preparation.record_count)) {
        services->abort_submission();
        return false;
    }
    terrain_water.blocker_detail = 0u;

    for (uint32_t index = 0u;
         index < XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT; ++index) {
        const uint32_t mask = preparation.scratch.write_masks[index];

        if (mask != 0u) {
            const uint32_t value = (terrain_water.scratch_words[index] & ~mask) |
                (preparation.scratch.values[index] & mask);
            psx_store_cycle_barrier();
            cpu->write_word(XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_ADDRESS +
                                index * 4u,
                            value);
        }
    }
    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldTerrainWaterRecord *record = &terrain_water.records[index];
        const XgRenderIrTriangle *triangle = &record->primitive.triangles[0];
        const uint32_t packet = preparation.packet_base +
            index * XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE;
        const uint32_t bucket = record->ordering_bucket;
        const uint32_t ot_address = preparation.ot_base + bucket * 4u;
        const uint32_t previous_head = terrain_water.ot_heads[bucket];

        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            psx_store_cycle_barrier();
            cpu->write_word(packet + 8u + vertex * 8u,
                xg_render_projected_xy(&record->projected_vertices[vertex]));
        }
        psx_store_cycle_barrier();
        cpu->write_word(packet + 12u,
            xg_render_ir_fixed_uv(&triangle->vertices[0]) |
                ((uint32_t)record->encoded_clut << 16u));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 20u,
            xg_render_ir_fixed_uv(&triangle->vertices[1]) |
                ((uint32_t)record->encoded_tpage << 16u));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 28u, terrain_water.packet_uv2_high[index] |
            xg_render_ir_fixed_uv(&triangle->vertices[2]));
        psx_store_cycle_barrier();
        cpu->write_word(packet, previous_head | UINT32_C(0x07000000));
        terrain_water.ot_heads[bucket] = packet & UINT32_C(0x00ffffff);
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, terrain_water.ot_heads[bucket]);
    }
    psx_store_cycle_barrier();
    cpu->write_word(XG_WORLD_TERRAIN_WATER_NATIVE_FINAL_COUNT_ADDRESS,
                    preparation.final_count);
    for (uint32_t index = 0u; index < XG_WORLD_TERRAIN_WATER_TILE_COUNT;
         ++index) {
        const XgWorldTerrainWaterTileSource *tile =
            &terrain_water.source.tiles[index];

        if (tile->active && tile->has_data && tile->grid_index < 81u)
            terrain_water.temporal_tiles[tile->grid_index] = *tile;
    }
    cpu->pc = preparation.continuation_pc;
    return true;
}

bool xg_render_world_entity_shadows_cutover(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    XgWorldEntityShadowsCaptureRequest request = {0};
    XgWorldEntityShadowsAuthenticatedReader reader;
    XgRenderWorldReaderContext reader_context = {cpu, services};
    XgWorldEntityShadowsCapture capture;
    XgWorldEntityShadowsNativePreparation preparation;
    XgWorldEntityShadowsShadowSnapshot shadow_snapshot;
    GpuDrawState draw = {0};
    uint64_t generation;
    uint32_t packet_base = 0u;
    uint32_t ot_base = 0u;

    if (!services_valid(services) ||
        services->coordinated_dispatch_blocker() != 0u || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31],
                                 XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC) ||
        !services->authentication_generation(&generation))
        return false;
    gpu_get_draw_state(&draw);
    request = (XgWorldEntityShadowsCaptureRequest){
        .authentication_generation = generation,
        .producer_callsite = XG_WORLD_ENTITY_SHADOWS_PRODUCER_CALLSITE,
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither,
            .mask_set = draw.mask_set,
            .mask_check = draw.mask_check,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldEntityShadowsAuthenticatedReader){
        .context = &reader_context,
        .read_u8 = read_native_u8,
        .read_u16 = read_native_u16,
        .read_u32 = read_native_u32,
        .authorize_source_range = authorize_entity_range,
        .authentication_generation = generation,
        .authenticated = true,
    };
    memset(entity_shadows.ot_touched, 0, sizeof(entity_shadows.ot_touched));
    if (xg_world_entity_shadows_source_capture(
            &request, &reader, &capture) !=
            XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK ||
        xg_world_entity_shadows_prepare_native_cutover(
            &capture, generation, entity_shadows.records,
            XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY, &preparation) !=
            XG_WORLD_ENTITY_SHADOWS_OK ||
        !preparation.authenticated || !preparation.sealed ||
        preparation.authentication_generation != generation ||
        preparation.record_count > XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY ||
        !physical_address_equals(preparation.continuation_pc, cpu->gpr[31]))
        return false;

    if (preparation.record_count != 0u) {
        const uint32_t buffer_index = cpu->read_word(
            XG_WORLD_ENTITY_SHADOWS_BUFFER_INDEX_ADDRESS);
        const uint32_t context = cpu->read_word(
            XG_WORLD_ENTITY_SHADOWS_CONTEXT_ADDRESS);

        if (buffer_index >= XG_WORLD_ENTITY_SHADOW_PACKET_BUFFER_COUNT ||
            !services->authorize_guest_range(
                context, XG_WORLD_ENTITY_SHADOWS_CONTEXT_OT_OFFSET + 4u,
                4u, false))
            return false;
        packet_base = cpu->read_word(
            XG_WORLD_ENTITY_SHADOWS_PACKET_BASES_ADDRESS + buffer_index * 4u);
        ot_base = cpu->read_word(
            context + XG_WORLD_ENTITY_SHADOWS_CONTEXT_OT_OFFSET);
    }
    xg_world_entity_shadows_shadow_snapshot(&shadow_snapshot);
    if (shadow_snapshot.native_cutover_count == UINT64_MAX ||
        shadow_snapshot.native_primitive_count >
            UINT64_MAX - preparation.accepted_record_count)
        return false;
    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldEntityShadowRecord *record =
            &entity_shadows.records[index];
        const uint32_t packet = packet_base + record->packet_offset;

        if (record->packet_word_write_mask == 0u) continue;
        if (!services->authorize_guest_range(
                packet, XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE, 4u, false) ||
            cpu->read_word(packet + 4u) != record->material_word ||
            (cpu->read_word(packet + 12u) & UINT32_C(0x0000ffff)) !=
                record->uv[0] ||
            (cpu->read_word(packet + 12u) >> 16u) != record->clut ||
            (cpu->read_word(packet + 20u) & UINT32_C(0x0000ffff)) !=
                record->uv[1] ||
            (cpu->read_word(packet + 20u) >> 16u) != record->tpage ||
            (cpu->read_word(packet + 28u) & UINT32_C(0x0000ffff)) !=
                record->uv[2] ||
            (cpu->read_word(packet + 36u) & UINT32_C(0x0000ffff)) !=
                record->uv[3])
            return false;
        entity_shadows.packet_tags[index] = cpu->read_word(packet);
        if ((entity_shadows.packet_tags[index] >> 24u) !=
                XG_WORLD_ENTITY_SHADOW_PACKET_PAYLOAD_WORDS)
            return false;
        for (uint32_t vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex)
            if ((record->packet_word_write_mask &
                 (UINT32_C(1) << (2u + vertex * 2u))) == 0u)
                return false;
        if (record->accepted) {
            const uint32_t bucket = record->ordering_bucket;
            const uint32_t ot_address = ot_base + bucket * 4u;

            if (bucket >= XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT ||
                !services->authorize_guest_range(ot_address, 4u, 4u, false))
                return false;
            if (!entity_shadows.ot_touched[bucket]) {
                entity_shadows.ot_heads[bucket] = cpu->read_word(ot_address);
                entity_shadows.ot_touched[bucket] = true;
            }
        }
    }
    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldEntityShadowRecord *record =
            &entity_shadows.records[index];
        const uint32_t packet = packet_base + record->packet_offset;

        if (record->accepted && !services->stage_native(
                &record->primitive, packet,
                UINT32_C(0x64000000) |
                    ((packet & UINT32_C(0x001ffffc)) >> 2u),
                XG_WORLD_ENTITY_SHADOWS_ENTRY_PC, record->source_index)) {
            services->abort_submission();
            return false;
        }
        if (!record->accepted && record->primitive.triangle_count != 0u) {
            const GpuRenderTemporalCullPolicy policy = {
                .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                    GPU_RENDER_TEMPORAL_CULL_DEPTH,
                .depth_min_inclusive = 1,
                .depth_max_exclusive = 0x1000,
                .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MINIMUM,
                .ordering_depth_shift = 4u,
            };

            if (!services->stage_temporal(
                    &record->primitive, XG_WORLD_ENTITY_SHADOWS_ENTRY_PC,
                    record->source_index, &policy)) {
                services->abort_submission();
                return false;
            }
        }
    }
    if (!xg_world_entity_shadows_shadow_record_native_cutover(
            preparation.accepted_record_count)) {
        services->abort_submission();
        return false;
    }

    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldEntityShadowRecord *record =
            &entity_shadows.records[index];
        const uint32_t packet = packet_base + record->packet_offset;

        if (record->packet_word_write_mask == 0u) continue;
        for (uint32_t vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            psx_store_cycle_barrier();
            cpu->write_word(packet + 8u + vertex * 8u,
                            xg_render_projected_xy(&record->vertices[vertex]));
        }
        if (record->accepted) {
            const uint32_t bucket = record->ordering_bucket;
            const uint32_t ot_address = ot_base + bucket * 4u;
            const uint32_t previous_head = entity_shadows.ot_heads[bucket];

            psx_store_cycle_barrier();
            cpu->write_word(packet,
                (entity_shadows.packet_tags[index] & UINT32_C(0xff000000)) |
                (previous_head & UINT32_C(0x00ffffff)));
            entity_shadows.ot_heads[bucket] =
                (previous_head & UINT32_C(0xff000000)) |
                (packet & UINT32_C(0x00ffffff));
            psx_store_cycle_barrier();
            cpu->write_word(ot_address, entity_shadows.ot_heads[bucket]);
        }
    }
    if (preparation.side_effects.pending_count_written) {
        psx_store_cycle_barrier();
        cpu->write_word(XG_WORLD_ENTITY_SHADOWS_PENDING_COUNT_ADDRESS,
                        preparation.side_effects.pending_count_after);
    }
    cpu->pc = preparation.continuation_pc;
    return true;
}

bool xg_render_world_decorations_cutover(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    XgWorldDecorationsNativePreparation preparation;
    XgWorldDecorationsNativeRequest request = {0};
    XgWorldDecorationsAuthenticatedReader reader;
    XgRenderWorldReaderContext reader_context = {cpu, services};
    XgWorldDecorationsShadowSnapshot shadow_snapshot;
    GpuDrawState draw = {0};
    uint64_t generation;
    uint32_t shared_count_before;
    uint32_t native_count = 0u;

    if (!services_valid(services) ||
        services->coordinated_dispatch_blocker() != 0u || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL || cpu->write_half == NULL ||
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN) ||
        !services->authentication_generation(&generation))
        return false;
    gpu_get_draw_state(&draw);
    request = (XgWorldDecorationsNativeRequest){
        .authentication_generation = generation,
        .entry_pc = XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = services->screen_x_cull_margin(),
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither,
            .mask_set = draw.mask_set,
            .mask_check = draw.mask_check,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldDecorationsAuthenticatedReader){
        .context = &reader_context,
        .read_u16 = read_native_u16,
        .read_u32 = read_native_u32,
        .authentication_generation = generation,
        .authenticated = true,
    };
    memset(decorations.ot_touched, 0, sizeof(decorations.ot_touched));
    shared_count_before = cpu->read_word(
        XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS);
    if (xg_world_decorations_native_prepare_temporal(
            &request, &reader, decorations.records,
            XG_WORLD_DECORATIONS_PACKET_CAPACITY, decorations.temporal_records,
            XG_WORLD_DECORATIONS_TEMPORAL_CAPACITY, &preparation) !=
            XG_WORLD_DECORATIONS_NATIVE_OK ||
        !preparation.authenticated || !preparation.sealed ||
        preparation.record_count > XG_WORLD_DECORATIONS_PACKET_CAPACITY ||
        !physical_address_equals(preparation.continuation, cpu->gpr[31]) ||
        !services->authorize_guest_range(
            XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS, 0x88u, 2u, true) ||
        !services->authorize_guest_range(
            XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS, 4u, 4u, false))
        return false;

    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldDecorationsRecord *record = &decorations.records[index];
        const uint32_t packet = preparation.packet_base +
            record->packet_index * XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE;

        if (record->tag_payload_word_count == 0u) continue;
        if (record->tag_payload_word_count !=
                XG_WORLD_DECORATIONS_NATIVE_PACKET_TAG_WORD_COUNT ||
            record->packet_index != index ||
            record->ordering_bucket >=
                XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT ||
            !services->authorize_guest_range(
                packet, XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE, 4u,
                false) ||
            !services->authorize_guest_range(
                preparation.ot_base + record->ordering_bucket * 4u,
                4u, 4u, false))
            return false;
        for (uint32_t payload = 0u;
             payload < XG_WORLD_DECORATIONS_FT4_PAYLOAD_WORD_COUNT; ++payload) {
            const uint32_t actual = cpu->read_word(packet + 4u + payload * 4u);
            const bool full_write = payload == 1u || payload == 3u ||
                payload == 5u || payload == 7u;
            const bool low_half_semantic =
                payload == 2u || payload == 6u || payload == 8u;

            if (!full_write && !low_half_semantic &&
                actual != record->ft4_payload_words[payload])
                return false;
            if (low_half_semantic &&
                (actual & UINT32_C(0x0000ffff)) !=
                    (record->ft4_payload_words[payload] &
                     UINT32_C(0x0000ffff)))
                return false;
        }
        if (!decorations.ot_touched[record->ordering_bucket]) {
            decorations.ot_heads[record->ordering_bucket] = cpu->read_word(
                preparation.ot_base + record->ordering_bucket * 4u);
            decorations.simulated_ot_heads[record->ordering_bucket] =
                decorations.ot_heads[record->ordering_bucket];
            decorations.ot_touched[record->ordering_bucket] = true;
        }
        if (((decorations.simulated_ot_heads[record->ordering_bucket] |
              UINT32_C(0x09000000)) >> 24u) !=
                XG_WORLD_DECORATIONS_NATIVE_PACKET_TAG_WORD_COUNT)
            return false;
        decorations.simulated_ot_heads[record->ordering_bucket] =
            packet & UINT32_C(0x00ffffff);
        ++native_count;
    }
    if (native_count != preparation.record_count) return false;
    xg_world_decorations_shadow_snapshot(&shadow_snapshot);
    if (shadow_snapshot.native_cutover_count == UINT64_MAX ||
        shadow_snapshot.native_primitive_count > UINT64_MAX - native_count)
        return false;
    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldDecorationsRecord *record = &decorations.records[index];
        const uint32_t packet = preparation.packet_base +
            record->packet_index * XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE;

        if (!services->stage_native(
                &record->primitive, packet,
                UINT32_C(0x65000000) |
                    ((packet & UINT32_C(0x001ffffc)) >> 2u),
                XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC, record->semantic_id)) {
            services->abort_submission();
            return false;
        }
    }
    for (uint32_t index = 0u; index < preparation.temporal_record_count;
         ++index) {
        const XgWorldDecorationsRecord *record =
            &decorations.temporal_records[index];
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN | GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = -request.screen_x_cull_margin * INT32_C(65536),
            .screen_top = 0,
            .screen_right_exclusive =
                (320 + request.screen_x_cull_margin) * INT32_C(65536),
            .screen_bottom_exclusive = 216 * INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x0e00,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = 4u,
        };

        if (!services->stage_temporal(
                &record->primitive, XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
                record->semantic_id, &policy)) {
            services->abort_submission();
            return false;
        }
    }
    if (!xg_world_decorations_shadow_record_native_cutover(native_count)) {
        services->abort_submission();
        return false;
    }

    for (uint32_t index = 0u; index < XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT;
         ++index) {
        const XgHost3dVector *vertex = &preparation.scratch.vertices[index];
        const uint32_t address = XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS +
            index * sizeof(*vertex);

        psx_store_cycle_barrier();
        cpu->write_half(address, (uint16_t)vertex->x);
        psx_store_cycle_barrier();
        cpu->write_half(address + 2u, (uint16_t)vertex->y);
        psx_store_cycle_barrier();
        cpu->write_half(address + 4u, (uint16_t)vertex->z);
    }
    xg_render_world_sky_store_matrix(
        cpu, XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS +
                 XG_WORLD_DECORATIONS_NATIVE_SCRATCH_CAMERA_OFFSET,
        &preparation.scratch.camera_matrix);
    xg_render_world_sky_store_matrix(
        cpu, XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS +
                 XG_WORLD_DECORATIONS_NATIVE_SCRATCH_DECORATION_OFFSET,
        &preparation.scratch.decoration_matrix);
    for (uint32_t index = 0u; index < XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT;
         ++index) {
        psx_store_cycle_barrier();
        cpu->write_half(
            XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS +
                XG_WORLD_DECORATIONS_NATIVE_SCRATCH_CLUT_OFFSET + index * 2u,
            preparation.scratch.depth_clut[index]);
    }
    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        const XgWorldDecorationsRecord *record = &decorations.records[index];
        const uint32_t packet = preparation.packet_base +
            record->packet_index * XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE;
        const uint32_t bucket = record->ordering_bucket;
        const uint32_t ot_address = preparation.ot_base + bucket * 4u;
        const uint32_t previous_head = decorations.ot_heads[bucket];

        psx_store_cycle_barrier();
        cpu->write_word(packet + 8u, record->ft4_payload_words[1]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 0x10u, record->ft4_payload_words[3]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 0x18u, record->ft4_payload_words[5]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 0x20u, record->ft4_payload_words[7]);
        decorations.ot_heads[bucket] = packet & UINT32_C(0x00ffffff);
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, decorations.ot_heads[bucket]);
        psx_store_cycle_barrier();
        cpu->write_word(packet, previous_head | UINT32_C(0x09000000));
        psx_store_cycle_barrier();
        cpu->write_half(packet + 0x0eu,
                        (uint16_t)(record->ft4_payload_words[2] >> 16u));
    }
    psx_store_cycle_barrier();
    cpu->write_word(XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS,
        (shared_count_before & ~preparation.shared_count_write_mask) |
            (preparation.final_shared_count & preparation.shared_count_write_mask));
    cpu->pc = preparation.continuation;
    return true;
}

static bool read_source_u16(void *context, uint32_t address,
                            uint16_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL) return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_source_u32(void *context, uint32_t address,
                            uint32_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL) return false;
    *out_value = cpu->read_word(address);
    return true;
}

static uint32_t capture_horizon(
        CPUState *cpu, XgWorldHorizonCapture *capture,
        XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT],
        const XgRenderWorldSimpleServices *services) {
    XgWorldHorizonCaptureRequest request = {0};
    XgWorldHorizonAuthenticatedReader reader = {0};
    GpuDrawState draw = {0};
    uint64_t generation;
    const XgNativeView *view;

    if (!services_valid(services) || cpu == NULL || capture == NULL ||
        records == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        !services->authentication_generation(&generation))
        return 1u;
    gpu_get_draw_state(&draw);
    request = (XgWorldHorizonCaptureRequest){
        .authentication_generation = generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither != 0u,
            .mask_set = draw.mask_set != 0u,
            .mask_check = draw.mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldHorizonAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_source_u16,
        .read_u32 = read_source_u32,
        .authentication_generation = generation,
        .authenticated = true,
    };
    if (xg_world_horizon_source_capture(&request, &reader, capture) !=
            XG_WORLD_HORIZON_CAPTURE_OK)
        return 1u;
    view = services->native_view();
    if (xg_world_horizon_build_for_view(
            &capture->source, view != NULL && view->enabled ? view : NULL,
            records) != XG_WORLD_HORIZON_OK)
        return 2u;
    return 0u;
}

static uint32_t capture_effects(
        CPUState *cpu, XgWorldEffectsCapture *capture,
        XgWorldEffectsRecord records[XG_WORLD_EFFECTS_SOURCE_CAPACITY],
        uint32_t *out_count, XgWorldEffectsRecord *temporal_records,
        uint32_t *out_temporal_count,
        const XgRenderWorldSimpleServices *services) {
    XgWorldEffectsCaptureRequest request = {0};
    XgWorldEffectsAuthenticatedReader reader = {0};
    GpuDrawState draw = {0};
    uint64_t generation;

    if (!services_valid(services) || cpu == NULL || capture == NULL ||
        records == NULL || out_count == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL ||
        !services->authentication_generation(&generation))
        return 1u;
    gpu_get_draw_state(&draw);
    request = (XgWorldEffectsCaptureRequest){
        .authentication_generation = generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = services->screen_x_cull_margin(),
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither != 0u,
            .mask_set = draw.mask_set != 0u,
            .mask_check = draw.mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldEffectsAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_source_u16,
        .read_u32 = read_source_u32,
        .authentication_generation = generation,
        .authenticated = true,
    };
    if (xg_world_effects_source_capture(&request, &reader, capture) !=
            XG_WORLD_EFFECTS_CAPTURE_OK)
        return 1u;
    if (xg_world_effects_build_with_temporal(
            &capture->source, records, XG_WORLD_EFFECTS_SOURCE_CAPACITY,
            out_count, temporal_records, XG_WORLD_EFFECTS_SOURCE_CAPACITY,
            out_temporal_count) != XG_WORLD_EFFECTS_OK)
        return 2u;
    return 0u;
}

static uint32_t capture_minimap(
        CPUState *cpu, XgWorldMinimapCapture *capture,
        XgWorldMinimapBuildOutput *output,
        const XgRenderWorldSimpleServices *services) {
    XgWorldMinimapCaptureRequest request = {0};
    XgWorldMinimapAuthenticatedReader reader = {0};
    GpuDrawState draw = {0};
    uint64_t generation;

    if (!services_valid(services) || cpu == NULL || capture == NULL ||
        output == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        !services->authentication_generation(&generation))
        return 1u;
    gpu_get_draw_state(&draw);
    request = (XgWorldMinimapCaptureRequest){
        .authentication_generation = generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither != 0u,
            .mask_set = draw.mask_set != 0u,
            .mask_check = draw.mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldMinimapAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_source_u16,
        .read_u32 = read_source_u32,
        .authentication_generation = generation,
        .authenticated = true,
    };
    if (xg_world_minimap_source_capture(&request, &reader, capture) !=
            XG_WORLD_MINIMAP_CAPTURE_OK)
        return 1u;
    if (xg_world_minimap_build(&capture->source, output) !=
            XG_WORLD_MINIMAP_OK)
        return 2u;
    return 0u;
}

static bool compare_ft4_payload(
        CPUState *cpu, uint32_t packet_address,
        uint32_t expected_material_word, const uint16_t expected_uv[4],
        uint16_t expected_tpage, uint16_t expected_clut,
        PsxXgRenderFt4PayloadMismatch *first_mismatch) {
    PsxXgRenderFt4PayloadMismatch mismatch = {0};

    mismatch.packet_address = packet_address;
    mismatch.expected_material_word = expected_material_word;
    mismatch.actual_material_word = cpu->read_word(packet_address + 4u);
    mismatch.expected_tpage = expected_tpage;
    mismatch.actual_tpage = cpu->read_half(packet_address + 22u);
    mismatch.expected_clut = expected_clut;
    mismatch.actual_clut = cpu->read_half(packet_address + 14u);
    if (mismatch.actual_material_word != expected_material_word)
        mismatch.field_bits |= 1u << 0;
    if (mismatch.actual_tpage != expected_tpage) mismatch.field_bits |= 1u << 5;
    if (mismatch.actual_clut != expected_clut) mismatch.field_bits |= 1u << 6;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        mismatch.expected_uv[vertex] = expected_uv[vertex];
        mismatch.actual_uv[vertex] =
            cpu->read_half(packet_address + 12u + vertex * 8u);
        if (mismatch.actual_uv[vertex] != expected_uv[vertex])
            mismatch.field_bits |= (1u << 1) << vertex;
    }
    if (mismatch.field_bits != 0u && first_mismatch != NULL &&
        first_mismatch->field_bits == 0u)
        *first_mismatch = mismatch;
    return mismatch.field_bits == 0u;
}

void xg_render_world_terrain_water_shadow_begin(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    GpuDrawState draw = {0};
    uint64_t generation;

    if (!services_valid(services)) return;
    if (!services->authentication_generation(&generation)) generation = 0u;
    gpu_get_draw_state(&draw);
    (void)xg_world_terrain_water_shadow_begin(
        cpu, generation, &draw, services->screen_x_cull_margin());
}

void xg_render_world_terrain_water_shadow_finish(CPUState *cpu) {
    (void)xg_world_terrain_water_shadow_finish(cpu);
}

void xg_render_world_entity_shadows_shadow_begin(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    GpuDrawState draw = {0};
    uint64_t generation;

    if (!services_valid(services)) return;
    if (!services->authentication_generation(&generation)) generation = 0u;
    gpu_get_draw_state(&draw);
    (void)xg_world_entity_shadows_shadow_begin(cpu, generation, &draw);
}

void xg_render_world_entity_shadows_shadow_finish(CPUState *cpu) {
    (void)xg_world_entity_shadows_shadow_finish(cpu);
}

void xg_render_world_entity_shadows_shadow_observe_transform(CPUState *cpu) {
    xg_world_entity_shadows_shadow_observe_transform(cpu);
}

void xg_render_world_decorations_shadow_outer_begin(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    GpuDrawState draw = {0};
    uint64_t generation;

    if (!services_valid(services)) return;
    if (!services->authentication_generation(&generation)) generation = 0u;
    gpu_get_draw_state(&draw);
    (void)xg_world_decorations_shadow_outer_begin(
        cpu, generation, &draw, services->screen_x_cull_margin());
}

void xg_render_world_decorations_shadow_helper_begin(CPUState *cpu) {
    (void)xg_world_decorations_shadow_helper_begin(cpu);
}

void xg_render_world_decorations_shadow_helper_finish(CPUState *cpu) {
    (void)xg_world_decorations_shadow_helper_finish(cpu);
}

void xg_render_world_decorations_shadow_outer_finish(CPUState *cpu) {
    (void)xg_world_decorations_shadow_outer_finish(cpu);
}

void xg_render_world_minimap_shadow_begin(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    GpuDrawState draw = {0};
    uint64_t generation;

    if (!services_valid(services)) return;
    if (!services->authentication_generation(&generation)) generation = 0u;
    gpu_get_draw_state(&draw);
    (void)xg_world_minimap_shadow_begin(cpu, generation, &draw);
}

void xg_render_world_minimap_shadow_finish(CPUState *cpu) {
    (void)xg_world_minimap_shadow_finish(cpu);
}

void xg_render_world_horizon_shadow_clear_pending(void) {
    PsxXgRenderWorldHorizonShadowSnapshot snapshot = horizon.snapshot;

    if (!snapshot.pending) return;
    snapshot.pending = false;
    horizon = (XgRenderWorldHorizonState){.snapshot = snapshot};
}

static void block_horizon(uint32_t blocker) {
    xg_render_world_horizon_shadow_clear_pending();
    horizon.snapshot.blocked = true;
    if (horizon.snapshot.blocker == 0u) horizon.snapshot.blocker = blocker;
}

void xg_render_world_effects_shadow_clear_pending(void) {
    PsxXgRenderWorldEffectsShadowSnapshot snapshot = effects.snapshot;

    if (effects.count == 0u && !snapshot.pending) return;
    snapshot.pending = false;
    effects = (XgRenderWorldEffectsState){.snapshot = snapshot};
}

static void block_effects(uint32_t blocker) {
    xg_render_world_effects_shadow_clear_pending();
    effects.snapshot.blocked = true;
    if (effects.snapshot.blocker == 0u) effects.snapshot.blocker = blocker;
}

void xg_render_world_effects_shadow_begin(
        CPUState *cpu, bool enabled,
        const XgRenderWorldSimpleServices *services) {
    enum {
        PACKET_BASES = 0x8009be1cu,
        BUFFER_INDEX = 0x8009d7f0u,
        CONTEXT = 0x8009be3cu,
        PACKET_STRIDE = 0x28u,
        CAPACITY = 0x100u,
        OT_BUCKET_COUNT = 0xc0u,
    };
    uint32_t buffer_index;
    uint32_t packet_cursor;
    uint32_t context;
    uint32_t last_heads[OT_BUCKET_COUNT];
    uint32_t capture_result;

    if (!enabled) {
        xg_render_world_effects_shadow_clear_pending();
        return;
    }
    if (effects.snapshot.blocked) return;
    if (effects.snapshot.pending) {
        block_effects(100u);
        return;
    }
    if (!services_valid(services) || cpu == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071aa8)))
        return;
    if (!xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        effects.snapshot.begin_count == UINT64_MAX) {
        block_effects(101u);
        return;
    }
    buffer_index = cpu->read_word(BUFFER_INDEX);
    if (buffer_index >= 2u) {
        block_effects(101u);
        return;
    }
    packet_cursor = cpu->read_word(PACKET_BASES + buffer_index * 4u);
    if (!xg_render_runtime_word_address_is_valid(packet_cursor) ||
        packet_cursor > UINT32_MAX - CAPACITY * PACKET_STRIDE ||
        !xg_render_runtime_word_address_is_valid(
            packet_cursor + CAPACITY * PACKET_STRIDE - 4u)) {
        block_effects(101u);
        return;
    }
    ++effects.snapshot.begin_count;
    capture_result = capture_effects(
        cpu, &effects.capture, effects.records, &effects.count, NULL, NULL,
        services);
    if (capture_result == 1u) {
        ++effects.snapshot.source_capture_failure_count;
        block_effects(106u);
        return;
    }
    if (effects.snapshot.source_read_count >
            UINT64_MAX - effects.capture.authenticated_read_count ||
        effects.snapshot.source_read_bytes >
            UINT64_MAX - effects.capture.authenticated_read_bytes ||
        effects.snapshot.active_source_count >
            UINT64_MAX - effects.capture.active_source_count ||
        capture_result != 0u) {
        block_effects(107u);
        return;
    }
    effects.snapshot.source_read_count += effects.capture.authenticated_read_count;
    effects.snapshot.source_read_bytes += effects.capture.authenticated_read_bytes;
    effects.snapshot.active_source_count += effects.capture.active_source_count;

    context = cpu->read_word(CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !xg_render_runtime_word_address_is_valid(context + 0x70u)) {
        block_effects(108u);
        return;
    }
    effects.ot_base = cpu->read_word(context + 0x70u);
    if (!xg_render_runtime_word_address_is_valid(effects.ot_base) ||
        effects.ot_base > UINT32_MAX - (OT_BUCKET_COUNT - 1u) * 4u) {
        block_effects(108u);
        return;
    }
    memset(last_heads, 0, sizeof(last_heads));
    for (uint32_t index = 0u; index < effects.count; ++index) {
        const uint32_t packet = packet_cursor + index * PACKET_STRIDE;
        const uint32_t bucket = effects.records[index].ordering_bucket;

        if (bucket >= OT_BUCKET_COUNT ||
            !xg_render_runtime_word_address_is_valid(packet) ||
            (cpu->read_word(packet) >> 24u) != 9u) {
            block_effects(109u);
            return;
        }
        effects.packet_addresses[index] = packet;
        if (!effects.ot_touched[bucket]) {
            const uint32_t initial =
                cpu->read_word(effects.ot_base + bucket * 4u);

            effects.ot_touched[bucket] = true;
            effects.initial_ot_words[bucket] = initial;
            last_heads[bucket] = initial & UINT32_C(0x00ffffff);
        }
        effects.expected_tags[index] =
            UINT32_C(0x09000000) | last_heads[bucket];
        last_heads[bucket] = packet & UINT32_C(0x00ffffff);
        effects.final_ot_packets[bucket] = packet;
    }
    effects.entry_stack_pointer = cpu->gpr[29];
    effects.initial_packet_cursor = packet_cursor;
    effects.snapshot.pending = true;
}

void xg_render_world_effects_shadow_finish(CPUState *cpu) {
    enum {
        STACK_FRAME_SIZE = 0x50u,
        PACKET_STRIDE = 0x28u,
        CAPACITY = 0x100u,
        PACKET_PAYLOAD_END = 0x24u,
    };
    uint32_t packet_cursor;
    uint32_t delta;
    uint32_t primitive_count;
    uint32_t compare_count;
    bool invocation_matches = true;

    if (!effects.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        effects.entry_stack_pointer < STACK_FRAME_SIZE ||
        cpu->gpr[29] != effects.entry_stack_pointer - STACK_FRAME_SIZE) {
        block_effects(102u);
        return;
    }
    packet_cursor = cpu->gpr[19];
    if (packet_cursor < effects.initial_packet_cursor) {
        block_effects(102u);
        return;
    }
    delta = packet_cursor - effects.initial_packet_cursor;
    primitive_count = delta / PACKET_STRIDE;
    if (delta % PACKET_STRIDE != 0u || primitive_count > CAPACITY ||
        cpu->gpr[21] != CAPACITY ||
        packet_cursor > UINT32_MAX - PACKET_PAYLOAD_END ||
        cpu->gpr[18] != packet_cursor + PACKET_PAYLOAD_END ||
        effects.snapshot.completion_count == UINT64_MAX ||
        effects.snapshot.primitive_count > UINT64_MAX - primitive_count ||
        effects.snapshot.candidate_count > UINT64_MAX - effects.count) {
        block_effects(103u);
        return;
    }
    ++effects.snapshot.completion_count;
    effects.snapshot.primitive_count += primitive_count;
    effects.snapshot.candidate_count += effects.count;
    effects.snapshot.last_primitive_count = primitive_count;
    effects.snapshot.last_candidate_count = effects.count;
    if (primitive_count != effects.count) {
        ++effects.snapshot.count_mismatch_count;
        invocation_matches = false;
    }
    compare_count = primitive_count < effects.count
        ? primitive_count : effects.count;
    for (uint32_t index = 0u; index < compare_count; ++index) {
        const XgWorldEffectsRecord *record = &effects.records[index];
        const uint32_t packet = effects.packet_addresses[index];
        bool payload_matches = compare_ft4_payload(
            cpu, packet, record->material_word, record->uv,
            record->tpage, record->clut,
            &effects.snapshot.first_payload_mismatch);
        bool geometry_matches = true;
        const bool tag_matches =
            cpu->read_word(packet) == effects.expected_tags[index];

        for (uint32_t vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t expected_xy =
                (uint16_t)record->vertices[vertex].x |
                ((uint32_t)(uint16_t)record->vertices[vertex].y << 16u);
            geometry_matches &= cpu->read_word(
                packet + 8u + vertex * 8u) == expected_xy;
        }
        if (!payload_matches) ++effects.snapshot.payload_mismatch_count;
        if (!geometry_matches) ++effects.snapshot.geometry_mismatch_count;
        if (!tag_matches) ++effects.snapshot.tag_mismatch_count;
        if (payload_matches && geometry_matches && tag_matches) {
            ++effects.snapshot.match_count;
        } else {
            ++effects.snapshot.mismatch_count;
            invocation_matches = false;
            if (effects.snapshot.first_mismatch_packet == 0u) {
                effects.snapshot.first_mismatch_packet = packet;
                effects.snapshot.first_mismatch_source = record->source_index;
            }
        }
    }
    if (primitive_count != effects.count) {
        const uint32_t difference = primitive_count > effects.count
            ? primitive_count - effects.count : effects.count - primitive_count;
        effects.snapshot.mismatch_count += difference;
        if (effects.snapshot.first_mismatch_packet == 0u)
            effects.snapshot.first_mismatch_packet =
                effects.initial_packet_cursor + compare_count * PACKET_STRIDE;
    }
    for (uint32_t index = 0u; index < 0xc0u; ++index) {
        uint32_t expected_ot;

        if (!effects.ot_touched[index]) continue;
        expected_ot = (effects.initial_ot_words[index] &
                       UINT32_C(0xff000000)) |
            (effects.final_ot_packets[index] & UINT32_C(0x00ffffff));
        if (cpu->read_word(effects.ot_base + index * 4u) != expected_ot) {
            ++effects.snapshot.ot_mismatch_count;
            invocation_matches = false;
            if (effects.snapshot.first_mismatch_packet == 0u)
                effects.snapshot.first_mismatch_packet =
                    effects.ot_base + index * 4u;
        }
    }
    if (invocation_matches)
        ++effects.snapshot.invocation_match_count;
    else
        ++effects.snapshot.invocation_mismatch_count;
    xg_render_world_effects_shadow_clear_pending();
}

void xg_render_world_horizon_shadow_begin(
        CPUState *cpu, bool enabled,
        const XgRenderWorldSimpleServices *services) {
    enum {
        CONTEXT = 0x8009be3cu,
        SET_WINDOW = 0x8009d3d8u,
        RESET_WINDOW = 0x8009d3e4u,
        OT_BUCKET_COUNT = 0x1000u,
    };
    uint32_t context;
    uint32_t ot_base;
    uint32_t capture_result;

    if (!enabled) {
        xg_render_world_horizon_shadow_clear_pending();
        return;
    }
    if (horizon.snapshot.blocked) return;
    if (horizon.snapshot.pending) {
        block_horizon(90u);
        return;
    }
    if (!services_valid(services) || cpu == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071b58)))
        return;
    if (!xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        horizon.snapshot.begin_count == UINT64_MAX) {
        block_horizon(91u);
        return;
    }
    ++horizon.snapshot.begin_count;
    capture_result = capture_horizon(
        cpu, &horizon.capture, horizon.records, services);
    if (capture_result == 1u) {
        ++horizon.snapshot.source_capture_failure_count;
        block_horizon(92u);
        return;
    }
    if (horizon.snapshot.source_read_count >
            UINT64_MAX - horizon.capture.authenticated_read_count ||
        horizon.snapshot.source_read_bytes >
            UINT64_MAX - horizon.capture.authenticated_read_bytes ||
        capture_result != 0u) {
        block_horizon(93u);
        return;
    }
    horizon.snapshot.source_read_count += horizon.capture.authenticated_read_count;
    horizon.snapshot.source_read_bytes += horizon.capture.authenticated_read_bytes;
    horizon.snapshot.last_ot_bucket = horizon.records[0].ordering_bucket;
    if (horizon.records[0].ordering_bucket >= OT_BUCKET_COUNT) {
        block_horizon(94u);
        return;
    }

    horizon.entry_stack_pointer = cpu->gpr[29];
    for (uint32_t quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        const uint32_t packet = UINT32_C(0x8009c744) + quad * 0x50u +
            horizon.capture.buffer_index * 0x28u;

        if (!xg_render_runtime_word_address_is_valid(packet) ||
            !xg_render_runtime_word_address_is_valid(packet + 0x24u)) {
            block_horizon(94u);
            return;
        }
        horizon.packet_addresses[quad] = packet;
        horizon.initial_packet_tags[quad] = cpu->read_word(packet);
        if ((horizon.initial_packet_tags[quad] >> 24u) != 9u) {
            block_horizon(95u);
            return;
        }
    }
    horizon.initial_texture_window_tags[0] = cpu->read_word(SET_WINDOW);
    horizon.initial_texture_window_tags[1] = cpu->read_word(RESET_WINDOW);
    if ((horizon.initial_texture_window_tags[0] >> 24u) != 2u ||
        (horizon.initial_texture_window_tags[1] >> 24u) != 2u) {
        block_horizon(95u);
        return;
    }
    context = cpu->read_word(CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !xg_render_runtime_word_address_is_valid(context + 0x70u)) {
        block_horizon(94u);
        return;
    }
    ot_base = cpu->read_word(context + 0x70u);
    if (ot_base > UINT32_MAX - horizon.records[0].ordering_bucket * 4u) {
        block_horizon(94u);
        return;
    }
    horizon.ot_address = ot_base + horizon.records[0].ordering_bucket * 4u;
    if (!xg_render_runtime_word_address_is_valid(horizon.ot_address)) {
        block_horizon(94u);
        return;
    }
    horizon.initial_ot_word = cpu->read_word(horizon.ot_address);
    horizon.snapshot.pending = true;
}

void xg_render_world_horizon_shadow_finish(CPUState *cpu) {
    enum {
        SET_WINDOW = 0x8009d3d8u,
        RESET_WINDOW = 0x8009d3e4u,
    };
    const bool accepted = horizon.records[0].accepted;
    const uint8_t u0 = (uint8_t)(horizon.capture.source.angle >> 2u) & 0x7fu;
    const uint8_t u1 = u0 | 0x80u;
    const uint16_t expected_uv[4] = {
        u0, u1, (uint16_t)(u0 | 0x3f00u), (uint16_t)(u1 | 0x3f00u),
    };
    bool payload_matches = true;
    bool geometry_matches = true;
    bool tag_matches = true;
    bool ot_matches;
    bool texture_window_matches;

    if (!horizon.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        horizon.entry_stack_pointer < 0x40u ||
        cpu->gpr[29] != horizon.entry_stack_pointer - 0x40u) {
        block_horizon(96u);
        return;
    }
    if (horizon.snapshot.completion_count == UINT64_MAX ||
        horizon.snapshot.match_count == UINT64_MAX ||
        horizon.snapshot.mismatch_count == UINT64_MAX ||
        (accepted &&
         (horizon.snapshot.accepted_invocation_count == UINT64_MAX ||
          horizon.snapshot.primitive_count > UINT64_MAX - 2u))) {
        block_horizon(97u);
        return;
    }

    for (uint32_t quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        const XgWorldHorizonRecord *record = &horizon.records[quad];
        const uint32_t packet = horizon.packet_addresses[quad];

        payload_matches &= compare_ft4_payload(
            cpu, packet, UINT32_C(0x2e303030), expected_uv,
            0x003eu, 0x7f91u, &horizon.snapshot.first_payload_mismatch);
        for (uint32_t vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t expected_xy =
                (uint16_t)record->vertices[vertex].x |
                ((uint32_t)(uint16_t)record->vertices[vertex].y << 16u);
            const uint32_t actual_xy =
                cpu->read_word(packet + 8u + vertex * 8u);
            const bool vertex_matches = actual_xy == expected_xy;

            if (!vertex_matches &&
                horizon.snapshot.first_geometry_mismatch_invocation == 0u) {
                horizon.snapshot.first_geometry_mismatch_invocation =
                    horizon.snapshot.completion_count + 1u;
                horizon.snapshot.first_geometry_mismatch_quad = quad;
                horizon.snapshot.first_geometry_mismatch_vertex = vertex;
                horizon.snapshot.first_geometry_expected_xy = expected_xy;
                horizon.snapshot.first_geometry_actual_xy = actual_xy;
            }
            geometry_matches &= vertex_matches;
        }
    }
    texture_window_matches =
        cpu->read_word(SET_WINDOW + 4u) == UINT32_C(0xe2000010) &&
        cpu->read_word(SET_WINDOW + 8u) == 0u &&
        cpu->read_word(RESET_WINDOW + 4u) == UINT32_C(0xe2000000) &&
        cpu->read_word(RESET_WINDOW + 8u) == 0u;
    if (accepted) {
        const uint32_t expected_packet_tags[2] = {
            UINT32_C(0x09000000) |
                (RESET_WINDOW & UINT32_C(0x00ffffff)),
            UINT32_C(0x09000000) |
                (horizon.packet_addresses[0] & UINT32_C(0x00ffffff)),
        };
        const uint32_t expected_window_tags[2] = {
            UINT32_C(0x02000000) |
                (horizon.packet_addresses[1] & UINT32_C(0x00ffffff)),
            UINT32_C(0x02000000) |
                (horizon.initial_ot_word & UINT32_C(0x00ffffff)),
        };

        for (uint32_t quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad)
            tag_matches &= cpu->read_word(horizon.packet_addresses[quad]) ==
                expected_packet_tags[quad];
        tag_matches &= cpu->read_word(SET_WINDOW) == expected_window_tags[0];
        tag_matches &= cpu->read_word(RESET_WINDOW) == expected_window_tags[1];
        ot_matches = cpu->read_word(horizon.ot_address) ==
            ((horizon.initial_ot_word & UINT32_C(0xff000000)) |
             (SET_WINDOW & UINT32_C(0x00ffffff)));
    } else {
        for (uint32_t quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad)
            tag_matches &= cpu->read_word(horizon.packet_addresses[quad]) ==
                horizon.initial_packet_tags[quad];
        tag_matches &= cpu->read_word(SET_WINDOW) ==
            horizon.initial_texture_window_tags[0];
        tag_matches &= cpu->read_word(RESET_WINDOW) ==
            horizon.initial_texture_window_tags[1];
        ot_matches = cpu->read_word(horizon.ot_address) ==
            horizon.initial_ot_word;
    }

    ++horizon.snapshot.completion_count;
    if (accepted) {
        ++horizon.snapshot.accepted_invocation_count;
        horizon.snapshot.primitive_count += 2u;
    }
    if (!payload_matches) ++horizon.snapshot.payload_mismatch_count;
    if (!geometry_matches) ++horizon.snapshot.geometry_mismatch_count;
    if (!tag_matches) ++horizon.snapshot.tag_mismatch_count;
    if (!ot_matches) ++horizon.snapshot.ot_mismatch_count;
    if (!texture_window_matches)
        ++horizon.snapshot.texture_window_mismatch_count;
    if (payload_matches && geometry_matches && tag_matches && ot_matches &&
        texture_window_matches) {
        ++horizon.snapshot.match_count;
    } else {
        if (horizon.snapshot.mismatch_count == 0u) {
            if (!payload_matches || !geometry_matches || !tag_matches)
                horizon.snapshot.first_mismatch_packet =
                    horizon.packet_addresses[0];
            else if (!texture_window_matches)
                horizon.snapshot.first_mismatch_packet = SET_WINDOW;
            else
                horizon.snapshot.first_mismatch_packet = horizon.ot_address;
        }
        ++horizon.snapshot.mismatch_count;
    }
    xg_render_world_horizon_shadow_clear_pending();
}

bool xg_render_world_horizon_cutover(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    enum {
        CONTEXT = 0x8009be3cu,
        SET_WINDOW = 0x8009d3d8u,
        RESET_WINDOW = 0x8009d3e4u,
        OT_BUCKET_COUNT = 0x1000u,
    };
    XgWorldHorizonCapture capture;
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t packet_addresses[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t packet_tags[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t window_tags[2];
    uint16_t uv[4];
    uint32_t context;
    uint32_t ot_base;
    uint32_t ot_address;
    uint32_t initial_ot_word;

    if (!services_valid(services) || !services->authorize_direct_dispatch() ||
        cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071b58)) ||
        capture_horizon(cpu, &capture, records, services) != 0u ||
        records[0].accepted != records[1].accepted ||
        records[0].ordering_bucket != records[1].ordering_bucket ||
        horizon.snapshot.native_cutover_count == UINT64_MAX ||
        (records[0].accepted &&
         horizon.snapshot.native_primitive_count >
             UINT64_MAX - XG_WORLD_HORIZON_QUAD_COUNT))
        return false;
    if (!records[0].accepted) {
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE,
        };

        for (uint32_t quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
            if (!services->stage_temporal(
                    &records[quad].primitive, UINT32_C(0x80073b04), quad,
                    &policy)) {
                services->abort_submission();
                return false;
            }
        }
        ++horizon.snapshot.native_cutover_count;
        cpu->pc = cpu->gpr[31];
        return true;
    }
    if (records[0].ordering_bucket >= OT_BUCKET_COUNT) return false;
    for (uint32_t quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        packet_addresses[quad] = UINT32_C(0x8009c744) + quad * 0x50u +
            capture.buffer_index * 0x28u;
        if (!xg_render_runtime_word_address_is_valid(packet_addresses[quad]) ||
            !xg_render_runtime_word_address_is_valid(
                packet_addresses[quad] + 0x24u))
            return false;
        packet_tags[quad] = cpu->read_word(packet_addresses[quad]);
        if ((packet_tags[quad] >> 24u) != 9u) return false;
    }
    window_tags[0] = cpu->read_word(SET_WINDOW);
    window_tags[1] = cpu->read_word(RESET_WINDOW);
    if ((window_tags[0] >> 24u) != 2u ||
        (window_tags[1] >> 24u) != 2u ||
        cpu->read_word(SET_WINDOW + 4u) != UINT32_C(0xe2000010) ||
        cpu->read_word(SET_WINDOW + 8u) != 0u ||
        cpu->read_word(RESET_WINDOW + 4u) != UINT32_C(0xe2000000) ||
        cpu->read_word(RESET_WINDOW + 8u) != 0u)
        return false;
    context = cpu->read_word(CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !xg_render_runtime_word_address_is_valid(context + 0x70u))
        return false;
    ot_base = cpu->read_word(context + 0x70u);
    if (ot_base > UINT32_MAX - records[0].ordering_bucket * 4u) return false;
    ot_address = ot_base + records[0].ordering_bucket * 4u;
    if (!xg_render_runtime_word_address_is_valid(ot_address)) return false;
    initial_ot_word = cpu->read_word(ot_address);

    for (uint32_t quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        if (!services->stage_native(
                &records[quad].primitive, packet_addresses[quad],
                UINT32_C(0x61000000) | quad, UINT32_C(0x80073b04), quad)) {
            services->abort_submission();
            return false;
        }
    }
    uv[0] = (uint8_t)(capture.source.angle >> 2u) & 0x7fu;
    uv[1] = uv[0] | 0x80u;
    uv[2] = uv[0] | 0x3f00u;
    uv[3] = uv[1] | 0x3f00u;
    for (uint32_t quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        const uint32_t packet = packet_addresses[quad];

        psx_store_cycle_barrier();
        cpu->write_word(packet + 4u, UINT32_C(0x2e303030));
        for (uint32_t vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t xy = (uint16_t)records[quad].vertices[vertex].x |
                ((uint32_t)(uint16_t)records[quad].vertices[vertex].y << 16u);
            psx_store_cycle_barrier();
            cpu->write_word(packet + 8u + vertex * 8u, xy);
        }
        psx_store_cycle_barrier();
        cpu->write_word(packet + 12u,
                        (uint32_t)uv[0] | UINT32_C(0x7f910000));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 20u,
                        (uint32_t)uv[1] | UINT32_C(0x003e0000));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 28u, uv[2]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 36u, uv[3]);
    }
    psx_store_cycle_barrier();
    cpu->write_word(SET_WINDOW,
        (window_tags[0] & UINT32_C(0xff000000)) |
        (packet_addresses[1] & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(packet_addresses[1],
        (packet_tags[1] & UINT32_C(0xff000000)) |
        (packet_addresses[0] & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(packet_addresses[0],
        (packet_tags[0] & UINT32_C(0xff000000)) |
        (RESET_WINDOW & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(RESET_WINDOW,
        (window_tags[1] & UINT32_C(0xff000000)) |
        (initial_ot_word & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(ot_address,
        (initial_ot_word & UINT32_C(0xff000000)) |
        (SET_WINDOW & UINT32_C(0x00ffffff)));
    ++horizon.snapshot.native_cutover_count;
    horizon.snapshot.native_primitive_count += XG_WORLD_HORIZON_QUAD_COUNT;
    cpu->pc = cpu->gpr[31];
    return true;
}

bool xg_render_world_effects_cutover(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    enum {
        PACKET_BASES = 0x8009be1cu,
        BUFFER_INDEX = 0x8009d7f0u,
        CONTEXT = 0x8009be3cu,
        PACKET_STRIDE = 0x28u,
        CAPACITY = 0x100u,
        OT_BUCKET_COUNT = 0xc0u,
    };
    XgWorldEffectsCapture capture;
    XgWorldEffectsRecord records[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    XgWorldEffectsRecord temporal_records[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t packet_addresses[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t packet_tags[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t ot_addresses[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t buffer_index;
    uint32_t packet_cursor;
    uint32_t context;
    uint32_t ot_base;
    uint32_t count;
    uint32_t temporal_count;

    if (!services_valid(services) || !services->authorize_direct_dispatch() ||
        cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071aa8)) ||
        capture_effects(cpu, &capture, records, &count, temporal_records,
                        &temporal_count, services) != 0u)
        return false;
    buffer_index = cpu->read_word(BUFFER_INDEX);
    if (buffer_index >= 2u) return false;
    packet_cursor = cpu->read_word(PACKET_BASES + buffer_index * 4u);
    if (!xg_render_runtime_word_address_is_valid(packet_cursor) ||
        packet_cursor > UINT32_MAX - CAPACITY * PACKET_STRIDE ||
        !xg_render_runtime_word_address_is_valid(
            packet_cursor + CAPACITY * PACKET_STRIDE - 4u))
        return false;
    context = cpu->read_word(CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !xg_render_runtime_word_address_is_valid(context + 0x70u))
        return false;
    ot_base = cpu->read_word(context + 0x70u);
    if (!xg_render_runtime_word_address_is_valid(ot_base) ||
        ot_base > UINT32_MAX - (OT_BUCKET_COUNT - 1u) * 4u)
        return false;
    if (effects.snapshot.native_cutover_count == UINT64_MAX ||
        effects.snapshot.native_primitive_count > UINT64_MAX - count)
        return false;
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t packet = packet_cursor + index * PACKET_STRIDE;
        const uint32_t bucket = records[index].ordering_bucket;

        if (bucket >= OT_BUCKET_COUNT ||
            !xg_render_runtime_word_address_is_valid(packet) ||
            !xg_render_runtime_word_address_is_valid(
                packet + PACKET_STRIDE - 4u))
            return false;
        packet_addresses[index] = packet;
        packet_tags[index] = cpu->read_word(packet);
        if ((packet_tags[index] >> 24u) != 9u) return false;
        ot_addresses[index] = ot_base + bucket * 4u;
        if (!xg_render_runtime_word_address_is_valid(ot_addresses[index]))
            return false;
    }
    for (uint32_t index = 0u; index < count; ++index) {
        if (!services->stage_native(
                &records[index].primitive, packet_addresses[index],
                UINT32_C(0x60000000) | records[index].source_index,
                UINT32_C(0x80089c78), records[index].source_index)) {
            services->abort_submission();
            return false;
        }
    }
    for (uint32_t index = 0u; index < temporal_count; ++index) {
        const XgWorldEffectsRecord *record = &temporal_records[index];
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = INT32_MIN,
            .screen_top = INT32_MIN,
            .screen_right_exclusive =
                (320 + capture.source.screen_x_cull_margin) * INT32_C(65536),
            .screen_bottom_exclusive = 216 * INT32_C(65536),
            .depth_min_inclusive = 0,
            .depth_max_exclusive = 0xc00,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = 4u,
        };

        if (!services->stage_temporal(
                &record->primitive, UINT32_C(0x80089c78),
                record->source_index, &policy)) {
            services->abort_submission();
            return false;
        }
    }
    for (uint32_t index = 0u; index < count; ++index) {
        const XgWorldEffectsRecord *record = &records[index];
        const uint32_t packet = packet_addresses[index];
        uint32_t previous_head;

        psx_store_cycle_barrier();
        cpu->write_word(packet + 4u, record->material_word);
        for (uint32_t vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t xy = (uint16_t)record->vertices[vertex].x |
                ((uint32_t)(uint16_t)record->vertices[vertex].y << 16u);
            psx_store_cycle_barrier();
            cpu->write_word(packet + 8u + vertex * 8u, xy);
        }
        psx_store_cycle_barrier();
        cpu->write_word(packet + 12u,
            (uint32_t)record->uv[0] | ((uint32_t)record->clut << 16u));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 20u,
            (uint32_t)record->uv[1] | ((uint32_t)record->tpage << 16u));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 28u, record->uv[2]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 36u, record->uv[3]);
        previous_head = cpu->read_word(ot_addresses[index]);
        psx_store_cycle_barrier();
        cpu->write_word(packet,
            (packet_tags[index] & UINT32_C(0xff000000)) |
            (previous_head & UINT32_C(0x00ffffff)));
        psx_store_cycle_barrier();
        cpu->write_word(ot_addresses[index],
            (previous_head & UINT32_C(0xff000000)) |
            (packet & UINT32_C(0x00ffffff)));
    }
    ++effects.snapshot.native_cutover_count;
    effects.snapshot.native_primitive_count += count;
    cpu->pc = cpu->gpr[31];
    return true;
}

bool xg_render_world_minimap_cutover(
        CPUState *cpu, const XgRenderWorldSimpleServices *services) {
    enum {
        CALLER_RETURN = 0x80071b84u,
        CONTINUATION = 0x80074298u,
        CONTEXT = 0x8009be3cu,
        CONTEXT_OT_OFFSET = 0x70u,
        STACK_RA_OFFSET = 0x30u,
        TRIANGLE_BASE = 0x8009a340u,
        PACKET_BASE = 0x8009c664u,
        PACKET_BUFFER_STRIDE = 0x70u,
        PACKET_STRIDE = 0x1cu,
        WORLD_POSITION = 0x8009d55cu,
    };
    XgWorldMinimapCapture capture;
    XgWorldMinimapBuildOutput output;
    XgWorldMinimapShadowSnapshot snapshot;
    GpuRenderSemantic semantics[XG_WORLD_MINIMAP_TRIANGLE_COUNT];
    uint32_t packet_tags[XG_WORLD_MINIMAP_TRIANGLE_COUNT];
    uint32_t scratch_words[10];
    uint32_t context;
    uint32_t ot_address;
    uint32_t initial_ot_word;
    uint32_t expected_packet_base;

    if (!services_valid(services) || !services->authorize_direct_dispatch() ||
        cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31], CALLER_RETURN) ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] > UINT32_MAX - STACK_RA_OFFSET ||
        !xg_render_runtime_word_address_is_valid(
            cpu->gpr[29] + STACK_RA_OFFSET) ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + STACK_RA_OFFSET), CALLER_RETURN) ||
        capture_minimap(cpu, &capture, &output, services) != 0u)
        return false;
    expected_packet_base = PACKET_BASE +
        capture.source.buffer_index * PACKET_BUFFER_STRIDE;
    if (!capture.authenticated || !capture.sealed ||
        output.ordering_count < XG_WORLD_MINIMAP_TRIANGLE_COUNT ||
        output.scratch.angle_address != XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS ||
        output.scratch.rotation_address !=
            XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS ||
        output.scratch.translation_address !=
            XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS ||
        cpu->gpr[2] != PACKET_BASE ||
        cpu->gpr[3] != capture.source.buffer_index * PACKET_BUFFER_STRIDE ||
        cpu->gpr[16] != expected_packet_base || cpu->gpr[17] != 0u ||
        cpu->gpr[18] != TRIANGLE_BASE ||
        cpu->gpr[19] != UINT32_C(0x1f800000) ||
        cpu->gpr[20] != UINT32_C(0x00ffffff) ||
        cpu->gpr[21] != XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS ||
        cpu->gpr[22] != UINT32_C(0xff000000) ||
        cpu->gpr[23] != WORLD_POSITION)
        return false;
    context = cpu->read_word(CONTEXT);
    if (context > UINT32_MAX - CONTEXT_OT_OFFSET ||
        !xg_render_runtime_word_address_is_valid(context + CONTEXT_OT_OFFSET))
        return false;
    ot_address = cpu->read_word(context + CONTEXT_OT_OFFSET);
    if (!xg_render_runtime_word_address_is_valid(ot_address)) return false;
    initial_ot_word = cpu->read_word(ot_address);
    xg_world_minimap_shadow_snapshot(&snapshot);
    if (snapshot.native_cutover_count == UINT64_MAX ||
        snapshot.native_primitive_count >
            UINT64_MAX - XG_WORLD_MINIMAP_TRIANGLE_COUNT)
        return false;
    for (uint32_t triangle = 0u;
         triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT; ++triangle) {
        const XgWorldMinimapTriangleRecord *record =
            &output.triangles[triangle];
        const uint32_t packet =
            expected_packet_base + triangle * PACKET_STRIDE;

        if (!record->submitted || record->packet_address != packet ||
            !xg_render_runtime_word_address_is_valid(packet) ||
            !xg_render_runtime_word_address_is_valid(packet + 24u) ||
            output.ordering[triangle].kind !=
                XG_WORLD_MINIMAP_ORDER_TRIANGLE ||
            output.ordering[triangle].packet_address != packet ||
            output.ordering[triangle].source_index != triangle ||
            output.ordering[triangle].payload_word_count != 6u ||
            xg_render_backend_translate_primitive(
                &record->primitive, &semantics[triangle]) !=
                XG_RENDER_BACKEND_OK)
            return false;
        for (uint32_t vertex = 0u;
             vertex < XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT; ++vertex)
            if (record->screen_xy_address[vertex] !=
                packet + 8u + vertex * 8u)
                return false;
        packet_tags[triangle] = cpu->read_word(packet);
        if ((packet_tags[triangle] >> 24u) != 6u) return false;
    }
    scratch_words[0] = (uint16_t)output.scratch.angle_x |
        ((uint32_t)(uint16_t)output.scratch.angle_y << 16u);
    scratch_words[1] =
        (cpu->read_word(XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS + 4u) &
         UINT32_C(0xffff0000)) |
        output.scratch.angle_z;
    scratch_words[2] = (uint16_t)output.scratch.rotation[0][0] |
        ((uint32_t)(uint16_t)output.scratch.rotation[0][1] << 16u);
    scratch_words[3] = (uint16_t)output.scratch.rotation[0][2] |
        ((uint32_t)(uint16_t)output.scratch.rotation[1][0] << 16u);
    scratch_words[4] = (uint16_t)output.scratch.rotation[1][1] |
        ((uint32_t)(uint16_t)output.scratch.rotation[1][2] << 16u);
    scratch_words[5] = (uint16_t)output.scratch.rotation[2][0] |
        ((uint32_t)(uint16_t)output.scratch.rotation[2][1] << 16u);
    scratch_words[6] =
        (cpu->read_word(XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS + 16u) &
         UINT32_C(0xffff0000)) |
        (uint16_t)output.scratch.rotation[2][2];
    scratch_words[7] = (uint32_t)output.scratch.translation[0];
    scratch_words[8] = (uint32_t)output.scratch.translation[1];
    scratch_words[9] = (uint32_t)output.scratch.translation[2];
    for (uint32_t word = 0u; word < 10u; ++word) {
        const uint32_t address = word < 2u
            ? XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS + word * 4u
            : word < 7u
                ? XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS +
                    (word - 2u) * 4u
                : XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS +
                    (word - 7u) * 4u;
        if (!xg_render_runtime_word_address_is_valid(address)) return false;
    }
    for (uint32_t triangle = 0u;
         triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT; ++triangle) {
        if (!services->stage_native(
                &output.triangles[triangle].primitive,
                output.triangles[triangle].packet_address,
                UINT32_C(0x62000000) |
                    (output.triangles[triangle].packet_address &
                     UINT32_C(0x001ffffc)),
                UINT32_C(0x8007412c), triangle)) {
            services->abort_submission();
            return false;
        }
    }
    if (!xg_world_minimap_shadow_record_native_cutover(
            XG_WORLD_MINIMAP_TRIANGLE_COUNT)) {
        services->abort_submission();
        return false;
    }
    for (uint32_t triangle = 0u;
         triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT; ++triangle) {
        const XgWorldMinimapTriangleRecord *record =
            &output.triangles[triangle];
        const uint32_t predecessor = triangle == 0u ? initial_ot_word
            : (initial_ot_word & UINT32_C(0xff000000)) |
                (output.triangles[triangle - 1u].packet_address &
                 UINT32_C(0x00ffffff));

        for (uint32_t word = 0u; word < 10u; ++word) {
            const uint32_t address = word < 2u
                ? XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS + word * 4u
                : word < 7u
                    ? XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS +
                        (word - 2u) * 4u
                    : XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS +
                        (word - 7u) * 4u;
            psx_store_cycle_barrier();
            cpu->write_word(address, scratch_words[word]);
        }
        for (uint32_t vertex = 0u;
             vertex < XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT; ++vertex) {
            const uint32_t xy = (uint16_t)record->screen_xy[vertex][0] |
                ((uint32_t)(uint16_t)record->screen_xy[vertex][1] << 16u);
            psx_store_cycle_barrier();
            cpu->write_word(record->screen_xy_address[vertex], xy);
        }
        psx_store_cycle_barrier();
        cpu->write_word(record->packet_address,
            (packet_tags[triangle] & UINT32_C(0xff000000)) |
            (predecessor & UINT32_C(0x00ffffff)));
        psx_store_cycle_barrier();
        cpu->write_word(ot_address,
            (predecessor & UINT32_C(0xff000000)) |
            (record->packet_address & UINT32_C(0x00ffffff)));
    }
    cpu->gpr[2] = 0u;
    cpu->gpr[3] = output.triangles[3].packet_address & UINT32_C(0x00ffffff);
    cpu->gpr[4] = ot_address;
    cpu->gpr[5] = context;
    cpu->gpr[16] = expected_packet_base + PACKET_BUFFER_STRIDE;
    cpu->gpr[17] = XG_WORLD_MINIMAP_TRIANGLE_COUNT;
    cpu->gpr[18] = TRIANGLE_BASE + 0x60u;
    cpu->pc = CONTINUATION;
    return true;
}

void xg_render_world_simple_invalidate_semantic_shadows(void) {
    (void)xg_world_terrain_water_shadow_lifecycle_invalidate();
    xg_world_entity_shadows_shadow_lifecycle_invalidate();
    xg_world_decorations_shadow_lifecycle_invalidate();
    xg_world_minimap_shadow_invalidate();
}

void xg_render_world_horizon_shadow_invalidate_code(void) {
    if (horizon.snapshot.pending)
        block_horizon(99u);
    else
        xg_render_world_horizon_shadow_clear_pending();
}

void xg_render_world_effects_shadow_invalidate_code(void) {
    if (effects.snapshot.pending)
        block_effects(105u);
    else
        xg_render_world_effects_shadow_clear_pending();
}

void xg_render_world_simple_scene_boundary(void) {
    if (horizon.snapshot.pending)
        block_horizon(98u);
    else
        xg_render_world_horizon_shadow_clear_pending();
    if (effects.snapshot.pending)
        block_effects(104u);
    else
        xg_render_world_effects_shadow_clear_pending();
}

void xg_render_world_horizon_snapshot(
        PsxXgRenderWorldHorizonShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = horizon.snapshot;
}

void xg_render_world_effects_snapshot(
        PsxXgRenderWorldEffectsShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = effects.snapshot;
}

void xg_render_world_terrain_water_snapshot(
        PsxXgRenderWorldTerrainWaterShadowSnapshot *out_snapshot) {
    (void)xg_world_terrain_water_shadow_snapshot(out_snapshot);
    if (out_snapshot == NULL) return;
    xg_world_terrain_water_build_diagnostics(&out_snapshot->build_diagnostics);
    out_snapshot->mesh_duplicate_vertices = terrain_water.duplicate_vertices;
    out_snapshot->mesh_cross_tile_duplicate_vertices =
        terrain_water.cross_tile_duplicate_vertices;
    out_snapshot->mesh_canonical_raster_conflicts =
        terrain_water.canonical_raster_conflicts;
    out_snapshot->mesh_native_raster_conflicts =
        terrain_water.native_raster_conflicts;
    out_snapshot->mesh_cross_tile_native_raster_conflicts =
        terrain_water.cross_tile_native_raster_conflicts;
    if (out_snapshot->native_cutover_count == 0u) {
        out_snapshot->last_caller_return = terrain_water.last_caller;
        out_snapshot->blocker_detail = terrain_water.blocker_detail;
    }
}

void xg_render_world_entity_shadows_snapshot(
        PsxXgRenderWorldEntityShadowsShadowSnapshot *out_snapshot) {
    (void)xg_world_entity_shadows_shadow_snapshot(out_snapshot);
}

void xg_render_world_decorations_snapshot(
        PsxXgRenderWorldDecorationsShadowSnapshot *out_snapshot) {
    (void)xg_world_decorations_shadow_snapshot(out_snapshot);
}

void xg_render_world_minimap_snapshot(
        PsxXgRenderWorldMinimapShadowSnapshot *out_snapshot) {
    xg_world_minimap_shadow_snapshot(out_snapshot);
}

void xg_render_world_simple_reset(void) {
    terrain_water = (XgRenderWorldTerrainWaterState){0};
    entity_shadows = (XgRenderWorldEntityShadowsState){0};
    decorations = (XgRenderWorldDecorationsState){0};
    horizon = (XgRenderWorldHorizonState){0};
    effects = (XgRenderWorldEffectsState){0};
    xg_world_terrain_water_shadow_reset();
    xg_world_entity_shadows_shadow_reset();
    xg_world_decorations_shadow_reset();
    xg_world_minimap_shadow_reset();
}

void xg_render_world_simple_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    const bool shared_data = xg_render_invalidation_has_code_class(
        event, PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);
    const bool horizon_code = event->mutation.semantic_authority_loss ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON);
    const bool effects_code = event->mutation.semantic_authority_loss ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS);
    const bool sky_code = event->mutation.semantic_authority_loss ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_SKY);

    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (horizon_code)
            xg_render_world_horizon_shadow_invalidate_code();
        else if (shared_data)
            xg_render_world_horizon_shadow_clear_pending();
        if (effects_code)
            xg_render_world_effects_shadow_invalidate_code();
        else if (shared_data)
            xg_render_world_effects_shadow_clear_pending();
        if (sky_code || horizon_code || effects_code || shared_data)
            xg_render_world_simple_invalidate_semantic_shadows();
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE) {
        xg_render_world_horizon_shadow_clear_pending();
        xg_render_world_effects_shadow_clear_pending();
        xg_render_world_simple_invalidate_semantic_shadows();
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        xg_render_world_simple_scene_boundary();
        xg_render_world_simple_invalidate_semantic_shadows();
    } else if (event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH) {
        xg_render_world_horizon_shadow_clear_pending();
        xg_render_world_effects_shadow_clear_pending();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_world_simple_reset();
    }
}
