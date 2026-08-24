#include "xg_world_minimap_shadow.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 0;                                                           \
        }                                                                       \
    } while (0)

enum {
    TEST_MEMORY_CAPACITY = 512,
};

#define TEST_CONTEXT UINT32_C(0x80100000)
#define TEST_OT UINT32_C(0x80101000)
#define TEST_STACK UINT32_C(0x801ff000)

typedef struct TestMemoryWord {
    uint32_t address;
    uint32_t value;
} TestMemoryWord;

typedef struct TestMemory {
    TestMemoryWord words[TEST_MEMORY_CAPACITY];
    uint32_t count;
    uint32_t read_count;
    uint32_t guest_write_count;
} TestMemory;

static TestMemory memory;

static const int16_t triangle_source[4][3][4] = {
    { { 0, 0, 0, 0 }, { -8, -8, 0, 0 }, { -4, -11, 0, 0 } },
    { { 0, 0, 0, 0 }, { -4, -11, 0, 0 }, { 0, -12, 0, 0 } },
    { { 0, 0, 0, 0 }, { 0, -12, 0, 0 }, { 4, -11, 0, 0 } },
    { { 0, 0, 0, 0 }, { 4, -11, 0, 0 }, { 8, -8, 0, 0 } },
};

static const int16_t projected_xy[4][3][2] = {
    { { 208, 120 }, { 200, 112 }, { 204, 109 } },
    { { 208, 120 }, { 204, 109 }, { 208, 108 } },
    { { 208, 120 }, { 208, 108 }, { 212, 109 } },
    { { 208, 120 }, { 212, 109 }, { 216, 112 } },
};

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static uint32_t load_word(uint32_t address) {
    uint32_t index;

    for (index = 0u; index < memory.count; ++index) {
        if (memory.words[index].address == address)
            return memory.words[index].value;
    }
    return 0u;
}

static void store_word(uint32_t address, uint32_t value) {
    uint32_t index;

    for (index = 0u; index < memory.count; ++index) {
        if (memory.words[index].address == address) {
            memory.words[index].value = value;
            return;
        }
    }
    if (memory.count < TEST_MEMORY_CAPACITY) {
        memory.words[memory.count].address = address;
        memory.words[memory.count].value = value;
        ++memory.count;
    }
}

static void store_half(uint32_t address, uint16_t value) {
    const uint32_t aligned = address & ~UINT32_C(3);
    const uint32_t shift = (address & 2u) * 8u;
    uint32_t word = load_word(aligned);

    word &= ~(UINT32_C(0xffff) << shift);
    word |= (uint32_t)value << shift;
    store_word(aligned, word);
}

static uint32_t read_word(uint32_t address) {
    ++memory.read_count;
    return load_word(address);
}

static uint16_t read_half(uint32_t address) {
    ++memory.read_count;
    return (uint16_t)(load_word(address & ~UINT32_C(3)) >>
                      ((address & 2u) * 8u));
}

static uint8_t read_byte(uint32_t address) {
    ++memory.read_count;
    return (uint8_t)(load_word(address & ~UINT32_C(3)) >>
                     ((address & 3u) * 8u));
}

static void guest_write_word(uint32_t address, uint32_t value) {
    ++memory.guest_write_count;
    store_word(address, value);
}

static void guest_write_half(uint32_t address, uint16_t value) {
    ++memory.guest_write_count;
    store_half(address, value);
}

static void guest_write_byte(uint32_t address, uint8_t value) {
    const uint32_t aligned = address & ~UINT32_C(3);
    const uint32_t shift = (address & 3u) * 8u;
    uint32_t word = load_word(aligned);

    ++memory.guest_write_count;
    word &= ~(UINT32_C(0xff) << shift);
    word |= (uint32_t)value << shift;
    store_word(aligned, word);
}

static uint32_t triangle_packet(uint32_t triangle) {
    return UINT32_C(0x8009c6d4) + triangle * 0x1cu;
}

static uint32_t marker_packet(uint32_t marker) {
    return UINT32_C(0x8009ca98) + marker * 0x10u;
}

