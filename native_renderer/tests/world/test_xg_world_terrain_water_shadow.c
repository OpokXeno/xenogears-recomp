#include "xg_world_terrain_water_shadow.h"

#include "xg_world_terrain_water.h"
#include "xg_world_terrain_water_source_capture.h"

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

#define TEST_CONTEXT UINT32_C(0x80008000)
#define TEST_OT_BASE UINT32_C(0x80010000)
#define TEST_PACKET_BASE UINT32_C(0x80011000)
#define TEST_RESOURCE UINT32_C(0x800a0000)
#define TEST_STACK UINT32_C(0x801ff000)
#define TEST_AUTHENTICATION UINT64_C(9)

typedef struct TestMemory {
    uint32_t ot[XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT];
    uint32_t packets[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY][8];
    uint32_t final_count;
    uint32_t saved_return;
    uint32_t write_count;
} TestMemory;

static TestMemory memory;

static uint32_t terrain_word(uint32_t index) {
    const int32_t height = (index & 1u) != 0u ? -1 : 1;

    return (uint8_t)height | ((index % 7u) << 8u) |
        ((index & 1u) << 11u) | ((index & 1u) << 12u) |
        ((index % 4u) << 13u) | (((index >> 2u) & 1u) << 15u) |
        ((index & 0x0fu) << 16u) | (((index / 9u) & 0x0fu) << 20u);
}

static uint32_t read_word(uint32_t address) {
    static const uint32_t camera[8] = {
        0x00001000u, 0x00000000u, 0xf0000000u, 0x00000000u,
        0x00000000u, 0u, 0u, 2048u,
    };
    static const uint32_t identity[8] = {
        0x00001000u, 0x00000000u, 0x00001000u, 0x00000000u,
        0x00001000u, 0u, 0u, 0u,
    };
    static const uint32_t clip_addresses[8] = {
        0x8009c828u, 0x8009c830u, 0x8009c844u, 0x8009c84cu,
        0x8009c878u, 0x8009c87cu, 0x8009c7f4u, 0x8009c7f8u,
    };
    uint32_t index;

    if (address >= TEST_OT_BASE &&
        address < TEST_OT_BASE + sizeof(memory.ot) && (address & 3u) == 0u)
        return memory.ot[(address - TEST_OT_BASE) / 4u];
    if (address >= TEST_PACKET_BASE &&
        address < TEST_PACKET_BASE + sizeof(memory.packets) &&
        (address & 3u) == 0u) {
        const uint32_t offset = address - TEST_PACKET_BASE;

        return memory.packets[offset / 0x20u][(offset & 0x1fu) / 4u];
    }
    if (address == XG_WORLD_TERRAIN_WATER_SHADOW_CONTEXT_GLOBAL)
        return TEST_CONTEXT;
    if (address == TEST_CONTEXT + 0x70u) return TEST_OT_BASE;
    if (address == TEST_CONTEXT + 0x74u) return TEST_PACKET_BASE;
    if (address == XG_WORLD_TERRAIN_WATER_SHADOW_FINAL_COUNT_GLOBAL)
        return memory.final_count;
    if (address == TEST_STACK - 4u) return memory.saved_return;
    if (address == UINT32_C(0x8009be28) ||
        address == UINT32_C(0x8009be30) ||
        address == UINT32_C(0x8009c5bc))
        return 0u;
    if (address == UINT32_C(0x8009c618)) return 0x400u;
    if (address == UINT32_C(0x8009d7cc)) return 0u;
    if (address >= UINT32_C(0x8009c808) &&
        address < UINT32_C(0x8009c828) && (address & 3u) == 0u)
        return camera[(address - UINT32_C(0x8009c808)) / 4u];
    if (address >= UINT32_C(0x8009d534) &&
        address < UINT32_C(0x8009d554) && (address & 3u) == 0u)
        return identity[(address - UINT32_C(0x8009d534)) / 4u];
    for (index = 0u; index < 8u; ++index) {
        if (address == clip_addresses[index]) return 0u;
    }
    if (address >= UINT32_C(0x800523f0) &&
        address < UINT32_C(0x800563f0) && (address & 3u) == 0u)
        return 0x00001000u;
    if (address >= UINT32_C(0x8009c184) &&
        address < UINT32_C(0x8009c584) && (address & 3u) == 0u) {
        const uint32_t terrain_id =
            (address - UINT32_C(0x8009c184)) / 4u;

        return terrain_id == 0u ? TEST_RESOURCE : 0u;
    }
    if (address >= TEST_RESOURCE && address < TEST_RESOURCE + 0x510u &&
        (address & 3u) == 0u) {
        static const uint32_t offsets[4] = { 0u, 0x144u, 0x288u, 0x3ccu };

        for (index = 0u; index < 4u; ++index) {
            if (address >= TEST_RESOURCE + offsets[index] &&
                address < TEST_RESOURCE + offsets[index] + 0x144u)
                return terrain_word(
                    (address - TEST_RESOURCE - offsets[index]) / 4u);
        }
    }
    return 0u;
}

