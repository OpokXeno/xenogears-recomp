#include "xg_world_models_native.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    RECORD_BASE_GLOBAL = 0x8009c620u,
    RECORD_COUNT_GLOBAL = 0x8009d7e0u,
    CAMERA_X_GLOBAL = 0x8009be28u,
    CAMERA_Z_GLOBAL = 0x8009be30u,
    CAMERA_MATRIX_GLOBAL = 0x8009c808u,
    BUFFER_INDEX_GLOBAL = 0x8009d7f0u,
    CONTEXT_POINTER_GLOBAL = 0x8009be3cu,
    WRAP_X_GLOBAL = 0x8009d160u,
    WRAP_Z_GLOBAL = 0x8009d2b4u,
    SCREEN_RIGHT_GLOBAL = 0x800500f8u,
    SCREEN_BOTTOM_GLOBAL = 0x800500fcu,
    ORDERING_SHIFT_GLOBAL = 0x80050100u,
    TPAGE_GLOBAL = 0x80059308u,
    CLUT_GLOBAL = 0x8005930cu,
    TPAGE_OVERRIDE_GLOBAL = 0x80059310u,
    CLUT_OVERRIDE_GLOBAL = 0x80059314u,
    DEPTH_CUE_COLOR_GLOBAL = 0x80059598u,
    TPAGE_MODE_GLOBAL = 0x80050108u,
    CLUT_MODE_GLOBAL = 0x8005010cu,
};

static const uint8_t attribute_sizes[XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
    4u, 8u, 4u, 8u, 4u, 8u, 4u, 8u, 4u,
    12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u,
};

static const uint8_t packet_strides[XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
    0x14u, 0x20u, 0x1cu, 0x28u, 0x14u, 0x20u, 0x1cu, 0x28u,
    0x18u, 0x28u, 0x24u, 0x34u, 0x18u, 0x28u, 0x24u, 0x34u,
    0x20u,
};

static uint8_t initial_packet_tag_payload_word_count(uint8_t family) {
    const uint8_t packet_word_count = packet_strides[family] / 4u;

    /* Family 8's initializer leaves the tag one word short; its renderer
     * replaces it with the full stride-derived tag before OT insertion. */
    return (uint8_t)(packet_word_count - (family == 8u ? 2u : 1u));
}

static uint8_t rendered_packet_tag_payload_word_count(uint8_t family) {
    return (uint8_t)(packet_strides[family] / 4u - 1u);
}

static bool packet_tag_payload_word_count_is_valid(uint8_t family,
                                                    uint8_t count) {
    return count == initial_packet_tag_payload_word_count(family) ||
        count == rendered_packet_tag_payload_word_count(family);
}

typedef struct NativeAccess {
    const XgWorldModelsNativeAuthenticatedReader *reader;
    uint32_t read_count;
    uint32_t read_bytes;
} NativeAccess;

static int32_t wrap_i32(uint32_t value) {
    if (value <= INT32_MAX) return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int64_t shift_right_floor_i64(int64_t value, unsigned bits) {
    uint64_t magnitude;

    if (value >= 0) return value / ((int64_t)1 << bits);
    magnitude = (uint64_t)(-(value + 1)) + 1u;
    return -(int64_t)((magnitude + (((uint64_t)1u << bits) - 1u)) >> bits);
}

static bool range_end(uint32_t address, uint32_t size, uint32_t *out_end) {
    if (size == 0u || address > UINT32_MAX - (size - 1u)) return false;
    *out_end = address + size - 1u;
    return true;
}

static bool authorize_range(NativeAccess *access,
                            XgWorldModelsNativeRangeKind kind,
                            uint32_t address, uint32_t size) {
    uint32_t ignored;

    return range_end(address, size, &ignored) &&
        access->reader->authorize_range(
            access->reader->context, kind, address, size);
}

static XgWorldModelsNativeResult read_u8(NativeAccess *access,
                                         uint32_t address,
                                         uint8_t *out_value) {
    if (access->read_count == UINT32_MAX ||
        access->read_bytes == UINT32_MAX)
        return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_u8(
            access->reader->context, address, out_value))
        return XG_WORLD_MODELS_NATIVE_READ_FAILED;
    ++access->read_count;
    ++access->read_bytes;
    return XG_WORLD_MODELS_NATIVE_OK;
}

static XgWorldModelsNativeResult read_u16(NativeAccess *access,
                                          uint32_t address,
                                          uint16_t *out_value) {
    if (access->read_count == UINT32_MAX ||
        access->read_bytes > UINT32_MAX - 2u)
        return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(
            access->reader->context, address, out_value))
        return XG_WORLD_MODELS_NATIVE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_MODELS_NATIVE_OK;
}

static XgWorldModelsNativeResult read_u32(NativeAccess *access,
                                          uint32_t address,
                                          uint32_t *out_value) {
    if (access->read_count == UINT32_MAX ||
        access->read_bytes > UINT32_MAX - 4u)
        return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(
            access->reader->context, address, out_value))
        return XG_WORLD_MODELS_NATIVE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_MODELS_NATIVE_OK;
}

static XgWorldModelsNativeResult read_packet_template(
    NativeAccess *access, uint32_t model_header_address,
    uint32_t packet_base_address, uint32_t packet_address,
    uint32_t attribute_address, uint8_t primitive_family,
    uint32_t *out_words, uint8_t word_count, uint64_t *out_resource_epoch) {
    const uint32_t size = (uint32_t)word_count * 4u;

    if (word_count == 0u || out_words == NULL || out_resource_epoch == NULL ||
        access->read_count == UINT32_MAX ||
        access->read_bytes > UINT32_MAX - size)
        return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_packet_template(
            access->reader->context, model_header_address,
            packet_base_address, packet_address, attribute_address,
            primitive_family, out_words, word_count, out_resource_epoch) ||
        *out_resource_epoch == 0u)
        return XG_WORLD_MODELS_NATIVE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += size;
    return XG_WORLD_MODELS_NATIVE_OK;
}

static int16_t low_s16(uint32_t word) {
    return (int16_t)(uint16_t)word;
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
        matrix->translation[index - 5u] = wrap_i32(word);
    }
}

static XgHost3dVector parse_vector(uint32_t xy, uint32_t zp) {
    XgHost3dVector vector = {
        low_s16(xy), low_s16(xy >> 16u), low_s16(zp),
        (uint16_t)(zp >> 16u),
    };

    return vector;
}

static bool caller_is_valid(uint32_t caller_return) {
    const uint32_t physical = caller_return & UINT32_C(0x1fffffff);

    return physical ==
            (XG_WORLD_MODELS_PRODUCER_CONTINUATION_0 & UINT32_C(0x1fffffff)) ||
        physical ==
            (XG_WORLD_MODELS_PRODUCER_CONTINUATION_1 & UINT32_C(0x1fffffff)) ||
        physical ==
            (XG_WORLD_MODELS_PRODUCER_CONTINUATION_2 & UINT32_C(0x1fffffff)) ||
        physical ==
            (XG_WORLD_MODELS_PRODUCER_CONTINUATION_3 & UINT32_C(0x1fffffff));
}

