#include "xg_world_terrain_water_source_capture.h"

#include <limits.h>
#include <string.h>

#define TERRAIN_CAMERA_MATRIX UINT32_C(0x8009c808)
#define TERRAIN_LOCAL_MATRIX UINT32_C(0x8009d534)
#define TERRAIN_POSITION_X UINT32_C(0x8009be28)
#define TERRAIN_POSITION_Z UINT32_C(0x8009be30)
#define TERRAIN_TILE_GRID UINT32_C(0x8009d570)
#define TERRAIN_ACTIVE_TILES UINT32_C(0x8009d618)
#define TERRAIN_QUADRANT_VISIBILITY UINT32_C(0x8009d650)
#define TERRAIN_POINTER_TABLE UINT32_C(0x8009c184)
#define TERRAIN_CLUT_TABLE UINT32_C(0x8009ccb4)
#define TERRAIN_TPAGE_TABLE UINT32_C(0x8009cd54)
#define TERRAIN_TPAGE_SCRATCH_7 UINT32_C(0x1f800316)
#define TERRAIN_WAVE_PHASE_X UINT32_C(0x8009c5bc)
#define TERRAIN_WAVE_PHASE_Z UINT32_C(0x8009c618)
#define TERRAIN_FOG_MODE UINT32_C(0x8009d7cc)
#define TERRAIN_GRID_CENTER_X UINT32_C(0x8009c838)
#define TERRAIN_GRID_CENTER_Z UINT32_C(0x8009c83c)
#define TERRAIN_TRIG_TABLE UINT32_C(0x800523f0)
#define TERRAIN_RESOURCE_MIN UINT32_C(0x80000000)
#define TERRAIN_RESOURCE_MAX UINT32_C(0x801fffff)
#define TERRAIN_RESOURCE_READ_SIZE UINT32_C(0x510)

typedef struct TerrainCaptureAccess {
    const XgWorldTerrainWaterAuthenticatedReader *reader;
    uint32_t resource_bases[XG_WORLD_TERRAIN_WATER_TILE_COUNT];
    uint32_t read_count;
    uint32_t read_bytes;
} TerrainCaptureAccess;

static bool address_in_aligned_range(uint32_t address, uint32_t start,
                                     uint32_t size, uint32_t alignment) {
    return address >= start && address <= start + size - alignment &&
           ((address - start) & (alignment - 1u)) == 0u;
}

static bool resource_address_is_allowed(const TerrainCaptureAccess *access,
                                        uint32_t address) {
    static const uint32_t offsets[4] = { 0u, 0x144u, 0x288u, 0x3ccu };
    uint32_t tile;

    for (tile = 0u; tile < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++tile) {
        uint32_t quadrant;

        if (access->resource_bases[tile] == 0u) continue;
        for (quadrant = 0u; quadrant < 4u; ++quadrant) {
            if (address_in_aligned_range(
                    address, access->resource_bases[tile] + offsets[quadrant],
                    0x144u, 4u))
                return true;
        }
    }
    return false;
}

static bool u16_address_is_allowed(const TerrainCaptureAccess *access,
                                   uint32_t address) {
    return address == TERRAIN_GRID_CENTER_X ||
           address == TERRAIN_GRID_CENTER_Z ||
           address == TERRAIN_TPAGE_SCRATCH_7 ||
           address_in_aligned_range(address, TERRAIN_CLUT_TABLE, 0x80u, 2u) ||
           address_in_aligned_range(address, TERRAIN_TPAGE_TABLE, 0x0eu, 2u) ||
           address_in_aligned_range(address, TERRAIN_TILE_GRID, 0xa2u, 2u) ||
           address_in_aligned_range(address, TERRAIN_ACTIVE_TILES, 0x32u, 2u) ||
           address_in_aligned_range(address, TERRAIN_QUADRANT_VISIBILITY,
                                    0xc8u, 2u);
}

static bool u32_address_is_allowed(const TerrainCaptureAccess *access,
                                   uint32_t address) {
    if (address_in_aligned_range(address, TERRAIN_CAMERA_MATRIX, 0x20u, 4u) ||
        address_in_aligned_range(address, TERRAIN_LOCAL_MATRIX, 0x20u, 4u) ||
        address_in_aligned_range(address, TERRAIN_POINTER_TABLE, 0x400u, 4u) ||
        address_in_aligned_range(address, TERRAIN_TRIG_TABLE, 0x4000u, 4u) ||
        resource_address_is_allowed(access, address) ||
        address == TERRAIN_POSITION_X || address == TERRAIN_POSITION_Z ||
        address == TERRAIN_WAVE_PHASE_X || address == TERRAIN_WAVE_PHASE_Z ||
        address == TERRAIN_FOG_MODE)
        return true;
    return false;
}