static void configure_source(uint32_t marker_mask) {
    uint32_t triangle;
    uint32_t vertex;
    uint32_t marker;

    store_half(UINT32_C(0x8009bd3a), 0u);
    store_word(UINT32_C(0x800523f0), UINT32_C(0x10000000));
    store_word(UINT32_C(0x8009d55c), 0u);
    store_word(UINT32_C(0x8009d564), 0u);
    store_word(UINT32_C(0x8009bcdc), 256u);
    store_word(UINT32_C(0x8009be0c), 120u);
    store_word(UINT32_C(0x8009d7f0), 1u);
    store_word(UINT32_C(0x8006f160), marker_mask);

    for (triangle = 0u; triangle < 4u; ++triangle) {
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t address = UINT32_C(0x8009a340) +
                (triangle * 3u + vertex) * 8u;

            store_word(address,
                       pack_s16(triangle_source[triangle][vertex][0],
                                triangle_source[triangle][vertex][1]));
            store_word(address + 4u,
                       pack_s16(triangle_source[triangle][vertex][2],
                                triangle_source[triangle][vertex][3]));
        }
    }

    for (marker = 0u; marker < 32u; ++marker) {
        uint16_t x = (uint16_t)(marker + 1u);
        uint16_t y = (uint16_t)(marker + 2u);

        if (marker == 24u) {
            x = 3150u;
            y = 3410u;
        } else if (marker == 31u) {
            x = UINT16_MAX;
            y = UINT16_MAX;
        }
        store_half(UINT32_C(0x8009b6f4) + marker * 4u, x);
        store_half(UINT32_C(0x8009b6f6) + marker * 4u, y);
        if (marker == 24u) {
            store_half(UINT32_C(0x8006ee60), x);
            store_half(UINT32_C(0x8006ee64), y);
        } else if (marker == 25u) {
            store_half(UINT32_C(0x8006ee82), x);
            store_half(UINT32_C(0x8006ee86), y);
        } else if (marker == 26u) {
            store_half(UINT32_C(0x8006ee78), x);
            store_half(UINT32_C(0x8006ee7a), y);
        }
    }
}

static void configure_packets(void) {
    uint32_t triangle;
    uint32_t marker;
    uint32_t word;

    for (triangle = 0u; triangle < 4u; ++triangle) {
        const uint32_t packet = triangle_packet(triangle);

        store_word(packet, UINT32_C(0x06111111) + triangle);
        for (word = 1u; word < 7u; ++word)
            store_word(packet + word * 4u,
                       UINT32_C(0xa1000000) + triangle * 0x100u + word);
    }
    store_word(UINT32_C(0x8009c5a0), UINT32_C(0x01123456));
    store_word(UINT32_C(0x8009c5a4), UINT32_C(0xe100043e));
    for (marker = 0u; marker < 32u; ++marker) {
        const uint32_t packet = marker_packet(marker);

        store_word(packet, UINT32_C(0x03000000) + marker);
        for (word = 1u; word < 4u; ++word)
            store_word(packet + word * 4u,
                       UINT32_C(0x51000000) + marker * 0x100u + word);
    }
    store_word(UINT32_C(0x8009c5e8), UINT32_C(0x09123456));
    for (word = 1u; word < 10u; ++word)
        store_word(UINT32_C(0x8009c5e8) + word * 4u,
                   UINT32_C(0xb1000000) + word);
}

static void configure_fixture(CPUState *cpu, GpuDrawState *draw,
                              uint32_t marker_mask) {
    memset(&memory, 0, sizeof(memory));
    memset(cpu, 0, sizeof(*cpu));
    memset(draw, 0, sizeof(*draw));

    configure_source(marker_mask);
    configure_packets();
    store_word(UINT32_C(0x8009be3c), TEST_CONTEXT);
    store_word(TEST_CONTEXT + 0x70u, TEST_OT);
    store_word(TEST_OT, UINT32_C(0x12abcdef));
    store_word(UINT32_C(0x1f8000bc), UINT32_C(0xabcd2222));
    store_word(UINT32_C(0x1f800100), UINT32_C(0xdcba3333));

    cpu->gpr[29] = TEST_STACK;
    cpu->gpr[31] = XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN;
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 120u << 16u;
    cpu->gte_ctrl[26] = 256u;
    cpu->read_word = read_word;
    cpu->read_half = read_half;
    cpu->read_byte = read_byte;
    cpu->write_word = guest_write_word;
    cpu->write_half = guest_write_half;
    cpu->write_byte = guest_write_byte;

    draw->right = 319u;
    draw->bottom = 239u;
    draw->dither = 1u;
}

static void write_tag(uint32_t packet, uint32_t payload_words,
                      uint32_t *predecessor) {
    store_word(packet, payload_words << 24u |
                       (*predecessor & UINT32_C(0x00ffffff)));
    *predecessor = packet;
}

