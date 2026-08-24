#include "xg_world_effects.h"
#include "xg_world_effects_source_capture.h"

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

/* Guest addresses exceed INT_MAX — enum constants would take type int on
 * clang/MSVC-ABI targets and sign-extend in 64-bit comparisons; UINT32_C
 * keeps them unsigned everywhere. */
#define TEST_MEMORY_BASE UINT32_C(0x80050000)
#define TEST_MEMORY_SIZE UINT32_C(0x60000)
#define TEST_PARTICLES UINT32_C(0x8009e000)

typedef struct TestReader {
    uint8_t memory[TEST_MEMORY_SIZE];
} TestReader;

static void store_u16(TestReader *reader, uint32_t address, uint16_t value) {
    const uint32_t offset = address - TEST_MEMORY_BASE;

    reader->memory[offset] = (uint8_t)value;
    reader->memory[offset + 1u] = (uint8_t)(value >> 8u);
}

static void store_u32(TestReader *reader, uint32_t address, uint32_t value) {
    store_u16(reader, address, (uint16_t)value);
    store_u16(reader, address + 2u, (uint16_t)(value >> 16u));
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    TestReader *reader = context;
    uint32_t offset;

    if (reader == NULL || out_value == NULL || address < TEST_MEMORY_BASE ||
        address + 2u > TEST_MEMORY_BASE + TEST_MEMORY_SIZE)
        return false;
    offset = address - TEST_MEMORY_BASE;
    *out_value = (uint16_t)reader->memory[offset] |
        ((uint16_t)reader->memory[offset + 1u] << 8u);
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

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static void store_identity_matrix(TestReader *reader, uint32_t address,
                                  int32_t translation_z) {
    store_u32(reader, address, pack_s16(4096, 0));
    store_u32(reader, address + 4u, pack_s16(0, 0));
    store_u32(reader, address + 8u, pack_s16(4096, 0));
    store_u32(reader, address + 12u, pack_s16(0, 0));
    store_u32(reader, address + 16u, pack_s16(4096, 0));
    store_u32(reader, address + 20u, 0u);
    store_u32(reader, address + 24u, 0u);
    store_u32(reader, address + 28u, (uint32_t)translation_z);
}

static void configure_reader(TestReader *reader) {
    static const XgHost3dVector vertices[4] = {
        { -32, -32, 0, 0u }, { 32, -32, 0, 0u },
        { -32, 32, 0, 0u }, { 32, 32, 0, 0u },
    };
    static const uint16_t uv[4] = {
        0x0000u, 0x003fu, 0x3f00u, 0x3f3fu,
    };
    uint32_t index;

    memset(reader, 0, sizeof(*reader));
    store_u32(reader, UINT32_C(0x8009bdf4), TEST_PARTICLES);
    store_identity_matrix(reader, UINT32_C(0x8009c808), 512);
    store_identity_matrix(reader, UINT32_C(0x8009a180), 0);
    for (index = 0u; index < 4u; ++index) {
        const uint32_t address = UINT32_C(0x8009b040) + 0x20u + index * 8u;

        store_u32(reader, address, pack_s16(vertices[index].x,
                                             vertices[index].y));
        store_u32(reader, address + 4u,
                  pack_s16(vertices[index].z, (int16_t)vertices[index].pad));
        store_u16(reader, UINT32_C(0x8009aff0) + 8u + index * 2u, uv[index]);
    }
    store_u16(reader, TEST_PARTICLES + 6u, 1u);
    store_u32(reader, TEST_PARTICLES + 8u, 0u);
    store_u32(reader, TEST_PARTICLES + 0x0cu, 0u);
    store_u32(reader, TEST_PARTICLES + 0x10u, 0u);
    store_u32(reader, TEST_PARTICLES + 0x38u,
              (uint32_t)4096u | ((uint32_t)4096u << 16u));
    store_u32(reader, TEST_PARTICLES + 0x40u, UINT32_C(0x00302010));
    store_u32(reader, TEST_PARTICLES + 0x44u, 0u);
    store_u16(reader, TEST_PARTICLES + 0x48u, 0x0123u);
}

static int test_capture_and_build_single_effect(void) {
    TestReader reader;
    XgWorldEffectsCapture capture;
    XgWorldEffectsRecord records[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    XgWorldEffectsRecord rejected[1];
    uint32_t count = 0u;
    uint32_t rejected_count = 0u;
    const XgWorldEffectsCaptureRequest request = {
        .authentication_generation = 7u,
        .caller_return = UINT32_C(0x80071aa8),
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 120 << 16,
        .projection_distance = 256u,
        .raster = {
            .draw_area_right = 319u,
            .draw_area_bottom = 239u,
        },
        .projection_state_authenticated = true,
    };
    const XgWorldEffectsAuthenticatedReader authenticated_reader = {
        .context = &reader,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 7u,
        .authenticated = true,
    };

    configure_reader(&reader);
    CHECK(xg_world_effects_source_capture(
              &request, &authenticated_reader, &capture) ==
          XG_WORLD_EFFECTS_CAPTURE_OK);
    CHECK(capture.active_source_count == 1u);
    CHECK(capture.authenticated_read_count == 385u);
    CHECK(capture.authenticated_read_bytes == 1024u);
    CHECK(capture.authenticated && capture.sealed);
    CHECK(xg_world_effects_build(
              &capture.source, records, XG_WORLD_EFFECTS_SOURCE_CAPACITY,
              &count) == XG_WORLD_EFFECTS_OK);
    CHECK(count == 1u);
    CHECK(records[0].source_index == 0u);
    CHECK(records[0].fourth_depth == 512u);
    CHECK(records[0].ordering_bucket == 32u);
    CHECK(records[0].vertices[0].x == 144);
    CHECK(records[0].vertices[0].y == 104);
    CHECK(records[0].vertices[3].x == 176);
    CHECK(records[0].vertices[3].y == 136);
    CHECK(!records[0].primitive.triangles[0].vertices[0]
               .projective_position);
    CHECK(records[0].material_word == UINT32_C(0x2e302010));
    CHECK(records[0].tpage == 0x0123u);
    CHECK(records[0].clut == 0x7fd0u);
    CHECK(records[0].uv[3] == 0x3f3fu);

    capture.source.particles[0].position[0] = 400 << 12;
    capture.source.screen_x_cull_margin = 0;
    CHECK(xg_world_effects_build_with_temporal(
              &capture.source, records, XG_WORLD_EFFECTS_SOURCE_CAPACITY,
              &count, rejected, 1u, &rejected_count) == XG_WORLD_EFFECTS_OK);
    CHECK(count == 0u);
    CHECK(rejected_count == 1u);
    CHECK(!rejected[0].accepted);
    CHECK(rejected[0].cull == XG_WORLD_EFFECTS_CULL_SCREEN);
    CHECK(rejected[0].source_index == 0u);
    CHECK(rejected[0].primitive.triangle_count == 2u);
    CHECK(!rejected[0].primitive.triangles[0].vertices[0]
               .projective_position);
    capture.source.screen_x_cull_margin = 53;
    CHECK(xg_world_effects_build(
              &capture.source, records, XG_WORLD_EFFECTS_SOURCE_CAPACITY,
              &count) == XG_WORLD_EFFECTS_OK);
    CHECK(count == 1u);
    return 1;
}

static int test_capture_fails_closed(void) {
    TestReader reader;
    XgWorldEffectsCapture capture;
    XgWorldEffectsCaptureRequest request = {
        .authentication_generation = 7u,
        .caller_return = UINT32_C(0x80071aa8),
        .projection_distance = 256u,
        .raster = { .draw_area_right = 319u, .draw_area_bottom = 239u },
        .projection_state_authenticated = true,
    };
    XgWorldEffectsAuthenticatedReader authenticated_reader = {
        .context = &reader,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 7u,
        .authenticated = true,
    };

    configure_reader(&reader);
    authenticated_reader.authenticated = false;
    CHECK(xg_world_effects_source_capture(
              &request, &authenticated_reader, &capture) ==
          XG_WORLD_EFFECTS_CAPTURE_UNAUTHENTICATED);
    authenticated_reader.authenticated = true;
    request.caller_return += 4u;
    CHECK(xg_world_effects_source_capture(
              &request, &authenticated_reader, &capture) ==
          XG_WORLD_EFFECTS_CAPTURE_INVALID_ARGUMENT);
    return 1;
}

int main(void) {
    int ok = 1;

    ok &= test_capture_and_build_single_effect();
    ok &= test_capture_fails_closed();
    return ok ? 0 : 1;
}
