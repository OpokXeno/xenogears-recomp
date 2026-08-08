#include "xg_world_effects_source_capture.h"

#include <limits.h>
#include <string.h>

enum {
    EFFECTS_CALLER_RETURN = 0x80071aa8u,
    EFFECTS_PARTICLE_POINTER = 0x8009bdf4u,
    EFFECTS_CAMERA_MATRIX = 0x8009c808u,
    EFFECTS_BILLBOARD_MATRIX = 0x8009a180u,
    EFFECTS_VERTEX_TABLE = 0x8009b040u,
    EFFECTS_UV_TABLE = 0x8009aff0u,
    EFFECTS_CAMERA_ORIGIN_X = 0x8009be28u,
    EFFECTS_CAMERA_ORIGIN_Z = 0x8009be30u,
    EFFECTS_WRAP_X = 0x8009d160u,
    EFFECTS_WRAP_Z = 0x8009d2b4u,
    EFFECTS_TRIG_TABLE = 0x800523f0u,
    EFFECTS_PARTICLE_STRIDE = 0x4cu,
};

typedef struct EffectsCaptureAccess {
    const XgWorldEffectsAuthenticatedReader *reader;
    uint32_t particle_base;
    uint32_t read_count;
    uint32_t read_bytes;
} EffectsCaptureAccess;

static bool address_in_aligned_range(uint32_t address, uint32_t start,
                                     uint32_t size) {
    return address >= start && address <= start + size - 4u &&
           ((address - start) & 3u) == 0u;
}

static bool particle_offset_is_allowed(uint32_t address, uint32_t base,
                                       uint32_t width) {
    uint32_t offset;
    uint32_t field;

    if (address < base || address > base +
            XG_WORLD_EFFECTS_SOURCE_CAPACITY * EFFECTS_PARTICLE_STRIDE - width)
        return false;
    offset = address - base;
    field = offset % EFFECTS_PARTICLE_STRIDE;
    if (width == 2u)
        return field == 2u || field == 6u || field == 0x48u;
    return field == 8u || field == 0x0cu || field == 0x10u ||
           field == 0x38u || field == 0x40u || field == 0x44u;
}

static bool u16_read_is_allowed(const EffectsCaptureAccess *access,
                                uint32_t address) {
    return particle_offset_is_allowed(address, access->particle_base, 2u);
}

static bool u32_read_is_allowed(const EffectsCaptureAccess *access,
                                uint32_t address) {
    return address == EFFECTS_PARTICLE_POINTER ||
           address == EFFECTS_CAMERA_ORIGIN_X ||
           address == EFFECTS_CAMERA_ORIGIN_Z || address == EFFECTS_WRAP_X ||
           address == EFFECTS_WRAP_Z ||
           address_in_aligned_range(address, EFFECTS_CAMERA_MATRIX, 0x20u) ||
           address_in_aligned_range(address, EFFECTS_BILLBOARD_MATRIX, 0x20u) ||
           address_in_aligned_range(address, EFFECTS_VERTEX_TABLE, 0x140u) ||
           address_in_aligned_range(address, EFFECTS_UV_TABLE, 0x50u) ||
           address_in_aligned_range(address, EFFECTS_TRIG_TABLE, 0x4000u) ||
           particle_offset_is_allowed(address, access->particle_base, 4u);
}

static XgWorldEffectsCaptureResult read_u16(EffectsCaptureAccess *access,
                                             uint32_t address,
                                             uint16_t *out_value) {
    if (access->read_count >= XG_WORLD_EFFECTS_MAX_AUTHENTICATED_READS ||
        !u16_read_is_allowed(access, address))
        return XG_WORLD_EFFECTS_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, address, out_value))
        return XG_WORLD_EFFECTS_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_EFFECTS_CAPTURE_OK;
}

static XgWorldEffectsCaptureResult read_u32(EffectsCaptureAccess *access,
                                             uint32_t address,
                                             uint32_t *out_value) {
    if (access->read_count >= XG_WORLD_EFFECTS_MAX_AUTHENTICATED_READS ||
        !u32_read_is_allowed(access, address))
        return XG_WORLD_EFFECTS_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_WORLD_EFFECTS_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_EFFECTS_CAPTURE_OK;
}