static bool raster_is_valid(const XgWorldModelsNativeRasterState *raster) {
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

static XgWorldModelsNativeResult build_result(XgWorldModelsResult result) {
    switch (result) {
    case XG_WORLD_MODELS_OK:
        return XG_WORLD_MODELS_NATIVE_OK;
    case XG_WORLD_MODELS_CAPACITY_EXCEEDED:
        return XG_WORLD_MODELS_NATIVE_CAPACITY_EXCEEDED;
    case XG_WORLD_MODELS_BUILD_FAILED:
        return XG_WORLD_MODELS_NATIVE_BUILD_FAILED;
    case XG_WORLD_MODELS_INVALID_SOURCE:
        return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
    case XG_WORLD_MODELS_INVALID_ARGUMENT:
    default:
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    }
}

static XgWorldModelsNativeResult capture_matrix(
    NativeAccess *access, uint32_t address, XgHost3dMatrix *matrix) {
    uint32_t word;
    uint32_t index;
    XgWorldModelsNativeResult result;

    for (index = 0u; index < 8u; ++index) {
        result = read_u32(access, address + index * 4u, &word);
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
        parse_matrix_word(matrix, index, word);
    }
    return XG_WORLD_MODELS_NATIVE_OK;
}

static XgWorldModelsNativeResult capture_vector(
    NativeAccess *access, XgWorldModelsNativeRangeKind kind,
    uint32_t address, XgHost3dVector *vector) {
    uint32_t xy;
    uint32_t zp;
    XgWorldModelsNativeResult result;

    if (!authorize_range(access, kind, address, 8u))
        return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
    result = read_u32(access, address, &xy);
    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
    result = read_u32(access, address + 4u, &zp);
    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
    *vector = parse_vector(xy, zp);
    return XG_WORLD_MODELS_NATIVE_OK;
}

static XgWorldModelsNativeResult read_model_header(
    NativeAccess *access, uint32_t address,
    XgWorldModelsNativeModelSource *model) {
    uint32_t word;
    uint32_t xy;
    uint32_t zp;
    uint16_t half;
    XgWorldModelsNativeResult result;

    if ((address & 3u) != 0u ||
        !authorize_range(access, XG_WORLD_MODELS_NATIVE_RANGE_MODEL_HEADER,
                         address, 0x38u))
        return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
    memset(model, 0, sizeof(*model));
    model->model_header_address = address;
#define READ_MODEL_U32(offset, output)                                         \
    do {                                                                        \
        result = read_u32(access, address + (offset), (output));                \
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;                 \
    } while (0)
    result = read_u16(access, address + 4u, &half);
    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
    model->vertex_count = half;
    result = read_u16(access, address + 6u, &half);
    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
    model->group_count = half;
    READ_MODEL_U32(0x08u, &model->vertex_base);
    READ_MODEL_U32(0x0cu, &model->auxiliary_vertex_base);
    READ_MODEL_U32(0x10u, &model->topology_base);
    READ_MODEL_U32(0x14u, &model->material_base);
    READ_MODEL_U32(0x18u, &model->model_18);
    READ_MODEL_U32(0x20u, &xy);
    READ_MODEL_U32(0x24u, &zp);
    model->bounds_min = parse_vector(xy, zp);
    READ_MODEL_U32(0x28u, &xy);
    READ_MODEL_U32(0x2cu, &zp);
    model->bounds_max = parse_vector(xy, zp);
    READ_MODEL_U32(0x34u, &word);
    model->packet_capacity_bytes = word;
#undef READ_MODEL_U32
    if ((model->vertex_base & 3u) != 0u ||
        (model->auxiliary_vertex_base & 3u) != 0u ||
        (model->topology_base & 1u) != 0u ||
        (model->material_base & 3u) != 0u ||
        (model->packet_capacity_bytes & 3u) != 0u ||
        (model->group_count != 0u && model->packet_capacity_bytes == 0u))
        return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
    return XG_WORLD_MODELS_NATIVE_OK;
}

static XgWorldModelsNativeResult consume_material_controls(
    NativeAccess *access, uint32_t *cursor, uint16_t *tpage, uint16_t *clut,
    uint32_t tpage_mode, uint16_t tpage_override,
    uint32_t clut_mode, uint16_t clut_override) {
    uint32_t control;

    for (control = 0u;
         control < XG_WORLD_MODELS_MAX_CONTROL_COMMANDS_PER_PRIMITIVE;
         ++control) {
        uint8_t command;
        uint16_t value;
        XgWorldModelsNativeResult result;

        if (!authorize_range(access, XG_WORLD_MODELS_NATIVE_RANGE_ATTRIBUTE,
                             *cursor, 4u))
            return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
        result = read_u8(access, *cursor + 3u, &command);
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
        if (command != 0xc4u && command != 0xc8u)
            return XG_WORLD_MODELS_NATIVE_OK;
        result = read_u16(access, *cursor, &value);
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
        if (command == 0xc4u) {
            if (tpage_mode == 1u)
                *tpage = (uint16_t)((value & 0xffe0u) | tpage_override);
            else if (tpage_mode == 2u)
                *tpage = tpage_override;
            else
                *tpage = value;
        } else {
            *clut = clut_mode == 0u
                ? (uint16_t)((value & 0x0fu) | clut_override) : value;
        }
        if (*cursor > UINT32_MAX - 4u)
            return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
        *cursor += 4u;
    }
    return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
}

static int16_t midpoint_i16(int16_t minimum, int16_t maximum) {
    return (int16_t)((int32_t)minimum +
                     ((int32_t)maximum - minimum) / 2);
}

static bool bounds_point_is_visible(const XgHost3dProjection *projection,
                                     const XgHost3dVector *point,
                                     uint32_t screen_right,
                                     uint32_t packed_screen_bottom,
                                     int32_t margin) {
    XgHost3dProjectedVertex projected;
    uint32_t flags;
    uint32_t packed;

    if (!xg_host_3d_rtps(projection, point, &projected, &flags)) return false;
    (void)flags;
    packed = (uint16_t)projected.x |
        ((uint32_t)(uint16_t)projected.y << 16u);
    return (((uint32_t)projected.z + 1u) & UINT32_C(0xffff)) >= 2u &&
        packed < packed_screen_bottom &&
        (margin > 0
             ? (int32_t)projected.x >= -margin &&
                   (int32_t)projected.x < (int32_t)screen_right + margin
             : (uint16_t)projected.x < screen_right);
}

static bool model_bounds_are_visible(
    const XgHost3dProjection *projection,
    const XgWorldModelsNativeModelSource *model, uint32_t screen_right,
    uint32_t packed_screen_bottom, int32_t margin) {
    const int16_t min_x = model->bounds_min.x;
    const int16_t min_y = model->bounds_min.y;
    const int16_t min_z = model->bounds_min.z;
    const int16_t max_x = model->bounds_max.x;
    const int16_t max_y = model->bounds_max.y;
    const int16_t max_z = model->bounds_max.z;
    const int16_t min_mid_x = midpoint_i16(min_x, max_x);
    const int16_t min_mid_y = midpoint_i16(min_y, max_y);
    const int16_t min_mid_z = midpoint_i16(min_z, max_z);
    const int16_t max_mid_x = midpoint_i16(max_x, min_x);
    const int16_t max_mid_y = midpoint_i16(max_y, min_y);
    const int16_t max_mid_z = midpoint_i16(max_z, min_z);
    const XgHost3dVector points[] = {
        {min_x, min_y, min_z, 0u},
        {max_x, max_y, max_z, 0u},
        {min_mid_x, min_mid_y, min_mid_z, 0u},
        {max_x, min_y, min_z, 0u},
        {min_x, max_y, min_z, 0u},
        {max_x, max_y, min_z, 0u},
        {min_x, min_y, max_z, 0u},
        {max_x, min_y, max_z, 0u},
        {min_x, max_y, max_z, 0u},
        {min_mid_x, min_y, min_z, 0u},
        {min_x, min_y, min_mid_z, 0u},
        {min_x, min_mid_y, min_z, 0u},
        {max_x, max_mid_y, min_z, 0u},
        {max_mid_x, max_y, min_z, 0u},
        {max_x, max_y, min_mid_z, 0u},
        {min_x, max_y, max_mid_z, 0u},
        {min_x, max_mid_y, max_z, 0u},
        {min_mid_x, max_y, max_z, 0u},
        {max_x, min_y, max_mid_z, 0u},
        {max_mid_x, min_y, max_z, 0u},
        {max_x, min_mid_y, max_z, 0u},
    };
    uint32_t index;

    for (index = 0u; index < sizeof(points) / sizeof(points[0]); ++index) {
        if (bounds_point_is_visible(projection, &points[index], screen_right,
                                    packed_screen_bottom, margin))
            return true;
    }
    return false;
}

static XgWorldModelsNativeResult capture_dispatch_primitives(
    NativeAccess *access, const XgWorldModelsRecordOutput *world_record,
    uint32_t source_index, uint32_t dispatch_index,
    uint32_t screen_right, uint32_t guest_xclip_bound,
    uint32_t packed_screen_bottom,
    uint16_t initial_tpage, uint16_t initial_clut,
    uint32_t tpage_mode, uint16_t tpage_override,
    uint32_t clut_mode, uint16_t clut_override,
    XgWorldModelsNativeDispatch *dispatch,
    XgWorldModelsNativePrimitiveSource *primitives,
    uint32_t primitive_capacity, uint32_t *primitive_count) {
    uint32_t topology_cursor;
    uint32_t material_cursor;
    uint32_t packet_cursor;
    uint32_t group;
    uint16_t tpage = initial_tpage;
    uint16_t clut = initial_clut;
    XgWorldModelsNativeResult result;

    result = read_model_header(access,
                               world_record->resident_call.model_header_address,
                               &dispatch->model);
    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
    dispatch->call = world_record->resident_call;
    dispatch->source_index = source_index;
    dispatch->primitive_start = *primitive_count;
    dispatch->bounds_accepted = model_bounds_are_visible(
        &world_record->projection, &dispatch->model, screen_right,
        packed_screen_bottom, xg_host_3d_native_view_margin());
    dispatch->guest_bounds_accepted = model_bounds_are_visible(
        &world_record->projection, &dispatch->model, guest_xclip_bound,
        packed_screen_bottom, 0);
    if (!dispatch->bounds_accepted && !dispatch->guest_bounds_accepted)
        return XG_WORLD_MODELS_NATIVE_OK;
    topology_cursor = dispatch->model.topology_base;
    material_cursor = dispatch->model.material_base;
    packet_cursor = dispatch->call.packet_base_address;
    if ((packet_cursor & 3u) != 0u ||
        (dispatch->model.packet_capacity_bytes != 0u &&
         !authorize_range(access, XG_WORLD_MODELS_NATIVE_RANGE_PACKET_OUTPUT,
                          packet_cursor,
                          dispatch->model.packet_capacity_bytes)))
        return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;

    for (group = 0u; group < dispatch->model.group_count; ++group) {
        uint8_t family;
        uint16_t group_primitive_count;
        uint32_t descriptors;
        uint32_t topology_size;
        uint32_t primitive;

        if (!authorize_range(access, XG_WORLD_MODELS_NATIVE_RANGE_TOPOLOGY,
                             topology_cursor, 4u))
            return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
        result = read_u8(access, topology_cursor, &family);
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
        result = read_u16(access, topology_cursor + 2u,
                          &group_primitive_count);
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
        if (family >= XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT ||
            group_primitive_count == 0u ||
            group_primitive_count > (UINT32_MAX - 4u) / 8u)
            return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
        topology_size = 4u + (uint32_t)group_primitive_count * 8u;
        if (!authorize_range(access, XG_WORLD_MODELS_NATIVE_RANGE_TOPOLOGY,
                             topology_cursor, topology_size))
            return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
        descriptors = topology_cursor + 4u;
        dispatch->primitive_family_mask |= UINT32_C(1) << family;

        for (primitive = 0u; primitive < group_primitive_count; ++primitive) {
            XgWorldModelsNativePrimitiveSource *source;
            const uint32_t packet_size = packet_strides[family];
            const uint32_t attribute_size = attribute_sizes[family];
            const uint32_t descriptor = descriptors + primitive * 8u;
            uint32_t index;

            result = consume_material_controls(
                access, &material_cursor, &tpage, &clut, tpage_mode,
                tpage_override, clut_mode, clut_override);
            if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
            if (primitives == NULL || *primitive_count >= primitive_capacity)
                return XG_WORLD_MODELS_NATIVE_CAPACITY_EXCEEDED;
            if (!authorize_range(access,
                                 XG_WORLD_MODELS_NATIVE_RANGE_ATTRIBUTE,
                                 material_cursor, attribute_size))
                return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
            if (packet_cursor < dispatch->call.packet_base_address ||
                packet_cursor - dispatch->call.packet_base_address >
                    dispatch->model.packet_capacity_bytes ||
                packet_size > dispatch->model.packet_capacity_bytes -
                    (packet_cursor - dispatch->call.packet_base_address))
                return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;

            source = &primitives[*primitive_count];
            memset(source, 0, sizeof(*source));
            source->projection = world_record->projection;
            source->model_header_address =
                dispatch->model.model_header_address;
            source->packet_base_address =
                dispatch->call.packet_base_address;
            source->packet_address = packet_cursor;
            source->attribute_address = material_cursor;
            source->source_index = source_index;
            source->dispatch_index = dispatch_index;
            source->primitive_index = dispatch->primitive_count;
            source->tpage = tpage;
            source->clut = clut;
            source->primitive_family = family;
            source->dispatch_mode = dispatch->call.dispatch_mode;
            source->vertex_count = family < 8u ? 3u :
                (family < 16u ? 4u : 3u);
            source->attribute_size = (uint8_t)attribute_size;
            source->packet_word_count = (uint8_t)(packet_size / 4u);

            for (index = 0u;
                 index < XG_WORLD_MODELS_PRIMITIVE_TOPOLOGY_WORD_COUNT;
                 ++index) {
                result = read_u16(access, descriptor + index * 2u,
                                  &source->topology[index]);
                if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
            }
            for (index = 0u; index < (attribute_size + 3u) / 4u; ++index) {
                result = read_u32(access, material_cursor + index * 4u,
                                  &source->attribute_words[index]);
                if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
            }
            for (index = 0u; index < source->vertex_count; ++index) {
                uint32_t vertex_address;

                if (source->topology[index] >
                    (UINT32_MAX - dispatch->model.vertex_base) / 8u)
                    return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
                vertex_address = dispatch->model.vertex_base +
                    (uint32_t)source->topology[index] * 8u;
                result = capture_vector(
                    access, XG_WORLD_MODELS_NATIVE_RANGE_VERTEX,
                    vertex_address, &source->vertices[index]);
                if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
                if (family == 16u) {
                    uint32_t auxiliary_address;

                    if (source->topology[index] >
                        (UINT32_MAX - dispatch->model.auxiliary_vertex_base) /
                            8u)
                        return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
                    auxiliary_address = dispatch->model.auxiliary_vertex_base +
                        (uint32_t)source->topology[index] * 8u;
                    result = capture_vector(
                        access,
                        XG_WORLD_MODELS_NATIVE_RANGE_AUXILIARY_VERTEX,
                        auxiliary_address, &source->auxiliary_vertices[index]);
                    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
                }
            }
            result = read_packet_template(
                access, source->model_header_address,
                source->packet_base_address, packet_cursor, material_cursor,
                family, source->template_words, source->packet_word_count,
                &source->resource_epoch);
            if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
            if (!packet_tag_payload_word_count_is_valid(
                    family, (uint8_t)(source->template_words[0] >> 24u)))
                return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;

            ++dispatch->primitive_count;
            ++*primitive_count;
            if (material_cursor > UINT32_MAX - attribute_size ||
                packet_cursor > UINT32_MAX - packet_size)
                return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
            material_cursor += attribute_size;
            packet_cursor += packet_size;
        }
        if (descriptors > UINT32_MAX - (uint32_t)group_primitive_count * 8u)
            return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
        topology_cursor = descriptors + (uint32_t)group_primitive_count * 8u;
    }
    dispatch->packet_cursor_after = packet_cursor;
    dispatch->group_cursor_after = topology_cursor;
    return XG_WORLD_MODELS_NATIVE_OK;
}

static XgWorldModelsNativeResult capture_anchor_sources(
    NativeAccess *access, const XgWorldModelsRecordSource *record_sources,
    const XgWorldModelsRecordOutput *records, uint32_t record_count,
    XgWorldModelsNativeAnchorSource *anchor_sources,
    uint32_t anchor_capacity, uint32_t *anchor_count) {
    uint32_t count = 0u;

    for (uint32_t source_index = 0u; source_index < record_count;
         ++source_index) {
        XgWorldModelsNativeModelSource model;
        XgWorldModelsNativeResult result;

        if (records[source_index].disposition == XG_WORLD_MODELS_INACTIVE)
            continue;
        result = read_model_header(
            access, record_sources[source_index].model_header_address, &model);
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
        if (model.vertex_count > anchor_capacity - count ||
            (model.vertex_count != 0u && anchor_sources == NULL))
            return XG_WORLD_MODELS_NATIVE_CAPACITY_EXCEEDED;
        for (uint32_t vertex_index = 0u;
             vertex_index < model.vertex_count; ++vertex_index) {
            XgWorldModelsNativeAnchorSource *source = &anchor_sources[count++];
            uint32_t vertex_address;

            if (vertex_index > (UINT32_MAX - model.vertex_base) / 8u)
                return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
            vertex_address = model.vertex_base + vertex_index * 8u;
            result = capture_vector(
                access, XG_WORLD_MODELS_NATIVE_RANGE_VERTEX,
                vertex_address, &source->vertex);
            if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
            source->projection = records[source_index].projection;
            source->model_header_address = model.model_header_address;
            source->source_index = source_index;
            source->vertex_index = (uint16_t)vertex_index;
        }
    }
    *anchor_count = count;
    return XG_WORLD_MODELS_NATIVE_OK;
}

XgWorldModelsNativeResult xg_world_models_native_prepare(
    const XgWorldModelsNativeRequest *request,
    const XgWorldModelsNativeAuthenticatedReader *reader,
    const XgWorldModelsNativeWorkspace *workspace,
    XgWorldModelsRecordOutput *records, uint32_t record_capacity,
    XgWorldModelsNodeSideEffect *node_side_effects,
    uint32_t node_side_effect_capacity,
    XgWorldModelsNativeDispatch *dispatches, uint32_t dispatch_capacity,
    XgWorldModelsNativePrimitiveSource *primitives,
    uint32_t primitive_capacity,
    XgWorldModelsNativeAnchorSource *anchor_sources,
    uint32_t anchor_capacity,
    XgWorldModelsNativePreparation *out_preparation) {
    XgWorldModelsNativePreparation preparation = {0};
    XgWorldModelsSource source = {0};
    NativeAccess access = {0};
    uint32_t record_base = 0u;
    uint32_t node_count = 0u;
    uint32_t word;
    uint32_t context;
    uint32_t ot_base = 0u;
    uint32_t index;
    uint16_t half;
    uint16_t initial_tpage = 0u;
    uint16_t initial_clut = 0u;
    uint16_t tpage_override = 0u;
    uint16_t clut_override = 0u;
    uint32_t tpage_mode = 0u;
    uint32_t clut_mode = 0u;
    XgWorldModelsResult world_result;
    XgWorldModelsNativeResult result;

    static const uint64_t digest_offset = UINT64_C(1469598103934665603);

    if (out_preparation == NULL)
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    memset(out_preparation, 0, sizeof(*out_preparation));
    if (request == NULL || reader == NULL || workspace == NULL ||
        reader->read_u8 == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || reader->read_packet_template == NULL ||
        reader->authorize_range == NULL ||
        request->authentication_generation == 0u ||
        request->entry_pc != XG_WORLD_MODELS_PRODUCER_ENTRY ||
        !caller_is_valid(request->caller_return) ||
        request->gte.projection_distance == 0u ||
        !request->projection_state_authenticated ||
        !request->lighting_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_MODELS_NATIVE_UNAUTHENTICATED;
    access.reader = reader;

#define READ_U16(address, output)                                               \
    do {                                                                        \
        result = read_u16(&access, (address), (output));                        \
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;                 \
    } while (0)
#define READ_U32(address, output)                                               \
    do {                                                                        \
        result = read_u32(&access, (address), (output));                        \
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;                 \
    } while (0)

    READ_U16(RECORD_COUNT_GLOBAL, &half);
    source.record_count = (int16_t)half;
    READ_U32(CAMERA_X_GLOBAL, &word);
    source.camera_x_12_12 = wrap_i32(word);
    READ_U32(CAMERA_Z_GLOBAL, &word);
    source.camera_z_12_12 = wrap_i32(word);
    source.gte = request->gte;

    if (source.record_count > 0) {
        const uint32_t count = (uint32_t)source.record_count;
        uint32_t records_size;

        if (workspace->record_sources == NULL || records == NULL ||
            count > workspace->record_capacity || count > record_capacity)
            return XG_WORLD_MODELS_NATIVE_CAPACITY_EXCEEDED;
        if (count > UINT32_MAX / XG_WORLD_MODELS_RECORD_STRIDE)
            return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
        records_size = count * XG_WORLD_MODELS_RECORD_STRIDE;
        READ_U32(RECORD_BASE_GLOBAL, &record_base);
        if ((record_base & 3u) != 0u ||
            !authorize_range(&access, XG_WORLD_MODELS_NATIVE_RANGE_RECORDS,
                             record_base, records_size))
            return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
        READ_U32(BUFFER_INDEX_GLOBAL, &word);
        if (word > 1u) return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
        source.buffer_index = (uint16_t)word;
        READ_U32(WRAP_X_GLOBAL, &word);
        source.wrap_x = wrap_i32(word);
        READ_U32(WRAP_Z_GLOBAL, &word);
        source.wrap_z = wrap_i32(word);
        result = capture_matrix(&access, CAMERA_MATRIX_GLOBAL,
                                &source.camera_matrix);
        if (result != XG_WORLD_MODELS_NATIVE_OK) return result;

        memset(workspace->record_sources, 0,
               sizeof(*workspace->record_sources) * count);
        for (index = 0u; index < count; ++index) {
            XgWorldModelsRecordSource *record =
                &workspace->record_sources[index];
            const uint32_t address =
                record_base + index * XG_WORLD_MODELS_RECORD_STRIDE;
            uint32_t node_address;
            uint32_t chain_start;

            READ_U16(address + XG_WORLD_MODELS_RECORD_STATE_OFFSET, &half);
            record->state = (int16_t)half;
            if (record->state != 0) continue;
            READ_U16(address + XG_WORLD_MODELS_RECORD_DISPATCH_SELECTOR_OFFSET,
                     &half);
            record->dispatch_selector = (int16_t)half;
            READ_U32(address + XG_WORLD_MODELS_RECORD_POSITION_X_OFFSET, &word);
            record->position_x = wrap_i32(word);
            READ_U32(address + XG_WORLD_MODELS_RECORD_POSITION_Y_OFFSET, &word);
            record->position_y = wrap_i32(word);
            READ_U32(address + XG_WORLD_MODELS_RECORD_STORED_Z_OFFSET, &word);
            record->stored_z = wrap_i32(word);
            result = capture_matrix(
                &access, address + XG_WORLD_MODELS_RECORD_MATRIX_OFFSET,
                &record->matrix);
            if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
            READ_U32(address + XG_WORLD_MODELS_RECORD_MODEL_HEADER_OFFSET,
                     &record->model_header_address);
            READ_U32(address + XG_WORLD_MODELS_RECORD_PACKET_BASE_0_OFFSET,
                     &record->packet_base[0]);
            READ_U32(address + XG_WORLD_MODELS_RECORD_PACKET_BASE_1_OFFSET,
                     &record->packet_base[1]);
            READ_U32(address + XG_WORLD_MODELS_RECORD_TRANSFORM_CHAIN_OFFSET,
                     &node_address);
            chain_start = node_count;
            while (node_address != 0u) {
                XgWorldModelsTransformNodeSource *node;
                uint32_t prior;
                uint32_t next;

                if (node_count >= workspace->transform_node_capacity ||
                    workspace->transform_nodes == NULL)
                    return XG_WORLD_MODELS_NATIVE_CAPACITY_EXCEEDED;
                if ((node_address & 3u) != 0u ||
                    !authorize_range(
                        &access,
                        XG_WORLD_MODELS_NATIVE_RANGE_TRANSFORM_NODE,
                        node_address, XG_WORLD_MODELS_RECORD_STRIDE))
                    return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
                for (prior = chain_start; prior < node_count; ++prior) {
                    if (workspace->transform_nodes[prior].guest_address ==
                        node_address)
                        return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
                }
                node = &workspace->transform_nodes[node_count++];
                memset(node, 0, sizeof(*node));
                node->guest_address = node_address;
                READ_U32(node_address + XG_WORLD_MODELS_NODE_POSITION_X_OFFSET,
                         &word);
                node->position_x = wrap_i32(word);
                READ_U32(node_address + XG_WORLD_MODELS_NODE_POSITION_Y_OFFSET,
                         &word);
                node->position_y = wrap_i32(word);
                READ_U32(node_address + XG_WORLD_MODELS_NODE_STORED_Z_OFFSET,
                         &word);
                node->stored_z = wrap_i32(word);
                result = capture_matrix(
                    &access, node_address + XG_WORLD_MODELS_NODE_MATRIX_OFFSET,
                    &node->matrix);
                if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
                READ_U32(node_address + XG_WORLD_MODELS_NODE_NEXT_OFFSET,
                         &next);
                node_address = next;
            }
            record->transform_nodes =
                node_count == chain_start ? NULL :
                    &workspace->transform_nodes[chain_start];
            record->transform_node_count = node_count - chain_start;
        }
        source.records = workspace->record_sources;
    }

    source.ordering_table_address = 0u;
    world_result = xg_world_models_build(
        &source, records, record_capacity, node_side_effects,
        node_side_effect_capacity, &preparation.world);
    result = build_result(world_result);
    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
    preparation.record_count = preparation.world.record_count;
    preparation.transform_node_count = preparation.world.node_side_effect_count;
    result = capture_anchor_sources(
        &access, workspace->record_sources, records, preparation.record_count,
        anchor_sources, anchor_capacity, &preparation.anchor_count);
    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;

    if (preparation.world.resident_dispatch_count != 0u) {
        if (dispatches == NULL ||
            preparation.world.resident_dispatch_count > dispatch_capacity)
            return XG_WORLD_MODELS_NATIVE_CAPACITY_EXCEEDED;
        READ_U32(CONTEXT_POINTER_GLOBAL, &context);
        if (context > UINT32_MAX - XG_WORLD_MODELS_CONTEXT_OT_OFFSET)
            return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
        READ_U32(context + XG_WORLD_MODELS_CONTEXT_OT_OFFSET, &ot_base);
        if ((ot_base & 3u) != 0u ||
            !authorize_range(
                &access,
                XG_WORLD_MODELS_NATIVE_RANGE_ORDERING_TABLE_OUTPUT, ot_base,
                XG_WORLD_MODELS_OT_BUCKET_COUNT * 4u))
            return XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE;
        READ_U32(ORDERING_SHIFT_GLOBAL, &preparation.ordering_shift);
        if (preparation.ordering_shift > 31u)
            return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
        READ_U32(SCREEN_RIGHT_GLOBAL, &preparation.screen_right);
        if (request->guest_xclip_bound < preparation.screen_right)
            return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
        preparation.guest_xclip_bound = request->guest_xclip_bound;
        READ_U32(SCREEN_BOTTOM_GLOBAL, &preparation.packed_screen_bottom);
        READ_U32(DEPTH_CUE_COLOR_GLOBAL,
                 &preparation.depth_cue_color_word);
        READ_U16(TPAGE_GLOBAL, &initial_tpage);
        READ_U16(CLUT_GLOBAL, &initial_clut);
        READ_U16(TPAGE_OVERRIDE_GLOBAL, &tpage_override);
        READ_U16(CLUT_OVERRIDE_GLOBAL, &clut_override);
        READ_U32(TPAGE_MODE_GLOBAL, &tpage_mode);
        READ_U32(CLUT_MODE_GLOBAL, &clut_mode);
        if (tpage_mode > 2u || clut_mode > 1u)
            return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;

        for (index = 0u; index < preparation.record_count; ++index) {
            XgWorldModelsNativeDispatch *dispatch;

            if (records[index].disposition !=
                XG_WORLD_MODELS_RESIDENT_DISPATCH)
                continue;
            dispatch = &dispatches[preparation.dispatch_count];
            memset(dispatch, 0, sizeof(*dispatch));
            records[index].resident_call.ordering_table_address = ot_base;
            result = capture_dispatch_primitives(
                &access, &records[index], index, preparation.dispatch_count,
                preparation.screen_right, preparation.guest_xclip_bound,
                preparation.packed_screen_bottom,
                initial_tpage, initial_clut, tpage_mode, tpage_override,
                clut_mode, clut_override, dispatch, primitives,
                primitive_capacity, &preparation.primitive_count);
            if (result != XG_WORLD_MODELS_NATIVE_OK) return result;
            preparation.primitive_family_mask |=
                dispatch->primitive_family_mask;
            preparation.dispatch_mode_mask |=
                UINT32_C(1) << dispatch->call.dispatch_mode;
            ++preparation.dispatch_count;
        }
    }

#undef READ_U16
#undef READ_U32
    if (preparation.dispatch_count !=
        preparation.world.resident_dispatch_count)
        return XG_WORLD_MODELS_NATIVE_BUILD_FAILED;
    preparation.gte = request->gte;
    preparation.raster = request->raster;
    preparation.sealed_dispatches = dispatches;
    preparation.sealed_primitives = primitives;
    preparation.sealed_anchor_sources = anchor_sources;
    preparation.dispatch_digest = digest_offset;
    preparation.primitive_digest = digest_offset;
    preparation.anchor_digest = digest_offset;
    for (index = 0u;
         index < preparation.dispatch_count * sizeof(dispatches[0]); ++index) {
        preparation.dispatch_digest ^=
            ((const uint8_t *)dispatches)[index];
        preparation.dispatch_digest *= UINT64_C(1099511628211);
    }
    for (index = 0u;
         index < preparation.primitive_count * sizeof(primitives[0]); ++index) {
        preparation.primitive_digest ^=
            ((const uint8_t *)primitives)[index];
        preparation.primitive_digest *= UINT64_C(1099511628211);
    }
    for (index = 0u;
         index < preparation.anchor_count * sizeof(anchor_sources[0]); ++index) {
        preparation.anchor_digest ^=
            ((const uint8_t *)anchor_sources)[index];
        preparation.anchor_digest *= UINT64_C(1099511628211);
    }
    preparation.authentication_generation = request->authentication_generation;
    preparation.continuation_pc = request->caller_return;
    preparation.authenticated_read_count = access.read_count;
    preparation.authenticated_read_bytes = access.read_bytes;
    preparation.authenticated = true;
    preparation.sealed = true;
    *out_preparation = preparation;
    return XG_WORLD_MODELS_NATIVE_OK;
}

XgWorldModelsNativeResult xg_world_models_native_build_anchor_vertex(
    XgWorldModelsNativePreparation *preparation,
    uint64_t authentication_generation,
    const XgWorldModelsNativeAnchorSource *source,
    XgRenderIrVertex *out_vertex) {
    XgHost3dProjectedVertex projected;
    uint32_t flags;

    if (out_vertex == NULL)
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    memset(out_vertex, 0, sizeof(*out_vertex));
    if (preparation == NULL || source == NULL ||
        authentication_generation == 0u || !preparation->authenticated ||
        !preparation->sealed || preparation->consumed ||
        preparation->authentication_generation != authentication_generation ||
        preparation->sealed_anchor_sources == NULL ||
        (uintptr_t)source < (uintptr_t)preparation->sealed_anchor_sources ||
        (uintptr_t)source >=
            (uintptr_t)preparation->sealed_anchor_sources +
                preparation->anchor_count *
                    sizeof(preparation->sealed_anchor_sources[0]) ||
        ((uintptr_t)source -
         (uintptr_t)preparation->sealed_anchor_sources) %
                sizeof(preparation->sealed_anchor_sources[0]) != 0u ||
        source->source_index >= preparation->record_count)
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    if (!preparation->anchor_digest_validated) {
        uint64_t digest = UINT64_C(1469598103934665603);

        for (uint32_t byte = 0u;
             byte < preparation->anchor_count *
                 sizeof(preparation->sealed_anchor_sources[0]); ++byte) {
            digest ^= ((const uint8_t *)preparation->sealed_anchor_sources)[byte];
            digest *= UINT64_C(1099511628211);
        }
        if (digest != preparation->anchor_digest)
            return XG_WORLD_MODELS_NATIVE_UNAUTHENTICATED;
        preparation->anchor_digest_validated = true;
    }
    if (!xg_host_3d_rtps(
            &source->projection, &source->vertex, &projected, &flags))
        return XG_WORLD_MODELS_NATIVE_BUILD_FAILED;
    (void)flags;
    *out_vertex = (XgRenderIrVertex){
        .x = (int32_t)projected.x * 65536,
        .y = (int32_t)projected.y * 65536,
        .native_view_x = projected.native_view_x_16_16,
        .native_view_y = projected.native_view_y_16_16,
        .native_view_position = projected.native_view_position != 0u,
        .projective_view_x = projected.projective_view_x,
        .projective_view_y = projected.projective_view_y,
        .projective_view_z = projected.projective_view_z,
        .projective_offset_x = projected.projective_offset_x_16_16,
        .projective_offset_y = projected.projective_offset_y_16_16,
        .projective_native_offset_x =
            projected.projective_native_offset_x_16_16,
        .projective_native_offset_y =
            projected.projective_native_offset_y_16_16,
        .projective_distance = projected.projective_distance,
        .projective_position = projected.projective_position != 0u,
        .interpolation_group_id = UINT32_C(0x64000000) |
            (source->model_header_address & UINT32_C(0x001fffff)),
        .interpolation_vertex_id =
            (source->source_index << 16u) | source->vertex_index,
        .interpolation_vertex_identity_valid = true,
    };
    return XG_WORLD_MODELS_NATIVE_OK;
}

static int32_t normal_clip(
    const XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT]) {
    const int64_t value =
        (int64_t)vertices[0].x * vertices[1].y +
        (int64_t)vertices[1].x * vertices[2].y +
        (int64_t)vertices[2].x * vertices[0].y -
        (int64_t)vertices[0].x * vertices[2].y -
        (int64_t)vertices[1].x * vertices[0].y -
        (int64_t)vertices[2].x * vertices[1].y;

    return wrap_i32((uint32_t)value);
}

static uint32_t packed_xy(const XgHost3dProjectedVertex *vertex) {
    return (uint16_t)vertex->x | ((uint32_t)(uint16_t)vertex->y << 16u);
}

static bool screen_accepts_with_margin(
    const XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT],
    uint8_t vertex_count, uint32_t screen_right, uint32_t packed_bottom,
    int32_t margin, bool *out_right_edge_rejected) {
    bool all_below = true;
    bool all_left = true;
    bool all_right = true;
    uint32_t vertex;

    for (vertex = 0u; vertex < vertex_count; ++vertex) {
        all_below &= packed_xy(&vertices[vertex]) >= packed_bottom;
        if (margin > 0) {
            all_left &= (int32_t)vertices[vertex].x < -margin;
            all_right &= (int32_t)vertices[vertex].x >=
                (int32_t)screen_right + margin;
        } else {
            all_left = false;
            all_right &= (uint16_t)vertices[vertex].x >= screen_right;
        }
    }
    if (out_right_edge_rejected != NULL)
        *out_right_edge_rejected = !all_below && all_right;
    return !all_below && !all_left && !all_right;
}