static void emulate_completed_guest(uint32_t marker_mask) {
    static const int16_t panel_xy[4][2] = {
        { 208, 120 }, { 311, 120 }, { 208, 215 }, { 311, 215 },
    };
    static const uint8_t panel_uv[4][2] = {
        { 0u, 128u }, { 127u, 128u }, { 0u, 255u }, { 127u, 255u },
    };
    uint32_t predecessor = UINT32_C(0x12abcdef);
    uint32_t triangle;
    uint32_t marker;
    uint32_t panel = UINT32_C(0x8009c5e8);

    for (triangle = 0u; triangle < 4u; ++triangle) {
        const uint32_t packet = triangle_packet(triangle);

        write_tag(packet, 6u, &predecessor);
        store_word(packet + 4u, UINT32_C(0x324040ff));
        store_word(packet + 8u,
                   pack_s16(projected_xy[triangle][0][0],
                            projected_xy[triangle][0][1]));
        store_word(packet + 12u, 0u);
        store_word(packet + 16u,
                   pack_s16(projected_xy[triangle][1][0],
                            projected_xy[triangle][1][1]));
        store_word(packet + 20u, 0u);
        store_word(packet + 24u,
                   pack_s16(projected_xy[triangle][2][0],
                            projected_xy[triangle][2][1]));
    }
    write_tag(UINT32_C(0x8009c5a0), 1u, &predecessor);
    store_word(UINT32_C(0x8009c5a4), UINT32_C(0xe100043e));
    for (marker = 0u; marker < 32u; ++marker) {
        uint16_t raw_x;
        uint16_t raw_y;
        int16_t x;
        int16_t y;
        uint32_t packet;

        if ((marker_mask & (UINT32_C(1) << marker)) == 0u) continue;
        packet = marker_packet(marker);
        if (marker == 24u) {
            raw_x = 3150u;
            raw_y = 3410u;
            x = (int16_t)(raw_x / 315u + 0xcfu);
            y = (int16_t)(raw_y / 341u + 0x77u);
        } else {
            raw_x = marker == 31u ? UINT16_MAX : (uint16_t)(marker + 1u);
            raw_y = marker == 31u ? UINT16_MAX : (uint16_t)(marker + 2u);
            x = (int16_t)(uint16_t)(raw_x + 0xd0u);
            y = (int16_t)(uint16_t)(raw_y + 0x78u);
        }
        write_tag(packet, 3u, &predecessor);
        store_word(packet + 4u, UINT32_C(0x60108080));
        store_word(packet + 8u, pack_s16(x, y));
        store_word(packet + 12u, UINT32_C(0x00020002));
    }

    write_tag(panel, 9u, &predecessor);
    store_word(panel + 4u, UINT32_C(0x2e808080));
    store_word(panel + 8u, pack_s16(panel_xy[0][0], panel_xy[0][1]));
    store_word(panel + 12u,
               panel_uv[0][0] | ((uint32_t)panel_uv[0][1] << 8u) |
                   UINT32_C(0x7f900000));
    store_word(panel + 16u, pack_s16(panel_xy[1][0], panel_xy[1][1]));
    store_word(panel + 20u,
               panel_uv[1][0] | ((uint32_t)panel_uv[1][1] << 8u) |
                   UINT32_C(0x001e0000));
    store_word(panel + 24u, pack_s16(panel_xy[2][0], panel_xy[2][1]));
    store_word(panel + 28u,
               panel_uv[2][0] | ((uint32_t)panel_uv[2][1] << 8u));
    store_word(panel + 32u, pack_s16(panel_xy[3][0], panel_xy[3][1]));
    store_word(panel + 36u,
               panel_uv[3][0] | ((uint32_t)panel_uv[3][1] << 8u));
    store_word(TEST_OT, UINT32_C(0x12000000) |
                        (panel & UINT32_C(0x00ffffff)));

    store_word(UINT32_C(0x1f8000b8), 0u);
    store_word(UINT32_C(0x1f8000bc), UINT32_C(0xabcd0000));
    store_word(UINT32_C(0x1f8000f0), UINT32_C(0x00001000));
    store_word(UINT32_C(0x1f8000f4), 0u);
    store_word(UINT32_C(0x1f8000f8), UINT32_C(0x00001000));
    store_word(UINT32_C(0x1f8000fc), 0u);
    store_word(UINT32_C(0x1f800100), UINT32_C(0xdcba1000));
    store_word(UINT32_C(0x1f800104), 48u);
    store_word(UINT32_C(0x1f800108), 0u);
    store_word(UINT32_C(0x1f80010c), 256u);
}

