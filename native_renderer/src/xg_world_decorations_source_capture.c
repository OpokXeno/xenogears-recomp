#include "xg_world_decorations_source_capture.h"

#include <limits.h>
#include <string.h>

_Static_assert(sizeof(XgHost3dVector) == 8u,
               "decoration scratch vertices must retain their LLE stride");
_Static_assert(sizeof(XgHost3dMatrix) == 0x20u,
               "decoration scratch matrices must retain their LLE layout");

enum {
    DECORATIONS_CALLER_RETURN = 0x8008639cu,
    DECORATIONS_CAMERA_MATRIX = 0x8009c808u,
    DECORATIONS_BASE_MATRIX = 0x8009a180u,
    DECORATIONS_ANGLE = 0x8009bd3cu,
    DECORATIONS_CAMERA_ORIGIN_X = 0x8009be28u,
    DECORATIONS_CAMERA_ORIGIN_Z = 0x8009be30u,
    DECORATIONS_WRAP_X = 0x8009d160u,
    DECORATIONS_WRAP_Z = 0x8009d2b4u,
    DECORATIONS_DEPTH_CLUT = 0x8009d478u,
    DECORATIONS_FOG_MODE = 0x8009d7ccu,
    DECORATIONS_TRIG_TABLE = 0x800523f0u,
    DECORATIONS_SOURCE_DESCRIPTORS = 0x8009c7ecu,
    DECORATIONS_GRID_OFFSET_X = 0x8009c838u,
    DECORATIONS_GRID_OFFSET_Z = 0x8009c83cu,
    DECORATIONS_GRID_LOOKUP = 0x8009d570u,
    DECORATIONS_VISIBILITY = 0x8009d618u,
    DECORATIONS_BUFFER_INDEX = 0x8009d7f0u,
    DECORATIONS_PACKET_BASES = 0x8009d7e8u,
    DECORATIONS_CONTEXT = 0x8009be3cu,
    DECORATIONS_CONTEXT_OT_OFFSET = 0x70u,
};

typedef struct DecorationsCaptureAccess {
    const XgWorldDecorationsAuthenticatedReader *reader;
    uint32_t position_base;
    uint32_t position_count;
    uint32_t trig_address;
    uint32_t read_count;
    uint32_t read_bytes;
    uint32_t max_read_count;
    uint32_t max_read_bytes;
} DecorationsCaptureAccess;

static bool address_in_aligned_range(uint32_t address, uint32_t start,
                                     uint32_t size) {
    return address >= start && address <= start + size - 4u &&
           ((address - start) & 3u) == 0u;
}

static bool position_read_is_allowed(const DecorationsCaptureAccess *access,
                                     uint32_t address) {
    uint32_t offset;

    if (access->position_count == 0u || address < access->position_base ||
        address > access->position_base + access->position_count * 8u - 4u)
        return false;
    offset = address - access->position_base;
    return (offset & 7u) == 0u || (offset & 7u) == 4u;
}

static bool u16_read_is_allowed(uint32_t address) {
    return address == DECORATIONS_ANGLE ||
           (address >= DECORATIONS_DEPTH_CLUT &&
            address < DECORATIONS_DEPTH_CLUT + 0x20u &&
            ((address - DECORATIONS_DEPTH_CLUT) & 1u) == 0u);
}

static bool u32_read_is_allowed(const DecorationsCaptureAccess *access,
                                uint32_t address) {
    return address_in_aligned_range(address, DECORATIONS_CAMERA_MATRIX,
                                    0x20u) ||
           address_in_aligned_range(address, DECORATIONS_BASE_MATRIX, 0x20u) ||
           address == DECORATIONS_CAMERA_ORIGIN_X ||
           address == DECORATIONS_CAMERA_ORIGIN_Z ||
           address == DECORATIONS_WRAP_X || address == DECORATIONS_WRAP_Z ||
           address == DECORATIONS_FOG_MODE || address == access->trig_address ||
           position_read_is_allowed(access, address);
}

static XgWorldDecorationsCaptureResult
read_u16(DecorationsCaptureAccess *access, uint32_t address,
         uint16_t *out_value) {
    if (access->read_count >= access->max_read_count ||
        access->read_bytes > access->max_read_bytes - 2u ||
        !u16_read_is_allowed(address))
        return XG_WORLD_DECORATIONS_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, address, out_value))
        return XG_WORLD_DECORATIONS_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_DECORATIONS_CAPTURE_OK;
}