static bool screen_accepts(
    const XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT],
    uint8_t vertex_count, uint32_t screen_right, uint32_t packed_bottom,
    bool *out_right_edge_rejected) {
    return screen_accepts_with_margin(
        vertices, vertex_count, screen_right, packed_bottom,
        xg_host_3d_native_view_margin(), out_right_edge_rejected);
}

static uint16_t clamp_otz(int64_t value) {
    const int32_t shifted = wrap_i32(
        (uint32_t)shift_right_floor_i64(value, 12u));

    if (shifted < 0) return 0u;
    if (shifted > UINT16_MAX) return UINT16_MAX;
    return (uint16_t)shifted;
}

static uint16_t average_depth3(
    const XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT],
    int16_t scale) {
    return clamp_otz((int64_t)scale *
                     ((uint32_t)vertices[0].z + vertices[1].z +
                      vertices[2].z));
}

static uint16_t selected_depth(
    const XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT],
    uint8_t vertex_count, bool farthest) {
    uint16_t depth = vertices[0].z;
    uint32_t vertex;

    for (vertex = 1u; vertex < vertex_count; ++vertex) {
        if ((farthest && vertices[vertex].z > depth) ||
            (!farthest && vertices[vertex].z < depth))
            depth = vertices[vertex].z;
    }
    return depth;
}

