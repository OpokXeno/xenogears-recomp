#include "xg_world_clouds.h"
#include "xg_world_clouds_source_capture.h"

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
    TEST_POSITION_BASE = 0x800e0000u,
    TEST_VELOCITY_BASE = 0x800e0500u,
};

typedef struct TestReader {
    int32_t camera_translation_z;
    uint32_t visible_count;
    uint16_t uv_group;
    uint32_t callback_count;
    uint32_t fail_address;
    bool mutate_far_table;
    bool touched_forbidden_source;
    bool custom_angles;
    int16_t pitch;
    int16_t yaw;
    bool saw_trig_index_one;
    bool saw_trig_index_4095;
} TestReader;

static XgWorldCloudRecord records[XG_WORLD_CLOUD_PACKET_CAPACITY];

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static XgHost3dVector expected_far_vertex(uint32_t index) {
    const uint32_t layer = index / 4u;
    const uint32_t vertex = index % 4u;
    XgHost3dVector value = { 0 };

    value.x = (vertex & 1u) != 0u ? 192 : -192;
    value.y = (int16_t)(-(int32_t)layer * 8);
    value.z = vertex >= 2u ? -192 : 192;
    return value;
}

static XgHost3dVector expected_middle_vertex(uint32_t index) {
    const uint32_t layer = index / 16u;
    const uint32_t quad = (index % 16u) / 4u;
    const uint32_t vertex = index % 4u;
    XgHost3dVector value = { 0 };

    value.x = (int16_t)(-192 + (int32_t)(quad & 1u) * 192 +
                        (int32_t)(vertex & 1u) * 192);
    value.y = (int16_t)(-(int32_t)layer * 8);
    value.z = (int16_t)(192 - (int32_t)(quad >> 1u) * 192 -
                        (vertex >= 2u ? 192 : 0));
    return value;
}

static uint32_t matrix_word(bool camera, int32_t translation_z,
                            uint32_t index) {
    static const uint32_t identity[8] = {
        0x00001000u, 0x00000000u, 0x00001000u, 0x00000000u,
        0x00001000u, 0u, 0u, 0u,
    };

    if (!camera) return identity[index];
    if (index == 4u) return pack_s16(-4096, 0);
    if (index == 7u) return (uint32_t)translation_z;
    return identity[index];
}

static bool address_is_forbidden(uint32_t address) {
    return (address >= UINT32_C(0x1f800000) &&
            address < UINT32_C(0x1f800400)) ||
           address == UINT32_C(0x8009d7f8) ||
           address == UINT32_C(0x8009d7fc);
}

static bool test_read_u16(void *context, uint32_t address,
                          uint16_t *out_value) {
    TestReader *reader = context;

    ++reader->callback_count;
    reader->touched_forbidden_source |= address_is_forbidden(address);
    if (address == reader->fail_address || out_value == NULL) return false;
    switch (address) {
    case UINT32_C(0x8009bd38):
        *out_value = (uint16_t)(reader->custom_angles
            ? reader->pitch : -0x400);
        return true;
    case UINT32_C(0x8009bd3a):
        *out_value = (uint16_t)(reader->custom_angles ? reader->yaw : 0);
        return true;
    case UINT32_C(0x8009bd40): *out_value = 0u; return true;
    case UINT32_C(0x8009bd44): *out_value = 1024u; return true;
    default: break;
    }
    if (address >= TEST_VELOCITY_BASE &&
        address < TEST_VELOCITY_BASE + XG_WORLD_CLOUD_COUNT * 8u) {
        const uint32_t field = (address - TEST_VELOCITY_BASE) % 8u;

        if (field == 0u || field == 4u) {
            *out_value = 0u;
            return true;
        }
        if (field == 2u) {
            *out_value = reader->uv_group;
            return true;
        }
    }
    return false;
}