static XgWorldDecorationsCaptureResult
read_u32(DecorationsCaptureAccess *access, uint32_t address,
         uint32_t *out_value) {
    if (access->read_count >= access->max_read_count ||
        access->read_bytes > access->max_read_bytes - 4u ||
        !u32_read_is_allowed(access, address))
        return XG_WORLD_DECORATIONS_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_WORLD_DECORATIONS_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_DECORATIONS_CAPTURE_OK;
}

static int16_t low_s16(uint32_t value) { return (int16_t)(uint16_t)value; }

static int32_t wrap_i32(uint32_t value) {
    if (value <= INT32_MAX)
        return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int16_t wrap_i16(uint32_t value) {
    const uint16_t low = (uint16_t)value;

    if (low <= INT16_MAX)
        return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static int32_t shift_right_floor(int32_t value, unsigned bits) {
    uint32_t magnitude;

    if (value >= 0)
        return value >> bits;
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

static int16_t rotated_component(int16_t left_factor, int16_t left_value,
                                 int16_t right_factor, int16_t right_value,
                                 bool subtract) {
    const uint32_t left =
        (uint32_t)(int32_t)left_factor * (uint32_t)(int32_t)left_value;
    const uint32_t right =
        (uint32_t)(int32_t)right_factor * (uint32_t)(int32_t)right_value;
    const int32_t sum = wrap_i32(subtract ? left - right : left + right);

    return wrap_i16((uint32_t)shift_right_floor(sum, 12u));
}

static void rotate_matrix_z(XgHost3dMatrix *matrix, int16_t sine,
                            int16_t cosine) {
    int16_t row0[3];
    int16_t row1[3];
    uint32_t column;

    memcpy(row0, matrix->rotation[0], sizeof(row0));
    memcpy(row1, matrix->rotation[1], sizeof(row1));
    for (column = 0u; column < 3u; ++column) {
        matrix->rotation[0][column] =
            rotated_component(cosine, row0[column], sine, row1[column], true);
        matrix->rotation[1][column] =
            rotated_component(sine, row0[column], cosine, row1[column], false);
    }
}

static bool compute_fog(uint32_t mode, int32_t projection_distance,
                        int16_t *depth_cue_a, int32_t *depth_cue_b) {
    const int32_t near_distance = mode == 2u ? 0x0b00 : 0x0800;
    const int32_t far_distance = 0x0e80;
    const int32_t range = far_distance - near_distance;
    int64_t value;

    if (depth_cue_a == NULL || depth_cue_b == NULL ||
        projection_distance == 0 || range <= 99)
        return false;
    value = ((int64_t)-near_distance * far_distance / range) * 256;
    value /= projection_distance;
    if (value < INT16_MIN)
        value = INT16_MIN;
    if (value > INT16_MAX)
        value = INT16_MAX;
    *depth_cue_a = (int16_t)value;
    value = ((int64_t)far_distance * 4096 / range) * 4096;
    *depth_cue_b = (int32_t)(uint32_t)value;
    return true;
}

static bool raster_is_valid(const XgWorldDecorationsRasterState *raster) {
    return raster->draw_area_left <= raster->draw_area_right &&
           raster->draw_area_top <= raster->draw_area_bottom &&
           raster->draw_area_right <= 1023u &&
           raster->draw_area_bottom <= 1023u &&
           raster->draw_offset_x >= -1024 && raster->draw_offset_x <= 1023 &&
           raster->draw_offset_y >= -1024 && raster->draw_offset_y <= 1023;
}

static void apply_fixed_contract(XgWorldDecorationsSource *source,
                                 const XgWorldDecorationsRasterState *raster) {
    static const XgHost3dVector vertices[4] = {
        {-24, -72, 0, 0u},
        {24, -72, 0, 0u},
        {-24, 0, 0, 0u},
        {24, 0, 0, 0u},
    };
    static const uint8_t uv[4][2] = {
        {0u, 64u},
        {31u, 64u},
        {0u, 111u},
        {31u, 111u},
    };

    memcpy(source->vertices, vertices, sizeof(vertices));
    memcpy(source->uv, uv, sizeof(uv));
    source->material = (XgRenderIrMaterialState){
        .tpage = 0x001eu,
        .texture_page_x = 14u,
        .texture_page_y = 1u,
        .clut_x = 240u,
        .clut_y = 511u,
        .draw_area_left = raster->draw_area_left,
        .draw_area_top = raster->draw_area_top,
        .draw_area_right = raster->draw_area_right,
        .draw_area_bottom = raster->draw_area_bottom,
        .draw_offset_x = raster->draw_offset_x,
        .draw_offset_y = raster->draw_offset_y,
        .texture_depth = XG_RENDER_IR_TEXTURE_4_BIT,
        .shading = XG_RENDER_IR_SHADING_FLAT,
        .textured = true,
        .blend_mode = XG_RENDER_IR_BLEND_AVERAGE,
        .dither = raster->dither,
        .mask_set = raster->mask_set,
        .mask_check = raster->mask_check,
    };
}

static XgWorldDecorationsCaptureResult capture_scratch_source(
    const XgWorldDecorationsCaptureRequest *request,
    DecorationsCaptureAccess *access, XgWorldDecorationsCapture *capture) {
    uint32_t word;
    uint32_t index;
    uint32_t magnitude;
    uint16_t angle;
    int32_t rotation_argument;
    int16_t sine;
    int16_t cosine;
    XgWorldDecorationsCaptureResult result;

    result = read_u16(access, DECORATIONS_ANGLE, &angle);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    capture->angle = (int16_t)angle;
    rotation_argument = -(int32_t)capture->angle;
    magnitude = rotation_argument < 0 ? (uint32_t)(-rotation_argument)
                                      : (uint32_t)rotation_argument;
    access->trig_address =
        DECORATIONS_TRIG_TABLE + (magnitude & 0x0fffu) * 4u;
    capture->trig_address = access->trig_address;
    result = read_u32(access, access->trig_address, &word);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    sine = low_s16(word);
    cosine = low_s16(word >> 16u);
    if (rotation_argument < 0)
        sine = wrap_i16(0u - (uint32_t)(int32_t)sine);

    for (index = 0u; index < 8u; ++index) {
        result = read_u32(access, DECORATIONS_CAMERA_MATRIX + index * 4u,
                          &word);
        if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
            return result;
        parse_matrix_word(&capture->source.camera_matrix, index, word);
        result = read_u32(access, DECORATIONS_BASE_MATRIX + index * 4u,
                          &word);
        if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
            return result;
        parse_matrix_word(&capture->source.decoration_matrix, index, word);
    }
    rotate_matrix_z(&capture->source.decoration_matrix, sine, cosine);
    for (index = 0u; index < XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT; ++index) {
        const uint16_t expected =
            (uint16_t)(((0x1f0u + index) << 6u) | (0xf0u >> 4u));

        result = read_u16(access, DECORATIONS_DEPTH_CLUT + index * 2u,
                          &capture->source.depth_clut[index]);
        if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
            return result;
        if (capture->source.depth_clut[index] != expected)
            return XG_WORLD_DECORATIONS_CAPTURE_SOURCE_MISMATCH;
    }

    apply_fixed_contract(&capture->source, &request->raster);
    capture->source.screen_offset_x = request->screen_offset_x;
    capture->source.screen_offset_y = request->screen_offset_y;
    capture->source.screen_x_cull_margin = request->screen_x_cull_margin;
    capture->source.projection_distance = request->projection_distance;
    return XG_WORLD_DECORATIONS_CAPTURE_OK;
}

static XgWorldDecorationsCaptureResult capture_helper_source(
    const XgWorldDecorationsCaptureRequest *request,
    DecorationsCaptureAccess *access, XgWorldDecorationsCapture *capture) {
    uint32_t word;
    uint32_t fog_mode;
    XgWorldDecorationsCaptureResult result;

    result = read_u32(access, DECORATIONS_CAMERA_ORIGIN_X, &word);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    capture->source.camera_origin_x = shift_right_floor((int32_t)word, 12u);
    result = read_u32(access, DECORATIONS_CAMERA_ORIGIN_Z, &word);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    capture->source.camera_origin_z = shift_right_floor((int32_t)word, 12u);
    result = read_u32(access, DECORATIONS_WRAP_X, &word);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    capture->source.wrap_x = (int32_t)word;
    result = read_u32(access, DECORATIONS_WRAP_Z, &word);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    capture->source.wrap_z = (int32_t)word;
    result = read_u32(access, DECORATIONS_FOG_MODE, &fog_mode);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    if (!compute_fog(fog_mode, request->projection_distance,
                     &capture->source.depth_cue_a,
                     &capture->source.depth_cue_b))
        return XG_WORLD_DECORATIONS_CAPTURE_SOURCE_MISMATCH;
    return XG_WORLD_DECORATIONS_CAPTURE_OK;
}

static XgWorldDecorationsCaptureResult capture_positions(
    DecorationsCaptureAccess *access, uint32_t position_address,
    uint32_t position_count, XgWorldDecorationsSource *source) {
    uint32_t index;
    uint32_t word;
    XgWorldDecorationsCaptureResult result;

    access->position_base = position_address;
    access->position_count = position_count;
    for (index = 0u; index < position_count; ++index) {
        XgWorldDecorationsPosition *position = &source->positions[index];
        const uint32_t address = position_address + index * 8u;

        result = read_u32(access, address, &word);
        if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
            return result;
        position->x = low_s16(word);
        position->y = low_s16(word >> 16u);
        result = read_u32(access, address + 4u, &word);
        if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
            return result;
        position->z = low_s16(word);
        position->pad = (uint16_t)(word >> 16u);
    }
    source->position_count = position_count;
    return XG_WORLD_DECORATIONS_CAPTURE_OK;
}

XgWorldDecorationsCaptureResult xg_world_decorations_source_capture(
    const XgWorldDecorationsCaptureRequest *request,
    const XgWorldDecorationsAuthenticatedReader *reader,
    XgWorldDecorationsCapture *out_capture) {
    XgWorldDecorationsCapture capture = {0};
    DecorationsCaptureAccess access = {0};
    XgWorldDecorationsCaptureResult result;

    if (out_capture == NULL)
        return XG_WORLD_DECORATIONS_CAPTURE_INVALID_ARGUMENT;
    memset(out_capture, 0, sizeof(*out_capture));
    if (request == NULL || reader == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || request->authentication_generation == 0u ||
        request->caller_return != DECORATIONS_CALLER_RETURN ||
        request->position_count == 0u ||
        request->position_count > XG_WORLD_DECORATIONS_POSITION_CAPACITY ||
        (request->position_address & 3u) != 0u ||
        request->position_address > UINT32_MAX - request->position_count * 8u ||
        request->screen_x_cull_margin < 0 ||
        request->screen_x_cull_margin > (INT32_MAX - 320) / 2 ||
        request->projection_distance == 0u ||
        !request->helper_arguments_authenticated ||
        !request->projection_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_DECORATIONS_CAPTURE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_DECORATIONS_CAPTURE_UNAUTHENTICATED;

    access.reader = reader;
    access.position_base = request->position_address;
    access.position_count = request->position_count;
    access.max_read_count = XG_WORLD_DECORATIONS_MAX_AUTHENTICATED_READS;
    access.max_read_bytes = XG_WORLD_DECORATIONS_MAX_AUTHENTICATED_BYTES;
    capture.position_address = request->position_address;
    result = capture_scratch_source(request, &access, &capture);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    result = capture_helper_source(request, &access, &capture);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    result = capture_positions(&access, request->position_address,
                               request->position_count, &capture.source);
    if (result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return result;
    capture.authentication_generation = request->authentication_generation;
    capture.authenticated_read_count = access.read_count;
    capture.authenticated_read_bytes = access.read_bytes;
    if (capture.authenticated_read_count >
            XG_WORLD_DECORATIONS_MAX_AUTHENTICATED_READS ||
        capture.authenticated_read_bytes >
            XG_WORLD_DECORATIONS_MAX_AUTHENTICATED_BYTES)
        return XG_WORLD_DECORATIONS_CAPTURE_FORBIDDEN_RANGE;
    capture.authenticated = true;
    capture.sealed = true;
    *out_capture = capture;
    return XG_WORLD_DECORATIONS_CAPTURE_OK;
}

static bool native_ram_range_is_valid(uint32_t address, uint32_t size,
                                      uint32_t alignment) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint64_t physical = address & UINT32_C(0x1fffffff);

    return size != 0u && alignment != 0u &&
           (address & (alignment - 1u)) == 0u &&
           (segment == 0u || segment == UINT32_C(0x80000000) ||
            segment == UINT32_C(0xa0000000)) &&
           physical + size <= UINT64_C(0x00200000) &&
           (uint64_t)address + size - 1u <= UINT32_MAX;
}

static XgWorldDecorationsNativeResult native_read_u16(
    DecorationsCaptureAccess *access, uint32_t address, uint16_t *out_value) {
    if (access->read_count >= access->max_read_count ||
        access->read_bytes > access->max_read_bytes - 2u)
        return XG_WORLD_DECORATIONS_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, address, out_value))
        return XG_WORLD_DECORATIONS_NATIVE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_DECORATIONS_NATIVE_OK;
}

static XgWorldDecorationsNativeResult native_read_u32(
    DecorationsCaptureAccess *access, uint32_t address, uint32_t *out_value) {
    if (access->read_count >= access->max_read_count ||
        access->read_bytes > access->max_read_bytes - 4u)
        return XG_WORLD_DECORATIONS_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_WORLD_DECORATIONS_NATIVE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_DECORATIONS_NATIVE_OK;
}

static XgWorldDecorationsNativeResult native_capture_result(
    XgWorldDecorationsCaptureResult result) {
    switch (result) {
    case XG_WORLD_DECORATIONS_CAPTURE_OK:
        return XG_WORLD_DECORATIONS_NATIVE_OK;
    case XG_WORLD_DECORATIONS_CAPTURE_UNAUTHENTICATED:
        return XG_WORLD_DECORATIONS_NATIVE_UNAUTHENTICATED;
    case XG_WORLD_DECORATIONS_CAPTURE_READ_FAILED:
        return XG_WORLD_DECORATIONS_NATIVE_READ_FAILED;
    case XG_WORLD_DECORATIONS_CAPTURE_SOURCE_MISMATCH:
        return XG_WORLD_DECORATIONS_NATIVE_SOURCE_MISMATCH;
    case XG_WORLD_DECORATIONS_CAPTURE_FORBIDDEN_RANGE:
        return XG_WORLD_DECORATIONS_NATIVE_FORBIDDEN_RANGE;
    case XG_WORLD_DECORATIONS_CAPTURE_INVALID_ARGUMENT:
    default:
        return XG_WORLD_DECORATIONS_NATIVE_INVALID_ARGUMENT;
    }
}

XgWorldDecorationsNativeResult xg_world_decorations_native_prepare(
    const XgWorldDecorationsNativeRequest *request,
    const XgWorldDecorationsAuthenticatedReader *reader,
    XgWorldDecorationsRecord *records, uint32_t record_capacity,
    XgWorldDecorationsNativePreparation *out_preparation) {
    XgWorldDecorationsNativePreparation preparation = {0};
    XgWorldDecorationsCaptureRequest source_request = {0};
    XgWorldDecorationsCapture capture = {0};
    DecorationsCaptureAccess access = {0};
    uint32_t helper_positions[XG_WORLD_DECORATIONS_NATIVE_GRID_CELL_COUNT];
    uint32_t helper_counts[XG_WORLD_DECORATIONS_NATIVE_GRID_CELL_COUNT];
    uint32_t source_descriptors;
    uint32_t context;
    uint32_t buffer_index;
    uint32_t shared_count;
    uint32_t row;
    uint32_t column;
    uint32_t helper;
    uint16_t grid_offset_x_word;
    uint16_t grid_offset_z_word;
    XgWorldDecorationsCaptureResult capture_result;
    XgWorldDecorationsNativeResult result;

    if (out_preparation == NULL)
        return XG_WORLD_DECORATIONS_NATIVE_INVALID_ARGUMENT;
    memset(out_preparation, 0, sizeof(*out_preparation));
    if (request == NULL || reader == NULL || records == NULL ||
        reader->read_u16 == NULL || reader->read_u32 == NULL ||
        record_capacity < XG_WORLD_DECORATIONS_PACKET_CAPACITY ||
        request->authentication_generation == 0u ||
        request->entry_pc != XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC ||
        request->caller_return != XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN ||
        request->screen_x_cull_margin < 0 ||
        request->screen_x_cull_margin > (INT32_MAX - 320) / 2 ||
        request->projection_distance == 0u ||
        !request->projection_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_DECORATIONS_NATIVE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_DECORATIONS_NATIVE_UNAUTHENTICATED;

    access.reader = reader;
    access.max_read_count =
        XG_WORLD_DECORATIONS_NATIVE_MAX_AUTHENTICATED_READS;
    access.max_read_bytes =
        XG_WORLD_DECORATIONS_NATIVE_MAX_AUTHENTICATED_BYTES;
    source_request = (XgWorldDecorationsCaptureRequest){
        .authentication_generation = request->authentication_generation,
        .caller_return = DECORATIONS_CALLER_RETURN,
        .screen_offset_x = request->screen_offset_x,
        .screen_offset_y = request->screen_offset_y,
        .screen_x_cull_margin = request->screen_x_cull_margin,
        .projection_distance = request->projection_distance,
        .raster = request->raster,
        .helper_arguments_authenticated = true,
        .projection_state_authenticated =
            request->projection_state_authenticated,
    };
    capture_result = capture_scratch_source(&source_request, &access, &capture);
    if (capture_result != XG_WORLD_DECORATIONS_CAPTURE_OK)
        return native_capture_result(capture_result);

    result = native_read_u32(&access, DECORATIONS_SOURCE_DESCRIPTORS,
                             &source_descriptors);
    if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
        return result;
    if (!native_ram_range_is_valid(
            source_descriptors,
            XG_WORLD_DECORATIONS_NATIVE_SOURCE_DESCRIPTOR_COUNT * 8u, 4u))
        return XG_WORLD_DECORATIONS_NATIVE_FORBIDDEN_RANGE;
    result = native_read_u16(&access, DECORATIONS_GRID_OFFSET_X,
                             &grid_offset_x_word);
    if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
        return result;
    result = native_read_u16(&access, DECORATIONS_GRID_OFFSET_Z,
                             &grid_offset_z_word);
    if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
        return result;

    for (row = 0u; row < 5u; ++row) {
        for (column = 0u; column < 5u; ++column) {
            const uint32_t cell = row * 5u + column;
            int32_t lookup_index;
            int16_t visibility;
            int16_t source_index;
            uint16_t half;
            uint32_t descriptor;
            uint32_t count;
            uint32_t positions;

            result = native_read_u16(
                &access, DECORATIONS_VISIBILITY + cell * 2u, &half);
            if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
                return result;
            visibility = low_s16(half);
            if (visibility == -1)
                continue;

            lookup_index = ((int32_t)row + low_s16(grid_offset_z_word)) * 9 +
                           (int32_t)column + low_s16(grid_offset_x_word);
            if (lookup_index < 0 ||
                lookup_index >= XG_WORLD_DECORATIONS_NATIVE_GRID_LOOKUP_COUNT)
                return XG_WORLD_DECORATIONS_NATIVE_SOURCE_MISMATCH;
            result = native_read_u16(
                &access, DECORATIONS_GRID_LOOKUP +
                             (uint32_t)lookup_index * 2u,
                &half);
            if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
                return result;
            source_index = low_s16(half);
            if (source_index < 0 ||
                source_index >=
                    XG_WORLD_DECORATIONS_NATIVE_SOURCE_DESCRIPTOR_COUNT)
                return XG_WORLD_DECORATIONS_NATIVE_SOURCE_MISMATCH;
            descriptor = source_descriptors + (uint32_t)source_index * 8u;
            result = native_read_u32(&access, descriptor + 4u, &count);
            if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
                return result;
            if (count == 0u)
                continue;
            result = native_read_u32(&access, descriptor, &positions);
            if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
                return result;
            helper_positions[preparation.helper_count] = positions;
            helper_counts[preparation.helper_count] = count;
            ++preparation.helper_count;
        }
    }

    if (preparation.helper_count != 0u) {
        capture_result = capture_helper_source(&source_request, &access,
                                               &capture);
        if (capture_result != XG_WORLD_DECORATIONS_CAPTURE_OK)
            return native_capture_result(capture_result);

        result = native_read_u32(&access, DECORATIONS_BUFFER_INDEX,
                                 &buffer_index);
        if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
            return result;
        if (buffer_index >= 2u)
            return XG_WORLD_DECORATIONS_NATIVE_INVALID_OUTPUT;
        result = native_read_u32(
            &access, DECORATIONS_PACKET_BASES + buffer_index * 4u,
            &preparation.packet_base);
        if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
            return result;
        result = native_read_u32(&access, DECORATIONS_CONTEXT, &context);
        if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
            return result;
        if (context > UINT32_MAX - DECORATIONS_CONTEXT_OT_OFFSET ||
            !native_ram_range_is_valid(
                context + DECORATIONS_CONTEXT_OT_OFFSET, 4u, 4u))
            return XG_WORLD_DECORATIONS_NATIVE_INVALID_OUTPUT;
        result = native_read_u32(
            &access, context + DECORATIONS_CONTEXT_OT_OFFSET,
            &preparation.ot_base);
        if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
            return result;
        result = native_read_u32(
            &access, XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS,
            &shared_count);
        if (result != XG_WORLD_DECORATIONS_NATIVE_OK)
            return result;
        if ((shared_count & UINT32_C(0xffff0000)) != 0u ||
            !native_ram_range_is_valid(
                preparation.packet_base,
                XG_WORLD_DECORATIONS_PACKET_CAPACITY *
                    XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE,
                4u) ||
            !native_ram_range_is_valid(
                preparation.ot_base,
                XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT * 4u, 4u))
            return XG_WORLD_DECORATIONS_NATIVE_INVALID_OUTPUT;
        preparation.shared_count_write_mask = UINT32_MAX;

        for (helper = 0u; helper < preparation.helper_count; ++helper) {
            XgWorldDecorationsResult build_result;

            if (preparation.record_count >=
                XG_WORLD_DECORATIONS_PACKET_CAPACITY)
                break;
            if (helper_counts[helper] >
                    XG_WORLD_DECORATIONS_POSITION_CAPACITY ||
                !native_ram_range_is_valid(helper_positions[helper],
                                           helper_counts[helper] * 8u, 4u))
                return XG_WORLD_DECORATIONS_NATIVE_FORBIDDEN_RANGE;
            capture_result = capture_positions(
                &access, helper_positions[helper], helper_counts[helper],
                &capture.source);
            if (capture_result != XG_WORLD_DECORATIONS_CAPTURE_OK)
                return native_capture_result(capture_result);
            build_result = xg_world_decorations_build(
                &capture.source, records, record_capacity,
                &preparation.record_count);
            if (build_result != XG_WORLD_DECORATIONS_OK)
                return XG_WORLD_DECORATIONS_NATIVE_BUILD_FAILED;
        }
    } else {
        preparation.shared_count_write_mask = UINT32_C(0x0000ffff);
    }

    memcpy(preparation.scratch.vertices, capture.source.vertices,
           sizeof(preparation.scratch.vertices));
    preparation.scratch.camera_matrix = capture.source.camera_matrix;
    preparation.scratch.decoration_matrix = capture.source.decoration_matrix;
    memcpy(preparation.scratch.depth_clut, capture.source.depth_clut,
           sizeof(preparation.scratch.depth_clut));
    preparation.authentication_generation = request->authentication_generation;
    preparation.final_shared_count = preparation.record_count;
    preparation.continuation = request->caller_return;
    preparation.authenticated_read_count = access.read_count;
    preparation.authenticated_read_bytes = access.read_bytes;
    if (preparation.authenticated_read_count >
            XG_WORLD_DECORATIONS_NATIVE_MAX_AUTHENTICATED_READS ||
        preparation.authenticated_read_bytes >
            XG_WORLD_DECORATIONS_NATIVE_MAX_AUTHENTICATED_BYTES)
        return XG_WORLD_DECORATIONS_NATIVE_FORBIDDEN_RANGE;
    preparation.authenticated = true;
    preparation.sealed = true;
    *out_preparation = preparation;
    return XG_WORLD_DECORATIONS_NATIVE_OK;
}
