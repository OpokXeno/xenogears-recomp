#include "xg_world_clouds_shadow.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 0;                                                           \
        }                                                                       \
    } while (0)

enum {
    RAM_SIZE = 0x200000u,
    SCRATCH_SIZE = 0x400u,
    POSITION_BASE = 0x800e0000u,
    VELOCITY_BASE = 0x800e0500u,
    CONTEXT_BASE = 0x800d0000u,
    PACKET_BASE = 0x800f0000u,
    OT_BASE = 0x800f4000u,
    ENTRY_SP = 0x801ff000u,
    FULL_RA = 0x80071b68u,
};

static uint8_t ram[RAM_SIZE];
static uint8_t scratch[SCRATCH_SIZE];
static uint32_t guest_write_count;
static XgWorldCloudRecord materialized_records[
    XG_WORLD_CLOUD_PACKET_CAPACITY];
static uint32_t materialized_count;
static XgWorldCloudsBuildStats materialized_stats;

static uint8_t *memory_pointer(uint32_t address, uint32_t size) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    if (physical <= RAM_SIZE && size <= RAM_SIZE - physical)
        return &ram[physical];
    if (physical >= UINT32_C(0x1f800000) &&
        physical - UINT32_C(0x1f800000) <= SCRATCH_SIZE &&
        size <= SCRATCH_SIZE - (physical - UINT32_C(0x1f800000)))
        return &scratch[physical - UINT32_C(0x1f800000)];
    return NULL;
}

static uint32_t load_word(uint32_t address) {
    uint32_t value = 0u;
    uint8_t *source = memory_pointer(address, sizeof(value));

    if (source != NULL) memcpy(&value, source, sizeof(value));
    return value;
}

static uint16_t load_half(uint32_t address) {
    uint16_t value = 0u;
    uint8_t *source = memory_pointer(address, sizeof(value));

    if (source != NULL) memcpy(&value, source, sizeof(value));
    return value;
}

static uint8_t load_byte(uint32_t address) {
    uint8_t *source = memory_pointer(address, 1u);

    return source != NULL ? *source : 0u;
}

static void store_word(uint32_t address, uint32_t value) {
    uint8_t *destination = memory_pointer(address, sizeof(value));

    if (destination != NULL) memcpy(destination, &value, sizeof(value));
}

static void store_half(uint32_t address, uint16_t value) {
    uint8_t *destination = memory_pointer(address, sizeof(value));

    if (destination != NULL) memcpy(destination, &value, sizeof(value));
}

static void guest_write_word(uint32_t address, uint32_t value) {
    ++guest_write_count;
    store_word(address, value);
}

static void guest_write_half(uint32_t address, uint16_t value) {
    ++guest_write_count;
    store_half(address, value);
}

static void guest_write_byte(uint32_t address, uint8_t value) {
    uint8_t *destination;

    ++guest_write_count;
    destination = memory_pointer(address, 1u);
    if (destination != NULL) *destination = value;
}

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static XgHost3dVector expected_far_vertex(uint32_t index) {
    const uint32_t layer = index / XG_HOST_3D_VERTEX_COUNT;
    const uint32_t vertex = index % XG_HOST_3D_VERTEX_COUNT;
    XgHost3dVector value = { 0 };

    value.x = (vertex & 1u) != 0u ? 192 : -192;
    value.y = (int16_t)(-(int32_t)layer * 8);
    value.z = vertex >= 2u ? -192 : 192;
    return value;
}

static XgHost3dVector expected_middle_vertex(uint32_t index) {
    const uint32_t layer = index / 16u;
    const uint32_t quad = (index % 16u) / XG_HOST_3D_VERTEX_COUNT;
    const uint32_t vertex = index % XG_HOST_3D_VERTEX_COUNT;
    XgHost3dVector value = { 0 };

    value.x = (int16_t)(-192 + (int32_t)(quad & 1u) * 192 +
                        (int32_t)(vertex & 1u) * 192);
    value.y = (int16_t)(-(int32_t)layer * 8);
    value.z = (int16_t)(192 - (int32_t)(quad >> 1u) * 192 -
                        (vertex >= 2u ? 192 : 0));
    return value;
}