static uint8_t clamp_color(int32_t value) {
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

static int16_t clamp_i16(int32_t value) {
    if (value < INT16_MIN) return INT16_MIN;
    if (value > INT16_MAX) return INT16_MAX;
    return (int16_t)value;
}

static uint8_t depth_cue_channel(uint8_t color, int32_t far_color,
                                 int16_t depth_cue) {
    const int16_t step = clamp_i16(far_color - (int32_t)color * 16);
    const int64_t mac = shift_right_floor_i64(
        (int64_t)color * 16 * 4096 + (int64_t)depth_cue * step, 12u);

    return clamp_color((int32_t)shift_right_floor_i64(mac, 4u));
}

static bool dispatch_mode_is_valid(uint8_t mode) {
    return mode == 0u || mode == 2u || mode == 3u || mode == 4u || mode == 5u;
}

static uint8_t expected_command_base(uint8_t family) {
    static const uint8_t bases[XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        0x20u, 0x24u, 0x30u, 0x34u, 0x20u, 0x24u, 0x30u, 0x34u,
        0x28u, 0x2cu, 0x38u, 0x3cu, 0x28u, 0x2cu, 0x38u, 0x3cu,
        0x24u,
    };

    return bases[family];
}

static uint8_t xy_word_index(uint8_t family, uint32_t vertex) {
    static const uint8_t triangle_indices[4][3] = {
        {2u, 3u, 4u}, {2u, 4u, 6u},
        {2u, 4u, 6u}, {2u, 5u, 8u},
    };
    static const uint8_t quad_indices[4][4] = {
        {2u, 3u, 4u, 5u}, {2u, 4u, 6u, 8u},
        {2u, 4u, 6u, 8u}, {2u, 5u, 8u, 11u},
    };

    if (family == 16u) return triangle_indices[1][vertex];
    if (family < 8u) return triangle_indices[family & 3u][vertex];
    return quad_indices[family & 3u][vertex];
}

static void set_guest_packet_word_write_mask(
    const XgWorldModelsNativePrimitiveSource *source,
    XgWorldModelsNativePrimitiveOutput *output,
    bool guest_passed_screen_cull, bool guest_right_edge_write) {
    if (guest_passed_screen_cull) {
        output->guest_packet_word_write_mask = output->packet_word_write_mask;
        return;
    }
    if (source->vertex_count == 4u && guest_right_edge_write &&
        (source->dispatch_mode == 4u || source->dispatch_mode == 5u))
        output->guest_packet_word_write_mask =
            UINT32_C(1) << xy_word_index(source->primitive_family, 0u);
}

static uint8_t uv_word_index(uint8_t family, uint32_t vertex) {
    static const uint8_t triangle[2][3] = {
        {3u, 5u, 7u}, {3u, 6u, 9u},
    };
    static const uint8_t quad[2][4] = {
        {3u, 5u, 7u, 9u}, {3u, 6u, 9u, 12u},
    };

    if (family == 16u) return triangle[0][vertex];
    if (family < 8u) return triangle[(family & 3u) == 3u][vertex];
    return quad[(family & 3u) == 3u][vertex];
}

static uint8_t color_word_index(uint8_t family, uint32_t vertex) {
    static const uint8_t triangle[2][3] = {
        {1u, 1u, 1u}, {1u, 3u, 5u},
    };
    static const uint8_t textured_triangle[3] = {1u, 4u, 7u};
    static const uint8_t quad[2][4] = {
        {1u, 1u, 1u, 1u}, {1u, 3u, 5u, 7u},
    };
    static const uint8_t textured_quad[4] = {1u, 4u, 7u, 10u};
    const uint8_t shape = family == 16u ? 1u : (family & 3u);

    if (family < 8u || family == 16u) {
        if (shape == 3u) return textured_triangle[vertex];
        return triangle[shape == 2u][vertex];
    }
    if (shape == 3u) return textured_quad[vertex];
    return quad[shape == 2u][vertex];
}

static bool family_is_textured(uint8_t family) {
    return family == 1u || family == 3u || family == 5u || family == 7u ||
        family == 9u || family == 11u || family == 13u || family == 15u ||
        family == 16u;
}

static bool family_is_gouraud(uint8_t family) {
    return family == 2u || family == 3u || family == 6u || family == 7u ||
        family == 10u || family == 11u || family == 14u || family == 15u;
}

static bool family_is_flat_textured(uint8_t family) {
    return family == 1u || family == 5u || family == 9u || family == 13u;
}

static XgWorldModelsNativeResult decode_primitive(
    const XgWorldModelsNativePreparation *preparation,
    const XgWorldModelsNativePrimitiveSource *source,
    XgWorldModelsNativePrimitiveOutput *output,
    uint8_t colors[XG_HOST_3D_VERTEX_COUNT][3],
    uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2]) {
    const bool textured = family_is_textured(source->primitive_family);
    const bool gouraud = family_is_gouraud(source->primitive_family);
    const uint8_t command =
        (uint8_t)(source->template_words[1] >> 24u);
    const uint8_t attribute_command =
        (uint8_t)(source->attribute_words[0] >> 24u);
    const bool depth_cued =
        (source->dispatch_mode == 4u || source->dispatch_mode == 5u) &&
        family_is_flat_textured(source->primitive_family);
    XgRenderIrMaterialState *material = &output->primitive.material;
    uint16_t tpage = source->tpage;
    uint16_t clut = source->clut;
    uint32_t vertex;

    if ((command & 0xfcu) !=
            expected_command_base(source->primitive_family) ||
        (source->primitive_family != 16u &&
         command != attribute_command &&
         (!depth_cued ||
          command != (uint8_t)(attribute_command & 0xfeu))))
        return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
    if (textured) {
        const uint8_t uv0 = uv_word_index(source->primitive_family, 0u);
        const uint8_t uv1 = uv_word_index(source->primitive_family, 1u);

        clut = (uint16_t)(source->template_words[uv0] >> 16u);
        tpage = (uint16_t)(source->template_words[uv1] >> 16u);
    }
    if (tpage > 0x1ffu || ((tpage >> 7u) & 3u) == 3u)
        return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
    if (clut > 0x7fffu)
        return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;
    *material = (XgRenderIrMaterialState){
        .tpage = tpage,
        .texture_page_x = tpage & 0x0fu,
        .texture_page_y = (tpage >> 4u) & 1u,
        .clut_x = (clut & 0x3fu) << 4u,
        .clut_y = clut >> 6u,
        .draw_area_left = preparation->raster.draw_area_left,
        .draw_area_top = preparation->raster.draw_area_top,
        .draw_area_right = preparation->raster.draw_area_right,
        .draw_area_bottom = preparation->raster.draw_area_bottom,
        .draw_offset_x = preparation->raster.draw_offset_x,
        .draw_offset_y = preparation->raster.draw_offset_y,
        .texture_depth = (XgRenderIrTextureDepth)((tpage >> 7u) & 3u),
        .texture_window_mask_x = preparation->raster.texture_window_mask_x,
        .texture_window_mask_y = preparation->raster.texture_window_mask_y,
        .texture_window_offset_x =
            preparation->raster.texture_window_offset_x,
        .texture_window_offset_y =
            preparation->raster.texture_window_offset_y,
        .shading = gouraud ? XG_RENDER_IR_SHADING_GOURAUD :
                             XG_RENDER_IR_SHADING_FLAT,
        .textured = textured,
        .raw_texture = textured && (command & 1u) != 0u,
        .semi_transparent = (command & 2u) != 0u,
        .blend_mode = (XgRenderIrBlendMode)((tpage >> 5u) & 3u),
        .dither = preparation->raster.dither,
        .mask_set = preparation->raster.mask_set,
        .mask_check = preparation->raster.mask_check,
    };
    for (vertex = 0u; vertex < source->vertex_count; ++vertex) {
        const uint32_t color = source->template_words[
            color_word_index(source->primitive_family, vertex)];

        colors[vertex][0] = (uint8_t)color;
        colors[vertex][1] = (uint8_t)(color >> 8u);
        colors[vertex][2] = (uint8_t)(color >> 16u);
        if (textured) {
            const uint32_t texture = source->template_words[
                uv_word_index(source->primitive_family, vertex)];
            uv[vertex][0] = (uint8_t)texture;
            uv[vertex][1] = (uint8_t)(texture >> 8u);
        }
    }
    return XG_WORLD_MODELS_NATIVE_OK;
}