static uint16_t read_half(uint32_t address) {
    static const uint16_t tpages[7] = {
        0x0088u, 0x008au, 0x008cu, 0x008eu,
        0x0096u, 0x0098u, 0x009au,
    };

    if (address == UINT32_C(0x1f800316)) return 0x009cu;
    if (address >= UINT32_C(0x8009d618) &&
        address < UINT32_C(0x8009d64a) && (address & 1u) == 0u)
        return address == UINT32_C(0x8009d618) ? 0u : UINT16_MAX;
    if (address >= UINT32_C(0x8009d650) &&
        address < UINT32_C(0x8009d718) && (address & 1u) == 0u) {
        const uint32_t tile = (address - UINT32_C(0x8009d650)) / 8u;

        return tile == 0u ? 0u : UINT16_MAX;
    }
    if (address == UINT32_C(0x8009c838) ||
        address == UINT32_C(0x8009c83c))
        return 2u;
    if (address >= UINT32_C(0x8009ccb4) &&
        address < UINT32_C(0x8009cd34) && (address & 1u) == 0u) {
        const uint32_t index =
            (address - UINT32_C(0x8009ccb4)) / 2u;

        return (uint16_t)((432u + index) << 6u);
    }
    if (address >= UINT32_C(0x8009cd54) &&
        address < UINT32_C(0x8009cd62) && (address & 1u) == 0u)
        return tpages[(address - UINT32_C(0x8009cd54)) / 2u];
    if (address >= UINT32_C(0x8009d570) &&
        address < UINT32_C(0x8009d612) && (address & 1u) == 0u) {
        const uint32_t grid_index =
            (address - UINT32_C(0x8009d570)) / 2u;
        const uint32_t row = grid_index / 9u;
        const uint32_t column = grid_index % 9u;

        if (row >= 2u && row <= 6u && column >= 2u && column <= 6u)
            return (uint16_t)((row - 2u) * 5u + column - 2u);
    }
    return (address & 2u) != 0u
        ? (uint16_t)(read_word(address & ~3u) >> 16u)
        : (uint16_t)read_word(address);
}

static uint8_t read_byte(uint32_t address) {
    return (uint8_t)(read_word(address & ~3u) >> ((address & 3u) * 8u));
}

static void write_word(uint32_t address, uint32_t value) {
    (void)address;
    (void)value;
    ++memory.write_count;
}

static void write_half(uint32_t address, uint16_t value) {
    (void)address;
    (void)value;
    ++memory.write_count;
}

static void write_byte(uint32_t address, uint8_t value) {
    (void)address;
    (void)value;
    ++memory.write_count;
}

static CPUState make_cpu(void) {
    CPUState cpu = { 0 };

    cpu.gpr[4] = TEST_OT_BASE;
    cpu.gpr[5] = TEST_PACKET_BASE;
    cpu.gpr[6] = XG_WORLD_TERRAIN_WATER_SHADOW_POSITION_GLOBAL;
    cpu.gpr[29] = TEST_STACK;
    cpu.gpr[31] = UINT32_C(0x80071b38);
    cpu.pc = XG_WORLD_TERRAIN_WATER_SHADOW_BEGIN_PC;
    cpu.gte_ctrl[24] = 160u << 16u;
    cpu.gte_ctrl[25] = 120u << 16u;
    cpu.gte_ctrl[26] = 32u;
    cpu.read_word = read_word;
    cpu.read_half = read_half;
    cpu.read_byte = read_byte;
    cpu.write_word = write_word;
    cpu.write_half = write_half;
    cpu.write_byte = write_byte;
    return cpu;
}