static uint32_t matrix_word(bool camera, uint32_t index) {
    static const uint32_t identity[8] = {
        0x00001000u, 0x00000000u, 0x00001000u, 0x00000000u,
        0x00001000u, 0u, 0u, 0u,
    };

    if (!camera) return identity[index];
    if (index == 4u) return pack_s16(-4096, 0);
    if (index == 7u) return 1100u;
    return identity[index];
}

static void initialize_source(void) {
    static const uint16_t uv[8] = {
        0x0000u, 0x0040u, 0x0080u, 0x0000u,
        0x00c0u, 0x4000u, 0x4040u, 0x0000u,
    };
    uint32_t index;

    store_word(UINT32_C(0x8009d150), POSITION_BASE);
    store_word(UINT32_C(0x8009ceb4), VELOCITY_BASE);
    for (index = 0u; index < 8u; ++index) {
        store_word(UINT32_C(0x8009c808) + index * 4u,
                   matrix_word(true, index));
        store_word(UINT32_C(0x8009a180) + index * 4u,
                   matrix_word(false, index));
    }
    for (index = 0u;
         index < XG_WORLD_CLOUD_FAR_QUAD_COUNT * XG_HOST_3D_VERTEX_COUNT;
         ++index) {
        const XgHost3dVector value = expected_far_vertex(index);
        const uint32_t address = UINT32_C(0x8009ad50) + index * 8u;

        store_word(address, pack_s16(value.x, value.y));
        store_word(address + 4u, pack_s16(value.z, (int16_t)value.pad));
    }
    for (index = 0u;
         index < XG_WORLD_CLOUD_MIDDLE_QUAD_COUNT * XG_HOST_3D_VERTEX_COUNT;
         ++index) {
        const XgHost3dVector value = expected_middle_vertex(index);
        const uint32_t address = UINT32_C(0x8009adb0) + index * 8u;

        store_word(address, pack_s16(value.x, value.y));
        store_word(address + 4u, pack_s16(value.z, (int16_t)value.pad));
    }
    for (index = 0u; index < 4u; ++index) {
        store_word(UINT32_C(0x8009ad40) + index * 4u,
                   uv[index * 2u] |
                       ((uint32_t)uv[index * 2u + 1u] << 16u));
    }

    store_word(UINT32_C(0x8009be28), 0u);
    store_word(UINT32_C(0x8009be30), 0u);
    store_word(UINT32_C(0x8009d7cc), 0u);
    store_half(UINT32_C(0x8009bd38), (uint16_t)-0x400);
    store_half(UINT32_C(0x8009bd3a), 0u);
    store_half(UINT32_C(0x8009bd40), 0u);
    store_half(UINT32_C(0x8009bd44), 1024u);
    store_word(UINT32_C(0x800523f0), pack_s16(0, 4096));
    store_word(UINT32_C(0x800523f0) +
                   (((uint32_t)-0x169 & 0xfffu) * 4u),
               pack_s16(-2048, 4096));
    store_word(UINT32_C(0x800523f0) + 0x169u * 4u,
               pack_s16(2048, 4096));

    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        const uint32_t position = POSITION_BASE + index * 0x10u;
        const uint32_t velocity = VELOCITY_BASE + index * 8u;

        store_word(position, index == 0u ? 0u : 4095u << 12u);
        store_word(position + 4u, index * 17u);
        store_word(position + 8u, (uint32_t)(-(100 << 12)));
        store_half(velocity, (uint16_t)(int16_t)(1 + index % 5u));
        store_half(velocity + 2u, 0u);
        store_half(velocity + 4u,
                   (uint16_t)(int16_t)(-1 - (int32_t)(index % 3u)));
    }
}

