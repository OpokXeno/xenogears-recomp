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

#define TEST_RESOURCE UINT32_C(0x800a0000)
#define TEST_CONTEXT UINT32_C(0x80008000)
#define TEST_OT_BASE UINT32_C(0x80010000)
#define TEST_PACKET_BASE UINT32_C(0x80011000)

typedef struct TestReader {
    bool touched_forbidden_output;
    bool invalid_resource_pointer;
    bool invalid_native_output;
    bool hide_lower_quadrants;
} TestReader;

static uint32_t terrain_word(uint32_t index) {
    const int32_t height = (index & 1u) != 0u ? -1 : 1;

    return (uint8_t)height | ((index % 7u) << 8u) |
        ((index & 1u) << 11u) | ((index & 1u) << 12u) |
        ((index % 4u) << 13u) | (((index >> 2u) & 1u) << 15u) |
        ((index & 0x0fu) << 16u) | (((index / 9u) & 0x0fu) << 20u);
}

static void expected_uv(uint32_t word, uint16_t uv[4]) {
    const uint16_t base =
        (uint16_t)(((word >> 12u) & 0x00f0u) | ((word >> 8u) & 0xf000u));
    static const uint16_t offsets[4][4] = {
        { 0x0000u, 0x000fu, 0x0f00u, 0x0f0fu },
        { 0x000fu, 0x0000u, 0x0f0fu, 0x0f00u },
        { 0x0f00u, 0x0f0fu, 0x0000u, 0x000fu },
        { 0x0f0fu, 0x0f00u, 0x000fu, 0x0000u },
    };
    uint32_t corner;

    for (corner = 0u; corner < 4u; ++corner)
        uv[corner] = (uint16_t)(base + offsets[(word >> 13u) & 3u][corner]);
}