static void enter_finish(CPUState *cpu, uint32_t saved_ra) {
    cpu->gpr[29] = TEST_STACK - 0x38u;
    store_word(cpu->gpr[29] + 0x30u, saved_ra);
}

static int test_representative_success_is_exact_and_read_only(void) {
    const uint32_t marker_mask = UINT32_C(1) |
        (UINT32_C(1) << 24u) | (UINT32_C(1) << 31u);
    CPUState cpu;
    GpuDrawState draw;
    XgWorldMinimapShadowSnapshot snapshot;

    configure_fixture(&cpu, &draw, marker_mask);
    xg_world_minimap_shadow_reset();
    CHECK(xg_world_minimap_shadow_begin(&cpu, 17u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_OK);
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(snapshot.pending && !snapshot.blocked);
    CHECK(snapshot.source_capture_count == 1u);
    CHECK(snapshot.source_read_count == 39u);
    CHECK(snapshot.last_active_marker_count == 3u);
    CHECK(snapshot.last_ordering_count == 9u);
    CHECK(snapshot.last_context == TEST_CONTEXT);
    CHECK(snapshot.last_ot_address == TEST_OT);

    emulate_completed_guest(marker_mask);
    enter_finish(&cpu, XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN);
    CHECK(xg_world_minimap_shadow_finish(&cpu) ==
          XG_WORLD_MINIMAP_SHADOW_OK);
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(!snapshot.pending && !snapshot.blocked);
    CHECK(snapshot.completion_count == 1u);
    CHECK(snapshot.invocation_match_count == 1u);
    CHECK(snapshot.invocation_mismatch_count == 0u);
    CHECK(snapshot.g3_match_count == 4u);
    CHECK(snapshot.draw_mode_match_count == 1u);
    CHECK(snapshot.marker_match_count == 3u);
    CHECK(snapshot.inactive_marker_match_count == 29u);
    CHECK(snapshot.panel_match_count == 1u);
    CHECK(snapshot.payload_mismatch_count == 0u);
    CHECK(snapshot.tag_mismatch_count == 0u);
    CHECK(snapshot.ot_mismatch_count == 0u);
    CHECK(snapshot.scratch_mismatch_count == 0u);
    CHECK(snapshot.first_mismatch.kind ==
          XG_WORLD_MINIMAP_SHADOW_MISMATCH_NONE);
    CHECK(memory.guest_write_count == 0u);
    return 1;
}

static int test_payload_and_inactive_marker_mismatches_are_attributed(void) {
    const uint32_t marker_mask = UINT32_C(1) |
        (UINT32_C(1) << 24u) | (UINT32_C(1) << 31u);
    CPUState cpu;
    GpuDrawState draw;
    XgWorldMinimapShadowSnapshot snapshot;

    configure_fixture(&cpu, &draw, marker_mask);
    xg_world_minimap_shadow_reset();
    CHECK(xg_world_minimap_shadow_begin(&cpu, 18u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_OK);
    emulate_completed_guest(marker_mask);
    store_word(triangle_packet(0u) + 8u,
               load_word(triangle_packet(0u) + 8u) ^ 1u);
    enter_finish(&cpu, XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN);
    CHECK(xg_world_minimap_shadow_finish(&cpu) ==
          XG_WORLD_MINIMAP_SHADOW_MISMATCH);
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(snapshot.invocation_mismatch_count == 1u);
    CHECK(snapshot.g3_mismatch_count == 1u);
    CHECK(snapshot.payload_mismatch_count == 1u);
    CHECK(snapshot.first_mismatch.kind ==
          XG_WORLD_MINIMAP_SHADOW_MISMATCH_G3_PAYLOAD);
    CHECK(snapshot.first_mismatch.address == triangle_packet(0u) + 8u);
    CHECK(snapshot.first_mismatch.word_index == 2u);
    CHECK(snapshot.first_mismatch.source_index == 0u);

    configure_fixture(&cpu, &draw, marker_mask);
    xg_world_minimap_shadow_reset();
    CHECK(xg_world_minimap_shadow_begin(&cpu, 19u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_OK);
    emulate_completed_guest(marker_mask);
    store_word(marker_packet(1u) + 4u,
               load_word(marker_packet(1u) + 4u) ^ 1u);
    enter_finish(&cpu, XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN);
    CHECK(xg_world_minimap_shadow_finish(&cpu) ==
          XG_WORLD_MINIMAP_SHADOW_MISMATCH);
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(snapshot.inactive_marker_mutation_count == 1u);
    CHECK(snapshot.first_mismatch.kind ==
          XG_WORLD_MINIMAP_SHADOW_MISMATCH_INACTIVE_MARKER_MUTATION);
    CHECK(snapshot.first_mismatch.address == marker_packet(1u) + 4u);
    CHECK(snapshot.first_mismatch.source_index == 1u);

    configure_fixture(&cpu, &draw, marker_mask);
    xg_world_minimap_shadow_reset();
    CHECK(xg_world_minimap_shadow_begin(&cpu, 20u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_OK);
    emulate_completed_guest(marker_mask);
    store_word(marker_packet(0u), load_word(marker_packet(0u)) ^ 1u);
    store_word(TEST_OT, load_word(TEST_OT) ^ 1u);
    store_word(UINT32_C(0x8009be3c), TEST_CONTEXT + 4u);
    store_word(TEST_CONTEXT + 0x70u, TEST_OT + 4u);
    enter_finish(&cpu, XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN);
    CHECK(xg_world_minimap_shadow_finish(&cpu) ==
          XG_WORLD_MINIMAP_SHADOW_MISMATCH);
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(snapshot.context_mismatch_count == 1u);
    CHECK(snapshot.ot_pointer_mismatch_count == 1u);
    CHECK(snapshot.tag_mismatch_count == 1u);
    CHECK(snapshot.ot_mismatch_count == 1u);
    CHECK(snapshot.first_mismatch.kind ==
          XG_WORLD_MINIMAP_SHADOW_MISMATCH_CONTEXT);
    CHECK(memory.guest_write_count == 0u);
    return 1;
}

static int test_caller_and_lifecycle_fail_closed(void) {
    const uint32_t marker_mask = UINT32_C(1);
    CPUState cpu;
    GpuDrawState draw;
    XgWorldMinimapShadowSnapshot snapshot;

    configure_fixture(&cpu, &draw, marker_mask);
    xg_world_minimap_shadow_reset();
    cpu.gpr[31] -= 4u;
    CHECK(xg_world_minimap_shadow_begin(&cpu, 20u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_NOT_APPLICABLE);
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(snapshot.rejected_caller_count == 1u);
    CHECK(!snapshot.pending && !snapshot.blocked);

    cpu.gpr[31] = XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN;
    CHECK(xg_world_minimap_shadow_begin(&cpu, 20u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_OK);
    CHECK(xg_world_minimap_shadow_begin(&cpu, 20u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR);
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocked && !snapshot.pending);
    CHECK(snapshot.blocker == XG_WORLD_MINIMAP_SHADOW_BLOCK_UNEXPECTED_BEGIN);

    configure_fixture(&cpu, &draw, marker_mask);
    xg_world_minimap_shadow_reset();
    CHECK(xg_world_minimap_shadow_begin(&cpu, 21u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_OK);
    xg_world_minimap_shadow_invalidate();
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocked && !snapshot.pending);
    CHECK(snapshot.invalidation_count == 1u);
    CHECK(snapshot.blocker ==
          XG_WORLD_MINIMAP_SHADOW_BLOCK_LIFECYCLE_INVALIDATED);

    configure_fixture(&cpu, &draw, marker_mask);
    xg_world_minimap_shadow_reset();
    CHECK(xg_world_minimap_shadow_begin(&cpu, 22u, &draw) ==
          XG_WORLD_MINIMAP_SHADOW_OK);
    emulate_completed_guest(marker_mask);
    enter_finish(&cpu, XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN - 4u);
    CHECK(xg_world_minimap_shadow_finish(&cpu) ==
          XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR);
    xg_world_minimap_shadow_snapshot(&snapshot);
    CHECK(snapshot.blocked && !snapshot.pending);
    CHECK(snapshot.saved_return_mismatch_count == 1u);
    CHECK(snapshot.blocker ==
          XG_WORLD_MINIMAP_SHADOW_BLOCK_SAVED_RETURN_ADDRESS);
    CHECK(snapshot.first_mismatch.kind ==
          XG_WORLD_MINIMAP_SHADOW_MISMATCH_SAVED_RETURN_ADDRESS);
    CHECK(memory.guest_write_count == 0u);
    return 1;
}

int main(void) {
    return test_representative_success_is_exact_and_read_only() &&
            test_payload_and_inactive_marker_mismatches_are_attributed() &&
            test_caller_and_lifecycle_fail_closed()
        ? 0
        : 1;
}