static void initialize_outputs(void) {
    uint32_t index;

    store_word(UINT32_C(0x8009d7f0), 0u);
    store_word(UINT32_C(0x8009d7f8), PACKET_BASE);
    store_word(UINT32_C(0x8009d7fc), UINT32_C(0x800f8000));
    store_word(UINT32_C(0x8009be3c), CONTEXT_BASE);
    store_word(CONTEXT_BASE + 0x70u, OT_BASE);
    for (index = 0u; index < XG_WORLD_CLOUD_PACKET_CAPACITY; ++index) {
        const uint32_t packet = PACKET_BASE + index * 0x28u;

        store_word(packet, UINT32_C(0x09000000));
        store_word(packet + 4u, UINT32_C(0x2e262626));
        store_word(packet + 8u, UINT32_C(0x11110000) + index);
        store_word(packet + 12u, UINT32_C(0x7f930000));
        store_word(packet + 16u, UINT32_C(0x22220000) + index);
        store_word(packet + 20u, UINT32_C(0x003f0000));
        store_word(packet + 24u, UINT32_C(0x33330000) + index);
        store_word(packet + 28u, 0u);
        store_word(packet + 32u, UINT32_C(0x44440000) + index);
        store_word(packet + 36u, 0u);
    }
    for (index = 0u; index < XG_WORLD_CLOUD_OT_BUCKET_COUNT; ++index)
        store_word(OT_BASE + index * 4u, UINT32_C(0x00100000) + index);
}

static GpuDrawState default_draw_state(void) {
    GpuDrawState draw = { 0 };

    draw.right = 319u;
    draw.bottom = 239u;
    draw.texture_window_mask_x = 1u;
    draw.texture_window_mask_y = 2u;
    draw.texture_window_offset_x = 3u;
    draw.texture_window_offset_y = 4u;
    return draw;
}

static void setup_fixture(CPUState *cpu, uint32_t callback_pointer) {
    memset(ram, 0, sizeof(ram));
    memset(scratch, 0, sizeof(scratch));
    memset(cpu, 0, sizeof(*cpu));
    memset(materialized_records, 0, sizeof(materialized_records));
    memset(&materialized_stats, 0, sizeof(materialized_stats));
    materialized_count = 0u;
    guest_write_count = 0u;

    initialize_source();
    initialize_outputs();
    store_word(UINT32_C(0x8009cd40), callback_pointer);
    cpu->gpr[29] = ENTRY_SP;
    cpu->gpr[31] = FULL_RA;
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 120u << 16u;
    cpu->gte_ctrl[26] = 256u;
    cpu->read_word = load_word;
    cpu->read_half = load_half;
    cpu->read_byte = load_byte;
    cpu->write_word = guest_write_word;
    cpu->write_half = guest_write_half;
    cpu->write_byte = guest_write_byte;
}

static bool capture_read_u16(void *context, uint32_t address,
                             uint16_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || out_value == NULL) return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool capture_read_u32(void *context, uint32_t address,
                             uint32_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || out_value == NULL) return false;
    *out_value = cpu->read_word(address);
    return true;
}