static bool test_read_u32(void *context, uint32_t address,
                          uint32_t *out_value) {
    static const uint16_t uv[8] = {
        0x0000u, 0x0040u, 0x0080u, 0x0000u,
        0x00c0u, 0x4000u, 0x4040u, 0x0000u,
    };
    TestReader *reader = context;

    ++reader->callback_count;
    reader->touched_forbidden_source |= address_is_forbidden(address);
    if (address == reader->fail_address || out_value == NULL) return false;
    switch (address) {
    case UINT32_C(0x8009d150): *out_value = TEST_POSITION_BASE; return true;
    case UINT32_C(0x8009ceb4): *out_value = TEST_VELOCITY_BASE; return true;
    case UINT32_C(0x8009be28):
    case UINT32_C(0x8009be30): *out_value = 0u; return true;
    case UINT32_C(0x8009d7cc): *out_value = 0u; return true;
    default: break;
    }
    if (address >= UINT32_C(0x8009c808) &&
        address < UINT32_C(0x8009c828) && (address & 3u) == 0u) {
        *out_value = matrix_word(true, reader->camera_translation_z + 200,
                                 (address - UINT32_C(0x8009c808)) / 4u);
        return true;
    }
    if (address >= UINT32_C(0x8009a180) &&
        address < UINT32_C(0x8009a1a0) && (address & 3u) == 0u) {
        *out_value = matrix_word(false, 0,
                                 (address - UINT32_C(0x8009a180)) / 4u);
        return true;
    }
    if (address >= UINT32_C(0x8009ad40) &&
        address < UINT32_C(0x8009ad50) && (address & 3u) == 0u) {
        const uint32_t index = (address - UINT32_C(0x8009ad40)) / 2u;

        *out_value = uv[index] | ((uint32_t)uv[index + 1u] << 16u);
        return true;
    }
    if (address >= UINT32_C(0x8009ad50) &&
        address < UINT32_C(0x8009adb0) && (address & 3u) == 0u) {
        const uint32_t offset = address - UINT32_C(0x8009ad50);
        XgHost3dVector value = expected_far_vertex(offset / 8u);

        if (reader->mutate_far_table && offset == 0u) ++value.x;
        *out_value = (offset & 4u) == 0u
            ? pack_s16(value.x, value.y)
            : pack_s16(value.z, (int16_t)value.pad);
        return true;
    }
    if (address >= UINT32_C(0x8009adb0) &&
        address < UINT32_C(0x8009af30) && (address & 3u) == 0u) {
        const uint32_t offset = address - UINT32_C(0x8009adb0);
        const XgHost3dVector value = expected_middle_vertex(offset / 8u);

        *out_value = (offset & 4u) == 0u
            ? pack_s16(value.x, value.y)
            : pack_s16(value.z, (int16_t)value.pad);
        return true;
    }
    if (address == UINT32_C(0x800523f0)) {
        *out_value = pack_s16(0, 4096);
        return true;
    }
    if (address == UINT32_C(0x800523f0) +
                       (((uint32_t)-0x169 & 0xfffu) * 4u)) {
        *out_value = pack_s16(-2048, 4096);
        return true;
    }
    if (address == UINT32_C(0x800523f0) + 0x169u * 4u) {
        *out_value = pack_s16(2048, 4096);
        return true;
    }
    if (reader->custom_angles && address >= UINT32_C(0x800523f0) &&
        address < UINT32_C(0x800563f0) && (address & 3u) == 0u) {
        const uint32_t index = (address - UINT32_C(0x800523f0)) / 4u;

        reader->saw_trig_index_one |= index == 1u;
        reader->saw_trig_index_4095 |= index == 4095u;
        *out_value = pack_s16(123, 4096);
        return true;
    }
    if (address >= TEST_POSITION_BASE &&
        address < TEST_POSITION_BASE + XG_WORLD_CLOUD_COUNT * 0x10u) {
        const uint32_t offset = address - TEST_POSITION_BASE;
        const uint32_t index = offset / 0x10u;
        const uint32_t field = offset % 0x10u;

        if (field == 0u) {
            *out_value = index < reader->visible_count ? 0u : 4095u << 12u;
            return true;
        }
        if (field == 4u) {
            *out_value = 0u;
            return true;
        }
        if (field == 8u) {
            *out_value = (uint32_t)(-(100 << 12));
            return true;
        }
    }
    return false;
}

