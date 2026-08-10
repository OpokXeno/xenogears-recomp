#include "xg_world_decorations_shadow.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            return 0;                                                          \
        }                                                                      \
    } while (0)

/* Guest addresses exceed INT_MAX — enum constants would take type int on
 * clang/MSVC-ABI targets and sign-extend in 64-bit comparisons; UINT32_C
 * keeps them unsigned everywhere. */
#define TEST_MEMORY_BASE UINT32_C(0x80050000)
#define TEST_MEMORY_SIZE UINT32_C(0x0e0000)
#define TEST_POSITIONS UINT32_C(0x8009e000)
#define TEST_CONTEXT UINT32_C(0x80100000)
#define TEST_PACKET_BASE UINT32_C(0x80110000)
#define TEST_OT_BASE UINT32_C(0x80120000)
#define TEST_OUTER_SP UINT32_C(0x8012f000)
#define TEST_BUCKET UINT32_C(160)

typedef struct TestMemory {
    uint8_t bytes[TEST_MEMORY_SIZE];
} TestMemory;

static TestMemory memory;
static XgWorldDecorationsShadow shadow;
static uint32_t guest_write_count;

static bool memory_range(uint32_t address, uint32_t size) {
    return address >= TEST_MEMORY_BASE && size <= TEST_MEMORY_SIZE &&
           address - TEST_MEMORY_BASE <= TEST_MEMORY_SIZE - size;
}

static void store_u16(uint32_t address, uint16_t value) {
    const uint32_t offset = address - TEST_MEMORY_BASE;

    memory.bytes[offset] = (uint8_t)value;
    memory.bytes[offset + 1u] = (uint8_t)(value >> 8u);
}

static void store_u32(uint32_t address, uint32_t value) {
    store_u16(address, (uint16_t)value);
    store_u16(address + 2u, (uint16_t)(value >> 16u));
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    TestMemory *test_memory = context;
    uint32_t offset;

    if (test_memory == NULL || out_value == NULL ||
        !memory_range(address, 2u))
        return false;
    offset = address - TEST_MEMORY_BASE;
    *out_value = (uint16_t)test_memory->bytes[offset] |
                 ((uint16_t)test_memory->bytes[offset + 1u] << 8u);
    return true;
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
    uint16_t low;
    uint16_t high;

    if (out_value == NULL || !read_u16(context, address, &low) ||
        !read_u16(context, address + 2u, &high))
        return false;
    *out_value = low | ((uint32_t)high << 16u);
    return true;
}

static uint16_t cpu_read_half(uint32_t address) {
    uint16_t value = 0u;

    (void)read_u16(&memory, address, &value);
    return value;
}

static uint32_t cpu_read_word(uint32_t address) {
    uint32_t value = 0u;

    (void)read_u32(&memory, address, &value);
    return value;
}

static void cpu_write_half(uint32_t address, uint16_t value) {
    (void)address;
    (void)value;
    ++guest_write_count;
}

static void cpu_write_word(uint32_t address, uint32_t value) {
    (void)address;
    (void)value;
    ++guest_write_count;
}

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static uint32_t packed_xy(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16u);
}

static void store_matrix(uint32_t address, int32_t translation_z) {
    store_u32(address, pack_s16(4096, 0));
    store_u32(address + 4u, pack_s16(0, 0));
    store_u32(address + 8u, pack_s16(4096, 0));
    store_u32(address + 12u, pack_s16(0, 0));
    store_u32(address + 16u, pack_s16(4096, 0));
    store_u32(address + 20u, 0u);
    store_u32(address + 24u, 0u);
    store_u32(address + 28u, (uint32_t)translation_z);
}