static bool forbidden_output_address(uint32_t address) {
    return (address >= UINT32_C(0x1f800000) &&
            address < UINT32_C(0x1f800400)) ||
           address == UINT32_C(0x8009d7dc) ||
           address == UINT32_C(0x8009beac) ||
           address == UINT32_C(0x8009beb0);
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    static const uint16_t tpages[7] = {
        0x0088u, 0x008au, 0x008cu, 0x008eu,
        0x0096u, 0x0098u, 0x009au,
    };
    TestReader *reader = context;

    if (address == UINT32_C(0x1f800316)) {
        *out_value = 0x009cu;
        return true;
    }
    if (address >= UINT32_C(0x8009d618) &&
        address < UINT32_C(0x8009d64a) && (address & 1u) == 0u) {
        *out_value = address == UINT32_C(0x8009d618) ? 0u : UINT16_MAX;
        return true;
    }
    if (address >= UINT32_C(0x8009d650) &&
        address < UINT32_C(0x8009d718) && (address & 1u) == 0u) {
        const uint32_t tile = (address - UINT32_C(0x8009d650)) / 8u;
        const uint32_t quadrant =
            ((address - UINT32_C(0x8009d650)) / 2u) % 4u;

        *out_value = tile == 0u &&
            (!reader->hide_lower_quadrants || quadrant < 2u)
                ? 0u : UINT16_MAX;
        return true;
    }
    reader->touched_forbidden_output |= forbidden_output_address(address);
    if (address == UINT32_C(0x8009c838) ||
        address == UINT32_C(0x8009c83c)) {
        *out_value = 2u;
        return true;
    }
    if (address >= UINT32_C(0x8009ccb4) &&
        address < UINT32_C(0x8009cd34) && (address & 1u) == 0u) {
        const uint32_t index = (address - UINT32_C(0x8009ccb4)) / 2u;

        *out_value = (uint16_t)((432u + index) << 6u);
        return true;
    }
    if (address >= UINT32_C(0x8009cd54) &&
        address < UINT32_C(0x8009cd62) && (address & 1u) == 0u) {
        *out_value = tpages[(address - UINT32_C(0x8009cd54)) / 2u];
        return true;
    }
    if (address >= UINT32_C(0x8009d570) &&
        address < UINT32_C(0x8009d612) && (address & 1u) == 0u) {
        const uint32_t grid_index =
            (address - UINT32_C(0x8009d570)) / 2u;
        const uint32_t row = grid_index / 9u;
        const uint32_t column = grid_index % 9u;

        if (row < 2u || row > 6u || column < 2u || column > 6u)
            return false;
        *out_value = (uint16_t)((row - 2u) * 5u + column - 2u);
        return true;
    }
    return false;
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
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
    TestReader *reader = context;
    uint32_t index;

    reader->touched_forbidden_output |= forbidden_output_address(address);
    if (address == XG_WORLD_TERRAIN_WATER_NATIVE_CONTEXT_ADDRESS) {
        *out_value = TEST_CONTEXT;
        return true;
    }
    if (address == TEST_CONTEXT + 0x70u) {
        *out_value = TEST_OT_BASE;
        return true;
    }
    if (address == TEST_CONTEXT + 0x74u) {
        *out_value = reader->invalid_native_output
            ? TEST_PACKET_BASE + 4u
            : TEST_PACKET_BASE;
        return true;
    }
    if (address == UINT32_C(0x8009be28) ||
        address == UINT32_C(0x8009be30) ||
        address == UINT32_C(0x8009c5bc)) {
        *out_value = 0u;
        return true;
    }
    if (address == UINT32_C(0x8009c618)) {
        *out_value = 0x400u;
        return true;
    }
    if (address == UINT32_C(0x8009d7cc)) {
        *out_value = 0u;
        return true;
    }
    if (address >= UINT32_C(0x8009c808) &&
        address < UINT32_C(0x8009c828) && (address & 3u) == 0u) {
        *out_value = camera[(address - UINT32_C(0x8009c808)) / 4u];
        return true;
    }
    if (address >= UINT32_C(0x8009d534) &&
        address < UINT32_C(0x8009d554) && (address & 3u) == 0u) {
        *out_value = identity[(address - UINT32_C(0x8009d534)) / 4u];
        return true;
    }
    for (index = 0u; index < 8u; ++index) {
        if (address == clip_addresses[index]) {
            *out_value = 0u;
            return true;
        }
    }
    if (address >= UINT32_C(0x800523f0) &&
        address < UINT32_C(0x800563f0) && (address & 3u) == 0u) {
        *out_value = 0x00001000u;
        return true;
    }
    if (address >= UINT32_C(0x8009c184) &&
        address < UINT32_C(0x8009c584) && (address & 3u) == 0u) {
        const uint32_t terrain_id =
            (address - UINT32_C(0x8009c184)) / 4u;

        *out_value = terrain_id == 0u
            ? (reader->invalid_resource_pointer ? UINT32_C(0x801ffff0)
                                                : TEST_RESOURCE)
            : 0u;
        return true;
    }
    if (address >= TEST_RESOURCE && address < TEST_RESOURCE + 0x510u &&
        (address & 3u) == 0u) {
        static const uint32_t offsets[4] = { 0u, 0x144u, 0x288u, 0x3ccu };

        for (index = 0u; index < 4u; ++index) {
            if (address >= TEST_RESOURCE + offsets[index] &&
                address < TEST_RESOURCE + offsets[index] + 0x144u) {
                *out_value = terrain_word(
                    (address - TEST_RESOURCE - offsets[index]) / 4u);
                return true;
            }
        }
    }
    return false;
}

static XgWorldTerrainWaterCaptureResult capture_source(
    TestReader *context, XgWorldTerrainWaterCapture *capture) {
    const XgWorldTerrainWaterCaptureRequest request = {
        .authentication_generation = 9u,
        .caller_return = UINT32_C(0x80071b38),
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 120 << 16,
        .projection_distance = 32u,
        .raster = {
            .draw_area_right = 319u,
            .draw_area_bottom = 239u,
            .texture_window_mask_x = 1u,
            .texture_window_mask_y = 2u,
            .texture_window_offset_x = 3u,
            .texture_window_offset_y = 4u,
        },
        .projection_state_authenticated = true,
    };
    const XgWorldTerrainWaterAuthenticatedReader reader = {
        .context = context,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 9u,
        .authenticated = true,
    };

    return xg_world_terrain_water_source_capture(&request, &reader, capture);
}

