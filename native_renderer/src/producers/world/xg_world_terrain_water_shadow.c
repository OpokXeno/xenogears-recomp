#include "xg_world_terrain_water_shadow.h"

#include "xg_world_terrain_water.h"
#include "xg_world_terrain_water_source_capture.h"

#include <limits.h>
#include <string.h>

enum {
    TERRAIN_CONTEXT_OT_OFFSET = 0x70,
    TERRAIN_CONTEXT_PACKET_OFFSET = 0x74,
    TERRAIN_ORIGINAL_PACKET_LIMIT = XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY,
};

typedef struct TerrainShadowState {
    XgWorldTerrainWaterCapture capture;
    XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    XgWorldTerrainWaterShadowSnapshot snapshot;
    uint32_t expected_tags[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    uint32_t initial_material[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    uint16_t initial_uv2_high[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    uint32_t initial_ot[XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT];
    uint32_t expected_ot[XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT];
    uint32_t last_record_by_bucket[
        XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT];
    uint32_t entry_stack_pointer;
    uint32_t ot_base;
    uint32_t packet_base;
    uint32_t count;
} TerrainShadowState;

typedef struct TerrainFinishStats {
    uint64_t invocation_match_count;
    uint64_t invocation_mismatch_count;
    uint64_t original_primitive_count;
    uint64_t packet_compared_count;
    uint64_t packet_match_count;
    uint64_t packet_mismatch_count;
    uint64_t missing_primitive_count;
    uint64_t unexpected_primitive_count;
    uint64_t count_mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t material_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t geometry_vertex_mismatch_count;
    uint64_t uv_mismatch_count;
    uint64_t clut_mismatch_count;
    uint64_t tpage_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t packet_unchanged_mismatch_count;
    uint64_t touched_ot_bucket_count;
    uint64_t untouched_ot_bucket_count;
    uint64_t touched_ot_mismatch_count;
    uint64_t untouched_ot_mismatch_count;
} TerrainFinishStats;

static TerrainShadowState terrain_shadow;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
           (right & UINT32_C(0x1fffffff));
}

static bool ram_range_is_valid(uint32_t address, uint32_t size,
                               uint32_t alignment) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint64_t physical = address & UINT32_C(0x1fffffff);

    return size != 0u && alignment != 0u &&
        (address & (alignment - 1u)) == 0u &&
        (segment == 0u || segment == UINT32_C(0x80000000) ||
         segment == UINT32_C(0xa0000000)) &&
        physical + size <= UINT64_C(0x200000) &&
        (uint64_t)address + size - 1u <= UINT32_MAX;
}

static bool stack_address_is_valid(uint32_t address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    return (segment == 0u || segment == UINT32_C(0x80000000) ||
            segment == UINT32_C(0xa0000000)) &&
        (physical & 3u) == 0u && physical <= UINT32_C(0x001fffd8);
}

static void clear_pending(void) {
    terrain_shadow.snapshot.pending = false;
    terrain_shadow.entry_stack_pointer = 0u;
    terrain_shadow.ot_base = 0u;
    terrain_shadow.packet_base = 0u;
    terrain_shadow.count = 0u;
}

static XgWorldTerrainWaterShadowResult block_shadow(
    XgWorldTerrainWaterShadowBlocker blocker, uint32_t detail,
    uint32_t address, uint32_t expected, uint32_t actual,
    XgWorldTerrainWaterShadowResult result) {
    clear_pending();
    if (terrain_shadow.snapshot.block_count != UINT64_MAX)
        ++terrain_shadow.snapshot.block_count;
    terrain_shadow.snapshot.blocked = true;
    if (terrain_shadow.snapshot.blocker ==
        XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_NONE) {
        terrain_shadow.snapshot.blocker = blocker;
        terrain_shadow.snapshot.blocker_detail = detail;
        terrain_shadow.snapshot.blocker_address = address;
        terrain_shadow.snapshot.blocker_expected = expected;
        terrain_shadow.snapshot.blocker_actual = actual;
    }
    return result;
}

static bool add_would_overflow(uint64_t value, uint64_t addition) {
    return value > UINT64_MAX - addition;
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static uint32_t vertex_xy(const XgRenderIrVertex *vertex) {
    const uint32_t x = (uint32_t)vertex->x >> 16u;
    const uint32_t y = (uint32_t)vertex->y >> 16u;

    return (x & UINT32_C(0xffff)) | (y << 16u);
}

static uint16_t vertex_uv(const XgRenderIrVertex *vertex) {
    const uint16_t u = (uint8_t)((uint32_t)vertex->u >> 16u);
    const uint16_t v = (uint8_t)((uint32_t)vertex->v >> 16u);

    return (uint16_t)(u | (uint16_t)(v << 8u));
}

static uint32_t material_word(const XgWorldTerrainWaterRecord *record) {
    const XgRenderIrMaterialState *material = &record->primitive.material;
    const XgRenderIrVertex *vertex =
        &record->primitive.triangles[0].vertices[0];
    uint8_t command = 0x20u;

    if (material->shading == XG_RENDER_IR_SHADING_GOURAUD) command |= 0x10u;
    if (material->textured) command |= 0x04u;
    if (material->semi_transparent) command |= 0x02u;
    if (material->raw_texture) command |= 0x01u;
    return (uint32_t)vertex->r | ((uint32_t)vertex->g << 8u) |
        ((uint32_t)vertex->b << 16u) | ((uint32_t)command << 24u);
}

static bool build_record_is_valid(const XgWorldTerrainWaterRecord *record,
                                  uint32_t index) {
    return record->allocation_ordinal == index &&
        record->ordering_bucket <
            XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT &&
        record->primitive.triangle_count == 1u &&
        record->primitive.triangles[0].split_index == 0u &&
        record->primitive.triangles[0].split_count == 1u &&
        record->primitive.material.shading == XG_RENDER_IR_SHADING_FLAT &&
        record->primitive.material.textured &&
        !record->primitive.material.raw_texture &&
        !record->primitive.material.semi_transparent;
}

static void set_count_mismatch(
    XgWorldTerrainWaterShadowMismatch *mismatch, uint64_t invocation,
    uint32_t expected, uint32_t actual) {
    if (mismatch->kind != XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_NONE) return;
    mismatch->kind = XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_COUNT;
    mismatch->invocation = invocation;
    mismatch->field_bits = XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_COUNT;
    mismatch->record_index = UINT32_MAX;
    mismatch->source_primitive_index = UINT32_MAX;
    mismatch->address = XG_WORLD_TERRAIN_WATER_SHADOW_FINAL_COUNT_GLOBAL;
    mismatch->expected = expected;
    mismatch->actual = actual;
}

static void set_packet_mismatch(
    XgWorldTerrainWaterShadowMismatch *mismatch, uint64_t invocation,
    uint32_t field_bits, uint32_t index, uint32_t packet,
    uint32_t actual_tag, uint32_t expected_material,
    uint32_t actual_material, const uint32_t expected_xy[3],
    const uint32_t actual_xy[3], const uint16_t expected_uv[3],
    const uint16_t actual_uv[3], uint16_t actual_clut,
    uint16_t actual_tpage, uint16_t expected_uv2_high,
    uint16_t actual_uv2_high) {
    XgWorldTerrainWaterShadowMismatch *out = mismatch;
    const XgWorldTerrainWaterRecord *record = &terrain_shadow.records[index];

    if (out->kind != XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_NONE) return;
    out->kind = XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_PACKET;
    out->invocation = invocation;
    out->field_bits = field_bits;
    out->record_index = index;
    out->source_primitive_index = record->source_primitive_index;
    out->packet_address = packet;
    out->address = packet;
    out->expected_tag = terrain_shadow.expected_tags[index];
    out->actual_tag = actual_tag;
    out->expected_material = expected_material;
    out->actual_material = actual_material;
    memcpy(out->expected_xy, expected_xy, sizeof(out->expected_xy));
    memcpy(out->actual_xy, actual_xy, sizeof(out->actual_xy));
    memcpy(out->expected_uv, expected_uv, sizeof(out->expected_uv));
    memcpy(out->actual_uv, actual_uv, sizeof(out->actual_uv));
    out->expected_clut = record->encoded_clut;
    out->actual_clut = actual_clut;
    out->expected_tpage = record->encoded_tpage;
    out->actual_tpage = actual_tpage;
    out->expected_uv2_high = expected_uv2_high;
    out->actual_uv2_high = actual_uv2_high;
}

static void set_ot_mismatch(
    XgWorldTerrainWaterShadowMismatch *mismatch, uint64_t invocation,
    uint32_t bucket, uint32_t expected, uint32_t actual, bool touched) {
    if (mismatch->kind != XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_NONE) return;
    mismatch->kind = touched
        ? XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_TOUCHED_OT
        : XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_UNTOUCHED_OT;
    mismatch->invocation = invocation;
    mismatch->field_bits = touched
        ? XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TOUCHED_OT
        : XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNTOUCHED_OT;
    mismatch->record_index = UINT32_MAX;
    mismatch->source_primitive_index = UINT32_MAX;
    mismatch->address = terrain_shadow.ot_base + bucket * 4u;
    mismatch->expected = expected;
    mismatch->actual = actual;
}

void xg_world_terrain_water_shadow_reset(void) {
    memset(&terrain_shadow, 0, sizeof(terrain_shadow));
}

XgWorldTerrainWaterShadowResult xg_world_terrain_water_shadow_begin(
    CPUState *cpu, uint64_t authentication_generation,
    const GpuDrawState *draw_state, int32_t screen_x_cull_margin) {
    XgWorldTerrainWaterCaptureRequest request = { 0 };
    XgWorldTerrainWaterAuthenticatedReader reader = { 0 };
    XgWorldTerrainWaterCaptureResult capture_result;
    XgWorldTerrainWaterResult build_result;
    uint32_t context;
    uint32_t context_ot;
    uint32_t context_packets;
    uint32_t index;

    if (terrain_shadow.snapshot.blocked)
        return XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED;
    if (terrain_shadow.snapshot.pending)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_NESTED_BEGIN, 0u, 0u, 0u,
            0u, XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_STATE);
    if (terrain_shadow.snapshot.begin_count == UINT64_MAX)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_COUNTER_OVERFLOW, 0u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
    ++terrain_shadow.snapshot.begin_count;
    if (cpu == NULL || draw_state == NULL)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_ARGUMENT, 0u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    if (authentication_generation == 0u)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_AUTHENTICATION, 0u,
            0u, 1u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    if (cpu->read_word == NULL || cpu->read_half == NULL)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_CPU_ACCESS, 0u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    if (cpu->pc != XG_WORLD_TERRAIN_WATER_SHADOW_BEGIN_PC)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_ENTRY_PC, 0u,
            cpu->pc, XG_WORLD_TERRAIN_WATER_SHADOW_BEGIN_PC, cpu->pc,
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    if (terrain_shadow.snapshot.authentication_generation != 0u &&
        terrain_shadow.snapshot.authentication_generation !=
            authentication_generation)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_AUTHENTICATION, 1u,
            0u,
            (uint32_t)terrain_shadow.snapshot.authentication_generation,
            (uint32_t)authentication_generation,
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_STATE);
    if (!stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] < XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE ||
        !ram_range_is_valid(
            cpu->gpr[29] -
                XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE,
            XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE, 4u))
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_STACK, 0u,
            cpu->gpr[29], 0u, cpu->gpr[29],
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    if (cpu->gpr[6] != XG_WORLD_TERRAIN_WATER_SHADOW_POSITION_GLOBAL)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_CONTEXT, 1u, 0u,
            XG_WORLD_TERRAIN_WATER_SHADOW_POSITION_GLOBAL, cpu->gpr[6],
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);

    context = cpu->read_word(
        XG_WORLD_TERRAIN_WATER_SHADOW_CONTEXT_GLOBAL);
    if (!ram_range_is_valid(context, TERRAIN_CONTEXT_PACKET_OFFSET + 4u, 4u))
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_CONTEXT, 2u,
            XG_WORLD_TERRAIN_WATER_SHADOW_CONTEXT_GLOBAL, 0u, context,
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    context_ot = cpu->read_word(context + TERRAIN_CONTEXT_OT_OFFSET);
    context_packets = cpu->read_word(context + TERRAIN_CONTEXT_PACKET_OFFSET);
    if (cpu->gpr[4] != context_ot)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_CONTEXT, 3u,
            context, context_ot, cpu->gpr[4],
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    if (cpu->gpr[5] != context_packets)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_CONTEXT, 4u,
            context, context_packets, cpu->gpr[5],
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    if (!ram_range_is_valid(
            cpu->gpr[4],
            XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT * 4u, 4u) ||
        !ram_range_is_valid(cpu->gpr[5], 4u, 4u))
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_ADDRESS_RANGE, 0u,
            0u, 0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);

    terrain_shadow.snapshot.last_caller_return = cpu->gpr[31];
    terrain_shadow.snapshot.last_position_x = (int32_t)cpu->read_word(
        XG_WORLD_TERRAIN_WATER_SHADOW_POSITION_GLOBAL);
    terrain_shadow.snapshot.last_position_z = (int32_t)cpu->read_word(
        XG_WORLD_TERRAIN_WATER_SHADOW_POSITION_GLOBAL + 8u);
    terrain_shadow.snapshot.last_projection_distance =
        (uint16_t)cpu->gte_ctrl[26];

    request = (XgWorldTerrainWaterCaptureRequest){
        .authentication_generation = authentication_generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = screen_x_cull_margin,
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw_state->left,
            .draw_area_top = draw_state->top,
            .draw_area_right = draw_state->right,
            .draw_area_bottom = draw_state->bottom,
            .draw_offset_x = draw_state->offset_x,
            .draw_offset_y = draw_state->offset_y,
            .texture_window_mask_x = draw_state->texture_window_mask_x,
            .texture_window_mask_y = draw_state->texture_window_mask_y,
            .texture_window_offset_x = draw_state->texture_window_offset_x,
            .texture_window_offset_y = draw_state->texture_window_offset_y,
            .dither = draw_state->dither != 0u,
            .mask_set = draw_state->mask_set != 0u,
            .mask_check = draw_state->mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldTerrainWaterAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = authentication_generation,
        .authenticated = true,
    };
    capture_result = xg_world_terrain_water_source_capture(
        &request, &reader, &terrain_shadow.capture);
    terrain_shadow.snapshot.last_capture_result = (uint32_t)capture_result;
    if (capture_result != XG_WORLD_TERRAIN_WATER_CAPTURE_OK) {
        if (terrain_shadow.snapshot.source_capture_failure_count == UINT64_MAX)
            return block_shadow(
                XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_COUNTER_OVERFLOW, 1u,
                0u, 0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
        ++terrain_shadow.snapshot.source_capture_failure_count;
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_SOURCE_CAPTURE,
            (uint32_t)capture_result, 0u, XG_WORLD_TERRAIN_WATER_CAPTURE_OK,
            (uint32_t)capture_result,
            XG_WORLD_TERRAIN_WATER_SHADOW_CAPTURE_FAILED);
    }
    build_result = xg_world_terrain_water_build(
        &terrain_shadow.capture.source, terrain_shadow.records,
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &terrain_shadow.count);
    terrain_shadow.snapshot.last_build_result = (uint32_t)build_result;
    if (build_result != XG_WORLD_TERRAIN_WATER_OK) {
        if (terrain_shadow.snapshot.build_failure_count == UINT64_MAX)
            return block_shadow(
                XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_COUNTER_OVERFLOW, 2u,
                0u, 0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
        ++terrain_shadow.snapshot.build_failure_count;
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_BUILD,
            (uint32_t)build_result, 0u, XG_WORLD_TERRAIN_WATER_OK,
            (uint32_t)build_result,
            XG_WORLD_TERRAIN_WATER_SHADOW_BUILD_FAILED);
    }
    if (terrain_shadow.count > XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY ||
        !ram_range_is_valid(
            cpu->gpr[5],
            terrain_shadow.count == 0u
                ? 4u
                : terrain_shadow.count *
                    XG_WORLD_TERRAIN_WATER_SHADOW_PACKET_STRIDE,
            4u))
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_ADDRESS_RANGE, 1u,
            cpu->gpr[5], XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY,
            terrain_shadow.count,
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
    if (add_would_overflow(
            terrain_shadow.snapshot.source_read_count,
            terrain_shadow.capture.authenticated_read_count) ||
        add_would_overflow(
            terrain_shadow.snapshot.source_read_bytes,
            terrain_shadow.capture.authenticated_read_bytes) ||
        add_would_overflow(
            terrain_shadow.snapshot.captured_resource_count,
            terrain_shadow.capture.captured_resource_count) ||
        add_would_overflow(terrain_shadow.snapshot.candidate_count,
                           terrain_shadow.count))
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_COUNTER_OVERFLOW, 3u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);

    terrain_shadow.entry_stack_pointer = cpu->gpr[29];
    terrain_shadow.snapshot.authentication_generation =
        authentication_generation;
    terrain_shadow.ot_base = cpu->gpr[4];
    terrain_shadow.packet_base = cpu->gpr[5];
    terrain_shadow.snapshot.last_ot_base = terrain_shadow.ot_base;
    terrain_shadow.snapshot.last_packet_base = terrain_shadow.packet_base;
    terrain_shadow.snapshot.last_candidate_count = terrain_shadow.count;
    memset(terrain_shadow.ot_touched, 0, sizeof(terrain_shadow.ot_touched));
    for (index = 0u;
         index < XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT; ++index) {
        const uint32_t value = cpu->read_word(terrain_shadow.ot_base + index * 4u);

        terrain_shadow.initial_ot[index] = value;
        terrain_shadow.expected_ot[index] = value;
        terrain_shadow.last_record_by_bucket[index] = UINT32_MAX;
    }
    for (index = 0u; index < terrain_shadow.count; ++index) {
        const XgWorldTerrainWaterRecord *record =
            &terrain_shadow.records[index];
        uint32_t bucket;
        uint32_t packet;
        uint32_t predecessor;

        if (!build_record_is_valid(record, index))
            return block_shadow(
                XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_BUILD_OUTPUT,
                1u, 0u, index, record->allocation_ordinal,
                XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
        bucket = record->ordering_bucket;
        predecessor = terrain_shadow.last_record_by_bucket[bucket];
        if ((predecessor == UINT32_MAX) !=
                record->ordering_predecessor_is_external ||
            (predecessor != UINT32_MAX &&
             record->ordering_predecessor_record != predecessor))
            return block_shadow(
                XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_BUILD_OUTPUT,
                2u, 0u, predecessor,
                record->ordering_predecessor_record,
                XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
        packet = terrain_shadow.packet_base +
            index * XG_WORLD_TERRAIN_WATER_SHADOW_PACKET_STRIDE;
        terrain_shadow.expected_tags[index] =
            terrain_shadow.expected_ot[bucket] | UINT32_C(0x07000000);
        terrain_shadow.initial_material[index] = cpu->read_word(packet + 4u);
        terrain_shadow.initial_uv2_high[index] =
            (uint16_t)(cpu->read_word(packet + 28u) >> 16u);
        terrain_shadow.expected_ot[bucket] = packet & UINT32_C(0x00ffffff);
        terrain_shadow.last_record_by_bucket[bucket] = index;
        terrain_shadow.ot_touched[bucket] = true;
    }
    terrain_shadow.snapshot.source_read_count +=
        terrain_shadow.capture.authenticated_read_count;
    terrain_shadow.snapshot.source_read_bytes +=
        terrain_shadow.capture.authenticated_read_bytes;
    terrain_shadow.snapshot.captured_resource_count +=
        terrain_shadow.capture.captured_resource_count;
    terrain_shadow.snapshot.candidate_count += terrain_shadow.count;
    terrain_shadow.snapshot.pending = true;
    return XG_WORLD_TERRAIN_WATER_SHADOW_OK;
}

static bool finish_counters_would_overflow(const TerrainFinishStats *stats) {
#define CHECK_COUNTER(member)                                                   \
    if (add_would_overflow(terrain_shadow.snapshot.member, stats->member))      \
        return true
    CHECK_COUNTER(invocation_match_count);
    CHECK_COUNTER(invocation_mismatch_count);
    CHECK_COUNTER(original_primitive_count);
    CHECK_COUNTER(packet_compared_count);
    CHECK_COUNTER(packet_match_count);
    CHECK_COUNTER(packet_mismatch_count);
    CHECK_COUNTER(missing_primitive_count);
    CHECK_COUNTER(unexpected_primitive_count);
    CHECK_COUNTER(count_mismatch_count);
    CHECK_COUNTER(payload_mismatch_count);
    CHECK_COUNTER(material_mismatch_count);
    CHECK_COUNTER(geometry_mismatch_count);
    CHECK_COUNTER(geometry_vertex_mismatch_count);
    CHECK_COUNTER(uv_mismatch_count);
    CHECK_COUNTER(clut_mismatch_count);
    CHECK_COUNTER(tpage_mismatch_count);
    CHECK_COUNTER(tag_mismatch_count);
    CHECK_COUNTER(packet_unchanged_mismatch_count);
    CHECK_COUNTER(touched_ot_bucket_count);
    CHECK_COUNTER(untouched_ot_bucket_count);
    CHECK_COUNTER(touched_ot_mismatch_count);
    CHECK_COUNTER(untouched_ot_mismatch_count);
#undef CHECK_COUNTER
    return false;
}

static void add_finish_counters(const TerrainFinishStats *stats) {
#define ADD_COUNTER(member) terrain_shadow.snapshot.member += stats->member
    ADD_COUNTER(invocation_match_count);
    ADD_COUNTER(invocation_mismatch_count);
    ADD_COUNTER(original_primitive_count);
    ADD_COUNTER(packet_compared_count);
    ADD_COUNTER(packet_match_count);
    ADD_COUNTER(packet_mismatch_count);
    ADD_COUNTER(missing_primitive_count);
    ADD_COUNTER(unexpected_primitive_count);
    ADD_COUNTER(count_mismatch_count);
    ADD_COUNTER(payload_mismatch_count);
    ADD_COUNTER(material_mismatch_count);
    ADD_COUNTER(geometry_mismatch_count);
    ADD_COUNTER(geometry_vertex_mismatch_count);
    ADD_COUNTER(uv_mismatch_count);
    ADD_COUNTER(clut_mismatch_count);
    ADD_COUNTER(tpage_mismatch_count);
    ADD_COUNTER(tag_mismatch_count);
    ADD_COUNTER(packet_unchanged_mismatch_count);
    ADD_COUNTER(touched_ot_bucket_count);
    ADD_COUNTER(untouched_ot_bucket_count);
    ADD_COUNTER(touched_ot_mismatch_count);
    ADD_COUNTER(untouched_ot_mismatch_count);
#undef ADD_COUNTER
}

XgWorldTerrainWaterShadowResult xg_world_terrain_water_shadow_finish(
    CPUState *cpu) {
    TerrainFinishStats stats = { 0 };
    XgWorldTerrainWaterShadowMismatch first = { 0 };
    uint64_t invocation;
    uint32_t original_count;
    uint32_t compare_count;
    uint32_t index;
    bool invocation_matches = true;

    if (terrain_shadow.snapshot.blocked)
        return XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED;
    if (!terrain_shadow.snapshot.pending)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_UNMATCHED_FINISH, 0u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_STATE);
    if (cpu == NULL || cpu->read_word == NULL)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_CPU_ACCESS, 1u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT);
    if (cpu->pc != XG_WORLD_TERRAIN_WATER_SHADOW_FINISH_PC)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_FINISH_PC, 0u,
            cpu->pc, XG_WORLD_TERRAIN_WATER_SHADOW_FINISH_PC, cpu->pc,
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_STATE);
    if (terrain_shadow.entry_stack_pointer <
            XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE ||
        cpu->gpr[29] != terrain_shadow.entry_stack_pointer -
            XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE ||
        !ram_range_is_valid(
            cpu->gpr[29],
            XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE, 4u))
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_FINISH_STACK, 0u,
            cpu->gpr[29], terrain_shadow.entry_stack_pointer -
                XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE,
            cpu->gpr[29], XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_STATE);
    if (!physical_address_equals(
            cpu->read_word(
                cpu->gpr[29] +
                XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE - 4u),
            terrain_shadow.snapshot.last_caller_return))
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_FINISH_STACK, 1u,
            cpu->gpr[29] +
                XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE - 4u,
            terrain_shadow.snapshot.last_caller_return,
            cpu->read_word(
                cpu->gpr[29] +
                XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE - 4u),
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_STATE);
    if (terrain_shadow.snapshot.completion_count == UINT64_MAX)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_COUNTER_OVERFLOW, 4u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
    invocation = terrain_shadow.snapshot.completion_count + 1u;
    original_count = cpu->read_word(
        XG_WORLD_TERRAIN_WATER_SHADOW_FINAL_COUNT_GLOBAL);
    terrain_shadow.snapshot.last_original_count = original_count;
    if (original_count > TERRAIN_ORIGINAL_PACKET_LIMIT)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_ORIGINAL_COUNT, 0u,
            XG_WORLD_TERRAIN_WATER_SHADOW_FINAL_COUNT_GLOBAL,
            TERRAIN_ORIGINAL_PACKET_LIMIT, original_count,
            XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_STATE);

    stats.original_primitive_count = original_count;
    compare_count = original_count < terrain_shadow.count
        ? original_count : terrain_shadow.count;
    stats.packet_compared_count = compare_count;
    if (original_count != terrain_shadow.count) {
        const uint32_t difference = original_count > terrain_shadow.count
            ? original_count - terrain_shadow.count
            : terrain_shadow.count - original_count;

        invocation_matches = false;
        stats.count_mismatch_count = 1u;
        stats.packet_mismatch_count += difference;
        if (original_count > terrain_shadow.count)
            stats.unexpected_primitive_count = difference;
        else
            stats.missing_primitive_count = difference;
        set_count_mismatch(&first, invocation, terrain_shadow.count,
                           original_count);
    }

    for (index = 0u; index < compare_count; ++index) {
        const XgWorldTerrainWaterRecord *record =
            &terrain_shadow.records[index];
        const XgRenderIrTriangle *triangle =
            &record->primitive.triangles[0];
        const uint32_t packet = terrain_shadow.packet_base +
            index * XG_WORLD_TERRAIN_WATER_SHADOW_PACKET_STRIDE;
        uint32_t expected_xy[3];
        uint32_t actual_xy[3];
        uint16_t expected_uv[3];
        uint16_t actual_uv[3];
        uint32_t actual_tag;
        uint32_t expected_material;
        uint32_t actual_material;
        uint32_t uv_clut;
        uint32_t uv_tpage;
        uint32_t uv2_word;
        uint16_t actual_clut;
        uint16_t actual_tpage;
        uint16_t actual_uv2_high;
        uint32_t field_bits = 0u;
        uint32_t vertex;

        actual_tag = cpu->read_word(packet);
        expected_material = material_word(record);
        actual_material = cpu->read_word(packet + 4u);
        uv_clut = cpu->read_word(packet + 12u);
        uv_tpage = cpu->read_word(packet + 20u);
        uv2_word = cpu->read_word(packet + 28u);
        actual_clut = (uint16_t)(uv_clut >> 16u);
        actual_tpage = (uint16_t)(uv_tpage >> 16u);
        actual_uv2_high = (uint16_t)(uv2_word >> 16u);
        for (vertex = 0u; vertex < 3u; ++vertex) {
            expected_xy[vertex] = vertex_xy(&triangle->vertices[vertex]);
            actual_xy[vertex] = cpu->read_word(packet + 8u + vertex * 8u);
            expected_uv[vertex] = vertex_uv(&triangle->vertices[vertex]);
        }
        actual_uv[0] = (uint16_t)uv_clut;
        actual_uv[1] = (uint16_t)uv_tpage;
        actual_uv[2] = (uint16_t)uv2_word;
        if (actual_tag != terrain_shadow.expected_tags[index])
            field_bits |= XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TAG;
        if (actual_material != expected_material)
            field_bits |= XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_MATERIAL;
        if (actual_material != terrain_shadow.initial_material[index])
            field_bits |=
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNCHANGED_MATERIAL;
        if (actual_clut != record->encoded_clut)
            field_bits |= XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_CLUT;
        if (actual_tpage != record->encoded_tpage)
            field_bits |= XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TPAGE;
        if (actual_uv2_high != terrain_shadow.initial_uv2_high[index])
            field_bits |=
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNCHANGED_UV2_HIGH;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t xy_bit =
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_XY0 << (vertex * 3u);
            const uint32_t uv_bit =
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV0 << (vertex * 3u);

            if (actual_xy[vertex] != expected_xy[vertex])
                field_bits |= xy_bit;
            if (actual_uv[vertex] != expected_uv[vertex])
                field_bits |= uv_bit;
        }
        if ((field_bits & (
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_MATERIAL |
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV0 |
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV1 |
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV2 |
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_CLUT |
                XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TPAGE)) != 0u)
            ++stats.payload_mismatch_count;
        if ((field_bits & XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_MATERIAL) != 0u)
            ++stats.material_mismatch_count;
        if ((field_bits & (XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_XY0 |
                           XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_XY1 |
                           XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_XY2)) != 0u)
            ++stats.geometry_mismatch_count;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            if ((field_bits &
                 (XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_XY0 <<
                  (vertex * 3u))) != 0u)
                ++stats.geometry_vertex_mismatch_count;
            if ((field_bits &
                 (XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV0 <<
                  (vertex * 3u))) != 0u)
                ++stats.uv_mismatch_count;
        }
        if ((field_bits & XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_CLUT) != 0u)
            ++stats.clut_mismatch_count;
        if ((field_bits & XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TPAGE) != 0u)
            ++stats.tpage_mismatch_count;
        if ((field_bits & XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TAG) != 0u)
            ++stats.tag_mismatch_count;
        if ((field_bits &
             (XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNCHANGED_MATERIAL |
              XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNCHANGED_UV2_HIGH)) != 0u)
            ++stats.packet_unchanged_mismatch_count;
        if (field_bits == 0u) {
            ++stats.packet_match_count;
        } else {
            ++stats.packet_mismatch_count;
            invocation_matches = false;
            set_packet_mismatch(
                &first, invocation, field_bits, index, packet, actual_tag,
                expected_material, actual_material, expected_xy, actual_xy,
                expected_uv, actual_uv, actual_clut, actual_tpage,
                terrain_shadow.initial_uv2_high[index], actual_uv2_high);
        }
    }

    for (index = 0u;
         index < XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT; ++index) {
        const bool touched = terrain_shadow.ot_touched[index];
        const uint32_t expected = touched
            ? terrain_shadow.expected_ot[index]
            : terrain_shadow.initial_ot[index];
        const uint32_t actual =
            cpu->read_word(terrain_shadow.ot_base + index * 4u);

        if (touched)
            ++stats.touched_ot_bucket_count;
        else
            ++stats.untouched_ot_bucket_count;
        if (actual == expected) continue;
        invocation_matches = false;
        if (touched)
            ++stats.touched_ot_mismatch_count;
        else
            ++stats.untouched_ot_mismatch_count;
        set_ot_mismatch(&first, invocation, index, expected, actual, touched);
    }
    if (invocation_matches)
        stats.invocation_match_count = 1u;
    else
        stats.invocation_mismatch_count = 1u;
    if (finish_counters_would_overflow(&stats))
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_COUNTER_OVERFLOW, 5u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
    ++terrain_shadow.snapshot.completion_count;
    add_finish_counters(&stats);
    if (terrain_shadow.snapshot.first_mismatch.kind ==
            XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_NONE &&
        first.kind != XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_NONE)
        terrain_shadow.snapshot.first_mismatch = first;
    clear_pending();
    return XG_WORLD_TERRAIN_WATER_SHADOW_OK;
}