static void build_semantic_primitive(
    const XgWorldModelsNativePrimitiveSource *source,
    XgWorldModelsNativePrimitiveOutput *output, uint8_t vertex_count,
    const uint8_t colors[XG_HOST_3D_VERTEX_COUNT][3],
    const uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2]) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    uint32_t triangle_count = vertex_count == 4u ? 2u : 1u;
    uint32_t triangle;
    uint32_t vertex;

    output->primitive.triangle_count = (uint8_t)triangle_count;
    for (triangle = 0u; triangle < triangle_count; ++triangle) {
        XgRenderIrTriangle *target = &output->primitive.triangles[triangle];

        target->split_index = (uint8_t)triangle;
        target->split_count = (uint8_t)triangle_count;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t source_vertex = split[triangle][vertex];

            target->vertices[vertex] = (XgRenderIrVertex){
                .x = (int32_t)output->vertices[source_vertex].x * 65536,
                .y = (int32_t)output->vertices[source_vertex].y * 65536,
                .u = (int32_t)uv[source_vertex][0] * 65536,
                .v = (int32_t)uv[source_vertex][1] * 65536,
                .r = colors[source_vertex][0],
                .g = colors[source_vertex][1],
                .b = colors[source_vertex][2],
                .native_view_x =
                    output->vertices[source_vertex].native_view_x_16_16,
                .native_view_y =
                    output->vertices[source_vertex].native_view_y_16_16,
                .native_view_position =
                    output->vertices[source_vertex].native_view_position != 0u,
                .projective_view_x =
                    output->vertices[source_vertex].projective_view_x,
                .projective_view_y =
                    output->vertices[source_vertex].projective_view_y,
                .projective_view_z =
                    output->vertices[source_vertex].projective_view_z,
                .projective_offset_x = output->vertices[source_vertex]
                    .projective_offset_x_16_16,
                .projective_offset_y = output->vertices[source_vertex]
                    .projective_offset_y_16_16,
                .projective_native_offset_x = output->vertices[source_vertex]
                    .projective_native_offset_x_16_16,
                .projective_native_offset_y = output->vertices[source_vertex]
                    .projective_native_offset_y_16_16,
                .projective_distance =
                    output->vertices[source_vertex].projective_distance,
                .projective_position =
                    output->vertices[source_vertex].projective_position != 0u,
                .interpolation_group_id = UINT32_C(0x64000000) |
                    (source->model_header_address & UINT32_C(0x001fffff)),
                .interpolation_vertex_id =
                    (source->source_index << 16u) |
                    source->topology[source_vertex],
                .interpolation_vertex_identity_valid = true,
            };
        }
    }
}

