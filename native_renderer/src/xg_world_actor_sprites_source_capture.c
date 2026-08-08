#include "xg_world_actor_sprites_source_capture.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    ACTOR_SOURCE_SIZE = 0xb0,
    ACTOR_DATA_SIZE = 0x40,
    ACTOR_DESCRIPTOR_STRIDE = 0x18,
    ACTOR_PART_STRIDE = 8,
    ACTOR_CONTEXT_OT_OFFSET = 0x70,
    ACTOR_CAMERA_MATRIX = 0x8004fbb8u,
    ACTOR_CONTEXT = 0x8009be3cu,
    ACTOR_TRIG_TABLE = 0x800523f0u,
};

typedef struct ActorCaptureAccess {
    const XgWorldActorSpritesAuthenticatedReader *reader;
    uint32_t read_count;
    uint32_t read_bytes;
} ActorCaptureAccess;

static bool ram_range_is_valid(uint32_t address, uint32_t size,
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

static bool ram_pointer_is_valid(uint32_t address, uint32_t alignment) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    return alignment != 0u && (address & (alignment - 1u)) == 0u &&
           (segment == 0u || segment == UINT32_C(0x80000000) ||
            segment == UINT32_C(0xa0000000)) &&
           physical <= UINT32_C(0x00200000);
}

static bool ram_ranges_overlap(uint32_t left_address, uint32_t left_size,
                               uint32_t right_address, uint32_t right_size) {
    const uint64_t left = left_address & UINT32_C(0x1fffffff);
    const uint64_t right = right_address & UINT32_C(0x1fffffff);

    return left_size != 0u && right_size != 0u &&
           left < right + right_size && right < left + left_size;
}

static bool authorize_range(
    const XgWorldActorSpritesAuthenticatedReader *reader,
    XgWorldActorSpritesSourceRangeKind kind, uint32_t address, uint32_t size,
    uint32_t alignment) {
    return ram_range_is_valid(address, size, alignment) &&
           reader->authorize_source_range(reader->context, kind, address, size);
}

static XgWorldActorSpritesNativeResult read_u8(ActorCaptureAccess *access,
                                                uint32_t address,
                                                uint8_t *out_value) {
    if (access->read_count >=
            XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_READS ||
        access->read_bytes >
            XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_BYTES - 1u)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_u8(access->reader->context, address, out_value))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_READ_FAILED;
    ++access->read_count;
    ++access->read_bytes;
    return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;
}

static XgWorldActorSpritesNativeResult read_u16(ActorCaptureAccess *access,
                                                 uint32_t address,
                                                 uint16_t *out_value) {
    if (access->read_count >=
            XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_READS ||
        access->read_bytes >
            XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_BYTES - 2u)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, address, out_value))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;
}

static XgWorldActorSpritesNativeResult read_u32(ActorCaptureAccess *access,
                                                 uint32_t address,
                                                 uint32_t *out_value) {
    if (access->read_count >=
            XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_READS ||
        access->read_bytes >
            XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_BYTES - 4u)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;
}

