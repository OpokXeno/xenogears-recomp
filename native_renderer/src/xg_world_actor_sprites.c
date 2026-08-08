#include "xg_world_actor_sprites.h"

#include "xg_render_quad_builder.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static int32_t i32_from_u32(uint32_t value) {
    if (value <= INT32_MAX) return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int32_t add_i32_wrap(int32_t left, int32_t right) {
    return i32_from_u32((uint32_t)left + (uint32_t)right);
}

static int32_t negate_i32_wrap(int32_t value) {
    return i32_from_u32(0u - (uint32_t)value);
}

static int32_t shift_left_i32_wrap(int32_t value, uint8_t shift) {
    return i32_from_u32((uint32_t)value << (shift & 31u));
}

static int32_t shift_right_floor_i32(int32_t value, unsigned shift) {
    int64_t magnitude;

    if (shift == 0u || value >= 0) return value / (INT32_C(1) << shift);
    magnitude = -(int64_t)value;
    return (int32_t)(-((magnitude + ((INT64_C(1) << shift) - 1)) >> shift));
}

static int16_t i16_from_i32(int32_t value) {
    const uint16_t low = (uint16_t)(uint32_t)value;

    if (low <= INT16_MAX) return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static int16_t actor_component(int32_t value) {
    return i16_from_i32(i32_from_u32(((uint32_t)value << 4u) >> 16u));
}

static int32_t wrap_world_component(int32_t position, int32_t origin,
                                    int32_t wrap) {
    int32_t relative = i32_from_u32((uint32_t)position - (uint32_t)origin);
    const int32_t span = i32_from_u32((uint32_t)wrap << 23u);

    if (relative < -INT32_C(0x04000000))
        relative = add_i32_wrap(relative, span);
    else if (relative > INT32_C(0x04000000))
        relative = add_i32_wrap(relative, negate_i32_wrap(span));
    return relative;
}

static int32_t scale_offset(int8_t offset, uint8_t shift, int16_t scale,
                            bool reverse) {
    int32_t shifted = shift_left_i32_wrap(offset, shift);
    int32_t product;

    if (reverse) shifted = negate_i32_wrap(shifted);
    product = i32_from_u32((uint32_t)shifted * (uint32_t)(int32_t)scale);
    if (product < 0) product = add_i32_wrap(product, 0xfff);
    return shift_right_floor_i32(product, 12u);
}

static void matrix_from_projection(const XgHost3dProjection *projection,
                                   XgHost3dMatrix *matrix) {
    memset(matrix, 0, sizeof(*matrix));
    memcpy(matrix->rotation, projection->rotation, sizeof(matrix->rotation));
    memcpy(matrix->translation, projection->translation,
           sizeof(matrix->translation));
}

static void projection_from_matrix(const XgHost3dProjection *template_value,
                                   const XgHost3dMatrix *matrix,
                                   XgHost3dProjection *projection) {
    *projection = *template_value;
    memcpy(projection->rotation, matrix->rotation,
           sizeof(projection->rotation));
    memcpy(projection->translation, matrix->translation,
           sizeof(projection->translation));
}

static bool raster_is_valid(const XgRenderIrMaterialState *material) {
    return material->draw_area_left <= material->draw_area_right &&
           material->draw_area_top <= material->draw_area_bottom &&
           material->draw_area_right <= 1023u &&
           material->draw_area_bottom <= 1023u &&
           material->draw_offset_x >= -1024 &&
           material->draw_offset_x <= 1023 &&
           material->draw_offset_y >= -1024 &&
           material->draw_offset_y <= 1023 &&
           material->texture_window_mask_x <= 31u &&
           material->texture_window_mask_y <= 31u &&
           material->texture_window_offset_x <= 31u &&
           material->texture_window_offset_y <= 31u;
}

static bool material_for_descriptor(
    const XgRenderIrMaterialState *template_value,
    const XgWorldActorSpriteDescriptor *descriptor,
    bool shadow,
    XgRenderIrMaterialState *material) {
    const uint8_t command = shadow ? 0x2cu
        : (uint8_t)(descriptor->material_word >> 24u);
    const uint16_t tpage = descriptor->tpage;
    const uint16_t encoded_depth = (tpage >> 7u) & 3u;

    if ((!shadow && (command & 0xfcu) != 0x2cu) || tpage > 0x1ffu ||
        encoded_depth == 3u)
        return false;
    *material = *template_value;
    material->tpage = tpage;
    material->texture_page_x = tpage & 0x0fu;
    material->texture_page_y = (tpage >> 4u) & 1u;
    material->texture_depth = (XgRenderIrTextureDepth)encoded_depth;
    material->blend_mode = (XgRenderIrBlendMode)((tpage >> 5u) & 3u);
    material->clut_x = (descriptor->clut & 0x3fu) << 4u;
    material->clut_y = descriptor->clut >> 6u;
    material->shading = XG_RENDER_IR_SHADING_FLAT;
    material->textured = true;
    material->raw_texture = (command & 1u) != 0u;
    material->semi_transparent = (command & 2u) != 0u;
    return true;
}

static bool validate_source(const XgWorldActorSpritesValueSource *source) {
    uint32_t actor_index;

    if (source == NULL || source->actors == NULL || source->actor_count == 0u ||
        source->actor_count > XG_WORLD_ACTOR_SPRITES_ACTOR_CAPACITY ||
        source->camera_projection.projection_distance == 0u ||
        !raster_is_valid(&source->material_template))
        return false;
    for (actor_index = 0u; actor_index < source->actor_count; ++actor_index) {
        const XgWorldActorSpriteActorSource *actor =
            &source->actors[actor_index];
        uint32_t descriptor_index;

        if (actor->scale_shift > 31u ||
            actor->descriptor_count >
                XG_WORLD_ACTOR_SPRITES_DESCRIPTOR_CAPACITY ||
            (actor->descriptor_count != 0u && actor->descriptors == NULL))
            return false;
        for (descriptor_index = 0u;
             descriptor_index < actor->descriptor_count; ++descriptor_index) {
            const XgWorldActorSpriteDescriptor *descriptor =
                &actor->descriptors[descriptor_index];
            XgRenderIrMaterialState material;

            if (descriptor->part >= XG_WORLD_ACTOR_SPRITES_PART_CAPACITY ||
                !material_for_descriptor(&source->material_template,
                                         descriptor, false, &material) ||
                !material_for_descriptor(&source->material_template,
                                         descriptor, true, &material))
                return false;
        }
    }
    return true;
}

static XgWorldActorSpritesResult adapt_actor_unchecked(
    const XgWorldActorSpritesValueSource *source,
    uint32_t actor_index,
    XgWorldActorSpritesAdaptedActor *out_actor) {
    const XgWorldActorSpriteActorSource *actor = &source->actors[actor_index];
    XgWorldActorSpritesAdaptedActor adapted = { 0 };
    XgHost3dMatrix camera;
    XgHost3dMatrix body;
    XgHost3dMatrix shadow;
    XgHost3dLongVector position;
    XgHost3dLongVector transformed;
    XgHost3dLongVector shadow_scale;
    XgHost3dProjectedVertex depth_vertex;
    uint32_t flags;
    int32_t relative_x;
    int32_t relative_z;

    if (!actor->active || !actor->sprite_present) {
        *out_actor = adapted;
        return XG_WORLD_ACTOR_SPRITES_OK;
    }
    relative_x = wrap_world_component(actor->position[0],
                                      source->camera_origin_x, source->wrap_x);
    relative_z = wrap_world_component(actor->position[2],
                                      source->camera_origin_z, source->wrap_z);
    adapted.actor_position = (XgHost3dVector){
        actor_component(relative_x), actor_component(actor->position[1]),
        actor_component(negate_i32_wrap(relative_z)), 0u,
    };
    if (!xg_host_3d_rtps(&source->camera_projection,
                         &adapted.actor_position, &depth_vertex, &flags))
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    adapted.depth = depth_vertex.z;
    adapted.depth_projection_flags = flags;
    adapted.ordering_bucket = adapted.depth >> 4u;
    if (adapted.depth >= 0xb00u) {
        *out_actor = adapted;
        return XG_WORLD_ACTOR_SPRITES_OK;
    }

    matrix_from_projection(&source->camera_projection, &camera);
    position = (XgHost3dLongVector){
        adapted.actor_position.x, adapted.actor_position.y,
        adapted.actor_position.z,
    };
    if (!xg_host_3d_rt(&camera, &position, &transformed, &flags))
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    body = actor->resolved_sprite_matrix;
    body.translation[0] = add_i32_wrap(
        transformed.x,
        scale_offset(actor->origin_x, actor->scale_shift, actor->scale,
                     actor->origin_mirror_x));
    body.translation[1] = add_i32_wrap(
        transformed.y,
        scale_offset(actor->origin_y, actor->scale_shift, actor->scale,
                     false));
    body.translation[2] = transformed.z;
    projection_from_matrix(&source->camera_projection, &body,
                           &adapted.body_projection);

    shadow = camera;
    shadow_scale = (XgHost3dLongVector){
        actor->scale,
        actor->scale >= 0 ? actor->scale / 2 : -(-(int32_t)actor->scale / 2),
        0,
    };
    if (!xg_host_3d_scale_matrix(&shadow, &shadow_scale))
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    position.y = actor->shadow_y;
    if (!xg_host_3d_rt(&camera, &position, &transformed, &flags))
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    shadow.translation[0] = transformed.x;
    shadow.translation[1] = transformed.y;
    shadow.translation[2] = transformed.z;
    projection_from_matrix(&source->camera_projection, &shadow,
                           &adapted.shadow_projection);
    adapted.accepted = true;
    *out_actor = adapted;
    return XG_WORLD_ACTOR_SPRITES_OK;
}

XgWorldActorSpritesResult xg_world_actor_sprites_adapt_actor(
    const XgWorldActorSpritesValueSource *source,
    uint32_t actor_index,
    XgWorldActorSpritesAdaptedActor *out_actor) {
    if (source == NULL || out_actor == NULL)
        return XG_WORLD_ACTOR_SPRITES_INVALID_ARGUMENT;
    memset(out_actor, 0, sizeof(*out_actor));
    if (!validate_source(source) || actor_index >= source->actor_count)
        return XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE;
    return adapt_actor_unchecked(source, actor_index, out_actor);
}

static int32_t descriptor_extent(uint8_t size, int8_t adjustment,
                                 uint8_t shift) {
    const int16_t extent = (int16_t)((int32_t)size + adjustment);

    return shift_left_i32_wrap(extent, shift);
}

static void descriptor_vertices(
    const XgWorldActorSpriteActorSource *actor,
    const XgWorldActorSpriteDescriptor *descriptor,
    bool shadow,
    XgHost3dVector vertices[XG_HOST_3D_VERTEX_COUNT]) {
    int32_t x0 = shift_left_i32_wrap(descriptor->x, actor->scale_shift);
    int32_t y0 = shift_left_i32_wrap(descriptor->y, actor->scale_shift);
    int32_t width = descriptor_extent(descriptor->width,
                                      descriptor->extent_x_adjust,
                                      actor->scale_shift);
    int32_t height = descriptor_extent(descriptor->height,
                                       descriptor->extent_y_adjust,
                                       actor->scale_shift);
    int32_t x1;
    int32_t y1;

    memcpy(vertices,
           shadow ? actor->shadow_vertex_template
                  : actor->body_vertex_template,
           sizeof(actor->body_vertex_template));
    if (actor->flip_x) {
        x0 = negate_i32_wrap(x0);
        width = negate_i32_wrap(width);
    }
    x1 = add_i32_wrap(x0, width);
    if (shadow) {
        if (actor->flip_y != descriptor->reverse_y) {
            y0 = negate_i32_wrap(y0);
            height = negate_i32_wrap(height);
        }
    } else if (actor->flip_y) {
        y0 = negate_i32_wrap(y0);
        height = negate_i32_wrap(height);
    }
    y1 = add_i32_wrap(y0, height);

    if (descriptor->reverse_x) {
        vertices[0].x = vertices[3].x = i16_from_i32(x1);
        vertices[1].x = vertices[2].x = i16_from_i32(x0);
    } else {
        vertices[0].x = vertices[3].x = i16_from_i32(x0);
        vertices[1].x = vertices[2].x = i16_from_i32(x1);
    }
    if (shadow) {
        if (descriptor->reverse_y) {
            vertices[0].z = vertices[1].z = i16_from_i32(y1);
            vertices[2].z = vertices[3].z = i16_from_i32(y0);
        } else {
            vertices[0].z = vertices[1].z = i16_from_i32(y0);
            vertices[2].z = vertices[3].z = i16_from_i32(y1);
        }
    } else if (descriptor->reverse_y) {
        vertices[0].y = vertices[1].y = i16_from_i32(y1);
        vertices[2].y = vertices[3].y = i16_from_i32(y0);
    } else {
        vertices[0].y = vertices[1].y = i16_from_i32(y0);
        vertices[2].y = vertices[3].y = i16_from_i32(y1);
    }
}

static bool descriptor_is_visible(
    const XgWorldActorSpriteActorSource *actor,
    const XgWorldActorSpriteDescriptor *descriptor) {
    return (actor->hidden_part_mask & (uint8_t)(1u << descriptor->part)) == 0u;
}

static XgWorldActorSpritesResult body_projection_for_descriptor(
    const XgWorldActorSpriteActorSource *actor,
    const XgWorldActorSpriteDescriptor *descriptor,
    const XgWorldActorSpritesAdaptedActor *adapted,
    XgHost3dProjection *projection) {
    XgHost3dMatrix base = { 0 };
    XgHost3dMatrix part = { 0 };
    XgHost3dMatrix composed;

    *projection = adapted->body_projection;
    if (!actor->has_part_transforms ||
        !actor->parts[descriptor->part].enabled)
        return XG_WORLD_ACTOR_SPRITES_OK;
    memcpy(base.rotation, adapted->body_projection.rotation,
           sizeof(base.rotation));
    memcpy(part.rotation, actor->parts[descriptor->part].rotation.rotation,
           sizeof(part.rotation));
    if (!xg_host_3d_comp_matrix(&base, &part, &composed))
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    memcpy(projection->rotation, composed.rotation,
           sizeof(projection->rotation));
    projection->translation[0] = add_i32_wrap(
        projection->translation[0],
        actor->parts[descriptor->part].offset_x);
    projection->translation[1] = add_i32_wrap(
        projection->translation[1],
        actor->parts[descriptor->part].offset_y);
    return XG_WORLD_ACTOR_SPRITES_OK;
}

static void fill_record_identity(
    XgWorldActorSpriteRecord *record,
    uint32_t actor_index,
    uint32_t descriptor_index,
    const XgWorldActorSpriteDescriptor *descriptor,
    XgWorldActorSpriteFamily family,
    uint32_t ordering_bucket) {
    record->actor_index = actor_index;
    record->descriptor_index = descriptor_index;
    record->ordering_bucket = ordering_bucket;
    record->material_word = family == XG_WORLD_ACTOR_SPRITE_BODY
        ? descriptor->material_word : UINT32_C(0x2c000000);
    record->tpage = descriptor->tpage;
    record->clut = descriptor->clut;
    record->part = descriptor->part;
    record->family = family;
}

static uint32_t packed_xy(const XgHost3dProjectedVertex *vertex) {
    return (uint16_t)vertex->x |
        ((uint32_t)(uint16_t)vertex->y << 16u);
}

static void fill_packet_payload(XgWorldActorSpriteRecord *record) {
    static const uint32_t write_masks
        [XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT] = {
            UINT32_MAX, UINT32_MAX, UINT32_MAX,
            UINT32_MAX, UINT32_MAX, UINT32_MAX,
            UINT32_C(0x0000ffff), UINT32_MAX,
            UINT32_C(0x0000ffff),
        };

    record->packet_payload_words[0] = record->material_word;
    record->packet_payload_words[1] = packed_xy(&record->sprite.vertices[0]);
    record->packet_payload_words[2] = record->uv[0][0] |
        ((uint32_t)record->uv[0][1] << 8u) |
        ((uint32_t)record->clut << 16u);
    record->packet_payload_words[3] = packed_xy(&record->sprite.vertices[1]);
    record->packet_payload_words[4] = record->uv[1][0] |
        ((uint32_t)record->uv[1][1] << 8u) |
        ((uint32_t)record->tpage << 16u);
    record->packet_payload_words[5] = packed_xy(&record->sprite.vertices[2]);
    record->packet_payload_words[6] = record->uv[2][0] |
        ((uint32_t)record->uv[2][1] << 8u);
    record->packet_payload_words[7] = packed_xy(&record->sprite.vertices[3]);
    record->packet_payload_words[8] = record->uv[3][0] |
        ((uint32_t)record->uv[3][1] << 8u);
    memcpy(record->packet_payload_write_masks, write_masks,
           sizeof(write_masks));
    record->tag_payload_word_count =
        XG_WORLD_ACTOR_SPRITE_TAG_PAYLOAD_WORD_COUNT;
}

static XgWorldActorSpritesResult build_body_record(
    const XgWorldActorSpritesValueSource *source,
    const XgWorldActorSpriteActorSource *actor,
    const XgWorldActorSpriteDescriptor *descriptor,
    const XgWorldActorSpritesAdaptedActor *adapted,
    uint32_t actor_index,
    uint32_t descriptor_index,
    XgWorldActorSpriteRecord *record) {
    XgSpriteFt4Source sprite_source = { 0 };
    XgSpriteFt4Record provisional;
    XgWorldActorSpritesResult result;
    uint32_t bucket = adapted->ordering_bucket;

    if (actor->per_part_ordering) {
        if (bucket < descriptor->part)
            return XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE;
        bucket -= descriptor->part;
    }
    descriptor_vertices(actor, descriptor, false, sprite_source.vertices);
    result = body_projection_for_descriptor(actor, descriptor, adapted,
                                            &sprite_source.projection);
    if (result != XG_WORLD_ACTOR_SPRITES_OK) return result;
    if (!material_for_descriptor(&source->material_template, descriptor, false,
                                 &sprite_source.material))
        return XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE;
    sprite_source.color[0] = (uint8_t)descriptor->material_word;
    sprite_source.color[1] = (uint8_t)(descriptor->material_word >> 8u);
    sprite_source.color[2] = (uint8_t)(descriptor->material_word >> 16u);
    sprite_source.packet_vertex_for_projection[0] = 0u;
    sprite_source.packet_vertex_for_projection[1] = 1u;
    sprite_source.packet_vertex_for_projection[2] = 3u;
    sprite_source.packet_vertex_for_projection[3] = 2u;
    if (xg_sprite_ft4_build(&sprite_source, &provisional) !=
        XG_SPRITE_FT4_OK)
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    if (xg_sprite_ft4_map_uv(
            descriptor->u, descriptor->v, descriptor->width,
            descriptor->height,
            provisional.vertices[3].x < provisional.vertices[0].x,
            sprite_source.uv) != XG_SPRITE_FT4_OK)
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    if (xg_sprite_ft4_build(&sprite_source, &record->sprite) !=
        XG_SPRITE_FT4_OK)
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    memcpy(record->source_vertices, sprite_source.vertices,
           sizeof(record->source_vertices));
    memcpy(record->uv, sprite_source.uv, sizeof(record->uv));
    fill_record_identity(record, actor_index, descriptor_index, descriptor,
                         XG_WORLD_ACTOR_SPRITE_BODY, bucket);
    fill_packet_payload(record);
    return XG_WORLD_ACTOR_SPRITES_OK;
}

static int16_t average_s16(int16_t left, int16_t right) {
    const int16_t sum = i16_from_i32((int32_t)left + right);

    if (sum >= 0) return (int16_t)(sum / 2);
    return (int16_t)(-(-(int32_t)sum / 2));
}

static int32_t average_i32(int32_t left, int32_t right) {
    return (int32_t)(((int64_t)left + right) / 2);
}

static XgWorldActorSpritesResult build_shadow_record(
    const XgWorldActorSpritesValueSource *source,
    const XgWorldActorSpriteActorSource *actor,
    const XgWorldActorSpriteDescriptor *descriptor,
    const XgWorldActorSpritesAdaptedActor *adapted,
    uint32_t actor_index,
    uint32_t descriptor_index,
    XgWorldActorSpriteRecord *record) {
    static const uint8_t packet_vertex_for_projection[4] = { 0u, 1u, 3u, 2u };
    XgHost3dRotAverage4Input input = { 0 };
    XgHost3dRotAverage4Output projected;
    XgRenderQuadSource quad = { 0 };
    uint8_t uv[4][2];
    uint32_t vertex;
    int16_t top_y;
    int16_t bottom_y;

    descriptor_vertices(actor, descriptor, true, input.vertices);
    input.projection = adapted->shadow_projection;
    if (!xg_host_3d_rot_average4(&input, &projected))
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex)
        record->sprite.vertices[packet_vertex_for_projection[vertex]] =
            projected.vertices[vertex];
    top_y = average_s16(record->sprite.vertices[0].y,
                        record->sprite.vertices[1].y);
    bottom_y = average_s16(record->sprite.vertices[2].y,
                           record->sprite.vertices[3].y);
    record->sprite.vertices[0].y = record->sprite.vertices[1].y = top_y;
    record->sprite.vertices[2].y = record->sprite.vertices[3].y = bottom_y;
    if (record->sprite.vertices[0].native_view_position &&
        record->sprite.vertices[1].native_view_position) {
        const int32_t native_top_y = average_i32(
            record->sprite.vertices[0].native_view_y_16_16,
            record->sprite.vertices[1].native_view_y_16_16);

        record->sprite.vertices[0].native_view_y_16_16 = native_top_y;
        record->sprite.vertices[1].native_view_y_16_16 = native_top_y;
    } else {
        record->sprite.vertices[0].native_view_position = 0u;
        record->sprite.vertices[1].native_view_position = 0u;
    }
    if (record->sprite.vertices[2].native_view_position &&
        record->sprite.vertices[3].native_view_position) {
        const int32_t native_bottom_y = average_i32(
            record->sprite.vertices[2].native_view_y_16_16,
            record->sprite.vertices[3].native_view_y_16_16);

        record->sprite.vertices[2].native_view_y_16_16 = native_bottom_y;
        record->sprite.vertices[3].native_view_y_16_16 = native_bottom_y;
    } else {
        record->sprite.vertices[2].native_view_position = 0u;
        record->sprite.vertices[3].native_view_position = 0u;
    }
    record->sprite.depth_cue = projected.depth_cue;
    record->sprite.fourth_depth = projected.vertices[3].z >> 2u;
    record->sprite.projection_flags = projected.projection_flags;

    uv[0][0] = uv[2][0] = descriptor->u;
    uv[1][0] = uv[3][0] =
        (uint8_t)(descriptor->u + descriptor->width - 1u);
    uv[0][1] = uv[1][1] = descriptor->v;
    uv[2][1] = uv[3][1] =
        (uint8_t)(descriptor->v + descriptor->height - 1u);
    if (!material_for_descriptor(&source->material_template, descriptor, true,
                                  &quad.material))
        return XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        quad.vertices[vertex] = (XgRenderQuadSourceVertex){
            .u = uv[vertex][0],
            .v = uv[vertex][1],
        };
        xg_render_quad_set_projected_position(
            &quad.vertices[vertex], &record->sprite.vertices[vertex]);
    }
    if (xg_render_quad_build_primitive(&quad, &record->sprite.primitive) !=
        XG_RENDER_QUAD_BUILDER_OK)
        return XG_WORLD_ACTOR_SPRITES_BUILD_FAILED;
    memcpy(record->source_vertices, input.vertices,
           sizeof(record->source_vertices));
    memcpy(record->uv, uv, sizeof(record->uv));
    fill_record_identity(record, actor_index, descriptor_index, descriptor,
                         XG_WORLD_ACTOR_SPRITE_SHADOW,
                         adapted->ordering_bucket);
    fill_packet_payload(record);
    return XG_WORLD_ACTOR_SPRITES_OK;
}