static void write_xy_words(
    const XgWorldModelsNativePrimitiveSource *source,
    XgWorldModelsNativePrimitiveOutput *output, uint32_t count) {
    uint32_t vertex;

    for (vertex = 0u; vertex < count; ++vertex) {
        const uint8_t word = xy_word_index(source->primitive_family, vertex);

        output->packet_words[word] = packed_xy(&output->vertices[vertex]);
        output->packet_word_write_mask |= UINT32_C(1) << word;
    }
}

static int32_t rotated_mac(const int16_t matrix[3][3], uint32_t row,
                           const XgHost3dVector *vector) {
    const int64_t value =
        (int64_t)matrix[row][0] * vector->x +
        (int64_t)matrix[row][1] * vector->y +
        (int64_t)matrix[row][2] * vector->z;

    return (int32_t)shift_right_floor_i64(value, 12u);
}

XgWorldModelsNativeResult xg_world_models_native_build_primitive(
    XgWorldModelsNativePreparation *preparation,
    uint64_t authentication_generation,
    const XgWorldModelsNativePrimitiveSource *source,
    XgWorldModelsNativePrimitiveOutput *out_primitive) {
    XgWorldModelsNativePrimitiveOutput output = {0};
    XgHost3dProject4Input input = {0};
    XgHost3dRotAverage4Output projected;
    uint8_t colors[XG_HOST_3D_VERTEX_COUNT][3] = {{0}};
    uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2] = {{0}};
    uint16_t ordering_depth;
    uint8_t mode;
    uint32_t vertex;
    bool right_edge_rejected = false;
    bool guest_right_edge_rejected = false;
    bool guest_right_edge_write = false;
    bool guest_passed_screen_cull = false;
    bool guest_screen_accepts;
    bool native_screen_accepts;
    XgWorldModelsNativeResult result;

    if (out_primitive == NULL)
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    memset(out_primitive, 0, sizeof(*out_primitive));
    if (preparation == NULL || source == NULL ||
        authentication_generation == 0u || !preparation->authenticated ||
        !preparation->sealed || preparation->consumed ||
        preparation->authentication_generation !=
            authentication_generation ||
        preparation->sealed_primitives == NULL ||
        (uintptr_t)source < (uintptr_t)preparation->sealed_primitives ||
        (uintptr_t)source >= (uintptr_t)preparation->sealed_primitives +
            preparation->primitive_count *
                sizeof(preparation->sealed_primitives[0]) ||
        ((uintptr_t)source - (uintptr_t)preparation->sealed_primitives) %
                sizeof(preparation->sealed_primitives[0]) != 0u ||
        source->primitive_family >= XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT ||
        !dispatch_mode_is_valid(source->dispatch_mode) ||
        source->vertex_count != (source->primitive_family < 8u ? 3u :
            (source->primitive_family < 16u ? 4u : 3u)) ||
        source->packet_word_count !=
            packet_strides[source->primitive_family] / 4u ||
        source->attribute_size !=
            attribute_sizes[source->primitive_family] ||
        source->source_index >= preparation->record_count ||
        source->dispatch_index >= preparation->dispatch_count ||
        (preparation->primitive_family_mask &
         (UINT32_C(1) << source->primitive_family)) == 0u ||
        (preparation->dispatch_mode_mask &
         (UINT32_C(1) << source->dispatch_mode)) == 0u)
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    if (!preparation->primitive_digest_validated) {
        uint64_t primitive_digest = UINT64_C(1469598103934665603);
        uint32_t byte;

        for (byte = 0u;
             byte < preparation->primitive_count *
                 sizeof(preparation->sealed_primitives[0]); ++byte) {
            primitive_digest ^=
                ((const uint8_t *)preparation->sealed_primitives)[byte];
            primitive_digest *= UINT64_C(1099511628211);
        }
        if (primitive_digest != preparation->primitive_digest)
            return XG_WORLD_MODELS_NATIVE_UNAUTHENTICATED;
        preparation->primitive_digest_validated = true;
    }
    if (preparation->ordering_shift > 31u)
        return XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH;

    output.packet_address = source->packet_address;
    output.packet_word_count = source->packet_word_count;
    memcpy(output.packet_words, source->template_words,
           sizeof(uint32_t) * source->packet_word_count);
    result = decode_primitive(preparation, source, &output, colors, uv);
    if (result != XG_WORLD_MODELS_NATIVE_OK) return result;

    memcpy(input.vertices, source->vertices, sizeof(input.vertices));
    if (source->vertex_count == 3u)
        input.vertices[3] = input.vertices[2];
    input.projection = source->projection;
    if (!xg_host_3d_rot_average4(&input, &projected))
        return XG_WORLD_MODELS_NATIVE_BUILD_FAILED;
    memcpy(output.vertices, projected.vertices, sizeof(output.vertices));
    output.projection_flags = projected.projection_flags;
    output.nclip = normal_clip(output.vertices);
    build_semantic_primitive(
        source, &output, source->vertex_count, colors, uv);
    mode = source->dispatch_mode;
    if ((mode == 4u || mode == 5u) &&
        !family_is_flat_textured(source->primitive_family))
        mode = 0u;
    guest_screen_accepts = screen_accepts_with_margin(
        output.vertices, source->vertex_count, preparation->guest_xclip_bound,
        preparation->packed_screen_bottom, 0, &guest_right_edge_rejected);
    /* The resident handlers read COP2 data register 31 (LZCR), not control
     * register 31 (FLAG). LZCR is never negative, so their bltz checks do not
     * reject projection saturation. Native staging still honors FLAG below. */
    if (source->vertex_count == 4u) {
        guest_passed_screen_cull = output.nclip > 0 && guest_screen_accepts;
    } else {
        guest_passed_screen_cull = output.nclip > 0 && guest_screen_accepts;
    }
    guest_right_edge_write = source->vertex_count == 4u &&
        (mode == 4u || mode == 5u) &&
        output.nclip > 0 && !guest_screen_accepts &&
        guest_right_edge_rejected;
    output.counter_incremented = guest_passed_screen_cull;

    if (source->vertex_count == 4u) {
        if ((int32_t)projected.rtpt_flags < 0 &&
            !guest_passed_screen_cull) {
            set_guest_packet_word_write_mask(
                source, &output, guest_passed_screen_cull,
                guest_right_edge_write);
            *out_primitive = output;
            return XG_WORLD_MODELS_NATIVE_OK;
        }
        output.packet_cursor_masked = true;
        native_screen_accepts = screen_accepts(
            output.vertices, source->vertex_count, preparation->screen_right,
            preparation->packed_screen_bottom, &right_edge_rejected);
        if (output.nclip <= 0 ||
            (((int32_t)projected.rtpt_flags < 0 ||
              (int32_t)projected.rtps_flags < 0 ||
              !native_screen_accepts) && !guest_passed_screen_cull)) {
            if (right_edge_rejected && (mode == 4u || mode == 5u))
                write_xy_words(source, &output, 1u);
            set_guest_packet_word_write_mask(
                source, &output, guest_passed_screen_cull,
                guest_right_edge_write);
            *out_primitive = output;
            return XG_WORLD_MODELS_NATIVE_OK;
        }
        output.passed_screen_cull =
            (int32_t)projected.rtpt_flags >= 0 &&
            (int32_t)projected.rtps_flags >= 0 && native_screen_accepts;
    } else {
        native_screen_accepts = screen_accepts(
            output.vertices, source->vertex_count, preparation->screen_right,
            preparation->packed_screen_bottom, NULL);
        if (((int32_t)projected.rtpt_flags < 0 ||
             !native_screen_accepts) && !guest_passed_screen_cull) {
            set_guest_packet_word_write_mask(
                source, &output, guest_passed_screen_cull,
                guest_right_edge_write);
            *out_primitive = output;
            return XG_WORLD_MODELS_NATIVE_OK;
        }
        output.passed_screen_cull =
            (int32_t)projected.rtpt_flags >= 0 && native_screen_accepts;
    }

    if (source->primitive_family == 16u) {
        uint32_t uv_vertex;

        write_xy_words(source, &output, 2u);
        if (output.nclip <= 0) {
            set_guest_packet_word_write_mask(
                source, &output, guest_passed_screen_cull,
                guest_right_edge_write);
            *out_primitive = output;
            return XG_WORLD_MODELS_NATIVE_OK;
        }
        output.packet_cursor_masked = true;
        write_xy_words(source, &output, 3u);
        ordering_depth = average_depth3(
            output.vertices, preparation->gte.average_z_scale3);
        output.ordering_depth = ordering_depth;
        output.ordering_bucket = ordering_depth >> preparation->ordering_shift;
        if (ordering_depth == 0u) {
            set_guest_packet_word_write_mask(
                source, &output, guest_passed_screen_cull,
                guest_right_edge_write);
            *out_primitive = output;
            return XG_WORLD_MODELS_NATIVE_OK;
        }
        for (uv_vertex = 0u; uv_vertex < 3u; ++uv_vertex) {
            const uint8_t word = uv_word_index(16u, uv_vertex);
            const int32_t mac_x = rotated_mac(
                source->projection.rotation, 0u,
                &source->auxiliary_vertices[uv_vertex]);
            const int32_t mac_y = rotated_mac(
                source->projection.rotation, 1u,
                &source->auxiliary_vertices[uv_vertex]);
            const uint8_t u = (uint8_t)(((uint32_t)mac_x >> 6u) + 0x40u);
            const uint8_t v = (uint8_t)(((uint32_t)mac_y >> 6u) + 0x40u);

            uv[uv_vertex][0] = u;
            uv[uv_vertex][1] = v;
            output.packet_words[word] =
                (output.packet_words[word] & UINT32_C(0xffff0000)) |
                u | ((uint32_t)v << 8u);
            output.packet_word_write_mask |= UINT32_C(1) << word;
        }
        build_semantic_primitive(source, &output, 3u, colors, uv);
        output.accepted = output.passed_screen_cull;
        output.ordering_table_written = output.accepted;
        output.guest_ordering_table_written = guest_passed_screen_cull;
        if (output.accepted || output.guest_ordering_table_written) {
            output.packet_words[0] =
                ((uint32_t)rendered_packet_tag_payload_word_count(
                     source->primitive_family) << 24u) |
                (output.packet_words[0] & UINT32_C(0x00ffffff));
            output.packet_word_write_mask |= 1u;
        }
        if (output.ordering_bucket >= XG_WORLD_MODELS_OT_BUCKET_COUNT)
            return XG_WORLD_MODELS_NATIVE_BUILD_FAILED;
        set_guest_packet_word_write_mask(
            source, &output, guest_passed_screen_cull,
            guest_right_edge_write);
        *out_primitive = output;
        return XG_WORLD_MODELS_NATIVE_OK;
    }

    if (source->vertex_count == 3u && (mode == 0u || mode == 4u))
        output.packet_cursor_masked = true;
    if (output.nclip <= 0) {
        set_guest_packet_word_write_mask(
            source, &output, guest_passed_screen_cull,
            guest_right_edge_write);
        *out_primitive = output;
        return XG_WORLD_MODELS_NATIVE_OK;
    }
    if (source->vertex_count == 3u)
        output.packet_cursor_masked = true;

    if (mode == 0u || mode == 4u) {
        ordering_depth = source->vertex_count == 4u
            ? projected.ordering_depth
            : average_depth3(output.vertices,
                             preparation->gte.average_z_scale3);
        output.ordering_bucket =
            ordering_depth >> preparation->ordering_shift;
    } else {
        ordering_depth = selected_depth(
            output.vertices, source->vertex_count,
            mode == 2u || mode == 5u);
        output.ordering_bucket = ordering_depth >>
            ((preparation->ordering_shift + 2u) & 31u);
    }
    output.ordering_depth = ordering_depth;

    if (mode != 0u || source->vertex_count == 3u)
        write_xy_words(source, &output, source->vertex_count);
    if (ordering_depth == 0u) {
        set_guest_packet_word_write_mask(
            source, &output, guest_passed_screen_cull,
            guest_right_edge_write);
        *out_primitive = output;
        return XG_WORLD_MODELS_NATIVE_OK;
    }
    if (mode == 0u && source->vertex_count == 4u)
        write_xy_words(source, &output, source->vertex_count);
    if (mode == 4u || mode == 5u) {
        const uint32_t depth_cue_color = preparation->depth_cue_color_word;
        const uint32_t command =
            (source->template_words[1] >> 24u) & UINT32_C(0xfe);
        const uint8_t red = depth_cue_channel(
            (uint8_t)depth_cue_color, preparation->gte.far_color[0],
            projected.depth_cue);
        const uint8_t green = depth_cue_channel(
            (uint8_t)(depth_cue_color >> 8u), preparation->gte.far_color[1],
            projected.depth_cue);
        const uint8_t blue = depth_cue_channel(
            (uint8_t)(depth_cue_color >> 16u), preparation->gte.far_color[2],
            projected.depth_cue);

        output.packet_words[1] =
            (command << 24u) |
            red | ((uint32_t)green << 8u) | ((uint32_t)blue << 16u);
        output.packet_word_write_mask |= UINT32_C(1) << 1u;
        output.primitive.material.raw_texture = false;
        for (vertex = 0u; vertex < source->vertex_count; ++vertex) {
            colors[vertex][0] = red;
            colors[vertex][1] = green;
            colors[vertex][2] = blue;
        }
        build_semantic_primitive(
            source, &output, source->vertex_count, colors, uv);
    }
    if (output.ordering_bucket >= XG_WORLD_MODELS_OT_BUCKET_COUNT)
        return XG_WORLD_MODELS_NATIVE_BUILD_FAILED;
    output.accepted = output.passed_screen_cull;
    output.ordering_table_written = output.accepted;
    output.guest_ordering_table_written = guest_passed_screen_cull;
    if (output.accepted || output.guest_ordering_table_written) {
        output.packet_words[0] =
            ((uint32_t)rendered_packet_tag_payload_word_count(
                 source->primitive_family) << 24u) |
            (output.packet_words[0] & UINT32_C(0x00ffffff));
        output.packet_word_write_mask |= 1u;
    }
    set_guest_packet_word_write_mask(
        source, &output, guest_passed_screen_cull,
        guest_right_edge_write);
    *out_primitive = output;
    return XG_WORLD_MODELS_NATIVE_OK;
}