static int materialize_guest_output(CPUState *cpu, GpuDrawState *draw) {
    XgWorldCloudsCapture capture;
    XgWorldCloudPosition positions[XG_WORLD_CLOUD_COUNT];
    const XgWorldCloudsCaptureRequest request = {
        .authentication_generation = 7u,
        .caller_return = FULL_RA,
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw->left,
            .draw_area_top = draw->top,
            .draw_area_right = draw->right,
            .draw_area_bottom = draw->bottom,
            .draw_offset_x = draw->offset_x,
            .draw_offset_y = draw->offset_y,
            .texture_window_mask_x = draw->texture_window_mask_x,
            .texture_window_mask_y = draw->texture_window_mask_y,
            .texture_window_offset_x = draw->texture_window_offset_x,
            .texture_window_offset_y = draw->texture_window_offset_y,
            .dither = draw->dither != 0u,
            .mask_set = draw->mask_set != 0u,
            .mask_check = draw->mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    const XgWorldCloudsAuthenticatedReader reader = {
        .context = cpu,
        .read_u16 = capture_read_u16,
        .read_u32 = capture_read_u32,
        .authentication_generation = 7u,
        .authenticated = true,
    };
    uint32_t index;

    CHECK(xg_world_clouds_source_capture(&request, &reader, &capture) ==
          XG_WORLD_CLOUDS_CAPTURE_OK);
    CHECK(xg_world_clouds_build(
              &capture.source, materialized_records,
              XG_WORLD_CLOUD_PACKET_CAPACITY, &materialized_count,
              &materialized_stats) == XG_WORLD_CLOUDS_OK);
    memcpy(positions, capture.source.positions, sizeof(positions));
    CHECK(xg_world_clouds_step_positions(positions,
                                         capture.source.velocities) ==
          XG_WORLD_CLOUDS_OK);
    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        const uint32_t address = POSITION_BASE + index * 0x10u;

        store_word(address, (uint32_t)positions[index].x);
        store_word(address + 4u, (uint32_t)positions[index].y);
        store_word(address + 8u, (uint32_t)positions[index].z);
    }

    for (index = 0u; index < materialized_count; ++index) {
        const XgWorldCloudRecord *record = &materialized_records[index];
        const uint32_t packet = PACKET_BASE + index * 0x28u;
        const uint32_t ot = OT_BASE + record->ordering_bucket * 4u;
        const uint32_t prior = load_word(ot);
        uint32_t vertex;

        store_word(ot, packet & UINT32_C(0x00ffffff));
        store_word(packet, prior | UINT32_C(0x09000000));
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            store_word(packet + 8u + vertex * 8u,
                       (uint16_t)record->vertices[vertex].x |
                           ((uint32_t)(uint16_t)record->vertices[vertex].y <<
                            16u));
            store_half(packet + 12u + vertex * 8u, record->uv[vertex]);
        }
    }
    store_word(UINT32_C(0x1f8002f0), materialized_count);
    store_word(UINT32_C(0x1f8002f4),
               materialized_stats.quad_attempt_count -
                   materialized_stats.far_preinsert_depth_stops -
                   materialized_stats.far_postinsert_depth_stops);
    cpu->gpr[11] = PACKET_BASE + materialized_count * 0x28u;
    cpu->gpr[29] = ENTRY_SP - 0x50u;
    store_word(cpu->gpr[29] + 0x4cu, FULL_RA);
    return 1;
}

static int test_complete_match_is_read_only(void) {
    CPUState cpu;
    GpuDrawState draw = default_draw_state();
    XgWorldCloudsShadowSnapshot snapshot;
    const uint32_t uv2_padding = UINT32_C(0x5a5a0000);
    const uint32_t uv3_padding = UINT32_C(0xa5a50000);

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0xa0086700));
    store_word(PACKET_BASE + 28u, uv2_padding);
    store_word(PACKET_BASE + 36u, uv3_padding);
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.pending && !snapshot.blocked);
    CHECK(snapshot.begin_attempt_count == 1u && snapshot.begin_count == 1u);
    CHECK(snapshot.active_generation == 7u);
    CHECK(snapshot.last_callback_pointer == UINT32_C(0xa0086700));
    CHECK(snapshot.source_read_count ==
          XG_WORLD_CLOUDS_MAX_AUTHENTICATED_READS);
    CHECK(snapshot.source_read_bytes == 2052u);
    CHECK(snapshot.last_candidate_count == XG_WORLD_CLOUD_NEAR_QUAD_COUNT);
    CHECK(snapshot.expected_scratch_attempts ==
          XG_WORLD_CLOUD_NEAR_QUAD_COUNT);
    CHECK(guest_write_count == 0u);

    CHECK(materialize_guest_output(&cpu, &draw));
    CHECK(xg_world_clouds_shadow_finish(&cpu) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.phase == XG_WORLD_CLOUDS_SHADOW_IDLE);
    CHECK(!snapshot.pending && !snapshot.blocked);
    CHECK(snapshot.completion_count == 1u);
    CHECK(snapshot.invocation_match_count == 1u);
    CHECK(snapshot.invocation_mismatch_count == 0u);
    CHECK(snapshot.candidate_count == XG_WORLD_CLOUD_NEAR_QUAD_COUNT);
    CHECK(snapshot.primitive_count == XG_WORLD_CLOUD_NEAR_QUAD_COUNT);
    CHECK(snapshot.packet_match_count == XG_WORLD_CLOUD_NEAR_QUAD_COUNT);
    CHECK(snapshot.packet_mismatch_count == 0u);
    CHECK(snapshot.ot_mismatch_count == 0u);
    CHECK(snapshot.position_mismatch_count == 0u);
    CHECK(snapshot.actual_final_cursor == snapshot.expected_final_cursor);
    CHECK(snapshot.actual_scratch_emitted ==
          snapshot.expected_scratch_emitted);
    CHECK(snapshot.actual_scratch_attempts ==
          snapshot.expected_scratch_attempts);
    CHECK((load_word(PACKET_BASE + 28u) & UINT32_C(0xffff0000)) ==
          uv2_padding);
    CHECK((load_word(PACKET_BASE + 36u) & UINT32_C(0xffff0000)) ==
          uv3_padding);
    CHECK(guest_write_count == 0u);
    return 1;
}

