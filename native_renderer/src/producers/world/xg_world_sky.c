#include "xg_world_sky.h"

#include "xg_render_quad_builder.h"

#include <string.h>

static const uint8_t sky_colors[XG_WORLD_SKY_QUAD_COUNT][2][3] = {
    { { 0x70u, 0x7au, 0xffu }, { 0xe0u, 0xf5u, 0xffu } },
    { { 0x45u, 0x37u, 0xc0u }, { 0x70u, 0x7au, 0xffu } },
    { { 0x45u, 0x37u, 0xc0u }, { 0x45u, 0x37u, 0xc0u } },
    { { 0x45u, 0x37u, 0xc0u }, { 0x45u, 0x37u, 0xc0u } },
};

XgWorldSkyResult xg_world_sky_build(
    const XgWorldSkySource *source,
    XgWorldSkyRecord records[XG_WORLD_SKY_QUAD_COUNT]) {
    uint32_t quad_index;

    if (source == NULL || records == NULL)
        return XG_WORLD_SKY_INVALID_ARGUMENT;
    memset(records, 0, sizeof(*records) * XG_WORLD_SKY_QUAD_COUNT);
    if (source->buffer_index > 1u || source->ordering_shift > 31u ||
        source->material.shading != XG_RENDER_IR_SHADING_GOURAUD ||
        source->material.textured || source->material.raw_texture ||
        source->material.semi_transparent)
        return XG_WORLD_SKY_INVALID_SOURCE;

    for (quad_index = 0u; quad_index < XG_WORLD_SKY_QUAD_COUNT; ++quad_index) {
        XgHost3dProject4Input projection = { 0 };
        XgHost3dRotTransPers4Output projected;
        XgRenderQuadSource quad = { 0 };
        uint32_t vertex;

        memcpy(projection.vertices, source->vertices[quad_index],
               sizeof(projection.vertices));
        projection.projection = source->projection;
        if (!xg_host_3d_rot_trans_pers4(&projection, &projected))
            return XG_WORLD_SKY_BUILD_FAILED;
        records[quad_index].packet_address = UINT32_C(0x8009d194) +
            quad_index * 0x48u + source->buffer_index * 0x24u;
        records[quad_index].ordering_bucket =
            projected.fourth_depth >> source->ordering_shift;
        records[quad_index].projection_flags = projected.projection_flags;
        records[quad_index].accepted =
            (int32_t)projected.projection_flags >= 0;
        memcpy(records[quad_index].vertices, projected.vertices,
               sizeof(records[quad_index].vertices));
        quad.material = source->material;
        for (vertex = 0u; vertex < 4u; ++vertex) {
            const uint8_t *color = sky_colors[quad_index][vertex >= 2u];

            quad.vertices[vertex] = (XgRenderQuadSourceVertex){
                .red = color[0],
                .green = color[1],
                .blue = color[2],
            };
            xg_render_quad_set_projected_position(
                &quad.vertices[vertex], &projected.vertices[vertex]);
        }
        if (xg_render_quad_build_primitive(
                &quad, &records[quad_index].primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
            return XG_WORLD_SKY_BUILD_FAILED;
    }
    return XG_WORLD_SKY_OK;
}