static XgWorldCloudsCaptureResult capture_source(
    TestReader *reader, int32_t screen_y, XgWorldCloudsCapture *capture) {
    const XgWorldCloudsCaptureRequest request = {
        .authentication_generation = 11u,
        .caller_return = UINT32_C(0x80071b68),
        .screen_offset_x = 160 << 16,
        .screen_offset_y = screen_y << 16,
        .projection_distance = 256u,
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
    const XgWorldCloudsAuthenticatedReader authenticated_reader = {
        .context = reader,
        .read_u16 = test_read_u16,
        .read_u32 = test_read_u32,
        .authentication_generation = 11u,
        .authenticated = true,
    };

    return xg_world_clouds_source_capture(
        &request, &authenticated_reader, capture);
}

static int build_capture(TestReader *reader, int32_t screen_y,
                         XgWorldCloudsCapture *capture,
                         XgWorldCloudsBuildStats *stats,
                         uint32_t *count) {
    CHECK(capture_source(reader, screen_y, capture) ==
          XG_WORLD_CLOUDS_CAPTURE_OK);
    CHECK(xg_world_clouds_build(&capture->source, records,
                                XG_WORLD_CLOUD_PACKET_CAPACITY,
                                count, stats) == XG_WORLD_CLOUDS_OK);
    return 1;
}

static int test_position_update_wraps(void) {
    XgWorldCloudPosition positions[XG_WORLD_CLOUD_COUNT] = { 0 };
    XgWorldCloudVelocity velocities[XG_WORLD_CLOUD_COUNT] = { 0 };

    positions[0] = (XgWorldCloudPosition){ 0x1ffffff, 77, 0 };
    velocities[0].x = 1;
    velocities[0].z = -1;
    positions[1].x = -1;
    positions[2].x = 0x1fffffe;
    velocities[2].x = 1;
    CHECK(xg_world_clouds_step_positions(positions, velocities) ==
          XG_WORLD_CLOUDS_OK);
    CHECK(positions[0].x == 0);
    CHECK(positions[0].z == 0x1ffffff);
    CHECK(positions[0].y == 77);
    CHECK(positions[1].x == 0x1ffffff);
    CHECK(positions[2].x == 0x1ffffff);
    CHECK(xg_world_clouds_step_positions(NULL, velocities) ==
          XG_WORLD_CLOUDS_INVALID_ARGUMENT);
    return 1;
}

static int test_capture_contract(void) {
    TestReader reader = {
        .camera_translation_z = 900,
        .visible_count = 1u,
    };
    XgWorldCloudsCapture capture;
    XgWorldCloudsCaptureRequest request = {
        .authentication_generation = 11u,
        .caller_return = UINT32_C(0x80071b68),
        .projection_distance = 256u,
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
    XgWorldCloudsAuthenticatedReader authenticated_reader = {
        .context = &reader,
        .read_u16 = test_read_u16,
        .read_u32 = test_read_u32,
        .authentication_generation = 11u,
        .authenticated = true,
    };

    CHECK(capture_source(&reader, 120, &capture) ==
          XG_WORLD_CLOUDS_CAPTURE_OK);
    CHECK(capture.authenticated && capture.sealed);
    CHECK(capture.authenticated_read_count ==
          XG_WORLD_CLOUDS_MAX_AUTHENTICATED_READS);
    CHECK(capture.authenticated_read_bytes == 2052u);
    CHECK(reader.callback_count == XG_WORLD_CLOUDS_MAX_AUTHENTICATED_READS);
    CHECK(!reader.touched_forbidden_source);
    CHECK(capture.position_array_address == TEST_POSITION_BASE);
    CHECK(capture.velocity_array_address == TEST_VELOCITY_BASE);
    CHECK(capture.source.material.tpage == 0x003fu);
    CHECK(capture.source.material.clut_x == 304u);
    CHECK(capture.source.material.clut_y == 510u);
    CHECK(capture.source.material.texture_window_mask_x == 1u);
    CHECK(capture.source.material.texture_window_mask_y == 2u);
    CHECK(capture.source.material.texture_window_offset_x == 3u);
    CHECK(capture.source.material.texture_window_offset_y == 4u);
    CHECK(capture.source.wedge_offset_x == 0);
    CHECK(capture.source.wedge_offset_z == 0);
    CHECK(capture.source.screen_x_cull_margin == 0);

    authenticated_reader.authenticated = false;
    CHECK(xg_world_clouds_source_capture(
              &request, &authenticated_reader, &capture) ==
          XG_WORLD_CLOUDS_CAPTURE_UNAUTHENTICATED);
    authenticated_reader.authenticated = true;
    request.caller_return += 4u;
    CHECK(xg_world_clouds_source_capture(
              &request, &authenticated_reader, &capture) ==
          XG_WORLD_CLOUDS_CAPTURE_INVALID_ARGUMENT);
    reader.mutate_far_table = true;
    CHECK(capture_source(&reader, 120, &capture) ==
          XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH);
    reader.mutate_far_table = false;
    reader.fail_address = UINT32_C(0x8009d7cc);
    CHECK(capture_source(&reader, 120, &capture) ==
          XG_WORLD_CLOUDS_CAPTURE_READ_FAILED);
    reader.fail_address = 0u;
    reader.uv_group = 2u;
    CHECK(capture_source(&reader, 120, &capture) ==
          XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 900;
    reader.visible_count = 1u;
    reader.custom_angles = true;
    reader.pitch = -0x408;
    reader.yaw = 2;
    CHECK(capture_source(&reader, 120, &capture) ==
          XG_WORLD_CLOUDS_CAPTURE_OK);
    CHECK(reader.saw_trig_index_one);
    CHECK(!reader.saw_trig_index_4095);
    CHECK(capture.source.pitch_rotation.sine == -123);
    CHECK(capture.source.negative_yaw_rotation.sine == -123);
    CHECK(capture.source.positive_yaw_rotation.sine == 123);
    return 1;
}

static int test_all_lods_material_uv_and_order(void) {
    XgWorldCloudsCapture capture;
    XgWorldCloudsBuildStats stats;
    uint32_t count;
    TestReader reader = {
        .camera_translation_z = 900,
        .visible_count = 1u,
    };

    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == XG_WORLD_CLOUD_NEAR_QUAD_COUNT);
    CHECK(stats.quad_attempt_count == XG_WORLD_CLOUD_NEAR_QUAD_COUNT);
    CHECK(stats.clouds_world_culled == 79u);
    CHECK(stats.first_world_accepted_source == 0u);
    CHECK(records[0].lod == XG_WORLD_CLOUD_LOD_NEAR);
    CHECK(records[0].uv[0] == 0x0000u);
    CHECK(records[0].uv[1] == 0x000fu);
    CHECK(records[0].uv[2] == 0x0f00u);
    CHECK(records[0].uv[3] == 0x0f0fu);
    CHECK(records[0].material_word == UINT32_C(0x2e262626));
    CHECK(records[0].tpage == 0x003fu && records[0].clut == 0x7f93u);
    CHECK(records[0].primitive.triangle_count == 2u);
    CHECK(records[0].primitive.material.texture_window_mask_x == 1u);
    CHECK(records[0].primitive.material.texture_window_mask_y == 2u);
    CHECK(records[0].primitive.material.texture_window_offset_x == 3u);
    CHECK(records[0].primitive.material.texture_window_offset_y == 4u);
    CHECK(records[0].primitive.triangles[0].vertices[0].r == 0x26u);
    CHECK(records[0].prior_emission_in_bucket == UINT32_MAX);
    CHECK(records[1].ordering_bucket == records[0].ordering_bucket);
    CHECK(records[1].prior_emission_in_bucket == 0u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 1100;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == XG_WORLD_CLOUD_MIDDLE_QUAD_COUNT);
    CHECK(records[0].lod == XG_WORLD_CLOUD_LOD_MIDDLE);
    CHECK(records[0].uv[3] == 0x1f1fu);
    CHECK(records[1].uv[0] == 0x0020u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 925;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == XG_WORLD_CLOUD_MIDDLE_QUAD_COUNT);
    CHECK(records[0].lod == XG_WORLD_CLOUD_LOD_MIDDLE);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 1400;
    reader.visible_count = 1u;
    reader.uv_group = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == XG_WORLD_CLOUD_FAR_QUAD_COUNT);
    CHECK(records[0].lod == XG_WORLD_CLOUD_LOD_FAR);
    CHECK(records[0].uv[0] == 0x00c0u);
    CHECK(records[0].uv[3] == 0x3fffu);
    CHECK(records[1].uv[0] == 0x4000u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 1309;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == XG_WORLD_CLOUD_FAR_QUAD_COUNT);
    CHECK(records[0].lod == XG_WORLD_CLOUD_LOD_FAR);
    return 1;
}

static int test_culls_and_far_stops(void) {
    XgWorldCloudsCapture capture;
    XgWorldCloudsBuildStats stats;
    uint32_t count;
    TestReader reader = {
        .camera_translation_z = 900,
        .visible_count = 0u,
    };

    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == 0u && stats.clouds_world_culled == 80u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = -1000;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == 0u && stats.clouds_anchor_culled == 1u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 50;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == 0u && stats.near_groups_culled == 3u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 900;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 300, &capture, &stats, &count));
    CHECK(count == 0u);
    CHECK(stats.quad_screen_culled == XG_WORLD_CLOUD_NEAR_QUAD_COUNT);

    capture.source.camera.translation[2] = 1200;
    capture.source.projection_distance = UINT16_MAX;
    capture.source.screen_offset_y = 120 << 16;
    count = 0u;
    CHECK(xg_world_clouds_build(
              &capture.source, records, XG_WORLD_CLOUD_PACKET_CAPACITY,
              &count, &stats) == XG_WORLD_CLOUDS_OK);
    CHECK(count == 0u);
    CHECK(stats.quad_projection_culled == XG_WORLD_CLOUD_MIDDLE_QUAD_COUNT);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 3037;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == 0u && stats.far_preinsert_depth_stops == 1u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 2525;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == 1u && stats.far_postinsert_depth_stops == 1u);
    CHECK(records[0].ordering_depth > 0xb00u);
    CHECK(records[0].ordering_depth <= 0xd00u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 2524;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == XG_WORLD_CLOUD_FAR_QUAD_COUNT);
    CHECK(records[0].ordering_depth == 0xb00u);
    CHECK(stats.far_postinsert_depth_stops == 0u);

    memset(&reader, 0, sizeof(reader));
    reader.camera_translation_z = 3036;
    reader.visible_count = 1u;
    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == 1u);
    CHECK(records[0].ordering_depth == 0xd00u);
    CHECK(stats.far_preinsert_depth_stops == 0u);
    CHECK(stats.far_postinsert_depth_stops == 1u);
    return 1;
}

