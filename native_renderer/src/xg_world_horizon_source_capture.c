#include "xg_world_horizon_source_capture.h"

#include <limits.h>
#include <string.h>

enum {
    HORIZON_CALLER_RETURN = 0x80071b58u,
    HORIZON_VERTEX_BASE = 0x8009a300u,
    HORIZON_CAMERA_MATRIX = 0x8009c808u,
    HORIZON_ANGLE = 0x8009bd3au,
    HORIZON_BUFFER_INDEX = 0x8009d7f0u,
    HORIZON_FOG_MODE = 0x8009d7ccu,
    HORIZON_ORDERING_SHIFT = 0x80050100u,
    HORIZON_TRIG_TABLE = 0x800523f0u,
};

typedef struct HorizonCaptureAccess {
    const XgWorldHorizonAuthenticatedReader *reader;
    uint32_t trig_address;
    uint32_t read_count;
    uint32_t read_bytes;
} HorizonCaptureAccess;

static bool address_in_aligned_range(uint32_t address, uint32_t start,
                                     uint32_t size) {
    return address >= start && address <= start + size - 4u &&
           ((address - start) & 3u) == 0u;
}

static bool u32_read_is_allowed(const HorizonCaptureAccess *access,
                                uint32_t address) {
    return address_in_aligned_range(address, HORIZON_CAMERA_MATRIX, 0x20u) ||
           address_in_aligned_range(address, HORIZON_VERTEX_BASE, 0x40u) ||
           address == HORIZON_BUFFER_INDEX || address == HORIZON_FOG_MODE ||
           address == HORIZON_ORDERING_SHIFT ||
           address == access->trig_address;
}

static XgWorldHorizonCaptureResult read_u32(HorizonCaptureAccess *access,
                                             uint32_t address,
                                             uint32_t *out_value) {
    if (access->read_count >= XG_WORLD_HORIZON_MAX_AUTHENTICATED_READS ||
        !u32_read_is_allowed(access, address))
        return XG_WORLD_HORIZON_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_WORLD_HORIZON_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_HORIZON_CAPTURE_OK;
}

static XgWorldHorizonCaptureResult read_angle(HorizonCaptureAccess *access,
                                               uint16_t *out_value) {
    if (access->read_count >= XG_WORLD_HORIZON_MAX_AUTHENTICATED_READS)
        return XG_WORLD_HORIZON_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, HORIZON_ANGLE,
                                  out_value))
        return XG_WORLD_HORIZON_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_HORIZON_CAPTURE_OK;
}

static int16_t low_s16(uint32_t value) {
    return (int16_t)(uint16_t)value;
}

static bool compute_fog(uint32_t mode, int32_t projection_distance,
                        int16_t *depth_cue_a, int32_t *depth_cue_b) {
    const int32_t near_distance = mode == 2u ? 0xb00 : 0x800;
    const int32_t far_distance = 0xe80;
    const int32_t range = far_distance - near_distance;
    int64_t value;

    if (depth_cue_a == NULL || depth_cue_b == NULL ||
        projection_distance == 0 || range <= 99)
        return false;
    value = ((int64_t)-near_distance * far_distance / range) * 256;
    value /= projection_distance;
    if (value < INT16_MIN) value = INT16_MIN;
    if (value > INT16_MAX) value = INT16_MAX;
    *depth_cue_a = (int16_t)value;
    value = ((int64_t)far_distance * 4096 / range) * 4096;
    *depth_cue_b = (int32_t)(uint32_t)value;
    return true;
}

static bool raster_is_valid(const XgWorldHorizonRasterState *raster) {
    return raster->draw_area_left <= raster->draw_area_right &&
           raster->draw_area_top <= raster->draw_area_bottom &&
           raster->draw_area_right <= 1023u &&
           raster->draw_area_bottom <= 1023u &&
           raster->draw_offset_x >= -1024 && raster->draw_offset_x <= 1023 &&
           raster->draw_offset_y >= -1024 && raster->draw_offset_y <= 1023;
}

static void apply_material(const XgWorldHorizonRasterState *raster,
                           XgRenderIrMaterialState *material) {
    *material = (XgRenderIrMaterialState){
        .tpage = 0x003eu,
        .texture_page_x = 14u,
        .texture_page_y = 1u,
        .clut_x = 272u,
        .clut_y = 510u,
        .draw_area_left = raster->draw_area_left,
        .draw_area_top = raster->draw_area_top,
        .draw_area_right = raster->draw_area_right,
        .draw_area_bottom = raster->draw_area_bottom,
        .draw_offset_x = raster->draw_offset_x,
        .draw_offset_y = raster->draw_offset_y,
        .texture_depth = XG_RENDER_IR_TEXTURE_4_BIT,
        .texture_window_mask_x = 16u,
        .shading = XG_RENDER_IR_SHADING_FLAT,
        .textured = true,
        .semi_transparent = true,
        .blend_mode = XG_RENDER_IR_BLEND_ADD,
        .dither = raster->dither,
        .mask_set = raster->mask_set,
        .mask_check = raster->mask_check,
    };
}