static void configure_memory(void) {
    uint32_t index;

    memset(&memory, 0, sizeof(memory));
    store_matrix(UINT32_C(0x8009c808), 512);
    store_matrix(UINT32_C(0x8009a180), 0);
    store_u16(UINT32_C(0x8009bd3c), 1024u);
    store_u32(UINT32_C(0x800533f0), UINT32_C(0x00001000));
    store_u32(UINT32_C(0x8009be28), 0u);
    store_u32(UINT32_C(0x8009be30), 0u);
    store_u32(UINT32_C(0x8009d160), 32u);
    store_u32(UINT32_C(0x8009d2b4), 32u);
    store_u32(UINT32_C(0x8009d7cc), 0u);
    for (index = 0u; index < XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT; ++index) {
        store_u16(UINT32_C(0x8009d478) + index * 2u,
                  (uint16_t)(((0x1f0u + index) << 6u) | 0x0fu));
    }
    store_u32(TEST_POSITIONS, pack_s16(0, 0));
    store_u32(TEST_POSITIONS + 4u, pack_s16(-2048, 0));
    store_u32(XG_WORLD_DECORATIONS_SHADOW_BUFFER_ADDRESS, 0u);
    store_u32(XG_WORLD_DECORATIONS_SHADOW_PACKET_BASES, TEST_PACKET_BASE);
    store_u32(XG_WORLD_DECORATIONS_SHADOW_CONTEXT, TEST_CONTEXT);
    store_u32(TEST_CONTEXT + 0x70u, TEST_OT_BASE);
    store_u32(XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, 0u);
    store_u32(TEST_OT_BASE + TEST_BUCKET * 4u, UINT32_C(0x00123456));
    store_u32(TEST_OUTER_SP - 4u,
              XG_WORLD_DECORATIONS_SHADOW_OUTER_RETURN);
}

static XgWorldDecorationsShadowObservation observation(void) {
    return (XgWorldDecorationsShadowObservation){
        .read_context = &memory,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 19u,
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 120 << 16,
        .projection_distance = 256u,
        .raster =
            {
                .draw_area_right = 319u,
                .draw_area_bottom = 239u,
            },
        .authenticated = true,
        .helper_arguments_authenticated = true,
        .projection_state_authenticated = true,
    };
}

static XgWorldDecorationsShadowObservation outer_begin_observation(void) {
    XgWorldDecorationsShadowObservation result = observation();

    result.pc = XG_WORLD_DECORATIONS_SHADOW_OUTER_BEGIN_PC;
    result.return_address = XG_WORLD_DECORATIONS_SHADOW_OUTER_RETURN;
    result.stack_pointer = TEST_OUTER_SP;
    return result;
}

static XgWorldDecorationsShadowObservation helper_begin_observation(
    uint32_t count) {
    XgWorldDecorationsShadowObservation result = observation();

    result.pc = XG_WORLD_DECORATIONS_SHADOW_HELPER_BEGIN_PC;
    result.return_address = XG_WORLD_DECORATIONS_SHADOW_HELPER_RETURN;
    result.stack_pointer =
        TEST_OUTER_SP - XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE;
    result.a0 = TEST_POSITIONS;
    result.a1 = 1u;
    result.a2 = TEST_OT_BASE;
    result.a3 = TEST_PACKET_BASE +
                count * XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;
    return result;
}

static XgWorldDecorationsShadowObservation helper_finish_observation(
    uint32_t count) {
    XgWorldDecorationsShadowObservation result = observation();

    result.pc = XG_WORLD_DECORATIONS_SHADOW_HELPER_FINISH_PC;
    result.return_address = XG_WORLD_DECORATIONS_SHADOW_HELPER_RETURN;
    result.stack_pointer =
        TEST_OUTER_SP - XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE -
        XG_WORLD_DECORATIONS_SHADOW_HELPER_FRAME_SIZE;
    result.a3 = TEST_PACKET_BASE +
                count * XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;
    result.s0 = count;
    return result;
}

static XgWorldDecorationsShadowObservation outer_finish_observation(void) {
    XgWorldDecorationsShadowObservation result = observation();

    result.pc = XG_WORLD_DECORATIONS_SHADOW_OUTER_FINISH_PC;
    result.stack_pointer =
        TEST_OUTER_SP - XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE;
    result.s0 = 5u;
    return result;
}

