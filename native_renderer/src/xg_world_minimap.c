#include "xg_world_minimap.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static const uint32_t TRIANGLE_PACKET_BASE = UINT32_C(0x8009c664);
static const uint32_t TRIANGLE_BUFFER_STRIDE = 0x70u;
static const uint32_t TRIANGLE_PACKET_STRIDE = 0x1cu;
static const uint32_t MARKER_PACKET_BASE = UINT32_C(0x8009c898);
static const uint32_t MARKER_BUFFER_STRIDE = 0x200u;
static const uint32_t MARKER_PACKET_STRIDE = 0x10u;
static const uint32_t DRAW_MODE_PACKET = UINT32_C(0x8009c5a0);
static const uint32_t PANEL_PACKET_BASE = UINT32_C(0x8009c5c0);
static const uint32_t PANEL_PACKET_STRIDE = 0x28u;

static const XgHost3dVector expected_triangles[4][3] = {
    { { 0, 0, 0, 0u }, { -8, -8, 0, 0u }, { -4, -11, 0, 0u } },
    { { 0, 0, 0, 0u }, { -4, -11, 0, 0u }, { 0, -12, 0, 0u } },
    { { 0, 0, 0, 0u }, { 0, -12, 0, 0u }, { 4, -11, 0, 0u } },
    { { 0, 0, 0, 0u }, { 4, -11, 0, 0u }, { 8, -8, 0, 0u } },
};

static int64_t shift_right_floor(int64_t value, unsigned bits) {
    uint64_t magnitude;

    if (value >= 0) return value >> bits;
    magnitude = (uint64_t)(-(value + 1)) + 1u;
    return -(int64_t)((magnitude + (((uint64_t)1u << bits) - 1u)) >> bits);
}