XgWorldActorSpritesResult xg_world_actor_sprites_build(
    const XgWorldActorSpritesValueSource *source,
    XgWorldActorSpriteRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count) {
    XgWorldActorSpritesAdaptedActor
        adapted[XG_WORLD_ACTOR_SPRITES_ACTOR_CAPACITY];
    uint32_t clear_count = record_capacity;
    uint32_t required_count = 0u;
    uint32_t record_count = 0u;
    uint32_t actor_index;

    if (out_record_count == NULL) return XG_WORLD_ACTOR_SPRITES_INVALID_ARGUMENT;
    *out_record_count = 0u;
    if (clear_count > XG_WORLD_ACTOR_SPRITES_RECORD_CAPACITY)
        clear_count = XG_WORLD_ACTOR_SPRITES_RECORD_CAPACITY;
    if (records != NULL && clear_count != 0u)
        memset(records, 0, sizeof(*records) * clear_count);
    if (source == NULL || records == NULL)
        return XG_WORLD_ACTOR_SPRITES_INVALID_ARGUMENT;
    if (!validate_source(source))
        return XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE;

    memset(adapted, 0, sizeof(adapted));
    for (actor_index = 0u; actor_index < source->actor_count; ++actor_index) {
        const XgWorldActorSpriteActorSource *actor =
            &source->actors[actor_index];
        uint32_t visible_count = 0u;
        uint32_t descriptor_index;
        XgWorldActorSpritesResult result = adapt_actor_unchecked(
            source, actor_index, &adapted[actor_index]);

        if (result != XG_WORLD_ACTOR_SPRITES_OK) return result;
        if (!adapted[actor_index].accepted) continue;
        for (descriptor_index = 0u;
             descriptor_index < actor->descriptor_count; ++descriptor_index) {
            const XgWorldActorSpriteDescriptor *descriptor =
                &actor->descriptors[descriptor_index];

            if (!descriptor_is_visible(actor, descriptor)) continue;
            if (actor->per_part_ordering &&
                adapted[actor_index].ordering_bucket < descriptor->part)
                return XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE;
            ++visible_count;
        }
        required_count += visible_count;
        if (actor->shadow_enabled) required_count += visible_count;
    }
    if (required_count > record_capacity)
        return XG_WORLD_ACTOR_SPRITES_CAPACITY_EXCEEDED;

    for (actor_index = 0u; actor_index < source->actor_count; ++actor_index) {
        const XgWorldActorSpriteActorSource *actor =
            &source->actors[actor_index];
        uint32_t descriptor_index;

        if (!adapted[actor_index].accepted) continue;
        for (descriptor_index = 0u;
             descriptor_index < actor->descriptor_count; ++descriptor_index) {
            XgWorldActorSpritesResult result;
            const XgWorldActorSpriteDescriptor *descriptor =
                &actor->descriptors[descriptor_index];

            if (!descriptor_is_visible(actor, descriptor)) continue;
            result = build_body_record(source, actor, descriptor,
                                       &adapted[actor_index], actor_index,
                                       descriptor_index,
                                       &records[record_count]);
            if (result != XG_WORLD_ACTOR_SPRITES_OK) return result;
            ++record_count;
        }
        if (!actor->shadow_enabled) continue;
        for (descriptor_index = 0u;
             descriptor_index < actor->descriptor_count; ++descriptor_index) {
            XgWorldActorSpritesResult result;
            const XgWorldActorSpriteDescriptor *descriptor =
                &actor->descriptors[descriptor_index];

            if (!descriptor_is_visible(actor, descriptor)) continue;
            result = build_shadow_record(source, actor, descriptor,
                                         &adapted[actor_index], actor_index,
                                         descriptor_index,
                                         &records[record_count]);
            if (result != XG_WORLD_ACTOR_SPRITES_OK) return result;
            ++record_count;
        }
    }
    *out_record_count = record_count;
    return XG_WORLD_ACTOR_SPRITES_OK;
}