static int16_t low_s16(uint32_t value) {
    const uint16_t low = (uint16_t)value;

    if (low <= INT16_MAX) return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static int32_t i32_from_u32(uint32_t value) {
    if (value <= INT32_MAX) return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int32_t add_i32_wrap(int32_t left, int32_t right) {
    return i32_from_u32((uint32_t)left + (uint32_t)right);
}

static int32_t shift_right_floor_i32(int32_t value, unsigned shift) {
    int64_t magnitude;

    if (shift == 0u || value >= 0) return value / (INT32_C(1) << shift);
    magnitude = -(int64_t)value;
    return (int32_t)(-((magnitude + ((INT64_C(1) << shift) - 1)) >> shift));
}

static int32_t scaled_offset(int8_t offset, uint8_t shift, int16_t scale,
                             bool reverse) {
    int32_t shifted = i32_from_u32((uint32_t)(int32_t)offset << shift);
    int32_t product;

    if (reverse) shifted = i32_from_u32(0u - (uint32_t)shifted);
    product = i32_from_u32((uint32_t)shifted * (uint32_t)(int32_t)scale);
    if (product < 0) product = add_i32_wrap(product, 0xfff);
    return shift_right_floor_i32(product, 12u);
}

static int32_t mul_shift_12(int32_t left, int32_t right) {
    const int64_t product = (int64_t)left * right;

    if (product >= 0) return (int32_t)(product / 4096);
    return (int32_t)(-((-product + 4095) / 4096));
}

static int16_t mul_shift_12_s16(int32_t left, int32_t right) {
    return low_s16((uint32_t)mul_shift_12(left, right));
}

static void rotation_matrix_from_trig(uint32_t trig_x, uint32_t trig_y,
                                      uint32_t trig_z,
                                      XgHost3dMatrix *matrix) {
    const int16_t sine_x = low_s16(trig_x);
    const int16_t cosine_x = low_s16(trig_x >> 16u);
    const int16_t sine_y = low_s16(trig_y);
    const int16_t cosine_y = low_s16(trig_y >> 16u);
    const int16_t sine_z = low_s16(trig_z);
    const int16_t cosine_z = low_s16(trig_z >> 16u);
    const int32_t zy = mul_shift_12(cosine_z, -(int32_t)sine_y);
    const int32_t sy = mul_shift_12(sine_z, -(int32_t)sine_y);

    memset(matrix, 0, sizeof(*matrix));
    matrix->rotation[0][0] = mul_shift_12_s16(cosine_z, cosine_y);
    matrix->rotation[1][0] = low_s16((uint32_t)(
        (int32_t)mul_shift_12_s16(sine_z, cosine_x) -
        mul_shift_12_s16(zy, sine_x)));
    matrix->rotation[2][0] = low_s16((uint32_t)(
        (int32_t)mul_shift_12_s16(zy, cosine_x) +
        mul_shift_12_s16(sine_z, sine_x)));
    matrix->rotation[0][1] =
        low_s16((uint32_t)mul_shift_12(-(int32_t)sine_z, cosine_y));
    matrix->rotation[1][1] = low_s16((uint32_t)(
        (int32_t)mul_shift_12_s16(cosine_z, cosine_x) +
        mul_shift_12_s16(sy, sine_x)));
    matrix->rotation[2][1] = low_s16((uint32_t)(
        (int32_t)mul_shift_12_s16(cosine_z, sine_x) -
        mul_shift_12_s16(sy, cosine_x)));
    matrix->rotation[0][2] = sine_y;
    matrix->rotation[1][2] =
        low_s16((uint32_t)mul_shift_12(-(int32_t)cosine_y, sine_x));
    matrix->rotation[2][2] = mul_shift_12_s16(cosine_y, cosine_x);
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

static bool raster_is_valid(const XgWorldActorSpritesRasterState *raster) {
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

static void apply_material(const XgWorldActorSpritesRasterState *raster,
                           XgRenderIrMaterialState *material) {
    *material = (XgRenderIrMaterialState){
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
        .shading = XG_RENDER_IR_SHADING_FLAT,
        .textured = true,
        .dither = raster->dither,
        .mask_set = raster->mask_set,
        .mask_check = raster->mask_check,
    };
}

static XgWorldActorSpritesNativeResult capture_matrix(
    ActorCaptureAccess *access, uint32_t address, XgHost3dMatrix *matrix) {
    uint32_t index;

    memset(matrix, 0, sizeof(*matrix));
    for (index = 0u; index < 8u; ++index) {
        uint32_t word;
        XgWorldActorSpritesNativeResult result =
            read_u32(access, address + index * 4u, &word);

        if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
        parse_matrix_word(matrix, index, word);
    }
    return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;
}

static XgWorldActorSpritesNativeResult capture_vertex_template(
    ActorCaptureAccess *access, uint32_t address,
    XgHost3dVector vertices[XG_HOST_3D_VERTEX_COUNT]) {
    uint32_t index;

    for (index = 0u; index < XG_HOST_3D_VERTEX_COUNT; ++index) {
        uint32_t xy;
        uint32_t zp;
        XgWorldActorSpritesNativeResult result =
            read_u32(access, address + index * 8u, &xy);

        if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
        result = read_u32(access, address + index * 8u + 4u, &zp);
        if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
        vertices[index] = (XgHost3dVector){
            low_s16(xy), low_s16(xy >> 16u), low_s16(zp),
            (uint16_t)(zp >> 16u),
        };
    }
    return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;
}

static XgWorldActorSpritesNativeResult capture_part(
    ActorCaptureAccess *access, uint32_t address,
    const XgWorldActorSpriteActorSource *actor,
    XgWorldActorSpritePartTransform *part) {
    uint16_t values[4];
    uint32_t trig[3];
    uint32_t index;

    if (!authorize_range(access->reader, XG_WORLD_ACTOR_SPRITES_SOURCE_PARTS,
                         address, ACTOR_PART_STRIDE, 2u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    for (index = 0u; index < 4u; ++index) {
        XgWorldActorSpritesNativeResult result =
            read_u16(access, address + index * 2u, &values[index]);

        if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
    }
    part->enabled = values[0] != 0u || low_s16(values[3]) != 0;
    if (!part->enabled) return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;

    part->offset_x = scaled_offset((int8_t)(uint8_t)values[0],
                                   actor->scale_shift, actor->scale,
                                   actor->flip_x);
    part->offset_y = scaled_offset((int8_t)(uint8_t)(values[0] >> 8u),
                                   actor->scale_shift, actor->scale, false);
    if (actor->flip_x)
        values[3] = (uint16_t)(0u - values[3]);
    for (index = 0u; index < 3u; ++index) {
        const uint32_t trig_address = ACTOR_TRIG_TABLE +
            ((uint32_t)values[index + 1u] & 0xfffu) * 4u;
        XgWorldActorSpritesNativeResult result;

        if (!authorize_range(
                access->reader, XG_WORLD_ACTOR_SPRITES_SOURCE_TRIG_TABLE,
                trig_address, 4u, 4u))
            return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
        result = read_u32(access, trig_address, &trig[index]);
        if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
    }
    rotation_matrix_from_trig(trig[0], trig[1], trig[2], &part->rotation);
    return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;
}

static XgWorldActorSpritesNativeResult build_result(
    XgWorldActorSpritesResult result) {
    switch (result) {
    case XG_WORLD_ACTOR_SPRITES_OK:
        return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;
    case XG_WORLD_ACTOR_SPRITES_CAPACITY_EXCEEDED:
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;
    case XG_WORLD_ACTOR_SPRITES_BUILD_FAILED:
        return XG_WORLD_ACTOR_SPRITES_NATIVE_BUILD_FAILED;
    case XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE:
        return XG_WORLD_ACTOR_SPRITES_NATIVE_SOURCE_MISMATCH;
    case XG_WORLD_ACTOR_SPRITES_INVALID_ARGUMENT:
    default:
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_ARGUMENT;
    }
}

static bool family_capacity(uint32_t cursor, uint32_t descriptor_count,
                            uint32_t limit, bool *out_enabled) {
    const uint32_t bytes =
        descriptor_count * XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE;

    if (cursor > UINT32_MAX - bytes) return false;
    *out_enabled = cursor + bytes < limit;
    return true;
}

XgWorldActorSpritesNativeResult xg_world_actor_sprites_native_prepare(
    const XgWorldActorSpritesNativeRequest *request,
    const XgWorldActorSpritesAuthenticatedReader *reader,
    XgWorldActorSpriteRecord *records, uint32_t record_capacity,
    XgWorldActorSpritesNativePreparation *out_preparation) {
    XgWorldActorSpritesNativePreparation preparation = {0};
    XgWorldActorSpriteActorSource actor = {0};
    XgWorldActorSpriteDescriptor
        descriptors[XG_WORLD_ACTOR_SPRITES_DESCRIPTOR_CAPACITY];
    XgWorldActorSpritesValueSource source = {0};
    XgWorldActorSpritesAdaptedActor adapted;
    ActorCaptureAccess access = {0};
    XgHost3dMatrix camera;
    uint32_t actor_flags;
    uint32_t actor_config;
    uint32_t actor_aux_flags;
    uint32_t part_address;
    uint32_t context;
    uint32_t ot_base;
    uint32_t used_parts = 0u;
    uint32_t built_count = 0u;
    uint32_t body_built_count = 0u;
    uint32_t cursor_after_body;
    uint32_t index;
    uint32_t output_count = 0u;
    uint16_t half;
    uint8_t byte;
    bool body_enabled;
    bool shadow_enabled = false;
    XgWorldActorSpritesResult actor_result;
    XgWorldActorSpritesNativeResult result;

    if (out_preparation == NULL)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_ARGUMENT;
    memset(out_preparation, 0, sizeof(*out_preparation));
    if (request == NULL || reader == NULL || records == NULL ||
        reader->read_u8 == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || reader->authorize_source_range == NULL ||
        request->authentication_generation == 0u ||
        request->resident_entry_pc != XG_WORLD_ACTOR_SPRITES_RESIDENT_ENTRY ||
        request->prepared_seam_pc != XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM ||
        request->resident_caller_return !=
            XG_WORLD_ACTOR_SPRITES_RESIDENT_CALLER_RETURN ||
        request->projection_distance == 0u ||
        !request->resident_context_authenticated ||
        !request->projection_state_authenticated ||
        !raster_is_valid(&request->raster))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_ARGUMENT;
    if (!reader->authenticated ||
        reader->authentication_generation != request->authentication_generation)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_UNAUTHENTICATED;
    if (!authorize_range(reader, XG_WORLD_ACTOR_SPRITES_SOURCE_ACTOR,
                         request->actor_address, ACTOR_SOURCE_SIZE, 4u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;

    access.reader = reader;
#define READ_U8(address, output)                                                \
    do {                                                                        \
        result = read_u8(&access, (address), (output));                          \
        if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;           \
    } while (0)
#define READ_U16(address, output)                                               \
    do {                                                                        \
        result = read_u16(&access, (address), (output));                         \
        if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;           \
    } while (0)
#define READ_U32(address, output)                                               \
    do {                                                                        \
        result = read_u32(&access, (address), (output));                         \
        if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;           \
    } while (0)

    READ_U32(request->actor_address + 0x20u, &preparation.data_address);
    if (!authorize_range(reader, XG_WORLD_ACTOR_SPRITES_SOURCE_DATA,
                         preparation.data_address, ACTOR_DATA_SIZE, 4u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    READ_U32(request->actor_address + 0x3cu, &actor_flags);
    READ_U32(request->actor_address + 0x40u, &actor_config);
    READ_U32(request->actor_address + 0xacu, &actor_aux_flags);
    READ_U16(request->actor_address + 0x2cu, &half);
    actor.scale = low_s16(half);
    READ_U16(request->actor_address + 2u, &half);
    actor.position[0] = (int32_t)low_s16(half) * 4096;
    READ_U16(request->actor_address + 6u, &half);
    actor.position[1] = (int32_t)low_s16(half) * 4096;
    READ_U16(request->actor_address + 10u, &half);
    actor.position[2] = -(int32_t)low_s16(half) * 4096;
    READ_U16(request->actor_address + 0x84u, &half);
    actor.shadow_y = low_s16(half);

    actor.scale_shift = (uint8_t)((actor_config >> 8u) & 0x1fu);
    actor.descriptor_count = (actor_config >> 2u) & 0x3fu;
    actor.hidden_part_mask = (uint8_t)(actor_flags >> 8u);
    actor.origin_mirror_x = ((actor_aux_flags >> 2u) & 1u) != 0u;
    actor.flip_x = ((actor_flags >> 3u) & 1u) != 0u;
    actor.flip_y = ((actor_flags >> 4u) & 1u) != 0u;
    actor.shadow_enabled = ((actor_flags >> 2u) & 1u) != 0u;
    actor.per_part_ordering = ((actor_flags >> 27u) & 1u) != 0u;
    actor.active = true;
    actor.sprite_present = true;

    result = capture_matrix(&access, preparation.data_address + 0x0cu,
                            &actor.resolved_sprite_matrix);
    if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
    READ_U32(preparation.data_address + 0x30u,
             &preparation.descriptor_address);
    READ_U32(preparation.data_address + 0x34u, &part_address);
    READ_U8(preparation.data_address + 0x3cu, &byte);
    actor.origin_x = (int8_t)byte;
    READ_U8(preparation.data_address + 0x3du, &byte);
    actor.origin_y = (int8_t)byte;

    if (actor.descriptor_count != 0u) {
        const uint32_t descriptor_bytes =
            actor.descriptor_count * ACTOR_DESCRIPTOR_STRIDE;

        if (!authorize_range(reader,
                             XG_WORLD_ACTOR_SPRITES_SOURCE_DESCRIPTORS,
                             preparation.descriptor_address,
                             descriptor_bytes, 4u))
            return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    }
    actor.descriptors = descriptors;
    for (index = 0u; index < actor.descriptor_count; ++index) {
        XgWorldActorSpriteDescriptor *descriptor = &descriptors[index];
        const uint32_t address = preparation.descriptor_address +
            index * ACTOR_DESCRIPTOR_STRIDE;
        uint32_t descriptor_flags;

        READ_U16(address, &half);
        descriptor->x = low_s16(half);
        READ_U16(address + 2u, &half);
        descriptor->y = low_s16(half);
        READ_U8(address + 4u, &descriptor->u);
        READ_U8(address + 5u, &descriptor->v);
        READ_U8(address + 6u, &descriptor->width);
        READ_U8(address + 7u, &descriptor->height);
        READ_U8(address + 8u, &byte);
        descriptor->extent_x_adjust = (int8_t)byte;
        READ_U8(address + 9u, &byte);
        descriptor->extent_y_adjust = (int8_t)byte;
        READ_U16(address + 0x0au, &descriptor->tpage);
        READ_U16(address + 0x0cu, &descriptor->clut);
        READ_U32(address + 0x10u, &descriptor->material_word);
        READ_U32(address + 0x14u, &descriptor_flags);
        descriptor->part = (uint8_t)(descriptor_flags & 7u);
        descriptor->reverse_x = ((descriptor_flags >> 4u) & 1u) != 0u;
        descriptor->reverse_y = ((descriptor_flags >> 5u) & 1u) != 0u;
        used_parts |= UINT32_C(1) << descriptor->part;
    }

    actor.has_part_transforms = part_address != 0u;
    if (actor.has_part_transforms && used_parts != 0u &&
        !ram_range_is_valid(
            part_address,
            XG_WORLD_ACTOR_SPRITES_PART_CAPACITY * ACTOR_PART_STRIDE, 2u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    if (actor.has_part_transforms && used_parts != 0u) {
        for (index = 0u; index < XG_WORLD_ACTOR_SPRITES_PART_CAPACITY;
             ++index) {
            if ((used_parts & (UINT32_C(1) << index)) == 0u) continue;
            result = capture_part(&access,
                                  part_address + index * ACTOR_PART_STRIDE,
                                  &actor, &actor.parts[index]);
            if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
        }
    }

    if (!authorize_range(reader, XG_WORLD_ACTOR_SPRITES_SOURCE_CAMERA,
                         ACTOR_CAMERA_MATRIX, 0x20u, 4u) ||
        !authorize_range(reader,
                         XG_WORLD_ACTOR_SPRITES_SOURCE_SCRATCH_TEMPLATE,
                         XG_WORLD_ACTOR_SPRITES_BODY_SCRATCH, 0x20u, 4u) ||
        !authorize_range(reader,
                         XG_WORLD_ACTOR_SPRITES_SOURCE_SCRATCH_TEMPLATE,
                         XG_WORLD_ACTOR_SPRITES_SHADOW_SCRATCH, 0x20u, 4u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    result = capture_matrix(&access, ACTOR_CAMERA_MATRIX, &camera);
    if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
    result = capture_vertex_template(
        &access, XG_WORLD_ACTOR_SPRITES_BODY_SCRATCH,
        actor.body_vertex_template);
    if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
    result = capture_vertex_template(
        &access, XG_WORLD_ACTOR_SPRITES_SHADOW_SCRATCH,
        actor.shadow_vertex_template);
    if (result != XG_WORLD_ACTOR_SPRITES_NATIVE_OK) return result;
    memcpy(preparation.body_scratch.vertices, actor.body_vertex_template,
           sizeof(preparation.body_scratch.vertices));
    preparation.body_scratch.address = XG_WORLD_ACTOR_SPRITES_BODY_SCRATCH;
    preparation.body_scratch.written = true;
    memcpy(preparation.shadow_scratch.vertices, actor.shadow_vertex_template,
           sizeof(preparation.shadow_scratch.vertices));
    preparation.shadow_scratch.address = XG_WORLD_ACTOR_SPRITES_SHADOW_SCRATCH;
    preparation.shadow_scratch.written = true;

    memcpy(source.camera_projection.rotation, camera.rotation,
           sizeof(camera.rotation));
    memcpy(source.camera_projection.translation, camera.translation,
           sizeof(camera.translation));
    source.camera_projection.screen_offset_x = request->screen_offset_x;
    source.camera_projection.screen_offset_y = request->screen_offset_y;
    source.camera_projection.projection_distance = request->projection_distance;
    source.camera_projection.depth_cue_a = request->depth_cue_a;
    source.camera_projection.depth_cue_b = request->depth_cue_b;
    source.camera_projection.average_z_scale4 = request->average_z_scale4;
    apply_material(&request->raster, &source.material_template);
    source.actors = &actor;
    source.actor_count = 1u;

    actor_result = xg_world_actor_sprites_adapt_actor(&source, 0u, &adapted);
    if (actor_result != XG_WORLD_ACTOR_SPRITES_OK)
        return build_result(actor_result);
    if (!adapted.accepted ||
        memcmp(adapted.body_projection.rotation,
               actor.resolved_sprite_matrix.rotation,
               sizeof(adapted.body_projection.rotation)) != 0 ||
        memcmp(adapted.body_projection.translation,
               actor.resolved_sprite_matrix.translation,
               sizeof(adapted.body_projection.translation)) != 0)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_SOURCE_MISMATCH;

    if (!authorize_range(reader, XG_WORLD_ACTOR_SPRITES_SOURCE_GLOBAL,
                         ACTOR_CONTEXT, 4u, 4u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    READ_U32(ACTOR_CONTEXT, &context);
    if (context > UINT32_MAX - ACTOR_CONTEXT_OT_OFFSET ||
        !authorize_range(reader, XG_WORLD_ACTOR_SPRITES_SOURCE_CONTEXT,
                         context + ACTOR_CONTEXT_OT_OFFSET, 4u, 4u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    READ_U32(context + ACTOR_CONTEXT_OT_OFFSET, &ot_base);
    if (!ram_range_is_valid(
            ot_base, XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT * 4u, 4u) ||
        adapted.ordering_bucket >= XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT ||
        ot_base > UINT32_MAX - adapted.ordering_bucket * 4u ||
        request->ordering_table_address !=
            ot_base + adapted.ordering_bucket * 4u)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;

    if (!authorize_range(reader, XG_WORLD_ACTOR_SPRITES_SOURCE_GLOBAL,
                         XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR, 4u, 4u) ||
        !authorize_range(reader, XG_WORLD_ACTOR_SPRITES_SOURCE_GLOBAL,
                         XG_WORLD_ACTOR_SPRITES_PACKET_LIMIT, 4u, 4u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    READ_U32(XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR,
             &preparation.initial_packet_cursor);
    READ_U32(XG_WORLD_ACTOR_SPRITES_PACKET_LIMIT, &preparation.packet_limit);
    if (!ram_pointer_is_valid(preparation.initial_packet_cursor, 4u) ||
        !ram_pointer_is_valid(preparation.packet_limit, 4u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;

    actor_result = xg_world_actor_sprites_build(
        &source, records, record_capacity, &built_count);
    if (actor_result != XG_WORLD_ACTOR_SPRITES_OK)
        return build_result(actor_result);
    for (index = 0u; index < built_count; ++index)
        body_built_count +=
            records[index].family == XG_WORLD_ACTOR_SPRITE_BODY;

    if (!family_capacity(preparation.initial_packet_cursor,
                         actor.descriptor_count, preparation.packet_limit,
                         &body_enabled))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;
    cursor_after_body = preparation.initial_packet_cursor;
    if (body_enabled) {
        if (cursor_after_body > UINT32_MAX -
                body_built_count * XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE)
            return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;
        cursor_after_body +=
            body_built_count * XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE;
    }
    if (actor.shadow_enabled &&
        !family_capacity(cursor_after_body, actor.descriptor_count,
                         preparation.packet_limit, &shadow_enabled))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;

    for (index = 0u; index < built_count; ++index) {
        const bool keep = records[index].family == XG_WORLD_ACTOR_SPRITE_BODY
            ? body_enabled : shadow_enabled;
        XgWorldActorSpriteRecord *record;
        uint32_t ot_address = request->ordering_table_address;

        if (!keep) continue;
        if (output_count != index) records[output_count] = records[index];
        record = &records[output_count];
        if (preparation.initial_packet_cursor > UINT32_MAX -
                output_count * XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE)
            return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;
        record->packet_address = preparation.initial_packet_cursor +
            output_count * XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE;
        if (record->family == XG_WORLD_ACTOR_SPRITE_BODY &&
            actor.per_part_ordering) {
            const uint32_t delta = record->part * 4u;

            if (ot_address < delta)
                return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;
            ot_address -= delta;
        }
        if (!ram_range_is_valid(record->packet_address,
                                XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE, 4u) ||
            !ram_range_is_valid(ot_address, 4u, 4u) ||
            ot_address < ot_base ||
            ot_address > ot_base +
                (XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT - 1u) * 4u)
            return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;
        record->ordering_table_address = ot_address;

        if (record->family == XG_WORLD_ACTOR_SPRITE_BODY) {
            memcpy(preparation.body_scratch.vertices,
                   record->source_vertices,
                   sizeof(preparation.body_scratch.vertices));
            preparation.body_scratch.component_write_mask =
                XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_X |
                XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_Y;
            ++preparation.body_record_count;
        } else {
            memcpy(preparation.shadow_scratch.vertices,
                   record->source_vertices,
                   sizeof(preparation.shadow_scratch.vertices));
            preparation.shadow_scratch.component_write_mask =
                XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_X |
                XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_Z;
            ++preparation.shadow_record_count;
        }
        ++output_count;
    }

#undef READ_U8
#undef READ_U16
#undef READ_U32

    if (preparation.initial_packet_cursor > UINT32_MAX -
            output_count * XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;
    preparation.final_packet_cursor = preparation.initial_packet_cursor +
        output_count * XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE;
    if (ram_ranges_overlap(
            preparation.initial_packet_cursor,
            output_count * XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE,
            ot_base, XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT * 4u))
        return XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT;
    preparation.packet_cursor_written = output_count != 0u;
    preparation.record_count = output_count;
    preparation.actor_address = request->actor_address;
    preparation.authentication_generation = request->authentication_generation;
    preparation.continuation_pc = XG_WORLD_ACTOR_SPRITES_CONTINUATION;
    preparation.authenticated_read_count = access.read_count;
    preparation.authenticated_read_bytes = access.read_bytes;
    if (preparation.authenticated_read_count >
            XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_READS ||
        preparation.authenticated_read_bytes >
            XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_BYTES)
        return XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE;
    preparation.authenticated = true;
    preparation.sealed = true;
    *out_preparation = preparation;
    return XG_WORLD_ACTOR_SPRITES_NATIVE_OK;
}
