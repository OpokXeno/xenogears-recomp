#include "xg_world_minimap_source_capture.h"

#include <string.h>

static const uint32_t MINIMAP_CALLER_RETURN = UINT32_C(0x80071b84);
static const uint32_t MINIMAP_ANGLE = UINT32_C(0x8009bd3a);
static const uint32_t MINIMAP_TRIG_TABLE = UINT32_C(0x800523f0);
static const uint32_t MINIMAP_WORLD_X = UINT32_C(0x8009d55c);
static const uint32_t MINIMAP_WORLD_Z = UINT32_C(0x8009d564);
static const uint32_t MINIMAP_TRANSLATION_Z = UINT32_C(0x8009bcdc);
static const uint32_t MINIMAP_VERTICAL_OFFSET = UINT32_C(0x8009be0c);
static const uint32_t MINIMAP_BUFFER_INDEX = UINT32_C(0x8009d7f0);
static const uint32_t MINIMAP_TRIANGLES = UINT32_C(0x8009a340);
static const uint32_t MINIMAP_MARKER_MASK = UINT32_C(0x8006f160);
static const uint32_t MINIMAP_MARKER_COORDINATES = UINT32_C(0x8009b6f4);

static const XgHost3dVector expected_triangles[4][3] = {
    { { 0, 0, 0, 0u }, { -8, -8, 0, 0u }, { -4, -11, 0, 0u } },
    { { 0, 0, 0, 0u }, { -4, -11, 0, 0u }, { 0, -12, 0, 0u } },
    { { 0, 0, 0, 0u }, { 0, -12, 0, 0u }, { 4, -11, 0, 0u } },
    { { 0, 0, 0, 0u }, { 4, -11, 0, 0u }, { 8, -8, 0, 0u } },
};

typedef struct MinimapCaptureAccess {
    const XgWorldMinimapAuthenticatedReader *reader;
    uint32_t read_count;
    uint32_t read_bytes;
} MinimapCaptureAccess;

static bool address_in_aligned_range(uint32_t address, uint32_t start,
                                     uint32_t size) {
    return address >= start && address <= start + size - 4u &&
           ((address - start) & 3u) == 0u;
}

static bool u16_read_is_allowed(uint32_t address) {
    if (address == MINIMAP_ANGLE || address == UINT32_C(0x8006ee60) ||
        address == UINT32_C(0x8006ee64) ||
        address == UINT32_C(0x8006ee78) ||
        address == UINT32_C(0x8006ee7a) ||
        address == UINT32_C(0x8006ee82) ||
        address == UINT32_C(0x8006ee86))
        return true;
    return address >= MINIMAP_MARKER_COORDINATES &&
           address < MINIMAP_MARKER_COORDINATES +
                         XG_WORLD_MINIMAP_MARKER_CAPACITY * 4u &&
           ((address - MINIMAP_MARKER_COORDINATES) & 1u) == 0u;
}

static bool u32_read_is_allowed(uint32_t address) {
    return address == MINIMAP_WORLD_X || address == MINIMAP_WORLD_Z ||
           address == MINIMAP_TRANSLATION_Z ||
           address == MINIMAP_VERTICAL_OFFSET ||
           address == MINIMAP_BUFFER_INDEX ||
           address == MINIMAP_MARKER_MASK ||
           address_in_aligned_range(address, MINIMAP_TRIG_TABLE, 0x4000u) ||
           address_in_aligned_range(address, MINIMAP_TRIANGLES, 0x60u);
}

static XgWorldMinimapCaptureResult read_u16(MinimapCaptureAccess *access,
                                             uint32_t address,
                                             uint16_t *out_value) {
    if (access->read_count >= XG_WORLD_MINIMAP_MAX_AUTHENTICATED_READS ||
        !u16_read_is_allowed(address))
        return XG_WORLD_MINIMAP_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, address, out_value))
        return XG_WORLD_MINIMAP_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_MINIMAP_CAPTURE_OK;
}

static XgWorldMinimapCaptureResult read_u32(MinimapCaptureAccess *access,
                                             uint32_t address,
                                             uint32_t *out_value) {
    if (access->read_count >= XG_WORLD_MINIMAP_MAX_AUTHENTICATED_READS ||
        !u32_read_is_allowed(address))
        return XG_WORLD_MINIMAP_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_WORLD_MINIMAP_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_MINIMAP_CAPTURE_OK;
}

static bool raster_is_valid(const XgWorldMinimapRasterState *raster) {
    return raster->draw_area_left <= raster->draw_area_right &&
           raster->draw_area_top <= raster->draw_area_bottom &&
           raster->draw_area_right <= 1023u &&
           raster->draw_area_bottom <= 1023u &&
           raster->draw_offset_x >= -1024 && raster->draw_offset_x <= 1023 &&
           raster->draw_offset_y >= -1024 && raster->draw_offset_y <= 1023;
}

static int16_t low_s16(uint32_t value) {
    return (int16_t)(uint16_t)value;
}

static uint32_t marker_x_address(uint32_t marker) {
    if (marker == 24u) return UINT32_C(0x8006ee60);
    if (marker == 25u) return UINT32_C(0x8006ee82);
    if (marker == 26u) return UINT32_C(0x8006ee78);
    return MINIMAP_MARKER_COORDINATES + marker * 4u;
}

