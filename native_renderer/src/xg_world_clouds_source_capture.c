#include "xg_world_clouds_source_capture.h"

#include <limits.h>
#include <string.h>

enum {
    CLOUD_POSITION_POINTER = 0x8009d150u,
    CLOUD_VELOCITY_POINTER = 0x8009ceb4u,
    CLOUD_UV_TABLE = 0x8009ad40u,
    CLOUD_FAR_VERTEX_TABLE = 0x8009ad50u,
    CLOUD_MIDDLE_VERTEX_TABLE = 0x8009adb0u,
    CLOUD_BILLBOARD_MATRIX = 0x8009a180u,
    CLOUD_CAMERA_MATRIX = 0x8009c808u,
    CLOUD_CAMERA_ORIGIN_X = 0x8009be28u,
    CLOUD_CAMERA_ORIGIN_Z = 0x8009be30u,
    CLOUD_FOG_MODE = 0x8009d7ccu,
    CLOUD_PITCH = 0x8009bd38u,
    CLOUD_YAW = 0x8009bd3au,
    CLOUD_WEDGE_OFFSET_X = 0x8009bd40u,
    CLOUD_WEDGE_OFFSET_Z = 0x8009bd44u,
    CLOUD_TRIG_TABLE = 0x800523f0u,
    CLOUD_POSITION_STRIDE = 0x10u,
    CLOUD_VELOCITY_STRIDE = 0x8u,
    PSX_RAM_START = 0x80000000u,
    PSX_RAM_END = 0x80200000u,
};

typedef struct CloudCaptureAccess {
    const XgWorldCloudsAuthenticatedReader *reader;
    uint32_t position_base;
    uint32_t velocity_base;
    uint32_t trig_addresses[6];
    uint32_t read_count;
    uint32_t read_bytes;
} CloudCaptureAccess;

static bool aligned_range_contains(uint32_t address, uint32_t start,
                                   uint32_t size) {
    return address >= start && address <= start + size - 4u &&
           ((address - start) & 3u) == 0u;
}

static bool dynamic_field(uint32_t address, uint32_t base, uint32_t count,
                          uint32_t stride, uint32_t width,
                          uint32_t field0, uint32_t field1,
                          uint32_t field2) {
    uint32_t offset;
    uint32_t field;

    if (address < base || address > base + count * stride - width)
        return false;
    offset = address - base;
    field = offset % stride;
    return field == field0 || field == field1 || field == field2;
}

static bool u16_is_allowed(const CloudCaptureAccess *access,
                           uint32_t address) {
    return address == CLOUD_PITCH || address == CLOUD_YAW ||
           address == CLOUD_WEDGE_OFFSET_X ||
           address == CLOUD_WEDGE_OFFSET_Z ||
           dynamic_field(address, access->velocity_base,
                         XG_WORLD_CLOUD_COUNT, CLOUD_VELOCITY_STRIDE, 2u,
                         0u, 2u, 4u);
}

static bool u32_is_allowed(const CloudCaptureAccess *access,
                           uint32_t address) {
    uint32_t index;

    if (address == CLOUD_POSITION_POINTER ||
        address == CLOUD_VELOCITY_POINTER ||
        address == CLOUD_CAMERA_ORIGIN_X ||
        address == CLOUD_CAMERA_ORIGIN_Z || address == CLOUD_FOG_MODE ||
        aligned_range_contains(address, CLOUD_CAMERA_MATRIX, 0x20u) ||
        aligned_range_contains(address, CLOUD_BILLBOARD_MATRIX, 0x20u) ||
        aligned_range_contains(address, CLOUD_UV_TABLE, 0x10u) ||
        aligned_range_contains(address, CLOUD_FAR_VERTEX_TABLE, 0x60u) ||
        aligned_range_contains(address, CLOUD_MIDDLE_VERTEX_TABLE, 0x180u) ||
        dynamic_field(address, access->position_base,
                      XG_WORLD_CLOUD_COUNT, CLOUD_POSITION_STRIDE, 4u,
                      0u, 4u, 8u))
        return true;
    for (index = 0u; index < 6u; ++index) {
        if (address == access->trig_addresses[index]) return true;
    }
    return false;
}