static int test_packet_capacity_and_invalid_sources(void) {
    XgWorldCloudsCapture capture;
    XgWorldCloudsBuildStats stats;
    uint32_t count = 99u;
    TestReader reader = {
        .camera_translation_z = 900,
        .visible_count = 6u,
    };

    CHECK(build_capture(&reader, 120, &capture, &stats, &count));
    CHECK(count == XG_WORLD_CLOUD_PACKET_CAPACITY);
    CHECK(stats.clouds_entered == 6u);
    CHECK(stats.packet_entry_limit_stopped);
    CHECK(records[287].emission_index == 287u);

    count = 99u;
    CHECK(xg_world_clouds_build(
              &capture.source, records, XG_WORLD_CLOUD_PACKET_CAPACITY - 1u,
              &count, &stats) == XG_WORLD_CLOUDS_CAPACITY_EXCEEDED);
    CHECK(count == 0u);
    capture.source.uv_base[0] = 1u;
    CHECK(xg_world_clouds_build(
              &capture.source, records, XG_WORLD_CLOUD_PACKET_CAPACITY,
              &count, &stats) == XG_WORLD_CLOUDS_INVALID_SOURCE);
    return 1;
}

int main(void) {
    return test_position_update_wraps() && test_capture_contract() &&
           test_all_lods_material_uv_and_order() &&
           test_culls_and_far_stops() &&
           test_packet_capacity_and_invalid_sources()
        ? 0 : 1;
}