static uint32_t marker_y_address(uint32_t marker) {
    if (marker == 24u) return UINT32_C(0x8006ee64);
    if (marker == 25u) return UINT32_C(0x8006ee86);
    if (marker == 26u) return UINT32_C(0x8006ee7a);
    return MINIMAP_MARKER_COORDINATES + marker * 4u + 2u;
}

XgWorldMinimapCaptureResult xg_world_minimap_source_capture(
    const XgWorldMinimapCaptureRequest *request,
    const XgWorldMinimapAuthenticatedReader *reader,
    XgWorldMinimapCapture *out_capture) {
    XgWorldMinimapCapture capture = { 0 };
    MinimapCaptureAccess access = { 0 };
    uint32_t trig_zero;
    uint32_t trig_angle;
    uint32_t word;
    uint32_t triangle;
    uint32_t vertex;
    uint32_t marker;
    XgWorldMinimapCaptureResult result;

    if (out_capture == NULL)
        return XG_WORLD_MINIMAP_CAPTURE_INVALID_ARGUMENT;
    memset(out_capture, 0, sizeof(*out_capture));
    if (request == NULL || reader == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || request->authentication_generation == 0u ||
        request->caller_return != MINIMAP_CALLER_RETURN ||
        request->projection_distance == 0u ||
        !request->projection_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_MINIMAP_CAPTURE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_MINIMAP_CAPTURE_UNAUTHENTICATED;
    access.reader = reader;

#define READ_U16(address, output)                                               \
    do {                                                                        \
        result = read_u16(&access, (address), (output));                         \
        if (result != XG_WORLD_MINIMAP_CAPTURE_OK) return result;                \
    } while (0)
#define READ_U32(address, output)                                               \
    do {                                                                        \
        result = read_u32(&access, (address), (output));                         \
        if (result != XG_WORLD_MINIMAP_CAPTURE_OK) return result;                \
    } while (0)

    READ_U16(MINIMAP_ANGLE, &capture.source.angle);
    READ_U32(MINIMAP_TRIG_TABLE, &trig_zero);
    READ_U32(MINIMAP_TRIG_TABLE +
                 ((uint32_t)capture.source.angle & 0xfffu) * 4u,
             &trig_angle);
    if (trig_zero != UINT32_C(0x10000000))
        return XG_WORLD_MINIMAP_CAPTURE_SOURCE_MISMATCH;
    capture.source.sine = low_s16(trig_angle);
    capture.source.cosine = low_s16(trig_angle >> 16u);

    READ_U32(MINIMAP_WORLD_X, &word);
    capture.source.world_x_20_12 = (int32_t)word;
    READ_U32(MINIMAP_WORLD_Z, &word);
    capture.source.world_z_20_12 = (int32_t)word;
    READ_U32(MINIMAP_TRANSLATION_Z, &word);
    capture.source.translation_z = (int32_t)word;
    READ_U32(MINIMAP_VERTICAL_OFFSET, &word);
    capture.source.vertical_offset = (int32_t)word;
    READ_U32(MINIMAP_BUFFER_INDEX, &capture.source.buffer_index);
    READ_U32(MINIMAP_MARKER_MASK, &capture.source.marker_mask);
    if (capture.source.buffer_index > 1u)
        return XG_WORLD_MINIMAP_CAPTURE_SOURCE_MISMATCH;

    for (triangle = 0u; triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT;
         ++triangle) {
        for (vertex = 0u; vertex < XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT;
             ++vertex) {
            XgHost3dVector *value = &capture.source.triangles[triangle][vertex];
            const uint32_t address = MINIMAP_TRIANGLES +
                (triangle * XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT + vertex) *
                    8u;

            READ_U32(address, &word);
            value->x = low_s16(word);
            value->y = low_s16(word >> 16u);
            READ_U32(address + 4u, &word);
            value->z = low_s16(word);
            value->pad = (uint16_t)(word >> 16u);
            if (memcmp(value, &expected_triangles[triangle][vertex],
                       sizeof(*value)) != 0)
                return XG_WORLD_MINIMAP_CAPTURE_SOURCE_MISMATCH;
        }
    }

    for (marker = 0u; marker < XG_WORLD_MINIMAP_MARKER_CAPACITY; ++marker) {
        if ((capture.source.marker_mask & (UINT32_C(1) << marker)) == 0u)
            continue;
        READ_U16(marker_x_address(marker),
                 &capture.source.markers[marker].raw_x);
        READ_U16(marker_y_address(marker),
                 &capture.source.markers[marker].raw_y);
        ++capture.active_marker_count;
    }

#undef READ_U16
#undef READ_U32

    capture.source.raster = request->raster;
    capture.source.screen_offset_x = request->screen_offset_x;
    capture.source.screen_offset_y = request->screen_offset_y;
    capture.source.projection_distance = request->projection_distance;
    capture.authenticated_read_count = access.read_count;
    capture.authenticated_read_bytes = access.read_bytes;
    capture.authenticated = true;
    capture.sealed = true;
    *out_capture = capture;
    return XG_WORLD_MINIMAP_CAPTURE_OK;
}
