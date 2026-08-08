#include "xg_world_sky.h"

#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

static XgRenderIrMaterialState sky_material(void) {
    XgRenderIrMaterialState material = { 0 };

    material.draw_area_right = 319u;
    material.draw_area_bottom = 239u;
    material.shading = XG_RENDER_IR_SHADING_GOURAUD;
    return material;
}

static void set_sky_vertices(XgWorldSkySource *source) {
    static const XgHost3dVector vertices[4][4] = {
        { { -4096, -768, 4096, 0u }, { 4096, -768, 4096, 0u },
          { -4096, 1024, 4096, 0u }, { 4096, 1024, 4096, 0u } },
        { { -4096, -1152, 4096, 0u }, { 4096, -1152, 4096, 0u },
          { -4096, -768, 4096, 0u }, { 4096, -768, 4096, 0u } },
        { { -4096, -3200, 3072, 0u }, { 4096, -3200, 3072, 0u },
          { -4096, -1152, 4096, 0u }, { 4096, -1152, 4096, 0u } },
        { { -4096, -4096, 1024, 0u }, { 4096, -4096, 1024, 0u },
          { -4096, -3200, 3072, 0u }, { 4096, -3200, 3072, 0u } },
    };

    memcpy(source->vertices, vertices, sizeof(vertices));
}

static int test_captured_world_projection(void) {
    static const int16_t expected_xy[4][4][2] = {
        { { -105, -18 }, { 424, -18 }, { -56, 93 }, { 375, 93 } },
        { { -119, -49 }, { 438, -49 }, { -105, -18 }, { 424, -18 } },
        { { -404, -416 }, { 723, -416 }, { -119, -49 }, { 438, -49 } },
        { { -1024, -1024 }, { 1023, -1024 }, { -404, -416 }, { 723, -416 } },
    };
    XgWorldSkySource source = { 0 };
    XgWorldSkyRecord records[XG_WORLD_SKY_QUAD_COUNT];
    uint32_t quad;
    uint32_t vertex;

    source.projection.rotation[0][0] = 4096;
    source.projection.rotation[1][1] = 3548;
    source.projection.rotation[1][2] = -2046;
    source.projection.rotation[2][1] = 2046;
    source.projection.rotation[2][2] = 3548;
    source.projection.translation[0] = 0;
    source.projection.translation[1] = 273;
    source.projection.translation[2] = 798;
    source.projection.screen_offset_x = 160 << 16;
    source.projection.screen_offset_y = 140 << 16;
    source.projection.projection_distance = 0x100u;
    source.material = sky_material();
    set_sky_vertices(&source);
    source.ordering_shift = 2u;
    source.buffer_index = 1u;

    CHECK(xg_world_sky_build(&source, records) == XG_WORLD_SKY_OK);
    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        CHECK(records[quad].packet_address ==
              UINT32_C(0x8009d1b8) + quad * 0x48u);
        CHECK(records[quad].ordering_bucket ==
              (records[quad].vertices[3].z >> 4u));
        for (vertex = 0u; vertex < 4u; ++vertex) {
            CHECK(records[quad].vertices[vertex].x ==
                  expected_xy[quad][vertex][0]);
            CHECK(records[quad].vertices[vertex].y ==
                  expected_xy[quad][vertex][1]);
        }
    }
    CHECK(records[0].primitive.material.shading ==
          XG_RENDER_IR_SHADING_GOURAUD);
    CHECK(records[0].primitive.triangles[0].vertices[0].r == 0x70u);
    CHECK(records[0].primitive.triangles[1].vertices[2].r == 0xe0u);
    CHECK(records[1].primitive.triangles[1].vertices[2].r == 0x70u);
    CHECK(records[2].accepted);
    CHECK(!records[3].accepted);
    return 1;
}

static int test_invalid_world_source(void) {
    XgWorldSkySource source = { 0 };
    XgWorldSkyRecord records[XG_WORLD_SKY_QUAD_COUNT];

    CHECK(xg_world_sky_build(NULL, records) ==
          XG_WORLD_SKY_INVALID_ARGUMENT);
    CHECK(xg_world_sky_build(&source, NULL) ==
          XG_WORLD_SKY_INVALID_ARGUMENT);
    source.material = sky_material();
    source.buffer_index = 2u;
    CHECK(xg_world_sky_build(&source, records) ==
          XG_WORLD_SKY_INVALID_SOURCE);
    return 1;
}

int main(void) {
    return test_captured_world_projection() && test_invalid_world_source()
        ? 0 : 1;
}