XgWorldHorizonCaptureResult xg_world_horizon_source_capture(
    const XgWorldHorizonCaptureRequest *request,
    const XgWorldHorizonAuthenticatedReader *reader,
    XgWorldHorizonCapture *out_capture) {
    static const XgHost3dVector expected_vertices[2][4] = {
        { { -4096, -896, 4032, 0u }, { 0, -896, 4032, 0u },
          { -4096, -640, 4032, 0u }, { 0, -640, 4032, 0u } },
        { { 0, -896, 4032, 0u }, { 4096, -896, 4032, 0u },
          { 0, -640, 4032, 0u }, { 4096, -640, 4032, 0u } },
    };
    XgWorldHorizonCapture capture = { 0 };
    HorizonCaptureAccess access = { 0 };
    XgHost3dMatrix camera = { 0 };
    XgHost3dMatrix local = { 0 };
    XgHost3dMatrix composed;
    uint32_t fog_mode;
    uint32_t angle_word;
    uint32_t word;
    uint32_t quad;
    uint32_t vertex;
    XgWorldHorizonCaptureResult result;

    if (out_capture == NULL) return XG_WORLD_HORIZON_CAPTURE_INVALID_ARGUMENT;
    memset(out_capture, 0, sizeof(*out_capture));
    if (request == NULL || reader == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || request->authentication_generation == 0u ||
        request->caller_return != HORIZON_CALLER_RETURN ||
        request->projection_distance == 0u ||
        !request->projection_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_HORIZON_CAPTURE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_HORIZON_CAPTURE_UNAUTHENTICATED;

    access.reader = reader;
    result = read_angle(&access, &capture.source.angle);
    if (result != XG_WORLD_HORIZON_CAPTURE_OK) return result;
    access.trig_address = HORIZON_TRIG_TABLE +
        ((uint32_t)capture.source.angle & 0xfffu) * 4u;
    result = read_u32(&access, access.trig_address, &angle_word);
    if (result != XG_WORLD_HORIZON_CAPTURE_OK) return result;

#define READ_U32(address, output)                                               \
    do {                                                                        \
        result = read_u32(&access, (address), (output));                         \
        if (result != XG_WORLD_HORIZON_CAPTURE_OK) return result;                \
    } while (0)

    for (quad = 0u; quad < 8u; ++quad) {
        READ_U32(HORIZON_CAMERA_MATRIX + quad * 4u, &word);
        if (quad == 0u) {
            camera.rotation[0][0] = low_s16(word);
            camera.rotation[0][1] = low_s16(word >> 16u);
        } else if (quad == 1u) {
            camera.rotation[0][2] = low_s16(word);
            camera.rotation[1][0] = low_s16(word >> 16u);
        } else if (quad == 2u) {
            camera.rotation[1][1] = low_s16(word);
            camera.rotation[1][2] = low_s16(word >> 16u);
        } else if (quad == 3u) {
            camera.rotation[2][0] = low_s16(word);
            camera.rotation[2][1] = low_s16(word >> 16u);
        } else if (quad == 4u) {
            camera.rotation[2][2] = low_s16(word);
            camera.pad = (uint16_t)(word >> 16u);
        } else {
            camera.translation[quad - 5u] = (int32_t)word;
        }
    }
    for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            XgHost3dVector *value = &capture.source.vertices[quad][vertex];
            const uint32_t address = HORIZON_VERTEX_BASE +
                (quad * XG_HOST_3D_VERTEX_COUNT + vertex) * 8u;

            READ_U32(address, &word);
            value->x = low_s16(word);
            value->y = low_s16(word >> 16u);
            READ_U32(address + 4u, &word);
            value->z = low_s16(word);
            value->pad = (uint16_t)(word >> 16u);
            if (memcmp(value, &expected_vertices[quad][vertex],
                       sizeof(*value)) != 0)
                return XG_WORLD_HORIZON_CAPTURE_SOURCE_MISMATCH;
        }
    }
    READ_U32(HORIZON_FOG_MODE, &fog_mode);
    READ_U32(HORIZON_ORDERING_SHIFT, &capture.source.ordering_shift);
    READ_U32(HORIZON_BUFFER_INDEX, &capture.buffer_index);

#undef READ_U32

    if (capture.source.ordering_shift > 31u || capture.buffer_index > 1u)
        return XG_WORLD_HORIZON_CAPTURE_SOURCE_MISMATCH;
    local.rotation[0][0] = low_s16(angle_word >> 16u);
    local.rotation[0][2] = low_s16(angle_word);
    local.rotation[1][1] = 0x1000;
    local.rotation[2][0] = (int16_t)-(int32_t)low_s16(angle_word);
    local.rotation[2][2] = low_s16(angle_word >> 16u);
    if (!xg_host_3d_comp_matrix(&camera, &local, &composed))
        return XG_WORLD_HORIZON_CAPTURE_SOURCE_MISMATCH;
    memcpy(capture.source.projection.rotation, composed.rotation,
           sizeof(composed.rotation));
    memcpy(capture.source.projection.translation, composed.translation,
           sizeof(composed.translation));
    capture.source.projection.screen_offset_x = request->screen_offset_x;
    capture.source.projection.screen_offset_y = request->screen_offset_y;
    capture.source.projection.projection_distance =
        request->projection_distance;
    if (!compute_fog(fog_mode, request->projection_distance,
                     &capture.source.projection.depth_cue_a,
                     &capture.source.projection.depth_cue_b))
        return XG_WORLD_HORIZON_CAPTURE_SOURCE_MISMATCH;
    apply_material(&request->raster, &capture.source.material);
    capture.authenticated_read_count = access.read_count;
    capture.authenticated_read_bytes = access.read_bytes;
    capture.authenticated = true;
    capture.sealed = true;
    *out_capture = capture;
    return XG_WORLD_HORIZON_CAPTURE_OK;
}