static int test_exact_mismatch_reporting(void) {
    CPUState cpu;
    GpuDrawState draw = default_draw_state();
    XgWorldCloudsShadowSnapshot snapshot;
    uint32_t packet;
    uint32_t ot;

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    CHECK(materialize_guest_output(&cpu, &draw));
    CHECK(materialized_count == XG_WORLD_CLOUD_NEAR_QUAD_COUNT);

    packet = PACKET_BASE;
    store_word(packet, load_word(packet) ^ 1u);
    store_word(packet + 4u, load_word(packet + 4u) ^ 1u);
    store_word(packet + 8u, load_word(packet + 8u) ^ 1u);
    store_word(PACKET_BASE + materialized_count * 0x28u + 4u,
               UINT32_C(0xdeadbeef));
    ot = OT_BASE + materialized_records[0].ordering_bucket * 4u;
    store_word(ot, load_word(ot) ^ 1u);
    store_word(POSITION_BASE + 79u * 0x10u,
               load_word(POSITION_BASE + 79u * 0x10u) + 1u);
    store_word(UINT32_C(0x1f8002f0),
               load_word(UINT32_C(0x1f8002f0)) + 1u);
    store_word(UINT32_C(0x1f8002f4),
               load_word(UINT32_C(0x1f8002f4)) + 1u);
    cpu.gpr[11] -= 0x28u;

    CHECK(xg_world_clouds_shadow_finish(&cpu) ==
          XG_WORLD_CLOUDS_SHADOW_MISMATCH);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.completion_count == 1u);
    CHECK(snapshot.invocation_mismatch_count == 1u);
    CHECK(snapshot.packet_mismatch_count == 1u);
    CHECK(snapshot.tag_mismatch_count == 1u);
    CHECK(snapshot.payload_mismatch_count == 1u);
    CHECK(snapshot.geometry_mismatch_count == 1u);
    CHECK(snapshot.unexpected_packet_write_count == 1u);
    CHECK(snapshot.ot_mismatch_count == 1u);
    CHECK(snapshot.cursor_mismatch_count == 1u);
    CHECK(snapshot.position_mismatch_count == 1u);
    CHECK(snapshot.scratch_emitted_mismatch_count == 1u);
    CHECK(snapshot.scratch_attempt_mismatch_count == 1u);
    CHECK(snapshot.has_first_packet_mismatch);
    CHECK(snapshot.has_first_ot_mismatch);
    CHECK(snapshot.has_first_position_mismatch);
    CHECK(snapshot.first_packet_mismatch.packet_address == PACKET_BASE);
    CHECK((snapshot.first_packet_mismatch.word_mismatch_mask & 7u) == 7u);
    CHECK(snapshot.first_ot_mismatch.address == ot);
    CHECK(snapshot.first_position_mismatch.position_index == 79u);
    CHECK(snapshot.first_position_mismatch.component_mask == 1u);
    CHECK(!snapshot.pending && !snapshot.blocked);
    CHECK(guest_write_count == 0u);
    return 1;
}

static int test_callback_rejection_blocks_without_capture(void) {
    CPUState cpu;
    GpuDrawState draw = default_draw_state();
    XgWorldCloudsShadowSnapshot snapshot;

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086704));
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_BLOCKED);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocked && !snapshot.pending);
    CHECK(snapshot.blocker ==
          XG_WORLD_CLOUDS_SHADOW_BLOCK_CALLBACK_POINTER);
    CHECK(snapshot.callback_rejection_count == 1u);
    CHECK(snapshot.begin_count == 0u);
    CHECK(snapshot.source_read_count == 0u);
    CHECK(guest_write_count == 0u);
    return 1;
}