static void write_expected_packet(uint32_t index, uint32_t tag) {
    const uint16_t clut = (uint16_t)((0x1f7u << 6u) | 0x0fu);
    const uint32_t packet =
        TEST_PACKET_BASE + index * XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;
    const uint32_t payload[9] = {
        UINT32_C(0x2c808080),
        packed_xy(152, 122),
        ((uint32_t)clut << 16u) | UINT32_C(0x00004000),
        packed_xy(152, 117),
        UINT32_C(0x001e401f),
        packed_xy(160, 122),
        UINT32_C(0x00006f00),
        packed_xy(160, 117),
        UINT32_C(0x00006f1f),
    };
    uint32_t word;

    store_u32(packet, tag);
    for (word = 0u; word < 9u; ++word)
        store_u32(packet + 4u + word * 4u, payload[word]);
}

static void write_packet_template(uint32_t index) {
    const uint32_t packet =
        TEST_PACKET_BASE + index * XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;
    const uint32_t payload[9] = {
        UINT32_C(0x2c808080), 0u, UINT32_C(0x00004000), 0u,
        UINT32_C(0x001e401f), 0u, UINT32_C(0x00006f00), 0u,
        UINT32_C(0x00006f1f),
    };

    for (uint32_t word = 0u; word < 9u; ++word)
        store_u32(packet + 4u + word * 4u, payload[word]);
}