static XgWorldCloudsCaptureResult read_u16(CloudCaptureAccess *access,
                                            uint32_t address,
                                            uint16_t *out_value) {
    if (access->read_count >= XG_WORLD_CLOUDS_MAX_AUTHENTICATED_READS ||
        !u16_is_allowed(access, address))
        return XG_WORLD_CLOUDS_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, address, out_value))
        return XG_WORLD_CLOUDS_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_CLOUDS_CAPTURE_OK;
}

static XgWorldCloudsCaptureResult read_u32(CloudCaptureAccess *access,
                                            uint32_t address,
                                            uint32_t *out_value) {
    if (access->read_count >= XG_WORLD_CLOUDS_MAX_AUTHENTICATED_READS ||
        !u32_is_allowed(access, address))
        return XG_WORLD_CLOUDS_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_WORLD_CLOUDS_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_CLOUDS_CAPTURE_OK;
}

static int16_t low_s16(uint32_t value) {
    return (int16_t)(uint16_t)value;
}

static int16_t negate_s16(int16_t value) {
    return (int16_t)(uint16_t)(0u - (uint16_t)value);
}

static uint32_t rotation_trig_index(int32_t angle) {
    const uint32_t magnitude = angle < 0
        ? (uint32_t)(-(int64_t)angle) : (uint32_t)angle;

    return magnitude & 0xfffu;
}

static int32_t shift_right_floor(int32_t value, unsigned bits) {
    uint32_t magnitude;

    if (value >= 0) return value / (int32_t)(1u << bits);
    magnitude = (uint32_t)(-(value + 1)) + 1u;
    return -(int32_t)((magnitude + ((1u << bits) - 1u)) >> bits);
}

static void parse_matrix_word(XgHost3dMatrix *matrix, uint32_t index,
                              uint32_t word) {
    if (index == 0u) {
        matrix->rotation[0][0] = low_s16(word);
        matrix->rotation[0][1] = low_s16(word >> 16u);
    } else if (index == 1u) {
        matrix->rotation[0][2] = low_s16(word);
        matrix->rotation[1][0] = low_s16(word >> 16u);
    } else if (index == 2u) {
        matrix->rotation[1][1] = low_s16(word);
        matrix->rotation[1][2] = low_s16(word >> 16u);
    } else if (index == 3u) {
        matrix->rotation[2][0] = low_s16(word);
        matrix->rotation[2][1] = low_s16(word >> 16u);
    } else if (index == 4u) {
        matrix->rotation[2][2] = low_s16(word);
        matrix->pad = (uint16_t)(word >> 16u);
    } else {
        matrix->translation[index - 5u] = (int32_t)word;
    }
}

static bool raster_is_valid(const XgWorldCloudsRasterState *raster) {
    return raster->draw_area_left <= raster->draw_area_right &&
           raster->draw_area_top <= raster->draw_area_bottom &&
           raster->draw_area_right <= 1023u &&
           raster->draw_area_bottom <= 1023u &&
           raster->draw_offset_x >= -1024 && raster->draw_offset_x <= 1023 &&
           raster->draw_offset_y >= -1024 && raster->draw_offset_y <= 1023 &&
           raster->texture_window_mask_x <= 31u &&
           raster->texture_window_mask_y <= 31u &&
           raster->texture_window_offset_x <= 31u &&
           raster->texture_window_offset_y <= 31u;
}

static bool compute_fog(uint32_t mode, int32_t projection_distance,
                        int16_t *depth_cue_a, int32_t *depth_cue_b) {
    const int32_t near_distance = mode == 2u ? 0xb00 : 0x800;
    const int32_t far_distance = 0xe80;
    const int32_t range = far_distance - near_distance;
    int64_t value;

    if (projection_distance == 0 || range <= 99) return false;
    value = ((int64_t)-near_distance * far_distance / range) * 256;
    value /= projection_distance;
    if (value < INT16_MIN) value = INT16_MIN;
    if (value > INT16_MAX) value = INT16_MAX;
    *depth_cue_a = (int16_t)value;
    value = ((int64_t)far_distance * 4096 / range) * 4096;
    *depth_cue_b = (int32_t)(uint32_t)value;
    return true;
}

static bool caller_is_cloud_renderer(uint32_t caller_return) {
    return caller_return == UINT32_C(0x80071b68) ||
           caller_return == UINT32_C(0x80076b1c) ||
           caller_return == UINT32_C(0x80077a4c) ||
           caller_return == UINT32_C(0x80078a48);
}