static GpuDrawState make_draw_state(void) {
    GpuDrawState draw = { 0 };

    draw.right = 319u;
    draw.bottom = 239u;
    draw.texture_window_mask_x = 1u;
    draw.texture_window_mask_y = 2u;
    draw.texture_window_offset_x = 3u;
    draw.texture_window_offset_y = 4u;
    return draw;
}

static void reset_memory(void) {
    uint32_t index;

    memset(&memory, 0, sizeof(memory));
    memory.saved_return = UINT32_C(0x80071b38);
    for (index = 0u;
         index < XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT; ++index)
        memory.ot[index] = UINT32_C(0x00f00000) + index;
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY; ++index) {
        memory.packets[index][0] = UINT32_C(0xdead0000) | index;
        memory.packets[index][1] = UINT32_C(0x24808080);
        memory.packets[index][2] = UINT32_C(0x11111111);
        memory.packets[index][3] = UINT32_C(0x22222222);
        memory.packets[index][4] = UINT32_C(0x33333333);
        memory.packets[index][5] = UINT32_C(0x44444444);
        memory.packets[index][6] = UINT32_C(0x55555555);
        memory.packets[index][7] = UINT32_C(0x5a5abeef);
    }
}

static bool capture_read_u16(void *context, uint32_t address,
                             uint16_t *out_value) {
    CPUState *cpu = context;

    *out_value = cpu->read_half(address);
    return true;
}

static bool capture_read_u32(void *context, uint32_t address,
                             uint32_t *out_value) {
    CPUState *cpu = context;

    *out_value = cpu->read_word(address);
    return true;
}

static int build_expected_records(
    CPUState *cpu, const GpuDrawState *draw,
    XgWorldTerrainWaterRecord records[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY],
    uint32_t *out_count) {
    XgWorldTerrainWaterCapture capture;
    const XgWorldTerrainWaterCaptureRequest request = {
        .authentication_generation = TEST_AUTHENTICATION,
        .caller_return = cpu->gpr[31],
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
    const XgWorldTerrainWaterAuthenticatedReader reader = {
        .context = cpu,
        .read_u16 = capture_read_u16,
        .read_u32 = capture_read_u32,
        .authentication_generation = TEST_AUTHENTICATION,
        .authenticated = true,
    };

    if (xg_world_terrain_water_source_capture(&request, &reader, &capture) !=
        XG_WORLD_TERRAIN_WATER_CAPTURE_OK)
        return 0;
    return xg_world_terrain_water_build(
               &capture.source, records,
               XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, out_count) ==
        XG_WORLD_TERRAIN_WATER_OK;
}

static uint32_t expected_xy(const XgRenderIrVertex *vertex) {
    return (((uint32_t)vertex->x >> 16u) & UINT32_C(0xffff)) |
        ((uint32_t)vertex->y & UINT32_C(0xffff0000));
}

static uint16_t expected_uv(const XgRenderIrVertex *vertex) {
    return (uint16_t)((uint8_t)((uint32_t)vertex->u >> 16u) |
        ((uint16_t)(uint8_t)((uint32_t)vertex->v >> 16u) << 8u));
}

static void simulate_original(
    const XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY],
    uint32_t count) {
    uint32_t heads[XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT];
    uint32_t index;

    memcpy(heads, memory.ot, sizeof(heads));
    for (index = 0u; index < count; ++index) {
        const XgWorldTerrainWaterRecord *record = &records[index];
        const XgRenderIrTriangle *triangle =
            &record->primitive.triangles[0];
        const uint32_t bucket = record->ordering_bucket;
        const uint32_t packet = TEST_PACKET_BASE + index * 0x20u;

        memory.packets[index][0] = heads[bucket] | UINT32_C(0x07000000);
        memory.packets[index][2] = expected_xy(&triangle->vertices[0]);
        memory.packets[index][3] =
            expected_uv(&triangle->vertices[0]) |
            ((uint32_t)record->encoded_clut << 16u);
        memory.packets[index][4] = expected_xy(&triangle->vertices[1]);
        memory.packets[index][5] =
            expected_uv(&triangle->vertices[1]) |
            ((uint32_t)record->encoded_tpage << 16u);
        memory.packets[index][6] = expected_xy(&triangle->vertices[2]);
        memory.packets[index][7] =
            (memory.packets[index][7] & UINT32_C(0xffff0000)) |
            expected_uv(&triangle->vertices[2]);
        memory.ot[bucket] = packet & UINT32_C(0x00ffffff);
        heads[bucket] = packet & UINT32_C(0x00ffffff);
    }
    memory.final_count = count;
}