static int32_t wrap_i32(uint32_t value) {
    if (value <= INT32_MAX) return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int16_t wrap_i16(int32_t value) {
    const uint16_t low = (uint16_t)(uint32_t)value;

    if (low <= INT16_MAX) return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static bool raster_is_valid(const XgWorldMinimapRasterState *raster) {
    return raster->draw_area_left <= raster->draw_area_right &&
           raster->draw_area_top <= raster->draw_area_bottom &&
           raster->draw_area_right <= 1023u &&
           raster->draw_area_bottom <= 1023u &&
           raster->draw_offset_x >= -1024 && raster->draw_offset_x <= 1023 &&
           raster->draw_offset_y >= -1024 && raster->draw_offset_y <= 1023;
}

static void apply_raster(const XgWorldMinimapRasterState *raster,
                         XgRenderIrMaterialState *material) {
    material->draw_area_left = raster->draw_area_left;
    material->draw_area_top = raster->draw_area_top;
    material->draw_area_right = raster->draw_area_right;
    material->draw_area_bottom = raster->draw_area_bottom;
    material->draw_offset_x = raster->draw_offset_x;
    material->draw_offset_y = raster->draw_offset_y;
    material->dither = raster->dither;
    material->mask_set = raster->mask_set;
    material->mask_check = raster->mask_check;
}

static XgRenderIrMaterialState triangle_material(
    const XgWorldMinimapRasterState *raster) {
    XgRenderIrMaterialState material = { 0 };

    apply_raster(raster, &material);
    material.tpage = 0x003eu;
    material.texture_page_x = 14u;
    material.texture_page_y = 1u;
    material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    material.shading = XG_RENDER_IR_SHADING_GOURAUD;
    material.semi_transparent = true;
    material.blend_mode = XG_RENDER_IR_BLEND_ADD;
    material.dither = false;
    return material;
}

static XgRenderIrMaterialState marker_material(
    const XgWorldMinimapRasterState *raster) {
    XgRenderIrMaterialState material = { 0 };

    apply_raster(raster, &material);
    material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    material.shading = XG_RENDER_IR_SHADING_FLAT;
    return material;
}

static XgRenderIrMaterialState panel_material(
    const XgWorldMinimapRasterState *raster) {
    XgRenderIrMaterialState material = { 0 };

    apply_raster(raster, &material);
    material.tpage = 0x001eu;
    material.texture_page_x = 14u;
    material.texture_page_y = 1u;
    material.clut_x = 256u;
    material.clut_y = 510u;
    material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    material.shading = XG_RENDER_IR_SHADING_FLAT;
    material.textured = true;
    material.semi_transparent = true;
    material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
    return material;
}

static XgRenderIrVertex ir_vertex(int32_t x, int32_t y, uint8_t u,
                                   uint8_t v, const uint8_t color[3],
                                   const XgHost3dProjectedVertex *projected) {
    XgRenderIrVertex vertex = { 0 };

    vertex.x = x * INT32_C(65536);
    vertex.y = y * INT32_C(65536);
    vertex.u = (int32_t)u * INT32_C(65536);
    vertex.v = (int32_t)v * INT32_C(65536);
    vertex.r = color[0];
    vertex.g = color[1];
    vertex.b = color[2];
    if (projected != NULL) {
        vertex.native_view_x = projected->native_view_x_16_16;
        vertex.native_view_y = projected->native_view_y_16_16;
        vertex.native_view_position = projected->native_view_position != 0u;
    }
    return vertex;
}

static void build_triangle_primitive(
    const int16_t *xy,
    const XgHost3dProjectedVertex projected[3],
    const XgRenderIrMaterialState *material,
    XgRenderIrNativePrimitive *primitive) {
    static const uint8_t colors[3][3] = {
        { 0xffu, 0x40u, 0x40u }, { 0u, 0u, 0u }, { 0u, 0u, 0u },
    };
    uint32_t vertex;

    memset(primitive, 0, sizeof(*primitive));
    primitive->material = *material;
    primitive->triangle_count = 1u;
    primitive->triangles[0].split_count = 1u;
    for (vertex = 0u; vertex < 3u; ++vertex)
        primitive->triangles[0].vertices[vertex] = ir_vertex(
            xy[vertex * 2u], xy[vertex * 2u + 1u], 0u, 0u,
            colors[vertex], &projected[vertex]);
}

static void build_quad_primitive(const int16_t *xy, const uint8_t *uv,
                                 const uint8_t color[3],
                                 const XgRenderIrMaterialState *material,
                                 XgRenderIrNativePrimitive *primitive) {
    static const uint8_t split[2][3] = { { 0u, 1u, 2u },
                                         { 2u, 1u, 3u } };
    uint32_t triangle;
    uint32_t vertex;

    memset(primitive, 0, sizeof(*primitive));
    primitive->material = *material;
    primitive->triangle_count = 2u;
    for (triangle = 0u; triangle < 2u; ++triangle) {
        primitive->triangles[triangle].split_index = (uint8_t)triangle;
        primitive->triangles[triangle].split_count = 2u;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint8_t index = split[triangle][vertex];

            primitive->triangles[triangle].vertices[vertex] = ir_vertex(
                xy[index * 2u], xy[index * 2u + 1u], uv[index * 2u],
                uv[index * 2u + 1u], color, NULL);
        }
    }
}

static void append_order_event(XgWorldMinimapBuildOutput *output,
                               XgWorldMinimapOrderKind kind,
                               uint32_t packet_address, uint8_t source_index,
                               uint8_t payload_word_count) {
    XgWorldMinimapOrderEvent *event =
        &output->ordering[output->ordering_count];

    event->kind = kind;
    event->packet_address = packet_address;
    event->insertion_ordinal = output->ordering_count;
    event->successor_event_index = output->ordering_count == 0u
        ? XG_WORLD_MINIMAP_NO_ORDER_EVENT
        : output->ordering_count - 1u;
    event->source_index = source_index;
    event->payload_word_count = payload_word_count;
    ++output->ordering_count;
}

XgWorldMinimapResult xg_world_minimap_build(
    const XgWorldMinimapSource *source,
    XgWorldMinimapBuildOutput *output) {
    static const int16_t panel_xy[4][2] = {
        { 208, 120 }, { 311, 120 }, { 208, 215 }, { 311, 215 },
    };
    static const uint8_t panel_uv[4][2] = {
        { 0u, 128u }, { 127u, 128u }, { 0u, 255u }, { 127u, 255u },
    };
    static const uint8_t panel_color[3] = { 0x80u, 0x80u, 0x80u };
    static const uint8_t marker_color[3] = { 0x80u, 0x80u, 0x10u };
    XgRenderIrMaterialState triangle_state;
    XgRenderIrMaterialState marker_state;
    XgRenderIrMaterialState panel_state;
    uint32_t triangle;
    uint32_t marker;

    if (source == NULL || output == NULL)
        return XG_WORLD_MINIMAP_INVALID_ARGUMENT;
    memset(output, 0, sizeof(*output));
    if (source->buffer_index > 1u || source->projection_distance == 0u ||
        !raster_is_valid(&source->raster) ||
        memcmp(source->triangles, expected_triangles,
               sizeof(expected_triangles)) != 0)
        return XG_WORLD_MINIMAP_INVALID_SOURCE;

    output->projection.rotation[0][0] = source->cosine;
    output->projection.rotation[0][1] =
        wrap_i16(-(int32_t)source->sine);
    output->projection.rotation[1][0] = source->sine;
    output->projection.rotation[1][1] = source->cosine;
    output->projection.rotation[2][2] = 0x1000;
    output->projection.translation[0] =
        (int32_t)(shift_right_floor(source->world_x_20_12, 12u) / 315) +
        0x30;
    output->projection.translation[1] = wrap_i32(
        (uint32_t)((int32_t)(shift_right_floor(source->world_z_20_12, 12u) /
                             341) +
                   0x78) -
        (uint32_t)source->vertical_offset);
    output->projection.translation[2] = source->translation_z;
    output->projection.screen_offset_x = source->screen_offset_x;
    output->projection.screen_offset_y = source->screen_offset_y;
    output->projection.projection_distance = source->projection_distance;

    output->scratch.angle_address = XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS;
    output->scratch.rotation_address =
        XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS;
    output->scratch.translation_address =
        XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS;
    output->scratch.angle_z = source->angle;
    memcpy(output->scratch.rotation, output->projection.rotation,
           sizeof(output->scratch.rotation));
    memcpy(output->scratch.translation, output->projection.translation,
           sizeof(output->scratch.translation));

    triangle_state = triangle_material(&source->raster);
    marker_state = marker_material(&source->raster);
    panel_state = panel_material(&source->raster);
    for (triangle = 0u; triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT;
         ++triangle) {
        XgWorldMinimapTriangleRecord *record =
            &output->triangles[triangle];
        XgHost3dProjectedVertex projected[3];
        uint32_t vertex;

        record->packet_address = TRIANGLE_PACKET_BASE +
            source->buffer_index * TRIANGLE_BUFFER_STRIDE +
            triangle * TRIANGLE_PACKET_STRIDE;
        record->submitted = true;
        for (vertex = 0u; vertex < XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT;
             ++vertex) {
            uint32_t flags;

            if (!xg_host_3d_rtps(&output->projection,
                                  &source->triangles[triangle][vertex],
                                  &projected[vertex], &flags))
                return XG_WORLD_MINIMAP_BUILD_FAILED;
            (void)flags;
            record->screen_xy[vertex][0] = projected[vertex].x;
            record->screen_xy[vertex][1] = projected[vertex].y;
            record->screen_xy_address[vertex] =
                record->packet_address + 8u + vertex * 8u;
        }
        build_triangle_primitive(&record->screen_xy[0][0], projected,
                                 &triangle_state, &record->primitive);
        append_order_event(output, XG_WORLD_MINIMAP_ORDER_TRIANGLE,
                           record->packet_address, (uint8_t)triangle, 6u);
    }

    output->draw_mode.packet_address = DRAW_MODE_PACKET;
    output->draw_mode.command_word = UINT32_C(0xe100043e);
    output->draw_mode.payload_word_count = 1u;
    append_order_event(output, XG_WORLD_MINIMAP_ORDER_DRAW_MODE,
                       output->draw_mode.packet_address, UINT8_MAX, 1u);

    for (marker = 0u; marker < XG_WORLD_MINIMAP_MARKER_CAPACITY; ++marker) {
        XgWorldMinimapMarkerRecord *record = &output->markers[marker];

        record->packet_address = MARKER_PACKET_BASE +
            source->buffer_index * MARKER_BUFFER_STRIDE +
            marker * MARKER_PACKET_STRIDE;
        record->screen_xy_address = record->packet_address + 8u;
        record->width = 2u;
        record->height = 2u;
        record->coordinate_rule = marker >= 24u && marker <= 26u
            ? XG_WORLD_MINIMAP_MARKER_RESIDENT_SCALED
            : XG_WORLD_MINIMAP_MARKER_OFFSET;
        record->active = (source->marker_mask & (UINT32_C(1) << marker)) != 0u;
        if (record->active) {
            int16_t xy[4][2];
            const uint8_t uv[4][2] = { { 0u, 0u }, { 0u, 0u },
                                       { 0u, 0u }, { 0u, 0u } };

            if (record->coordinate_rule ==
                XG_WORLD_MINIMAP_MARKER_RESIDENT_SCALED) {
                record->x = (int16_t)(source->markers[marker].raw_x / 315u +
                                      0xcfu);
                record->y = (int16_t)(source->markers[marker].raw_y / 341u +
                                      0x77u);
            } else {
                record->x = wrap_i16(
                    (int32_t)source->markers[marker].raw_x + 0xd0);
                record->y = wrap_i16(
                    (int32_t)source->markers[marker].raw_y + 0x78);
            }
            xy[0][0] = xy[2][0] = record->x;
            xy[1][0] = xy[3][0] = wrap_i16((int32_t)record->x + 2);
            xy[0][1] = xy[1][1] = record->y;
            xy[2][1] = xy[3][1] = wrap_i16((int32_t)record->y + 2);
            build_quad_primitive(&xy[0][0], &uv[0][0], marker_color,
                                 &marker_state,
                                 &record->primitive);
            append_order_event(output, XG_WORLD_MINIMAP_ORDER_MARKER,
                               record->packet_address, (uint8_t)marker, 3u);
            ++output->active_marker_count;
        }
    }

    output->panel.packet_address =
        PANEL_PACKET_BASE + source->buffer_index * PANEL_PACKET_STRIDE;
    memcpy(output->panel.screen_xy, panel_xy, sizeof(panel_xy));
    memcpy(output->panel.uv, panel_uv, sizeof(panel_uv));
    build_quad_primitive(&output->panel.screen_xy[0][0],
                         &output->panel.uv[0][0], panel_color, &panel_state,
                         &output->panel.primitive);
    append_order_event(output, XG_WORLD_MINIMAP_ORDER_PANEL,
                       output->panel.packet_address, UINT8_MAX, 9u);

    for (triangle = 0u; triangle < output->ordering_count; ++triangle)
        output->ordering[triangle].final_chain_ordinal =
            output->ordering_count - triangle - 1u;
    output->requires_external_ot_tail = true;
    return XG_WORLD_MINIMAP_OK;
}