static bool ram_array_is_valid(uint32_t address, uint32_t size,
                               uint32_t alignment) {
    return address >= PSX_RAM_START && address <= PSX_RAM_END - size &&
           (address & (alignment - 1u)) == 0u;
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
    const int32_t column = (int32_t)(quad & 1u);
    const int32_t row = (int32_t)(quad >> 1u);
    XgHost3dVector value = { 0 };

    value.x = (int16_t)(-192 + column * 192 +
                        (int32_t)(vertex & 1u) * 192);
    value.y = (int16_t)(-(int32_t)layer * 8);
    value.z = (int16_t)(192 - row * 192 -
                        (vertex >= 2u ? 192 : 0));
    return value;
}

static void apply_material(const XgWorldCloudsRasterState *raster,
                           XgRenderIrMaterialState *material) {
    *material = (XgRenderIrMaterialState){
        .tpage = 0x003fu,
        .texture_page_x = 15u,
        .texture_page_y = 1u,
        .clut_x = 304u,
        .clut_y = 510u,
        .draw_area_left = raster->draw_area_left,
        .draw_area_top = raster->draw_area_top,
        .draw_area_right = raster->draw_area_right,
        .draw_area_bottom = raster->draw_area_bottom,
        .draw_offset_x = raster->draw_offset_x,
        .draw_offset_y = raster->draw_offset_y,
        .texture_window_mask_x = raster->texture_window_mask_x,
        .texture_window_mask_y = raster->texture_window_mask_y,
        .texture_window_offset_x = raster->texture_window_offset_x,
        .texture_window_offset_y = raster->texture_window_offset_y,
        .texture_depth = XG_RENDER_IR_TEXTURE_4_BIT,
        .shading = XG_RENDER_IR_SHADING_FLAT,
        .textured = true,
        .semi_transparent = true,
        .blend_mode = XG_RENDER_IR_BLEND_ADD,
        .dither = raster->dither,
        .mask_set = raster->mask_set,
        .mask_check = raster->mask_check,
    };
}

static XgWorldCloudsCaptureResult read_trig(
    CloudCaptureAccess *access, uint32_t slot,
    XgWorldCloudTrigValue *out_value) {
    uint32_t word;
    XgWorldCloudsCaptureResult result = read_u32(
        access, access->trig_addresses[slot], &word);

    if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;
    out_value->sine = low_s16(word);
    out_value->cosine = low_s16(word >> 16u);
    return XG_WORLD_CLOUDS_CAPTURE_OK;
}