static int test_success_and_value_only_contract(void) {
    static XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    XgWorldTerrainWaterShadowSnapshot snapshot;
    CPUState cpu;
    GpuDrawState draw;
    uint32_t count = 0u;

    reset_memory();
    xg_world_terrain_water_shadow_reset();
    cpu = make_cpu();
    draw = make_draw_state();
    CHECK(build_expected_records(&cpu, &draw, records, &count));
    CHECK(count == 512u);
    CHECK(xg_world_terrain_water_shadow_begin(
              &cpu, TEST_AUTHENTICATION, &draw, 0) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(xg_world_terrain_water_shadow_snapshot(&snapshot) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(snapshot.pending && !snapshot.blocked);
    CHECK(snapshot.source_read_count == 564u);
    CHECK(snapshot.source_read_bytes == 1856u);
    CHECK(snapshot.captured_resource_count == 1u);
    simulate_original(records, count);
    cpu.pc = XG_WORLD_TERRAIN_WATER_SHADOW_FINISH_PC;
    cpu.gpr[29] -= XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE;
    CHECK(xg_world_terrain_water_shadow_finish(&cpu) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(xg_world_terrain_water_shadow_snapshot(&snapshot) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(!snapshot.pending && !snapshot.blocked);
    CHECK(snapshot.begin_count == 1u && snapshot.completion_count == 1u);
    CHECK(snapshot.invocation_match_count == 1u);
    CHECK(snapshot.invocation_mismatch_count == 0u);
    CHECK(snapshot.candidate_count == count);
    CHECK(snapshot.original_primitive_count == count);
    CHECK(snapshot.packet_match_count == count);
    CHECK(snapshot.packet_mismatch_count == 0u);
    CHECK(snapshot.touched_ot_bucket_count == 1u);
    CHECK(snapshot.untouched_ot_bucket_count ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT - 1u);
    CHECK(snapshot.first_mismatch.kind ==
          XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_NONE);
    CHECK(memory.write_count == 0u);
    return 1;
}

static int test_raw_uv_mismatch_is_detailed(void) {
    static XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    XgWorldTerrainWaterShadowSnapshot snapshot;
    CPUState cpu;
    GpuDrawState draw;
    uint32_t count = 0u;

    reset_memory();
    xg_world_terrain_water_shadow_reset();
    cpu = make_cpu();
    draw = make_draw_state();
    CHECK(build_expected_records(&cpu, &draw, records, &count));
    CHECK(xg_world_terrain_water_shadow_begin(
              &cpu, TEST_AUTHENTICATION, &draw, 0) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    simulate_original(records, count);
    memory.packets[7][3] ^= 1u;
    cpu.pc = XG_WORLD_TERRAIN_WATER_SHADOW_FINISH_PC;
    cpu.gpr[29] -= XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE;
    CHECK(xg_world_terrain_water_shadow_finish(&cpu) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(xg_world_terrain_water_shadow_snapshot(&snapshot) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(snapshot.invocation_mismatch_count == 1u);
    CHECK(snapshot.packet_mismatch_count == 1u);
    CHECK(snapshot.payload_mismatch_count == 1u);
    CHECK(snapshot.uv_mismatch_count == 1u);
    CHECK(snapshot.first_mismatch.kind ==
          XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_PACKET);
    CHECK(snapshot.first_mismatch.record_index == 7u);
    CHECK((snapshot.first_mismatch.field_bits &
           XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV0) != 0u);
    CHECK(memory.write_count == 0u);
    return 1;
}

static int test_untouched_ot_and_preserved_fields_are_checked(void) {
    static XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    XgWorldTerrainWaterShadowSnapshot snapshot;
    bool touched[XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT] = { false };
    CPUState cpu;
    GpuDrawState draw;
    uint32_t untouched_bucket = UINT32_MAX;
    uint32_t count = 0u;
    uint32_t index;

    reset_memory();
    xg_world_terrain_water_shadow_reset();
    cpu = make_cpu();
    draw = make_draw_state();
    CHECK(build_expected_records(&cpu, &draw, records, &count));
    CHECK(xg_world_terrain_water_shadow_begin(
              &cpu, TEST_AUTHENTICATION, &draw, 0) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    simulate_original(records, count);
    for (index = 0u; index < count; ++index)
        touched[records[index].ordering_bucket] = true;
    for (index = 0u;
         index < XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT; ++index) {
        if (!touched[index]) {
            untouched_bucket = index;
            break;
        }
    }
    CHECK(untouched_bucket != UINT32_MAX);
    memory.ot[untouched_bucket] ^= 1u;
    memory.packets[0][1] ^= 1u;
    memory.packets[0][7] ^= UINT32_C(0x00010000);
    cpu.pc = XG_WORLD_TERRAIN_WATER_SHADOW_FINISH_PC;
    cpu.gpr[29] -= XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE;
    CHECK(xg_world_terrain_water_shadow_finish(&cpu) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(xg_world_terrain_water_shadow_snapshot(&snapshot) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(snapshot.packet_unchanged_mismatch_count == 1u);
    CHECK(snapshot.material_mismatch_count == 1u);
    CHECK(snapshot.untouched_ot_mismatch_count == 1u);
    CHECK((snapshot.first_mismatch.field_bits &
           XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNCHANGED_MATERIAL) != 0u);
    CHECK((snapshot.first_mismatch.field_bits &
           XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNCHANGED_UV2_HIGH) != 0u);
    CHECK(memory.write_count == 0u);
    return 1;
}

static int test_lifecycle_blocks_pending_observation(void) {
    XgWorldTerrainWaterShadowSnapshot snapshot;
    CPUState cpu;
    GpuDrawState draw;

    reset_memory();
    xg_world_terrain_water_shadow_reset();
    cpu = make_cpu();
    draw = make_draw_state();
    CHECK(xg_world_terrain_water_shadow_begin(
              &cpu, TEST_AUTHENTICATION, &draw, 0) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(xg_world_terrain_water_shadow_lifecycle_invalidate() ==
          XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
    CHECK(xg_world_terrain_water_shadow_snapshot(&snapshot) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(snapshot.blocked && !snapshot.pending);
    CHECK(snapshot.blocker ==
          XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_LIFECYCLE_PENDING);
    CHECK(snapshot.pending_block_count == 1u);
    CHECK(snapshot.lifecycle_invalidation_count == 1u);
    CHECK(snapshot.block_count == 1u);
    CHECK(xg_world_terrain_water_shadow_begin(
              &cpu, TEST_AUTHENTICATION, &draw, 0) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);

    reset_memory();
    xg_world_terrain_water_shadow_reset();
    cpu = make_cpu();
    CHECK(xg_world_terrain_water_shadow_begin(
              &cpu, TEST_AUTHENTICATION, &draw, 0) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(xg_world_terrain_water_shadow_block_pending() ==
          XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED);
    CHECK(xg_world_terrain_water_shadow_snapshot(&snapshot) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(snapshot.blocker ==
          XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_LIFECYCLE_PENDING);
    CHECK(snapshot.pending_block_count == 1u);
    CHECK(snapshot.lifecycle_invalidation_count == 0u);
    CHECK(memory.write_count == 0u);
    return 1;
}

static int test_native_cutover_accounting_is_fail_closed(void) {
    XgWorldTerrainWaterShadowSnapshot snapshot;
    CPUState cpu;
    GpuDrawState draw;

    reset_memory();
    xg_world_terrain_water_shadow_reset();
    CHECK(xg_world_terrain_water_shadow_record_native_cutover(512u));
    CHECK(xg_world_terrain_water_shadow_snapshot(&snapshot) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(snapshot.native_cutover_count == 1u);
    CHECK(snapshot.native_primitive_count == 512u);

    cpu = make_cpu();
    draw = make_draw_state();
    CHECK(xg_world_terrain_water_shadow_begin(
              &cpu, TEST_AUTHENTICATION, &draw, 0) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(!xg_world_terrain_water_shadow_record_native_cutover(1u));
    CHECK(xg_world_terrain_water_shadow_snapshot(&snapshot) ==
          XG_WORLD_TERRAIN_WATER_SHADOW_OK);
    CHECK(snapshot.native_cutover_count == 1u);
    CHECK(snapshot.native_primitive_count == 512u);
    return 1;
}

int main(void) {
    int ok = 1;

    ok &= test_success_and_value_only_contract();
    ok &= test_raw_uv_mismatch_is_detailed();
    ok &= test_untouched_ot_and_preserved_fields_are_checked();
    ok &= test_lifecycle_blocks_pending_observation();
    ok &= test_native_cutover_accounting_is_fail_closed();
    return ok ? 0 : 1;
}