XgWorldTerrainWaterShadowResult
xg_world_terrain_water_shadow_lifecycle_invalidate(void) {
    if (terrain_shadow.snapshot.blocked)
        return XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED;
    if (terrain_shadow.snapshot.lifecycle_invalidation_count == UINT64_MAX)
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_COUNTER_OVERFLOW, 6u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
    ++terrain_shadow.snapshot.lifecycle_invalidation_count;
    if (terrain_shadow.snapshot.pending) {
        if (terrain_shadow.snapshot.pending_block_count != UINT64_MAX)
            ++terrain_shadow.snapshot.pending_block_count;
        return block_shadow(
            XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_LIFECYCLE_PENDING, 0u, 0u,
            0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
    }
    clear_pending();
    terrain_shadow.snapshot.authentication_generation = 0u;
    return XG_WORLD_TERRAIN_WATER_SHADOW_OK;
}

XgWorldTerrainWaterShadowResult
xg_world_terrain_water_shadow_block_pending(void) {
    if (terrain_shadow.snapshot.blocked)
        return XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED;
    if (!terrain_shadow.snapshot.pending)
        return XG_WORLD_TERRAIN_WATER_SHADOW_OK;
    if (terrain_shadow.snapshot.pending_block_count != UINT64_MAX)
        ++terrain_shadow.snapshot.pending_block_count;
    return block_shadow(
        XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_LIFECYCLE_PENDING, 1u, 0u,
        0u, 0u, XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
}

bool xg_world_terrain_water_shadow_record_native_cutover(
    uint32_t primitive_count) {
    XgWorldTerrainWaterShadowSnapshot *snapshot = &terrain_shadow.snapshot;

    if (snapshot->blocked || snapshot->pending ||
        snapshot->native_cutover_count == UINT64_MAX ||
        snapshot->native_primitive_count > UINT64_MAX - primitive_count)
        return false;
    ++snapshot->native_cutover_count;
    snapshot->native_primitive_count += primitive_count;
    return true;
}

XgWorldTerrainWaterShadowResult xg_world_terrain_water_shadow_snapshot(
    XgWorldTerrainWaterShadowSnapshot *out_snapshot) {
    if (out_snapshot == NULL)
        return XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT;
    *out_snapshot = terrain_shadow.snapshot;
    return XG_WORLD_TERRAIN_WATER_SHADOW_OK;
}