XgWorldCloudsCaptureResult xg_world_clouds_source_capture(
    const XgWorldCloudsCaptureRequest *request,
    const XgWorldCloudsAuthenticatedReader *reader,
    XgWorldCloudsCapture *out_capture) {
    static const uint16_t expected_uv[XG_WORLD_CLOUD_UV_BASE_COUNT] = {
        0x0000u, 0x0040u, 0x0080u, 0x0000u,
        0x00c0u, 0x4000u, 0x4040u, 0x0000u,
    };
    XgWorldCloudsCapture capture = { 0 };
    CloudCaptureAccess access = { 0 };
    XgWorldCloudsCaptureResult result;
    uint32_t word;
    uint32_t index;
    uint16_t half;
    int16_t pitch;
    int16_t yaw;
    int16_t offset_x;
    int16_t offset_z;
    int32_t pitch_angle;
    int32_t negative_yaw_angle;
    int32_t positive_yaw_angle;
    uint32_t fog_mode;
    XgWorldCloudTrigValue wedge_center;

    if (out_capture == NULL)
        return XG_WORLD_CLOUDS_CAPTURE_INVALID_ARGUMENT;
    memset(out_capture, 0, sizeof(*out_capture));
    if (request == NULL || reader == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || request->authentication_generation == 0u ||
        !caller_is_cloud_renderer(request->caller_return) ||
        request->screen_x_cull_margin < 0 ||
        request->screen_x_cull_margin > (INT32_MAX - 320) / 2 ||
        request->projection_distance == 0u ||
        !request->projection_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_CLOUDS_CAPTURE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_CLOUDS_CAPTURE_UNAUTHENTICATED;

    access.reader = reader;
#define READ_U16(address, output)                                               \
    do {                                                                        \
        result = read_u16(&access, (address), (output));                         \
        if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;                 \
    } while (0)
#define READ_U32(address, output)                                               \
    do {                                                                        \
        result = read_u32(&access, (address), (output));                         \
        if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;                 \
    } while (0)

    READ_U32(CLOUD_POSITION_POINTER, &capture.position_array_address);
    access.position_base = capture.position_array_address;
    READ_U32(CLOUD_VELOCITY_POINTER, &capture.velocity_array_address);
    access.velocity_base = capture.velocity_array_address;
    if (!ram_array_is_valid(access.position_base,
                            XG_WORLD_CLOUD_COUNT * CLOUD_POSITION_STRIDE, 4u) ||
        !ram_array_is_valid(access.velocity_base,
                            XG_WORLD_CLOUD_COUNT * CLOUD_VELOCITY_STRIDE, 2u))
        return XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH;

    for (index = 0u; index < 8u; ++index) {
        READ_U32(CLOUD_CAMERA_MATRIX + index * 4u, &word);
        parse_matrix_word(&capture.source.camera, index, word);
        READ_U32(CLOUD_BILLBOARD_MATRIX + index * 4u, &word);
        parse_matrix_word(&capture.source.billboard, index, word);
    }
    for (index = 0u;
         index < XG_WORLD_CLOUD_FAR_QUAD_COUNT * XG_HOST_3D_VERTEX_COUNT;
         ++index) {
        XgHost3dVector *vertex =
            &capture.source.far_vertices[index / XG_HOST_3D_VERTEX_COUNT]
                                         [index % XG_HOST_3D_VERTEX_COUNT];
        const XgHost3dVector expected = expected_far_vertex(index);

        READ_U32(CLOUD_FAR_VERTEX_TABLE + index * 8u, &word);
        vertex->x = low_s16(word);
        vertex->y = low_s16(word >> 16u);
        READ_U32(CLOUD_FAR_VERTEX_TABLE + index * 8u + 4u, &word);
        vertex->z = low_s16(word);
        vertex->pad = (uint16_t)(word >> 16u);
        if (memcmp(vertex, &expected, sizeof(*vertex)) != 0)
            return XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH;
    }
    for (index = 0u;
         index < XG_WORLD_CLOUD_MIDDLE_QUAD_COUNT * XG_HOST_3D_VERTEX_COUNT;
         ++index) {
        XgHost3dVector *vertex =
            &capture.source.middle_vertices[index / XG_HOST_3D_VERTEX_COUNT]
                                            [index % XG_HOST_3D_VERTEX_COUNT];
        const XgHost3dVector expected = expected_middle_vertex(index);

        READ_U32(CLOUD_MIDDLE_VERTEX_TABLE + index * 8u, &word);
        vertex->x = low_s16(word);
        vertex->y = low_s16(word >> 16u);
        READ_U32(CLOUD_MIDDLE_VERTEX_TABLE + index * 8u + 4u, &word);
        vertex->z = low_s16(word);
        vertex->pad = (uint16_t)(word >> 16u);
        if (memcmp(vertex, &expected, sizeof(*vertex)) != 0)
            return XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH;
    }
    for (index = 0u; index < XG_WORLD_CLOUD_UV_BASE_COUNT / 2u; ++index) {
        READ_U32(CLOUD_UV_TABLE + index * 4u, &word);
        capture.source.uv_base[index * 2u] = (uint16_t)word;
        capture.source.uv_base[index * 2u + 1u] = (uint16_t)(word >> 16u);
    }
    if (memcmp(capture.source.uv_base, expected_uv, sizeof(expected_uv)) != 0)
        return XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH;

    READ_U32(CLOUD_CAMERA_ORIGIN_X, &word);
    capture.source.camera_origin_x = (int32_t)(word & UINT32_C(0x01ffffff));
    READ_U32(CLOUD_CAMERA_ORIGIN_Z, &word);
    capture.source.camera_origin_z = (int32_t)(word & UINT32_C(0x01ffffff));
    READ_U32(CLOUD_FOG_MODE, &fog_mode);
    READ_U16(CLOUD_PITCH, &half);
    pitch = (int16_t)half;
    READ_U16(CLOUD_YAW, &half);
    yaw = (int16_t)half;
    READ_U16(CLOUD_WEDGE_OFFSET_X, &half);
    offset_x = (int16_t)half;
    READ_U16(CLOUD_WEDGE_OFFSET_Z, &half);
    offset_z = (int16_t)half;

    pitch_angle = ((int32_t)pitch + 0x400) / 8;
    negative_yaw_angle = -(int32_t)yaw;
    positive_yaw_angle = (int32_t)yaw;
    access.trig_addresses[0] = CLOUD_TRIG_TABLE +
        rotation_trig_index(pitch_angle) * 4u;
    access.trig_addresses[1] = CLOUD_TRIG_TABLE +
        rotation_trig_index(negative_yaw_angle) * 4u;
    access.trig_addresses[2] = CLOUD_TRIG_TABLE +
        rotation_trig_index(positive_yaw_angle) * 4u;
    access.trig_addresses[3] = CLOUD_TRIG_TABLE +
        ((uint32_t)((int32_t)yaw - 0x169) & 0xfffu) * 4u;
    access.trig_addresses[4] = CLOUD_TRIG_TABLE +
        ((uint32_t)((int32_t)yaw + 0x169) & 0xfffu) * 4u;
    access.trig_addresses[5] = CLOUD_TRIG_TABLE +
        ((uint32_t)(int32_t)yaw & 0xfffu) * 4u;
    result = read_trig(&access, 0u, &capture.source.pitch_rotation);
    if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;
    result = read_trig(&access, 1u, &capture.source.negative_yaw_rotation);
    if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;
    result = read_trig(&access, 2u, &capture.source.positive_yaw_rotation);
    if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;
    if (pitch_angle < 0)
        capture.source.pitch_rotation.sine =
            negate_s16(capture.source.pitch_rotation.sine);
    if (negative_yaw_angle < 0)
        capture.source.negative_yaw_rotation.sine =
            negate_s16(capture.source.negative_yaw_rotation.sine);
    if (positive_yaw_angle < 0)
        capture.source.positive_yaw_rotation.sine =
            negate_s16(capture.source.positive_yaw_rotation.sine);
    result = read_trig(&access, 3u, &capture.source.wedge_left);
    if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;
    result = read_trig(&access, 4u, &capture.source.wedge_right);
    if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;
    result = read_trig(&access, 5u, &wedge_center);
    if (result != XG_WORLD_CLOUDS_CAPTURE_OK) return result;

    capture.source.wedge_offset_x = (int32_t)offset_x * 2 +
        shift_right_floor(-(int32_t)wedge_center.sine, 1u);
    capture.source.wedge_offset_z = (int32_t)offset_z * 2 +
        shift_right_floor(-(int32_t)wedge_center.cosine, 1u);

    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        const uint32_t position =
            access.position_base + index * CLOUD_POSITION_STRIDE;
        const uint32_t velocity =
            access.velocity_base + index * CLOUD_VELOCITY_STRIDE;

        READ_U32(position, &word);
        capture.source.positions[index].x = (int32_t)word;
        READ_U32(position + 4u, &word);
        capture.source.positions[index].y = (int32_t)word;
        READ_U32(position + 8u, &word);
        capture.source.positions[index].z = (int32_t)word;
        READ_U16(velocity, &half);
        capture.source.velocities[index].x = (int16_t)half;
        READ_U16(velocity + 2u, &half);
        capture.source.velocities[index].uv_group = half;
        READ_U16(velocity + 4u, &half);
        capture.source.velocities[index].z = (int16_t)half;
        if (capture.source.velocities[index].uv_group > 1u)
            return XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH;
    }

#undef READ_U16
#undef READ_U32

    capture.source.screen_offset_x = request->screen_offset_x;
    capture.source.screen_offset_y = request->screen_offset_y;
    capture.source.screen_x_cull_margin = request->screen_x_cull_margin;
    capture.source.projection_distance = request->projection_distance;
    if (!compute_fog(fog_mode, request->projection_distance,
                     &capture.source.depth_cue_a,
                     &capture.source.depth_cue_b))
        return XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH;
    apply_material(&request->raster, &capture.source.material);
    capture.authenticated_read_count = access.read_count;
    capture.authenticated_read_bytes = access.read_bytes;
    capture.authenticated = true;
    capture.sealed = true;
    *out_capture = capture;
    return XG_WORLD_CLOUDS_CAPTURE_OK;
}