XgWorldModelsNativeResult xg_world_models_native_finalize(
    XgWorldModelsNativePreparation *preparation,
    uint64_t authentication_generation,
    const XgWorldModelsNativeDispatch *dispatches,
    XgWorldModelsNativeDispatchOutput *dispatch_outputs,
    uint32_t dispatch_output_count,
    const XgWorldModelsNativePrimitiveSource *primitives,
    XgWorldModelsNativePrimitiveOutput *primitive_outputs,
    uint32_t primitive_output_count, void *submission_context,
    XgWorldModelsNativeBeginSubmission begin_submission,
    XgWorldModelsNativeStagePrimitive stage_primitive,
    XgWorldModelsNativeCommit *out_commit) {
    XgWorldModelsNativeCommit commit = {0};
    uint64_t dispatch_digest = UINT64_C(1469598103934665603);
    uint64_t primitive_digest = UINT64_C(1469598103934665603);
    uint64_t anchor_digest = UINT64_C(1469598103934665603);
    uint32_t primitive_index = 0u;
    uint32_t byte;
    uint32_t index;
    bool exact_submission_required = false;

    if (out_commit == NULL)
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    memset(out_commit, 0, sizeof(*out_commit));
    if (preparation == NULL || authentication_generation == 0u ||
        !preparation->authenticated || !preparation->sealed ||
        preparation->consumed ||
        preparation->authentication_generation != authentication_generation ||
        dispatches != preparation->sealed_dispatches ||
        primitives != preparation->sealed_primitives ||
        dispatch_output_count != preparation->dispatch_count ||
        primitive_output_count != preparation->primitive_count ||
        (preparation->anchor_count != 0u &&
         preparation->sealed_anchor_sources == NULL) ||
        (dispatch_output_count != 0u &&
         (dispatches == NULL || dispatch_outputs == NULL)) ||
        (primitive_output_count != 0u &&
         (primitives == NULL || primitive_outputs == NULL)) ||
        begin_submission == NULL || stage_primitive == NULL)
        return XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT;
    for (byte = 0u;
         byte < dispatch_output_count * sizeof(dispatches[0]); ++byte) {
        dispatch_digest ^= ((const uint8_t *)dispatches)[byte];
        dispatch_digest *= UINT64_C(1099511628211);
    }
    for (byte = 0u;
         byte < primitive_output_count * sizeof(primitives[0]); ++byte) {
        primitive_digest ^= ((const uint8_t *)primitives)[byte];
        primitive_digest *= UINT64_C(1099511628211);
    }
    if (preparation->anchor_count != 0u) {
        for (byte = 0u;
             byte < preparation->anchor_count *
                 sizeof(preparation->sealed_anchor_sources[0]); ++byte) {
            anchor_digest ^=
                ((const uint8_t *)preparation->sealed_anchor_sources)[byte];
            anchor_digest *= UINT64_C(1099511628211);
        }
    }
    if (dispatch_digest != preparation->dispatch_digest ||
        primitive_digest != preparation->primitive_digest ||
        (preparation->anchor_count != 0u &&
         anchor_digest != preparation->anchor_digest))
        return XG_WORLD_MODELS_NATIVE_UNAUTHENTICATED;
    commit.entry_side_effects = preparation->world.entry_side_effects;
    for (index = 0u; index < dispatch_output_count; ++index) {
        const XgWorldModelsNativeDispatch *dispatch = &dispatches[index];
        XgWorldModelsNativeDispatchOutput *output =
            &dispatch_outputs[index];
        uint32_t expected_packet_cursor;
        uint32_t processed_count = 0u;
        uint32_t accepted_count = 0u;
        uint32_t side_effect_count = 0u;
        uint32_t emitted_count = 0u;
        uint32_t family_mask = 0u;
        bool packet_cursor_masked = false;

        if (output->source_index != dispatch->source_index ||
            output->bounds_accepted != dispatch->bounds_accepted ||
            output->guest_bounds_accepted !=
                dispatch->guest_bounds_accepted ||
            output->staged_primitive_count != 0u ||
            output->staged_packet_side_effect_count != 0u)
            return XG_WORLD_MODELS_NATIVE_INCOMPLETE_OUTPUT;
        ++commit.completed_dispatch_count;
        if (!output->bounds_accepted && !output->guest_bounds_accepted) {
            if (output->processed_primitive_count != 0u ||
                output->accepted_primitive_count != 0u ||
                output->staged_primitive_count != 0u ||
                output->packet_side_effect_primitive_count != 0u ||
                output->staged_packet_side_effect_count != 0u ||
                output->emitted_count_delta != 0u ||
                output->primitive_family_mask != 0u ||
                output->packet_cursor_after != 0u ||
                output->group_cursor_after != 0u ||
                output->packet_cursor_masked)
                return XG_WORLD_MODELS_NATIVE_INCOMPLETE_OUTPUT;
            continue;
        }
        if (dispatch->primitive_start != primitive_index ||
            dispatch->primitive_count >
                primitive_output_count - primitive_index)
            return XG_WORLD_MODELS_NATIVE_INCOMPLETE_OUTPUT;
        for (; processed_count < dispatch->primitive_count;
             ++processed_count, ++primitive_index) {
            const XgWorldModelsNativePrimitiveSource *source =
                &primitives[primitive_index];
            XgWorldModelsNativePrimitiveOutput *primitive =
                &primitive_outputs[primitive_index];
            const uint32_t allowed_mask =
                (UINT32_C(1) << source->packet_word_count) - 1u;

            if (source->dispatch_index != index ||
                source->primitive_index != processed_count ||
                primitive->packet_address != source->packet_address ||
                primitive->packet_word_count != source->packet_word_count ||
                primitive->primitive_staged ||
                primitive->packet_side_effects_staged ||
                primitive->accepted != primitive->ordering_table_written ||
                (primitive->guest_ordering_table_written &&
                 !primitive->counter_incremented) ||
                (primitive->packet_word_write_mask & ~allowed_mask) != 0u ||
                (primitive->guest_packet_word_write_mask & ~allowed_mask) != 0u ||
                (primitive->guest_packet_word_write_mask &
                 ~primitive->packet_word_write_mask) != 0u ||
                ((primitive->accepted ||
                  primitive->guest_ordering_table_written) &&
                  ((primitive->packet_word_write_mask & 1u) == 0u ||
                   primitive->primitive.triangle_count == 0u)) ||
                (!primitive->accepted &&
                  !primitive->guest_ordering_table_written &&
                   (primitive->packet_word_write_mask & 1u) != 0u))
                return XG_WORLD_MODELS_NATIVE_INCOMPLETE_OUTPUT;
            exact_submission_required |= primitive->accepted ||
                primitive->guest_ordering_table_written;
            accepted_count += primitive->accepted ? 1u : 0u;
            side_effect_count +=
                primitive->packet_word_write_mask != 0u ? 1u : 0u;
            emitted_count += primitive->counter_incremented ? 1u : 0u;
            family_mask |= UINT32_C(1) << source->primitive_family;
            packet_cursor_masked |= primitive->packet_cursor_masked;
        }
        expected_packet_cursor = dispatch->packet_cursor_after;
        if (output->packet_cursor_masked)
            expected_packet_cursor &= UINT32_C(0x00ffffff);
        if (output->processed_primitive_count != processed_count ||
            output->accepted_primitive_count != accepted_count ||
             output->packet_side_effect_primitive_count != side_effect_count ||
             output->emitted_count_delta != emitted_count ||
            output->primitive_family_mask != family_mask ||
            output->packet_cursor_masked != packet_cursor_masked ||
            output->accepted_primitive_count >
                output->processed_primitive_count ||
             output->accepted_primitive_count >
                 output->packet_side_effect_primitive_count ||
             output->packet_side_effect_primitive_count >
                 output->processed_primitive_count ||
             output->emitted_count_delta >
                 output->processed_primitive_count ||
            output->primitive_family_mask !=
                dispatch->primitive_family_mask ||
            output->packet_cursor_after != expected_packet_cursor ||
            output->group_cursor_after != dispatch->group_cursor_after ||
            (dispatch->guest_bounds_accepted &&
             commit.resident_vertex_total > UINT32_MAX -
                 dispatch->model.vertex_count) ||
            commit.resident_emitted_count > UINT32_MAX -
                output->emitted_count_delta ||
            commit.processed_primitive_count > UINT32_MAX -
                output->processed_primitive_count ||
            commit.accepted_primitive_count > UINT32_MAX -
                output->accepted_primitive_count)
            return XG_WORLD_MODELS_NATIVE_INCOMPLETE_OUTPUT;

        if (dispatch->guest_bounds_accepted)
            commit.resident_vertex_total += dispatch->model.vertex_count;
        commit.resident_emitted_count += output->emitted_count_delta;
        commit.processed_primitive_count += output->processed_primitive_count;
        commit.accepted_primitive_count += output->accepted_primitive_count;
        if (dispatch->guest_bounds_accepted) {
            commit.resident_packet_cursor = output->packet_cursor_after;
            commit.resident_group_cursor = output->group_cursor_after;
            commit.resident_model_0c = dispatch->model.auxiliary_vertex_base;
            commit.resident_vertex_base = dispatch->model.vertex_base;
            commit.resident_ot_base = dispatch->call.ordering_table_address;
            commit.resident_model_18 = dispatch->model.model_18;
            commit.resident_dispatch_globals_written = true;
        }
    }
    if (commit.processed_primitive_count != preparation->primitive_count ||
        primitive_index != primitive_output_count)
        return XG_WORLD_MODELS_NATIVE_INCOMPLETE_OUTPUT;
    if (exact_submission_required && !begin_submission(submission_context))
        return XG_WORLD_MODELS_NATIVE_BUILD_FAILED;
    for (primitive_index = 0u; primitive_index < primitive_output_count;
         ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &primitives[primitive_index];
        XgWorldModelsNativePrimitiveOutput *primitive =
            &primitive_outputs[primitive_index];
        XgWorldModelsNativeDispatchOutput *output =
            &dispatch_outputs[source->dispatch_index];

        if (primitive->packet_word_write_mask != 0u) {
            primitive->packet_side_effects_staged = true;
            ++output->staged_packet_side_effect_count;
        }
        if (primitive->accepted || primitive->guest_ordering_table_written) {
            if (!stage_primitive(
                    submission_context, &primitive->primitive,
                    primitive->packet_address, primitive_index))
                return XG_WORLD_MODELS_NATIVE_BUILD_FAILED;
            primitive->primitive_staged = true;
            ++output->staged_primitive_count;
        }
    }
    commit.authentication_generation = authentication_generation;
    commit.continuation_pc = preparation->continuation_pc;
    commit.entry_side_effects.resident_vertex_total =
        commit.resident_vertex_total;
    commit.entry_side_effects.resident_emitted_count =
        commit.resident_emitted_count;
    commit.authenticated = true;
    commit.sealed = true;
    preparation->sealed = false;
    preparation->consumed = true;
    *out_commit = commit;
    return XG_WORLD_MODELS_NATIVE_OK;
}