static int test_far_stop_counter_contract(void) {
    CPUState cpu;
    GpuDrawState draw = default_draw_state();
    XgWorldCloudsShadowSnapshot snapshot;

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    store_word(UINT32_C(0x8009c824), 2726u);
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.last_candidate_count == 1u);
    CHECK(snapshot.last_build_stats.quad_attempt_count == 1u);
    CHECK(snapshot.last_build_stats.far_postinsert_depth_stops == 1u);
    CHECK(snapshot.expected_scratch_emitted == 1u);
    CHECK(snapshot.expected_scratch_attempts == 0u);
    CHECK(materialize_guest_output(&cpu, &draw));
    CHECK(load_word(UINT32_C(0x1f8002f0)) == 1u);
    CHECK(load_word(UINT32_C(0x1f8002f4)) == 0u);
    CHECK(xg_world_clouds_shadow_finish(&cpu) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    return 1;
}

static int test_lifecycle_invalidation_and_reset(void) {
    CPUState cpu;
    CPUState other;
    GpuDrawState draw = default_draw_state();
    XgWorldCloudsShadowSnapshot snapshot;

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    CHECK(xg_world_clouds_shadow_finish(&cpu) ==
          XG_WORLD_CLOUDS_SHADOW_BLOCKED);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocker ==
          XG_WORLD_CLOUDS_SHADOW_BLOCK_UNMATCHED_FINISH);

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    cpu.gpr[31] = UINT32_C(0xa0071b68);
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_BLOCKED);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocker == XG_WORLD_CLOUDS_SHADOW_BLOCK_CALLER_RETURN);

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    other = cpu;
    CHECK(xg_world_clouds_shadow_finish(&other) ==
          XG_WORLD_CLOUDS_SHADOW_BLOCKED);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocker == XG_WORLD_CLOUDS_SHADOW_BLOCK_FINISH_CONTEXT);

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    CHECK(materialize_guest_output(&cpu, &draw));
    store_word(cpu.gpr[29] + 0x4cu, UINT32_C(0xa0071b68));
    CHECK(xg_world_clouds_shadow_finish(&cpu) ==
          XG_WORLD_CLOUDS_SHADOW_BLOCKED);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocker == XG_WORLD_CLOUDS_SHADOW_BLOCK_FINISH_CONTEXT);
    CHECK(snapshot.actual_saved_return == UINT32_C(0xa0071b68));

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_BLOCKED);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocker == XG_WORLD_CLOUDS_SHADOW_BLOCK_NESTED_BEGIN);
    CHECK(!snapshot.pending);

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    CHECK(xg_world_clouds_shadow_begin(&cpu, 7u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    xg_world_clouds_shadow_invalidate();
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocked && !snapshot.pending);
    CHECK(snapshot.lifecycle_invalidation_count == 1u);
    CHECK(snapshot.blocker ==
          XG_WORLD_CLOUDS_SHADOW_BLOCK_LIFECYCLE_INVALIDATED);
    CHECK(xg_world_clouds_shadow_finish(&cpu) ==
          XG_WORLD_CLOUDS_SHADOW_BLOCKED);

    xg_world_clouds_shadow_reset();
    setup_fixture(&cpu, UINT32_C(0x80086700));
    CHECK(xg_world_clouds_shadow_begin(&cpu, 8u, &draw, 0) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    CHECK(materialize_guest_output(&cpu, &draw));
    CHECK(xg_world_clouds_shadow_finish(&cpu) ==
          XG_WORLD_CLOUDS_SHADOW_OK);
    xg_world_clouds_shadow_snapshot(&snapshot);
    CHECK(snapshot.last_generation == 8u);
    CHECK(!snapshot.blocked);
    return 1;
}

int main(void) {
    return test_complete_match_is_read_only() &&
           test_exact_mismatch_reporting() &&
           test_callback_rejection_blocks_without_capture() &&
           test_far_stop_counter_contract() &&
           test_lifecycle_invalidation_and_reset()
        ? 0 : 1;
}