static int run_helper(uint32_t initial_count, uint32_t initial_head) {
    XgWorldDecorationsShadowObservation begin =
        helper_begin_observation(initial_count);
    XgWorldDecorationsShadowObservation finish =
        helper_finish_observation(initial_count + 1u);
    const uint32_t packet =
        TEST_PACKET_BASE +
        initial_count * XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;

    store_u32(XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, initial_count);
    write_packet_template(initial_count);
    CHECK(xg_world_decorations_shadow_state_helper_begin(&shadow, &begin) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    write_expected_packet(initial_count,
                          UINT32_C(0x09000000) | initial_head);
    store_u32(TEST_OT_BASE + TEST_BUCKET * 4u,
              packet & UINT32_C(0x00ffffff));
    store_u32(XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, initial_count + 1u);
    CHECK(xg_world_decorations_shadow_state_helper_finish(&shadow, &finish) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    return 1;
}

static int test_successful_helper(void) {
    XgWorldDecorationsShadowObservation begin;
    XgWorldDecorationsShadowObservation finish;
    XgWorldDecorationsShadowSnapshot snapshot;

    configure_memory();
    xg_world_decorations_shadow_state_reset(&shadow);
    begin = outer_begin_observation();
    CHECK(xg_world_decorations_shadow_state_outer_begin(&shadow, &begin) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(run_helper(0u, UINT32_C(0x00123456)));
    finish = outer_finish_observation();
    CHECK(xg_world_decorations_shadow_state_outer_finish(&shadow, &finish) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(xg_world_decorations_shadow_state_snapshot(&shadow, &snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(snapshot.phase == XG_WORLD_DECORATIONS_SHADOW_PHASE_IDLE);
    CHECK(snapshot.outer_begin_count == 1u);
    CHECK(snapshot.outer_finish_count == 1u);
    CHECK(snapshot.outer_match_count == 1u);
    CHECK(snapshot.helper_begin_count == 1u);
    CHECK(snapshot.helper_finish_count == 1u);
    CHECK(snapshot.helper_match_count == 1u);
    CHECK(snapshot.source_capture_count == 1u);
    CHECK(snapshot.source_read_count == 41u);
    CHECK(snapshot.source_read_bytes == 130u);
    CHECK(snapshot.candidate_count == 1u);
    CHECK(snapshot.primitive_count == 1u);
    CHECK(snapshot.match_count == 1u);
    CHECK(snapshot.mismatch_count == 0u);
    CHECK(snapshot.ot_match_count == 1u);
    CHECK(!snapshot.has_first_mismatch);
    CHECK(!snapshot.blocked);
    return 1;
}

static int test_repeated_helpers_preserve_shared_counter(void) {
    XgWorldDecorationsShadowObservation begin;
    XgWorldDecorationsShadowObservation finish;
    XgWorldDecorationsShadowSnapshot snapshot;

    configure_memory();
    xg_world_decorations_shadow_state_reset(&shadow);
    begin = outer_begin_observation();
    CHECK(xg_world_decorations_shadow_state_outer_begin(&shadow, &begin) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(run_helper(0u, UINT32_C(0x00123456)));
    CHECK(run_helper(1u, TEST_PACKET_BASE & UINT32_C(0x00ffffff)));
    finish = outer_finish_observation();
    CHECK(xg_world_decorations_shadow_state_outer_finish(&shadow, &finish) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(xg_world_decorations_shadow_state_snapshot(&shadow, &snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(snapshot.helper_begin_count == 2u);
    CHECK(snapshot.helper_finish_count == 2u);
    CHECK(snapshot.helper_match_count == 2u);
    CHECK(snapshot.primitive_count == 2u);
    CHECK(snapshot.match_count == 2u);
    CHECK(snapshot.ot_match_count == 2u);
    CHECK(snapshot.last_initial_count == 1u);
    CHECK(snapshot.last_final_count == 2u);
    CHECK(snapshot.last_a3 == TEST_PACKET_BASE + 2u * 0x28u);
    CHECK(snapshot.last_s0 == 2u);
    CHECK(snapshot.count_mismatch_count == 0u);
    return 1;
}

static int test_mismatch_and_lifecycle_block(void) {
    XgWorldDecorationsShadowObservation begin;
    XgWorldDecorationsShadowObservation helper_begin;
    XgWorldDecorationsShadowObservation helper_finish;
    XgWorldDecorationsShadowSnapshot snapshot;

    configure_memory();
    xg_world_decorations_shadow_state_reset(&shadow);
    begin = outer_begin_observation();
    CHECK(xg_world_decorations_shadow_state_outer_begin(&shadow, &begin) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    helper_begin = helper_begin_observation(0u);
    write_packet_template(0u);
    CHECK(xg_world_decorations_shadow_state_helper_begin(&shadow,
                                                          &helper_begin) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    write_expected_packet(0u, 0u);
    store_u32(TEST_PACKET_BASE + 8u, UINT32_C(0xdeadbeef));
    store_u32(TEST_OT_BASE + TEST_BUCKET * 4u, UINT32_C(0x00abcdef));
    store_u32(XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, 2u);
    helper_finish = helper_finish_observation(2u);
    CHECK(xg_world_decorations_shadow_state_helper_finish(&shadow,
                                                           &helper_finish) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(xg_world_decorations_shadow_state_snapshot(&shadow, &snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(snapshot.helper_mismatch_count == 1u);
    CHECK(snapshot.mismatch_count == 1u);
    CHECK(snapshot.payload_mismatch_count == 1u);
    CHECK(snapshot.payload_word_mismatch_count == 1u);
    CHECK(snapshot.geometry_mismatch_count == 1u);
    CHECK(snapshot.tag_mismatch_count == 1u);
    CHECK(snapshot.ot_mismatch_count == 1u);
    CHECK(snapshot.count_mismatch_count == 1u);
    CHECK(snapshot.a3_mismatch_count == 1u);
    CHECK(snapshot.s0_mismatch_count == 1u);
    CHECK(snapshot.has_first_mismatch);

    CHECK(xg_world_decorations_shadow_state_lifecycle_invalidate(
              &shadow, 0x55u) == XG_WORLD_DECORATIONS_SHADOW_BLOCKED);
    CHECK(xg_world_decorations_shadow_state_snapshot(&shadow, &snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(snapshot.blocked);
    CHECK(snapshot.phase == XG_WORLD_DECORATIONS_SHADOW_PHASE_BLOCKED);
    CHECK(snapshot.blocker ==
          XG_WORLD_DECORATIONS_SHADOW_BLOCKER_LIFECYCLE_INVALIDATED);
    CHECK(snapshot.blocker_detail == 0x55u);
    CHECK(snapshot.lifecycle_invalidation_count == 1u);
    CHECK(xg_world_decorations_shadow_state_outer_begin(&shadow, &begin) ==
          XG_WORLD_DECORATIONS_SHADOW_BLOCKED);

    xg_world_decorations_shadow_state_reset(&shadow);
    helper_finish = helper_finish_observation(0u);
    CHECK(xg_world_decorations_shadow_state_helper_finish(&shadow,
                                                           &helper_finish) ==
          XG_WORLD_DECORATIONS_SHADOW_BLOCKED);
    CHECK(xg_world_decorations_shadow_state_snapshot(&shadow, &snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(snapshot.blocker ==
          XG_WORLD_DECORATIONS_SHADOW_BLOCKER_UNMATCHED_HELPER_FINISH);
    return 1;
}

static int test_runtime_surface_is_read_only(void) {
    CPUState cpu = {0};
    GpuDrawState draw = {0};
    XgWorldDecorationsShadowSnapshot snapshot;

    configure_memory();
    guest_write_count = 0u;
    cpu.read_half = cpu_read_half;
    cpu.read_word = cpu_read_word;
    cpu.write_half = cpu_write_half;
    cpu.write_word = cpu_write_word;
    cpu.gte_ctrl[24] = 160u << 16u;
    cpu.gte_ctrl[25] = 120u << 16u;
    cpu.gte_ctrl[26] = 256u;
    draw.right = 319u;
    draw.bottom = 239u;

    xg_world_decorations_shadow_reset();
    cpu.gpr[29] = TEST_OUTER_SP;
    cpu.gpr[31] = XG_WORLD_DECORATIONS_SHADOW_OUTER_RETURN;
    CHECK(xg_world_decorations_shadow_outer_begin(&cpu, 23u, &draw, 0) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);

    cpu.gpr[4] = TEST_POSITIONS;
    cpu.gpr[5] = 1u;
    cpu.gpr[6] = TEST_OT_BASE;
    cpu.gpr[7] = TEST_PACKET_BASE;
    cpu.gpr[29] =
        TEST_OUTER_SP - XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE;
    cpu.gpr[31] = XG_WORLD_DECORATIONS_SHADOW_HELPER_RETURN;
    write_packet_template(0u);
    CHECK(xg_world_decorations_shadow_helper_begin(&cpu) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);

    write_expected_packet(0u, UINT32_C(0x09123456));
    store_u32(TEST_OT_BASE + TEST_BUCKET * 4u,
              TEST_PACKET_BASE & UINT32_C(0x00ffffff));
    store_u32(XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, 1u);
    cpu.gpr[7] = TEST_PACKET_BASE +
                 XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;
    cpu.gpr[16] = 1u;
    cpu.gpr[29] -= XG_WORLD_DECORATIONS_SHADOW_HELPER_FRAME_SIZE;
    CHECK(xg_world_decorations_shadow_helper_finish(&cpu) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);

    cpu.gpr[16] = 5u;
    cpu.gpr[29] =
        TEST_OUTER_SP - XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE;
    CHECK(xg_world_decorations_shadow_outer_finish(&cpu) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(xg_world_decorations_shadow_snapshot(&snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(snapshot.outer_match_count == 1u);
    CHECK(snapshot.helper_match_count == 1u);
    CHECK(guest_write_count == 0u);

    xg_world_decorations_shadow_lifecycle_invalidate();
    CHECK(xg_world_decorations_shadow_snapshot(&snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(!snapshot.blocked);
    CHECK(snapshot.lifecycle_invalidation_count == 1u);
    xg_world_decorations_shadow_lifecycle_block();
    CHECK(xg_world_decorations_shadow_snapshot(&snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(snapshot.blocked);
    CHECK(snapshot.blocker ==
          XG_WORLD_DECORATIONS_SHADOW_BLOCKER_EXPLICIT);
    CHECK(guest_write_count == 0u);
    return 1;
}

static int test_native_cutover_accounting(void) {
    XgWorldDecorationsShadowSnapshot snapshot;

    xg_world_decorations_shadow_reset();
    CHECK(xg_world_decorations_shadow_record_native_cutover(3u));
    CHECK(xg_world_decorations_shadow_record_native_cutover(0u));
    CHECK(xg_world_decorations_shadow_snapshot(&snapshot) ==
          XG_WORLD_DECORATIONS_SHADOW_OK);
    CHECK(snapshot.native_cutover_count == 2u);
    CHECK(snapshot.native_primitive_count == 3u);
    CHECK(!snapshot.blocked);
    return 1;
}

int main(void) {
    return test_successful_helper() &&
                   test_repeated_helpers_preserve_shared_counter() &&
                   test_mismatch_and_lifecycle_block() &&
                   test_runtime_surface_is_read_only() &&
                   test_native_cutover_accounting()
               ? 0
               : 1;
}