static int16_t low_s16(uint32_t value) {
    return (int16_t)(uint16_t)value;
}

static int32_t shift_right_12(int32_t value) {
    uint32_t magnitude;

    if (value >= 0) return value / 4096;
    magnitude = (uint32_t)(-(value + 1)) + 1u;
    return -(int32_t)((magnitude + 4095u) >> 12u);
}

static bool raster_is_valid(const XgWorldEffectsRasterState *raster) {
    return raster->draw_area_left <= raster->draw_area_right &&
           raster->draw_area_top <= raster->draw_area_bottom &&
           raster->draw_area_right <= 1023u &&
           raster->draw_area_bottom <= 1023u &&
           raster->draw_offset_x >= -1024 && raster->draw_offset_x <= 1023 &&
           raster->draw_offset_y >= -1024 && raster->draw_offset_y <= 1023;
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

static void apply_material(const XgWorldEffectsRasterState *raster,
                           XgRenderIrMaterialState *material) {
    *material = (XgRenderIrMaterialState){
        .clut_x = 256u,
        .clut_y = 511u,
        .draw_area_left = raster->draw_area_left,
        .draw_area_top = raster->draw_area_top,
        .draw_area_right = raster->draw_area_right,
        .draw_area_bottom = raster->draw_area_bottom,
        .draw_offset_x = raster->draw_offset_x,
        .draw_offset_y = raster->draw_offset_y,
        .shading = XG_RENDER_IR_SHADING_FLAT,
        .textured = true,
        .semi_transparent = true,
        .dither = raster->dither,
        .mask_set = raster->mask_set,
        .mask_check = raster->mask_check,
    };
}

XgWorldEffectsCaptureResult xg_world_effects_source_capture(
    const XgWorldEffectsCaptureRequest *request,
    const XgWorldEffectsAuthenticatedReader *reader,
    XgWorldEffectsCapture *out_capture) {
    XgWorldEffectsCapture capture = { 0 };
    EffectsCaptureAccess access = { 0 };
    uint32_t word;
    uint32_t index;
    XgWorldEffectsCaptureResult result;

    if (out_capture == NULL)
        return XG_WORLD_EFFECTS_CAPTURE_INVALID_ARGUMENT;
    memset(out_capture, 0, sizeof(*out_capture));
    if (request == NULL || reader == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || request->authentication_generation == 0u ||
        request->caller_return != EFFECTS_CALLER_RETURN ||
        request->screen_x_cull_margin < 0 ||
        request->screen_x_cull_margin > INT32_MAX - 320 ||
        request->projection_distance == 0u ||
        !request->projection_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_EFFECTS_CAPTURE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_EFFECTS_CAPTURE_UNAUTHENTICATED;

    access.reader = reader;
    result = read_u32(&access, EFFECTS_PARTICLE_POINTER,
                      &capture.particle_array_address);
    if (result != XG_WORLD_EFFECTS_CAPTURE_OK) return result;
    access.particle_base = capture.particle_array_address;
    if ((access.particle_base & 3u) != 0u ||
        access.particle_base > UINT32_MAX -
            XG_WORLD_EFFECTS_SOURCE_CAPACITY * EFFECTS_PARTICLE_STRIDE)
        return XG_WORLD_EFFECTS_CAPTURE_SOURCE_MISMATCH;

#define READ_U16(address, output)                                               \
    do {                                                                        \
        result = read_u16(&access, (address), (output));                         \
        if (result != XG_WORLD_EFFECTS_CAPTURE_OK) return result;                \
    } while (0)
#define READ_U32(address, output)                                               \
    do {                                                                        \
        result = read_u32(&access, (address), (output));                         \
        if (result != XG_WORLD_EFFECTS_CAPTURE_OK) return result;                \
    } while (0)

    for (index = 0u; index < 8u; ++index) {
        READ_U32(EFFECTS_CAMERA_MATRIX + index * 4u, &word);
        parse_matrix_word(&capture.source.camera, index, word);
        READ_U32(EFFECTS_BILLBOARD_MATRIX + index * 4u, &word);
        parse_matrix_word(&capture.source.billboard, index, word);
    }
    for (index = 0u;
         index < XG_WORLD_EFFECTS_TYPE_COUNT * XG_HOST_3D_VERTEX_COUNT;
         ++index) {
        XgHost3dVector *vertex =
            &capture.source.vertices[index / XG_HOST_3D_VERTEX_COUNT]
                                    [index % XG_HOST_3D_VERTEX_COUNT];

        READ_U32(EFFECTS_VERTEX_TABLE + index * 8u, &word);
        vertex->x = low_s16(word);
        vertex->y = low_s16(word >> 16u);
        READ_U32(EFFECTS_VERTEX_TABLE + index * 8u + 4u, &word);
        vertex->z = low_s16(word);
        vertex->pad = (uint16_t)(word >> 16u);
    }
    for (index = 0u; index < XG_WORLD_EFFECTS_TYPE_COUNT * 2u; ++index) {
        READ_U32(EFFECTS_UV_TABLE + index * 4u, &word);
        capture.source.uv[index / 2u][(index % 2u) * 2u] = (uint16_t)word;
        capture.source.uv[index / 2u][(index % 2u) * 2u + 1u] =
            (uint16_t)(word >> 16u);
    }
    READ_U32(EFFECTS_CAMERA_ORIGIN_X, &word);
    capture.source.camera_origin_x = shift_right_12((int32_t)word);
    READ_U32(EFFECTS_CAMERA_ORIGIN_Z, &word);
    capture.source.camera_origin_z = shift_right_12((int32_t)word);
    READ_U32(EFFECTS_WRAP_X, &word);
    capture.source.wrap_x = (int32_t)word;
    READ_U32(EFFECTS_WRAP_Z, &word);
    capture.source.wrap_z = (int32_t)word;

    for (index = 0u; index < XG_WORLD_EFFECTS_SOURCE_CAPACITY; ++index) {
        XgWorldEffectsParticleSource *particle = &capture.source.particles[index];
        const uint32_t base = access.particle_base + index * EFFECTS_PARTICLE_STRIDE;
        uint16_t half;

        READ_U16(base + 6u, &half);
        if (half == 0u) continue;
        if (half >= XG_WORLD_EFFECTS_TYPE_COUNT)
            return XG_WORLD_EFFECTS_CAPTURE_SOURCE_MISMATCH;
        particle->type = (uint8_t)half;
        particle->active = true;
        ++capture.active_source_count;
        READ_U16(base + 2u, &half);
        particle->angle = (int16_t)half;
        READ_U32(base + 8u, &word);
        particle->position[0] = (int32_t)word;
        READ_U32(base + 0x0cu, &word);
        particle->position[1] = (int32_t)word;
        READ_U32(base + 0x10u, &word);
        particle->position[2] = (int32_t)word;
        READ_U32(base + 0x38u, &word);
        particle->scale_x = (uint16_t)word;
        particle->scale_y = (uint16_t)(word >> 16u);
        READ_U32(base + 0x40u, &word);
        particle->red = (uint8_t)word;
        particle->green = (uint8_t)(word >> 8u);
        particle->blue = (uint8_t)(word >> 16u);
        READ_U32(base + 0x44u, &word);
        particle->rotate = ((word >> 24u) & 1u) != 0u;
        READ_U16(base + 0x48u, &particle->tpage);
        if (particle->rotate) {
            const uint32_t magnitude = particle->angle < 0
                ? (uint32_t)(-(int32_t)particle->angle)
                : (uint32_t)particle->angle;

            READ_U32(EFFECTS_TRIG_TABLE + (magnitude & 0xfffu) * 4u, &word);
            particle->sine = low_s16(word);
            if (particle->angle < 0)
                particle->sine = (int16_t)-(int32_t)particle->sine;
            particle->cosine = low_s16(word >> 16u);
        }
    }

#undef READ_U16
#undef READ_U32

    capture.source.screen_offset_x = request->screen_offset_x;
    capture.source.screen_offset_y = request->screen_offset_y;
    capture.source.screen_x_cull_margin = request->screen_x_cull_margin;
    capture.source.projection_distance = request->projection_distance;
    apply_material(&request->raster, &capture.source.material);
    capture.authenticated_read_count = access.read_count;
    capture.authenticated_read_bytes = access.read_bytes;
    capture.authenticated = true;
    capture.sealed = true;
    *out_capture = capture;
    return XG_WORLD_EFFECTS_CAPTURE_OK;
}
