#include "xg_world_decorations.h"
#include "xg_world_decorations_source_capture.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__,   \
                    #condition);                                               \
            return 0;                                                          \
        }                                                                      \
    } while (0)

enum {
    TEST_MEMORY_BASE = 0x80050000u,
    TEST_MEMORY_SIZE = 0x60000u,
    TEST_POSITIONS = 0x8009e000u,
    TEST_SOURCE_DESCRIPTORS = 0x8009e800u,
    TEST_CONTEXT = 0x8009f800u,
    TEST_PACKET_BASE = 0x800a0000u,
    TEST_OT_BASE = 0x800a8000u,
};

typedef struct TestReader {
    uint8_t memory[TEST_MEMORY_SIZE];
    uint32_t fail_address;
    bool touched_forbidden_source;
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

static bool forbidden_source_address(uint32_t address) {
    return (address >= UINT32_C(0x1f800000) &&
            address < UINT32_C(0x1f800080)) ||
           (address >= UINT32_C(0x800a0000) && address < UINT32_C(0x800b0000));
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    TestReader *reader = context;
    uint32_t offset;

    if (reader == NULL || out_value == NULL)
        return false;
    reader->touched_forbidden_source |= forbidden_source_address(address);
    if (address == reader->fail_address || address < TEST_MEMORY_BASE ||
        address > TEST_MEMORY_BASE + TEST_MEMORY_SIZE - 2u)
        return false;
    offset = address - TEST_MEMORY_BASE;
    *out_value = (uint16_t)reader->memory[offset] |
                 ((uint16_t)reader->memory[offset + 1u] << 8u);
    return true;
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
    TestReader *reader = context;
    uint16_t low;
    uint16_t high;

    if (reader == NULL || out_value == NULL)
        return false;
    reader->touched_forbidden_source |= forbidden_source_address(address);
    if (address == reader->fail_address)
        return false;
    if (!read_u16(context, address, &low) ||
        !read_u16(context, address + 2u, &high))
        return false;
    *out_value = low | ((uint32_t)high << 16u);
    return true;
}

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static void store_matrix(TestReader *reader, uint32_t address,
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

static void store_position(TestReader *reader, uint32_t index, int16_t x,
                           int16_t y, int16_t z) {
    const uint32_t address = TEST_POSITIONS + index * 8u;

    store_u32(reader, address, pack_s16(x, y));
    store_u32(reader, address + 4u, pack_s16(z, 0));
}

static void configure_reader(TestReader *reader, uint32_t position_count) {
    uint32_t index;

    memset(reader, 0, sizeof(*reader));
    store_matrix(reader, UINT32_C(0x8009c808), 512);
    store_matrix(reader, UINT32_C(0x8009a180), 0);
    store_u16(reader, UINT32_C(0x8009bd3c), 1024u);
    store_u32(reader, UINT32_C(0x800533f0), UINT32_C(0x00001000));
    store_u32(reader, UINT32_C(0x8009be28), 0u);
    store_u32(reader, UINT32_C(0x8009be30), 0u);
    store_u32(reader, UINT32_C(0x8009d160), 32u);
    store_u32(reader, UINT32_C(0x8009d2b4), 32u);
    store_u32(reader, UINT32_C(0x8009d7cc), 0u);
    for (index = 0u; index < XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT; ++index) {
        store_u16(reader, UINT32_C(0x8009d478) + index * 2u,
                  (uint16_t)(((0x1f0u + index) << 6u) | 0x0fu));
    }
    for (index = 0u; index < position_count; ++index)
        store_position(reader, index, 0, 0, -2048);
}

static XgWorldDecorationsCaptureResult
capture_source(TestReader *context, uint32_t position_count,
               XgWorldDecorationsCapture *capture) {
    const XgWorldDecorationsCaptureRequest request = {
        .authentication_generation = 17u,
        .caller_return = UINT32_C(0x8008639c),
        .position_address = TEST_POSITIONS,
        .position_count = position_count,
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 120 << 16,
        .projection_distance = 256u,
        .raster =
            {
                .draw_area_right = 319u,
                .draw_area_bottom = 239u,
            },
        .helper_arguments_authenticated = true,
        .projection_state_authenticated = true,
    };
    const XgWorldDecorationsAuthenticatedReader reader = {
        .context = context,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 17u,
        .authenticated = true,
    };

    return xg_world_decorations_source_capture(&request, &reader, capture);
}

static void configure_native_outer(TestReader *reader, bool has_helper) {
    uint32_t cell;

    store_u32(reader, UINT32_C(0x8009c7ec), TEST_SOURCE_DESCRIPTORS);
    store_u16(reader, UINT32_C(0x8009c838), 0u);
    store_u16(reader, UINT32_C(0x8009c83c), 0u);
    for (cell = 0u; cell < XG_WORLD_DECORATIONS_NATIVE_GRID_CELL_COUNT;
         ++cell) {
        store_u16(reader, UINT32_C(0x8009d618) + cell * 2u, UINT16_MAX);
    }
    if (has_helper) {
        store_u16(reader, UINT32_C(0x8009d618), 0u);
        store_u16(reader, UINT32_C(0x8009d570), 3u);
        store_u32(reader, TEST_SOURCE_DESCRIPTORS + 3u * 8u,
                  TEST_POSITIONS);
        store_u32(reader, TEST_SOURCE_DESCRIPTORS + 3u * 8u + 4u, 1u);
    }
    store_u32(reader, UINT32_C(0x8009d7f0), 0u);
    store_u32(reader, UINT32_C(0x8009d7e8), TEST_PACKET_BASE);
    store_u32(reader, UINT32_C(0x8009be3c), TEST_CONTEXT);
    store_u32(reader, TEST_CONTEXT + 0x70u, TEST_OT_BASE);
    store_u32(reader, XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS, 7u);
}

static XgWorldDecorationsNativeRequest native_request(void) {
    return (XgWorldDecorationsNativeRequest){
        .authentication_generation = 17u,
        .entry_pc = XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
        .caller_return = XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN,
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 120 << 16,
        .projection_distance = 256u,
        .raster =
            {
                .draw_area_right = 319u,
                .draw_area_bottom = 239u,
            },
        .projection_state_authenticated = true,
    };
}

static uint32_t packed_xy(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16u);
}

static int test_capture_build_ft4_and_culls(void) {
    TestReader reader;
    XgWorldDecorationsCapture capture;
    XgWorldDecorationsRecord records[XG_WORLD_DECORATIONS_PACKET_CAPACITY] = {
        0};
    XgWorldDecorationsRecord *record;
    uint32_t count = 7u;

    configure_reader(&reader, 5u);
    store_position(&reader, 1u, 2000, 0, -2048);
    store_position(&reader, 2u, 0, 1000, -2048);
    store_position(&reader, 3u, 0, 0, -3072);
    store_position(&reader, 4u, 0, 0, 1024);
    CHECK(capture_source(&reader, 5u, &capture) ==
          XG_WORLD_DECORATIONS_CAPTURE_OK);
    CHECK(capture.authenticated && capture.sealed);
    CHECK(capture.authenticated_read_count == 49u);
    CHECK(capture.authenticated_read_bytes == 162u);
    CHECK(!reader.touched_forbidden_source);
    CHECK(capture.position_address == TEST_POSITIONS);
    CHECK(capture.trig_address == UINT32_C(0x800533f0));
    CHECK(capture.angle == 1024);
    CHECK(capture.source.positions[0].z == -2048);
    CHECK(capture.source.positions[1].x == 2000);
    CHECK(capture.source.positions[2].y == 1000);
    CHECK(capture.source.positions[3].z == -3072);
    CHECK(capture.source.positions[4].z == 1024);
    CHECK(capture.source.camera_matrix.rotation[0][0] == 4096);
    CHECK(capture.source.camera_matrix.translation[2] == 512);
    CHECK(capture.source.decoration_matrix.rotation[0][0] == 0);
    CHECK(capture.source.decoration_matrix.rotation[0][1] == 4096);
    CHECK(capture.source.decoration_matrix.rotation[1][0] == -4096);
    CHECK(capture.source.decoration_matrix.rotation[1][1] == 0);
    CHECK(capture.source.vertices[0].x == -24);
    CHECK(capture.source.vertices[0].y == -72);
    CHECK(capture.source.uv[0][0] == 0u && capture.source.uv[0][1] == 64u);
    CHECK(capture.source.uv[3][0] == 31u && capture.source.uv[3][1] == 111u);
    CHECK(capture.source.material.tpage == 0x001eu);
    CHECK(capture.source.material.texture_page_x == 14u);
    CHECK(capture.source.material.texture_page_y == 1u);
    CHECK(!capture.source.material.semi_transparent);

    CHECK(xg_world_decorations_build(&capture.source, records,
                                     XG_WORLD_DECORATIONS_PACKET_CAPACITY,
                                     &count) == XG_WORLD_DECORATIONS_OK);
    CHECK(count == 8u);
    record = &records[7];
    CHECK(record->source_index == 0u);
    CHECK(record->packet_index == 7u);
    CHECK(record->tag_payload_word_count == 9u);
    CHECK(record->third_depth == 2560u);
    CHECK(record->ordering_bucket == 160u);
    CHECK(record->ft4_vertices[0].x == 152);
    CHECK(record->ft4_vertices[0].y == 122);
    CHECK(record->ft4_vertices[1].x == 152);
    CHECK(record->ft4_vertices[1].y == 117);
    CHECK(record->ft4_vertices[2].x == 160);
    CHECK(record->ft4_vertices[2].y == 122);
    CHECK(record->ft4_vertices[3].x == 160);
    CHECK(record->ft4_vertices[3].y == 117);
    CHECK(record->clut == capture.source.depth_clut[7]);
    CHECK(record->primitive.material.clut_x == 240u);
    CHECK(record->primitive.material.clut_y == 503u);
    CHECK(record->primitive.triangles[0].vertices[0].x == 152 * 65536);
    CHECK(record->primitive.triangles[0].vertices[1].y == 117 * 65536);
    CHECK(record->primitive.triangles[1].vertices[0].x == 160 * 65536);
    CHECK(record->primitive.triangles[1].vertices[2].y == 117 * 65536);

    CHECK(record->ft4_payload_words[0] == UINT32_C(0x2c808080));
    CHECK(record->ft4_payload_words[1] == packed_xy(152, 122));
    CHECK(record->ft4_payload_words[2] ==
          (((uint32_t)record->clut << 16u) | UINT32_C(0x00004000)));
    CHECK(record->ft4_payload_words[3] == packed_xy(152, 117));
    CHECK(record->ft4_payload_words[4] == UINT32_C(0x001e401f));
    CHECK(record->ft4_payload_words[5] == packed_xy(160, 122));
    CHECK(record->ft4_payload_words[6] == UINT32_C(0x00006f00));
    CHECK(record->ft4_payload_words[7] == packed_xy(160, 117));
    CHECK(record->ft4_payload_words[8] == UINT32_C(0x00006f1f));

    capture.source.screen_x_cull_margin = 53;
    count = 7u;
    CHECK(xg_world_decorations_build(&capture.source, records,
                                     XG_WORLD_DECORATIONS_PACKET_CAPACITY,
                                     &count) == XG_WORLD_DECORATIONS_OK);
    CHECK(count == 9u);
    return 1;
}

static int test_packet_cap_and_count_side_effect(void) {
    TestReader reader;
    XgWorldDecorationsCapture capture;
    XgWorldDecorationsRecord records[XG_WORLD_DECORATIONS_PACKET_CAPACITY] = {
        0};
    uint32_t count = 0u;

    configure_reader(&reader, XG_WORLD_DECORATIONS_POSITION_CAPACITY);
    CHECK(capture_source(&reader, XG_WORLD_DECORATIONS_POSITION_CAPACITY,
                         &capture) == XG_WORLD_DECORATIONS_CAPTURE_OK);
    CHECK(capture.authenticated_read_count ==
          XG_WORLD_DECORATIONS_MAX_AUTHENTICATED_READS);
    CHECK(capture.authenticated_read_bytes ==
          XG_WORLD_DECORATIONS_MAX_AUTHENTICATED_BYTES);
    CHECK(xg_world_decorations_build(&capture.source, records,
                                     XG_WORLD_DECORATIONS_PACKET_CAPACITY,
                                     &count) == XG_WORLD_DECORATIONS_OK);
    CHECK(count == XG_WORLD_DECORATIONS_PACKET_CAPACITY);
    CHECK(records[0].packet_index == 0u);
    CHECK(records[511].packet_index == 511u);
    CHECK(records[511].source_index == 511u);

    records[0].source_index = UINT32_MAX;
    capture.source.position_count = 1u;
    CHECK(xg_world_decorations_build(&capture.source, records,
                                     XG_WORLD_DECORATIONS_PACKET_CAPACITY,
                                     &count) == XG_WORLD_DECORATIONS_OK);
    CHECK(count == XG_WORLD_DECORATIONS_PACKET_CAPACITY);
    CHECK(records[0].source_index == UINT32_MAX);
    return 1;
}

static int test_capture_fails_closed(void) {
    TestReader context;
    XgWorldDecorationsCapture capture;
    XgWorldDecorationsCaptureRequest request = {
        .authentication_generation = 17u,
        .caller_return = UINT32_C(0x8008639c),
        .position_address = TEST_POSITIONS,
        .position_count = 1u,
        .projection_distance = 256u,
        .raster = {.draw_area_right = 319u, .draw_area_bottom = 239u},
        .helper_arguments_authenticated = true,
        .projection_state_authenticated = true,
    };
    XgWorldDecorationsAuthenticatedReader reader = {
        .context = &context,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 17u,
        .authenticated = true,
    };

    configure_reader(&context, 1u);
    reader.authenticated = false;
    CHECK(xg_world_decorations_source_capture(&request, &reader, &capture) ==
          XG_WORLD_DECORATIONS_CAPTURE_UNAUTHENTICATED);
    reader.authenticated = true;
    request.helper_arguments_authenticated = false;
    CHECK(xg_world_decorations_source_capture(&request, &reader, &capture) ==
          XG_WORLD_DECORATIONS_CAPTURE_INVALID_ARGUMENT);
    request.helper_arguments_authenticated = true;
    request.caller_return += 4u;
    CHECK(xg_world_decorations_source_capture(&request, &reader, &capture) ==
          XG_WORLD_DECORATIONS_CAPTURE_INVALID_ARGUMENT);
    request.caller_return -= 4u;
    context.fail_address = UINT32_C(0x8009c808);
    CHECK(xg_world_decorations_source_capture(&request, &reader, &capture) ==
          XG_WORLD_DECORATIONS_CAPTURE_READ_FAILED);
    context.fail_address = 0u;
    store_u16(&context, UINT32_C(0x8009d478) + 2u, 0u);
    CHECK(xg_world_decorations_source_capture(&request, &reader, &capture) ==
          XG_WORLD_DECORATIONS_CAPTURE_SOURCE_MISMATCH);
    CHECK(!capture.authenticated && !capture.sealed);
    return 1;
}

static int test_native_outer_preparation_contract(void) {
    TestReader context;
    XgWorldDecorationsRecord records[XG_WORLD_DECORATIONS_PACKET_CAPACITY] = {
        0};
    XgWorldDecorationsNativePreparation preparation;
    XgWorldDecorationsNativeRequest request = native_request();
    XgWorldDecorationsAuthenticatedReader reader = {
        .context = &context,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 17u,
        .authenticated = true,
    };

    configure_reader(&context, 1u);
    configure_native_outer(&context, true);
    CHECK(xg_world_decorations_native_prepare(
              &request, &reader, records,
              XG_WORLD_DECORATIONS_PACKET_CAPACITY, &preparation) ==
          XG_WORLD_DECORATIONS_NATIVE_OK);
    CHECK(preparation.authenticated && preparation.sealed);
    CHECK(preparation.packet_base == TEST_PACKET_BASE);
    CHECK(preparation.ot_base == TEST_OT_BASE);
    CHECK(preparation.helper_count == 1u);
    CHECK(preparation.record_count == 1u);
    CHECK(preparation.final_shared_count == 1u);
    CHECK(preparation.shared_count_write_mask == UINT32_MAX);
    CHECK(preparation.continuation ==
          XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN);
    CHECK(preparation.authenticated_read_count == 77u);
    CHECK(preparation.authenticated_read_bytes == 218u);
    CHECK(!context.touched_forbidden_source);
    CHECK(preparation.scratch.vertices[0].x == -24);
    CHECK(preparation.scratch.vertices[0].y == -72);
    CHECK(preparation.scratch.vertices[3].x == 24);
    CHECK(preparation.scratch.vertices[3].y == 0);
    CHECK(preparation.scratch.camera_matrix.translation[2] == 512);
    CHECK(preparation.scratch.decoration_matrix.rotation[0][0] == 0);
    CHECK(preparation.scratch.decoration_matrix.rotation[0][1] == 4096);
    CHECK(preparation.scratch.decoration_matrix.rotation[1][0] == -4096);
    CHECK(preparation.scratch.depth_clut[7] ==
          (uint16_t)((0x1f7u << 6u) | 0x0fu));
    CHECK(records[0].packet_index == 0u);
    CHECK(records[0].source_index == 0u);
    CHECK(records[0].ordering_bucket == 160u);
    CHECK(records[0].ft4_payload_words[1] == packed_xy(152, 122));
    CHECK(records[0].ft4_payload_words[7] == packed_xy(160, 117));

    configure_reader(&context, 1u);
    configure_native_outer(&context, false);
    CHECK(xg_world_decorations_native_prepare(
              &request, &reader, records,
              XG_WORLD_DECORATIONS_PACKET_CAPACITY, &preparation) ==
          XG_WORLD_DECORATIONS_NATIVE_OK);
    CHECK(preparation.helper_count == 0u);
    CHECK(preparation.record_count == 0u);
    CHECK(preparation.packet_base == 0u && preparation.ot_base == 0u);
    CHECK(preparation.final_shared_count == 0u);
    CHECK(preparation.shared_count_write_mask == UINT32_C(0x0000ffff));
    CHECK(preparation.authenticated_read_count == 62u);
    CHECK(preparation.authenticated_read_bytes == 160u);
    return 1;
}

static int test_native_outer_preparation_fails_closed(void) {
    TestReader context;
    XgWorldDecorationsRecord records[XG_WORLD_DECORATIONS_PACKET_CAPACITY] = {
        0};
    XgWorldDecorationsNativePreparation preparation;
    XgWorldDecorationsNativeRequest request = native_request();
    XgWorldDecorationsAuthenticatedReader reader = {
        .context = &context,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 17u,
        .authenticated = true,
    };

    configure_reader(&context, 1u);
    configure_native_outer(&context, true);
    request.caller_return += 4u;
    CHECK(xg_world_decorations_native_prepare(
              &request, &reader, records,
              XG_WORLD_DECORATIONS_PACKET_CAPACITY, &preparation) ==
          XG_WORLD_DECORATIONS_NATIVE_INVALID_ARGUMENT);
    CHECK(!preparation.authenticated && !preparation.sealed);

    request = native_request();
    reader.authentication_generation = 18u;
    CHECK(xg_world_decorations_native_prepare(
              &request, &reader, records,
              XG_WORLD_DECORATIONS_PACKET_CAPACITY, &preparation) ==
          XG_WORLD_DECORATIONS_NATIVE_UNAUTHENTICATED);
    CHECK(!preparation.sealed);

    reader.authentication_generation = 17u;
    context.fail_address = UINT32_C(0x8009d618);
    CHECK(xg_world_decorations_native_prepare(
              &request, &reader, records,
              XG_WORLD_DECORATIONS_PACKET_CAPACITY, &preparation) ==
          XG_WORLD_DECORATIONS_NATIVE_READ_FAILED);
    CHECK(!preparation.sealed);

    context.fail_address = 0u;
    store_u32(&context, XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS,
              UINT32_C(0x00010000));
    CHECK(xg_world_decorations_native_prepare(
              &request, &reader, records,
              XG_WORLD_DECORATIONS_PACKET_CAPACITY, &preparation) ==
          XG_WORLD_DECORATIONS_NATIVE_INVALID_OUTPUT);
    CHECK(!preparation.authenticated && !preparation.sealed);
    return 1;
}

int main(void) {
    return test_capture_build_ft4_and_culls() &&
                   test_packet_cap_and_count_side_effect() &&
                   test_capture_fails_closed() &&
                   test_native_outer_preparation_contract() &&
                   test_native_outer_preparation_fails_closed()
               ? 0
               : 1;
}