static XgWorldTerrainWaterCaptureResult read_u16(
    TerrainCaptureAccess *access, uint32_t address, uint16_t *out_value) {
    if (access->read_count >=
            XG_WORLD_TERRAIN_WATER_MAX_AUTHENTICATED_READS ||
        !u16_address_is_allowed(access, address))
        return XG_WORLD_TERRAIN_WATER_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, address, out_value))
        return XG_WORLD_TERRAIN_WATER_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_TERRAIN_WATER_CAPTURE_OK;
}

static XgWorldTerrainWaterCaptureResult read_u32(
    TerrainCaptureAccess *access, uint32_t address, uint32_t *out_value) {
    if (access->read_count >=
            XG_WORLD_TERRAIN_WATER_MAX_AUTHENTICATED_READS ||
        !u32_address_is_allowed(access, address))
        return XG_WORLD_TERRAIN_WATER_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_WORLD_TERRAIN_WATER_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_TERRAIN_WATER_CAPTURE_OK;
}

static int16_t low_s16(uint32_t word) {
    return (int16_t)(uint16_t)word;
}

static int32_t shift_right_floor(int32_t value, unsigned bits) {
    uint32_t magnitude;

    if (value >= 0) return value >> bits;
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

bool xg_world_terrain_water_caller_is_valid(uint32_t caller_return) {
    return caller_return == UINT32_C(0x80071b38) ||
           caller_return == UINT32_C(0x80076aec) ||
           caller_return == UINT32_C(0x80078a18) ||
           caller_return == UINT32_C(0x80077a1c);
}

static bool raster_is_valid(const XgWorldTerrainWaterRasterState *raster) {
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

static void apply_material(const XgWorldTerrainWaterRasterState *raster,
                           XgRenderIrMaterialState *material) {
    *material = (XgRenderIrMaterialState){
        .draw_area_left = raster->draw_area_left,
        .draw_area_top = raster->draw_area_top,
        .draw_area_right = raster->draw_area_right,
        .draw_area_bottom = raster->draw_area_bottom,
        .draw_offset_x = raster->draw_offset_x,
        .draw_offset_y = raster->draw_offset_y,
        .texture_depth = XG_RENDER_IR_TEXTURE_4_BIT,
        .texture_window_mask_x = raster->texture_window_mask_x,
        .texture_window_mask_y = raster->texture_window_mask_y,
        .texture_window_offset_x = raster->texture_window_offset_x,
        .texture_window_offset_y = raster->texture_window_offset_y,
        .shading = XG_RENDER_IR_SHADING_FLAT,
        .textured = true,
        .blend_mode = XG_RENDER_IR_BLEND_AVERAGE,
        .dither = raster->dither,
        .mask_set = raster->mask_set,
        .mask_check = raster->mask_check,
    };
}

XgWorldTerrainWaterCaptureResult xg_world_terrain_water_source_capture(
    const XgWorldTerrainWaterCaptureRequest *request,
    const XgWorldTerrainWaterAuthenticatedReader *reader,
    XgWorldTerrainWaterCapture *out_capture) {
    static const uint32_t resource_offsets[4] = {
        0u, 0x144u, 0x288u, 0x3ccu,
    };
    XgWorldTerrainWaterCapture capture = { 0 };
    TerrainCaptureAccess access = { 0 };
    int32_t center_x;
    int32_t center_z;
    uint32_t word;
    uint32_t index;
    uint16_t half;
    XgWorldTerrainWaterCaptureResult result;

    if (out_capture == NULL)
        return XG_WORLD_TERRAIN_WATER_CAPTURE_INVALID_ARGUMENT;
    memset(out_capture, 0, sizeof(*out_capture));
    if (request == NULL || reader == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || request->authentication_generation == 0u ||
        !xg_world_terrain_water_caller_is_valid(request->caller_return) ||
        request->screen_x_cull_margin < 0 ||
        request->screen_x_cull_margin > (INT32_MAX - 320) / 2 ||
        request->projection_distance == 0u ||
        !request->projection_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_TERRAIN_WATER_CAPTURE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_TERRAIN_WATER_CAPTURE_UNAUTHENTICATED;
    access.reader = reader;

#define READ_U16(address, output)                                               \
    do {                                                                        \
        result = read_u16(&access, (address), (output));                         \
        if (result != XG_WORLD_TERRAIN_WATER_CAPTURE_OK) return result;          \
    } while (0)
#define READ_U32(address, output)                                               \
    do {                                                                        \
        result = read_u32(&access, (address), (output));                         \
        if (result != XG_WORLD_TERRAIN_WATER_CAPTURE_OK) return result;          \
    } while (0)

    READ_U32(TERRAIN_POSITION_X, &word);
    capture.source.position_x = (int32_t)word;
    READ_U32(TERRAIN_POSITION_Z, &word);
    capture.source.position_z = (int32_t)word;
    for (index = 0u; index < 8u; ++index) {
        READ_U32(TERRAIN_CAMERA_MATRIX + index * 4u, &word);
        parse_matrix_word(&capture.source.camera, index, word);
        READ_U32(TERRAIN_LOCAL_MATRIX + index * 4u, &word);
        parse_matrix_word(&capture.source.local, index, word);
    }
    READ_U32(TERRAIN_WAVE_PHASE_X, &capture.source.wave_phase_x);
    READ_U32(TERRAIN_WAVE_PHASE_Z, &capture.source.wave_phase_z);
    READ_U32(TERRAIN_FOG_MODE, &capture.source.fog_mode);
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE; ++index) {
        const uint32_t phase_x =
            (capture.source.wave_phase_x + index * 0x200u) & 0xfffu;
        const uint32_t phase_z =
            (capture.source.wave_phase_z + index * 0x200u) & 0xfffu;

        READ_U32(TERRAIN_TRIG_TABLE + phase_x * 4u, &word);
        capture.source.wave_sine_x[index] = low_s16(word);
        READ_U32(TERRAIN_TRIG_TABLE + phase_z * 4u, &word);
        capture.source.wave_sine_z[index] = low_s16(word);
    }
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_CLUT_COUNT; ++index)
        READ_U16(TERRAIN_CLUT_TABLE + index * 2u,
                 &capture.source.cluts[index]);
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_PAGE_COUNT - 1u; ++index)
        READ_U16(TERRAIN_TPAGE_TABLE + index * 2u,
                 &capture.source.tpages[index]);
    READ_U16(TERRAIN_TPAGE_SCRATCH_7,
             &capture.source.tpages[XG_WORLD_TERRAIN_WATER_PAGE_COUNT - 1u]);

    READ_U16(TERRAIN_GRID_CENTER_X, &half);
    center_x = (int16_t)half;
    READ_U16(TERRAIN_GRID_CENTER_Z, &half);
    center_z = (int16_t)half;
    if (center_x < 0 || center_x > 4 || center_z < 0 || center_z > 4)
        return XG_WORLD_TERRAIN_WATER_CAPTURE_SOURCE_MISMATCH;
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++index) {
        const uint32_t row = index / 5u;
        const uint32_t column = index % 5u;
        const uint32_t grid_index =
            ((uint32_t)center_z + row) * 9u + (uint32_t)center_x + column;
        uint16_t terrain_id;
        uint16_t active;
        uint32_t quadrant;

        READ_U16(TERRAIN_ACTIVE_TILES + index * 2u, &active);
        capture.source.tiles[index].active = active != UINT16_MAX;
        for (quadrant = 0u; quadrant < 4u; ++quadrant) {
            READ_U16(TERRAIN_QUADRANT_VISIBILITY + index * 8u + quadrant * 2u,
                     &capture.source.quadrant_visibility[index][quadrant]);
        }
        if (!capture.source.tiles[index].active) continue;
        READ_U16(TERRAIN_TILE_GRID + grid_index * 2u, &terrain_id);
        if (terrain_id >= 256u)
            return XG_WORLD_TERRAIN_WATER_CAPTURE_SOURCE_MISMATCH;
        capture.source.tiles[index].terrain_id = terrain_id;
        READ_U32(TERRAIN_POINTER_TABLE + (uint32_t)terrain_id * 4u,
                 &capture.source.tiles[index].resource_address);
        access.resource_bases[index] =
            capture.source.tiles[index].resource_address;
        if (access.resource_bases[index] != 0u) {
            if ((access.resource_bases[index] & 3u) != 0u ||
                access.resource_bases[index] < TERRAIN_RESOURCE_MIN ||
                access.resource_bases[index] >
                    TERRAIN_RESOURCE_MAX - TERRAIN_RESOURCE_READ_SIZE + 1u)
                return XG_WORLD_TERRAIN_WATER_CAPTURE_SOURCE_MISMATCH;
            capture.source.tiles[index].has_data = true;
            ++capture.captured_resource_count;
        }
    }
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++index) {
        uint32_t quadrant;

        if (!capture.source.tiles[index].has_data) continue;
        for (quadrant = 0u; quadrant < 4u; ++quadrant) {
            uint16_t combined_visibility = 0u;
            uint32_t sample;
            uint32_t other;

            for (other = 0u; other < 4u; ++other)
                combined_visibility |=
                    capture.source.quadrant_visibility[index][other];
            if (combined_visibility == UINT16_MAX &&
                capture.source.quadrant_visibility[index][quadrant] ==
                    UINT16_MAX)
                continue;

            for (sample = 0u; sample < 81u; ++sample) {
                READ_U32(access.resource_bases[index] +
                             resource_offsets[quadrant] + sample * 4u,
                         &capture.source.tiles[index].samples[quadrant][sample]);
            }
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
    return XG_WORLD_TERRAIN_WATER_CAPTURE_OK;
}