static int test_capture_and_all_ft3_variants(void) {
    static XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    TestReader reader = { 0 };
    XgWorldTerrainWaterCapture capture;
    uint32_t count = 0u;
    uint32_t index;
    uint32_t page_mask = 0u;
    uint32_t orientation_mask = 0u;
    uint32_t diagonal_mask = 0u;
    uint32_t animation_mask = 0u;
    uint32_t clut_bank_mask = 0u;

    CHECK(capture_source(&reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_OK);
    CHECK(capture.authenticated && capture.sealed);
    CHECK(capture.authenticated_read_count == 564u);
    CHECK(capture.authenticated_read_bytes == 1856u);
    CHECK(capture.captured_resource_count == 1u);
    CHECK(!reader.touched_forbidden_output);
    CHECK(xg_world_terrain_water_build(
              &capture.source, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    CHECK(count == 512u);
    CHECK(records[0].tile_index == 0u && records[0].quadrant_index == 0u);
    CHECK(records[0].cell_x == 0u && records[0].cell_z == 0u);
    CHECK(records[0].triangle_index == 0u);
    CHECK(records[0].source_vertices[0].x == -4096);
    CHECK(records[0].source_vertices[0].y == 8);
    CHECK(records[0].source_vertices[0].z == 4096);
    CHECK(records[0].projected_vertices[0].x == 96);
    CHECK(records[0].projected_vertices[0].y == 56);
    CHECK(records[0].ordering_bucket == 128u);
    CHECK(records[0].allocation_ordinal == 0u);
    CHECK(records[0].source_primitive_index == 0u);
    CHECK(records[0].interpolation_primitive_id ==
          xg_world_terrain_water_interpolation_primitive_id(
              capture.source.tiles[0].grid_index,
              capture.source.tiles[0].terrain_id, 0u));
    CHECK(records[0].ordering_predecessor_is_external);
    CHECK(records[0].primitive.triangle_count == 1u);
    CHECK(records[0].primitive.triangles[0].split_index == 0u);
    CHECK(records[0].primitive.triangles[0].split_count == 1u);
    CHECK(records[0].primitive.triangles[0].vertices[0].r == 0x80u);
    CHECK(records[0].primitive.triangles[0].vertices[0]
              .projective_position);
    CHECK(records[0].primitive.triangles[0].vertices[0]
              .projective_view_z ==
          records[0].projected_vertices[0].projective_view_z);
    CHECK(records[0].primitive.triangles[0].vertices[0]
              .projective_distance == capture.source.projection_distance);
    CHECK(records[0].primitive.triangles[0].vertices[0]
              .interpolation_vertex_identity_valid);
    CHECK(records[0].primitive.triangles[0].vertices[0]
              .interpolation_group_id == UINT32_C(0x63000000));
    CHECK(records[0].primitive.triangles[0].vertices[0]
              .interpolation_vertex_id == 32u * 145u + 32u);
    CHECK(records[0].primitive.triangles[0].vertices[1]
              .interpolation_vertex_id ==
          records[1].primitive.triangles[0].vertices[0]
              .interpolation_vertex_id);
    CHECK(records[0].primitive.triangles[0].vertices[2]
              .interpolation_vertex_id ==
          records[1].primitive.triangles[0].vertices[2]
              .interpolation_vertex_id);
    CHECK(records[1].ordering_predecessor_record == 0u);
    CHECK(!records[1].ordering_predecessor_is_external);

    for (index = 0u; index < count; ++index) {
        static const uint8_t primitive_corners[2][2][3] = {
            { { 0u, 1u, 2u }, { 1u, 3u, 2u } },
            { { 0u, 3u, 2u }, { 1u, 3u, 0u } },
        };
        const uint32_t word = terrain_word(
            records[index].cell_z * 9u + records[index].cell_x);
        uint16_t uv[4];
        uint32_t vertex;

        expected_uv(word, uv);
        page_mask |= 1u << records[index].page_selector;
        orientation_mask |= 1u << records[index].uv_orientation;
        diagonal_mask |= 1u << records[index].alternate_diagonal;
        animation_mask |= 1u << records[index].animated_height;
        clut_bank_mask |= 1u << records[index].alternate_clut_bank;
        CHECK(records[index].primitive.material.shading ==
              XG_RENDER_IR_SHADING_FLAT);
        CHECK(records[index].primitive.material.texture_depth ==
              XG_RENDER_IR_TEXTURE_8_BIT);
        CHECK(records[index].primitive.material.textured);
        CHECK(!records[index].primitive.material.semi_transparent);
        CHECK(records[index].primitive.material.texture_window_mask_x == 1u);
        CHECK(records[index].primitive.material.texture_window_mask_y == 2u);
        CHECK(records[index].primitive.material.texture_window_offset_x == 3u);
        CHECK(records[index].primitive.material.texture_window_offset_y == 4u);
        CHECK(records[index].encoded_tpage ==
              capture.source.tpages[records[index].page_selector]);
        CHECK(records[index].primitive.material.tpage ==
              records[index].encoded_tpage);
        CHECK(records[index].primitive.material.clut_y ==
              records[index].encoded_clut >> 6u);
        CHECK((((records[index].encoded_clut >> 6u) - 432u) >= 32u) ==
              records[index].alternate_clut_bank);
        CHECK(records[index].source_primitive_index == index);
        if (index != 0u) {
            CHECK(records[index].ordering_predecessor_record == index - 1u);
            CHECK(!records[index].ordering_predecessor_is_external);
        }
        if (records[index].triangle_index == 0u)
            CHECK(records[index].source_vertices[0].y ==
                  (records[index].animated_height ? 24 : 8));
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t corner = primitive_corners
                [records[index].alternate_diagonal]
                [records[index].triangle_index][vertex];

            CHECK(records[index].primitive.triangles[0].vertices[vertex].u ==
                  (int32_t)(uint8_t)uv[corner] * 65536);
            CHECK(records[index].primitive.triangles[0].vertices[vertex].v ==
                  (int32_t)(uint8_t)(uv[corner] >> 8u) * 65536);
        }
    }
    CHECK(page_mask == 0x7fu);
    CHECK(orientation_mask == 0x0fu);
    CHECK(diagonal_mask == 0x03u);
    CHECK(animation_mask == 0x03u);
    CHECK(clut_bank_mask == 0x03u);
    return 1;
}

static int test_interpolation_identity_survives_tile_slot_movement(void) {
    static XgWorldTerrainWaterRecord original[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    static XgWorldTerrainWaterRecord moved[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    TestReader reader = {0};
    XgWorldTerrainWaterCapture capture;
    XgWorldTerrainWaterSource source;
    uint32_t original_count = 0u;
    uint32_t moved_count = 0u;
    uint32_t original_group;
    uint32_t original_vertex;
    uint32_t original_primitive;

    CHECK(capture_source(&reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_OK);
    CHECK(xg_world_terrain_water_build(
              &capture.source, original,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &original_count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    CHECK(original_count == 512u);
    original_group = original[0].primitive.triangles[0].vertices[0]
        .interpolation_group_id;
    original_vertex = original[0].primitive.triangles[0].vertices[0]
        .interpolation_vertex_id;
    original_primitive = original[0].interpolation_primitive_id;

    source = capture.source;
    source.tiles[1] = source.tiles[0];
    memset(&source.tiles[0], 0, sizeof(source.tiles[0]));
    memcpy(source.quadrant_visibility[1],
           source.quadrant_visibility[0],
           sizeof(source.quadrant_visibility[1]));
    CHECK(xg_world_terrain_water_build(
              &source, moved, XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY,
              &moved_count) == XG_WORLD_TERRAIN_WATER_OK);
    CHECK(moved_count == original_count);
    CHECK(moved[0].tile_index == 1u);
    CHECK(moved[0].primitive.triangles[0].vertices[0]
              .interpolation_group_id == original_group);
    CHECK(moved[0].primitive.triangles[0].vertices[0]
              .interpolation_vertex_id == original_vertex);
    CHECK(moved[0].interpolation_primitive_id == original_primitive);

    source.tiles[1].grid_index++;
    CHECK(xg_world_terrain_water_build(
              &source, moved, XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY,
              &moved_count) == XG_WORLD_TERRAIN_WATER_OK);
    CHECK(moved[0].primitive.triangles[0].vertices[0]
              .interpolation_vertex_id != original_vertex);
    CHECK(moved[0].interpolation_primitive_id != original_primitive);

    source.tiles[1].grid_index--;
    source.tiles[1].terrain_id++;
    CHECK(xg_world_terrain_water_build(
              &source, moved, XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY,
              &moved_count) == XG_WORLD_TERRAIN_WATER_OK);
    CHECK(moved[0].primitive.triangles[0].vertices[0]
              .interpolation_vertex_id == original_vertex);
    CHECK(moved[0].interpolation_primitive_id != original_primitive);

    CHECK(xg_world_terrain_water_interpolation_primitive_id(1u, 0u, 0u) !=
          xg_world_terrain_water_interpolation_primitive_id(0u, 0u, 0u));
    CHECK(xg_world_terrain_water_interpolation_primitive_id(0u, 1u, 0u) !=
          xg_world_terrain_water_interpolation_primitive_id(0u, 0u, 0u));
    return 1;
}

static int test_exact_packet_stop_rule(void) {
    static XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    TestReader reader = { 0 };
    XgWorldTerrainWaterCapture capture;
    uint32_t count = 0u;
    uint32_t tile;

    CHECK(capture_source(&reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_OK);
    for (tile = 0u; tile < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++tile) {
        uint32_t quadrant;

        capture.source.tiles[tile].has_data = true;
        capture.source.tiles[tile].active = true;
        memcpy(capture.source.tiles[tile].samples,
               capture.source.tiles[0].samples,
               sizeof(capture.source.tiles[tile].samples));
        for (quadrant = 0u; quadrant < 4u; ++quadrant)
            capture.source.quadrant_visibility[tile][quadrant] = 0u;
    }
    CHECK(xg_world_terrain_water_build(
              &capture.source, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    CHECK(count == 0x7feu);
    CHECK(records[count - 1u].allocation_ordinal == count - 1u);
    return 1;
}

static int test_quadrant_cull_and_scratch_page_seven(void) {
    static XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    TestReader reader = { 0 };
    XgWorldTerrainWaterCapture capture;
    uint32_t count = 0u;
    uint32_t index;
    bool saw_page_seven = false;

    CHECK(capture_source(&reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_OK);
    capture.source.quadrant_visibility[0][2] = UINT16_MAX;
    capture.source.quadrant_visibility[0][3] = UINT16_MAX;
    CHECK(xg_world_terrain_water_build(
              &capture.source, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    CHECK(count == 256u);
    for (index = 0u; index < count; ++index)
        CHECK(records[index].quadrant_index < 2u);

    xg_world_terrain_water_set_temporal_coverage(true);
    CHECK(xg_world_terrain_water_build(
              &capture.source, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    CHECK(count == 512u);
    xg_world_terrain_water_set_temporal_coverage(false);

    capture.source.tiles[0].samples[0][0] |= 7u << 8u;
    CHECK(xg_world_terrain_water_build(
              &capture.source, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    for (index = 0u; index < count; ++index) {
        if (records[index].page_selector == 7u) {
            CHECK(records[index].encoded_tpage == 0x009cu);
            saw_page_seven = true;
        }
    }
    CHECK(saw_page_seven);

    capture.source.tiles[0].samples[0][0] &= ~(7u << 8u);
    capture.source.tpages[7] = UINT16_MAX;
    CHECK(xg_world_terrain_water_build(
              &capture.source, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    capture.source.tiles[0].samples[0][0] |= 7u << 8u;
    CHECK(xg_world_terrain_water_build(
              &capture.source, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, &count) ==
          XG_WORLD_TERRAIN_WATER_INVALID_SOURCE);
    return 1;
}

static int test_capture_and_source_fail_closed(void) {
    TestReader context = { 0 };
    XgWorldTerrainWaterCapture capture;
    XgWorldTerrainWaterCaptureRequest request = {
        .authentication_generation = 9u,
        .caller_return = UINT32_C(0x80071b38),
        .projection_distance = 32u,
        .raster = { .draw_area_right = 319u, .draw_area_bottom = 239u },
        .projection_state_authenticated = true,
    };
    XgWorldTerrainWaterAuthenticatedReader reader = {
        .context = &context,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 9u,
        .authenticated = true,
    };

    reader.authenticated = false;
    CHECK(xg_world_terrain_water_source_capture(
              &request, &reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_UNAUTHENTICATED);
    reader.authenticated = true;
    request.caller_return += 4u;
    CHECK(xg_world_terrain_water_source_capture(
              &request, &reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_INVALID_ARGUMENT);
    request.caller_return -= 4u;
    context.invalid_resource_pointer = true;
    CHECK(xg_world_terrain_water_source_capture(
              &request, &reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_SOURCE_MISMATCH);
    return 1;
}

static int test_temporal_coverage_captures_culled_quadrants(void) {
    TestReader reader = { .hide_lower_quadrants = true };
    XgWorldTerrainWaterCapture capture;

    xg_world_terrain_water_set_temporal_coverage(false);
    CHECK(capture_source(&reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_OK);
    CHECK(capture.source.tiles[0].samples[2][0] == 0u);

    xg_world_terrain_water_set_temporal_coverage(true);
    CHECK(capture_source(&reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_OK);
    CHECK(capture.source.tiles[0].samples[2][0] == terrain_word(0u));
    xg_world_terrain_water_set_temporal_coverage(false);
    return 1;
}

static int test_temporal_anchors_cover_retired_tile(void) {
    static XgWorldTerrainWaterAnchor anchors[
        XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY];
    TestReader reader = {0};
    XgWorldTerrainWaterCapture capture;
    XgWorldTerrainWaterSource current;
    uint32_t anchor_count = 0u;

    xg_world_terrain_water_set_temporal_coverage(true);
    CHECK(capture_source(&reader, &capture) ==
          XG_WORLD_TERRAIN_WATER_CAPTURE_OK);
    current = capture.source;
    current.tiles[0].active = false;
    current.tiles[0].has_data = false;
    current.screen_offset_x = 2000 << 16;
    CHECK(xg_world_terrain_water_append_temporal_anchors(
              &capture.source, &current, anchors,
              XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY, &anchor_count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    CHECK(anchor_count == 289u);
    CHECK(anchors[0].vertex.interpolation_group_id == UINT32_C(0x63000000));
    CHECK(anchors[0].vertex.interpolation_vertex_identity_valid);
    CHECK(xg_world_terrain_water_append_temporal_anchors(
              &capture.source, &current, anchors,
              XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY, &anchor_count) ==
          XG_WORLD_TERRAIN_WATER_OK);
    CHECK(anchor_count == 289u);
    xg_world_terrain_water_set_temporal_coverage(false);
    return 1;
}

static int test_native_preparation_and_scratch_ledger(void) {
    static XgWorldTerrainWaterRecord records[
        XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    static XgWorldTerrainWaterAnchor anchors[
        XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY];
    static XgWorldTerrainWaterSource prepared_source;
    TestReader context = { 0 };
    XgWorldTerrainWaterNativePreparation preparation;
    XgWorldTerrainWaterNativeRequest request = {
        .capture = {
            .authentication_generation = 9u,
            .caller_return = UINT32_C(0x80071b38),
            .screen_offset_x = 160 << 16,
            .screen_offset_y = 120 << 16,
            .projection_distance = 32u,
            .raster = {
                .draw_area_right = 319u,
                .draw_area_bottom = 239u,
                .texture_window_mask_x = 1u,
                .texture_window_mask_y = 2u,
                .texture_window_offset_x = 3u,
                .texture_window_offset_y = 4u,
            },
            .projection_state_authenticated = true,
        },
        .entry_pc = XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
        .ot_base = TEST_OT_BASE,
        .packet_base = TEST_PACKET_BASE,
        .position_address = XG_WORLD_TERRAIN_WATER_NATIVE_POSITION_ADDRESS,
    };
    const XgWorldTerrainWaterAuthenticatedReader reader = {
        .context = &context,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = 9u,
        .authenticated = true,
    };

#define SCRATCH_VALUE(offset) preparation.scratch.values[(offset) / 4u]
#define SCRATCH_MASK(offset) preparation.scratch.write_masks[(offset) / 4u]

    CHECK(xg_world_terrain_water_native_prepare(
              &request, &reader, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, anchors,
              XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY, &prepared_source,
              &preparation) ==
          XG_WORLD_TERRAIN_WATER_NATIVE_OK);
    CHECK(preparation.authenticated && preparation.sealed);
    CHECK(preparation.authentication_generation == 9u);
    CHECK(preparation.ot_base == TEST_OT_BASE);
    CHECK(preparation.packet_base == TEST_PACKET_BASE);
    CHECK(preparation.position_address ==
          XG_WORLD_TERRAIN_WATER_NATIVE_POSITION_ADDRESS);
    CHECK(preparation.record_count == 512u);
    CHECK(preparation.anchor_count == 289u);
    CHECK(anchors[0].vertex.interpolation_group_id == UINT32_C(0x63000000));
    CHECK(anchors[0].vertex.interpolation_vertex_identity_valid);
    CHECK(preparation.final_count == preparation.record_count);
    CHECK(preparation.continuation_pc == UINT32_C(0x80071b38));
    CHECK(preparation.authenticated_read_count == 567u);
    CHECK(preparation.authenticated_read_bytes == 1868u);
    CHECK(preparation.captured_resource_count == 1u);
    CHECK(((uint32_t)records[0].primitive.triangles[0].vertices[0].r |
           ((uint32_t)records[0].primitive.triangles[0].vertices[0].g << 8u) |
           ((uint32_t)records[0].primitive.triangles[0].vertices[0].b << 16u) |
           UINT32_C(0x24000000)) ==
          XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_MATERIAL_WORD);

    CHECK(SCRATCH_VALUE(0x000u) == UINT32_C(0x0008f400));
    CHECK(SCRATCH_MASK(0x000u) == UINT32_MAX);
    CHECK(SCRATCH_VALUE(0x004u) == UINT32_C(0x00000c00));
    CHECK(SCRATCH_MASK(0x004u) == UINT32_C(0x0000ffff));
    CHECK(SCRATCH_VALUE(0x288u) == UINT32_C(0x6c406c00));
    CHECK(SCRATCH_MASK(0x288u) == UINT32_MAX);
    CHECK(SCRATCH_VALUE(0x308u) == UINT32_C(0x008a0088));
    CHECK(SCRATCH_MASK(0x308u) == UINT32_MAX);
    CHECK(SCRATCH_VALUE(0x314u) == UINT32_C(0x0000009a));
    CHECK(SCRATCH_MASK(0x314u) == UINT32_C(0x0000ffff));
    CHECK(SCRATCH_VALUE(0x318u) == UINT32_C(0xfffff000));
    CHECK(SCRATCH_MASK(0x318u) == UINT32_MAX);
    CHECK(SCRATCH_MASK(0x31cu) == 0u);
    CHECK(SCRATCH_VALUE(0x320u) == UINT32_C(0xfffff000));
    CHECK(SCRATCH_VALUE(0x328u) == UINT32_C(0x00001800));
    CHECK(SCRATCH_MASK(0x328u) == UINT32_C(0x0000ffff));
    CHECK(SCRATCH_VALUE(0x32cu) == UINT32_C(0x0000e800));
    CHECK(SCRATCH_VALUE(0x330u) == UINT32_C(0x0000f400));
    CHECK(SCRATCH_VALUE(0x334u) == UINT32_C(0x00001000));
    CHECK(SCRATCH_VALUE(0x338u) == UINT32_C(0x0000f000));
    CHECK(SCRATCH_VALUE(0x33cu) == UINT32_C(0x00000c00));
    CHECK(SCRATCH_VALUE(0x340u) == UINT32_C(0x0000f400));
    CHECK(SCRATCH_VALUE(0x344u) == UINT32_C(0x00000c00));
    CHECK(SCRATCH_MASK(0x348u) == 0u);
    CHECK(SCRATCH_VALUE(0x350u) == UINT32_C(0x00001000));
    CHECK(SCRATCH_MASK(0x350u) == UINT32_MAX);
    CHECK(SCRATCH_VALUE(0x370u) == UINT32_C(0x00001000));
    CHECK(SCRATCH_MASK(0x38cu) == UINT32_MAX);
    CHECK(!context.touched_forbidden_output);

    context.invalid_native_output = true;
    memset(&preparation, 0xff, sizeof(preparation));
    CHECK(xg_world_terrain_water_native_prepare(
              &request, &reader, records,
              XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, anchors,
              XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY, &prepared_source,
              &preparation) ==
          XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_OUTPUT);
    CHECK(!preparation.authenticated && !preparation.sealed);
    CHECK(preparation.record_count == 0u);

#undef SCRATCH_VALUE
#undef SCRATCH_MASK
    return 1;
}

int main(void) {
    int ok = 1;

    ok &= test_capture_and_all_ft3_variants();
    ok &= test_interpolation_identity_survives_tile_slot_movement();
    ok &= test_exact_packet_stop_rule();
    ok &= test_quadrant_cull_and_scratch_page_seven();
    ok &= test_capture_and_source_fail_closed();
    ok &= test_temporal_coverage_captures_culled_quadrants();
    ok &= test_temporal_anchors_cover_retired_tile();
    ok &= test_native_preparation_and_scratch_ledger();
    return ok ? 0 : 1;
}
