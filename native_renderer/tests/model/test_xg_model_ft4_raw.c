#include "xg_model_ft4_raw.h"

#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

static XgModelFt4RawSource source_fixture(void) {
    XgModelFt4RawSource source = { 0 };

    source.vertices[0] = (XgHost3dVector){ -64, -64, 0, 0u };
    source.vertices[1] = (XgHost3dVector){ 64, -64, 0, 0u };
    source.vertices[2] = (XgHost3dVector){ -64, 64, 0, 0u };
    source.vertices[3] = (XgHost3dVector){ 64, 64, 0, 0u };
    source.projection.rotation[0][0] = 4096;
    source.projection.rotation[1][1] = 4096;
    source.projection.rotation[2][2] = 4096;
    source.projection.translation[2] = 512;
    source.projection.screen_offset_x = 160 << 16;
    source.projection.screen_offset_y = 120 << 16;
    source.projection.projection_distance = 256u;
    source.projection.average_z_scale4 = 1024;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = true;
    source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    source.uv[1][0] = 31u;
    source.uv[2][1] = 63u;
    source.screen_right = 319;
    source.packed_screen_bottom = UINT32_C(0x00ee0000);
    source.packet_address = UINT32_C(0x800b0000);
    source.ordering_shift = 4u;
    source.material_word = UINT32_C(0x2d112233);
    source.depth_cue_color_word = UINT32_C(0x00445566);
    return source;
}

static int test_builds_raw_ft4(void) {
    XgModelFt4RawSource source = source_fixture();
    XgModelFt4RawRecord record;

    CHECK(xg_model_ft4_raw_build(&source, &record) == XG_MODEL_FT4_RAW_OK);
    CHECK(record.nclip > 0);
    CHECK(record.passed_screen_cull);
    CHECK(record.accepted);
    CHECK(record.ordering_bucket != 0u);
    CHECK(record.primitive.material.textured);
    CHECK(record.primitive.material.raw_texture);
    CHECK(record.primitive.triangles[0].vertices[1].u == 31 * 65536);
    CHECK(record.primitive.triangles[1].vertices[0].v == 63 * 65536);
    return 1;
}

static int test_dispatch_modes(void) {
    static const struct {
        uint8_t mode;
        uint32_t bucket;
        bool depth_cued;
    } cases[] = {
        {XG_MODEL_FT4_RAW_DISPATCH_AVERAGE, 32u, false},
        {XG_MODEL_FT4_RAW_DISPATCH_FARTHEST, 8u, false},
        {XG_MODEL_FT4_RAW_DISPATCH_NEAREST, 8u, false},
        {XG_MODEL_FT4_RAW_DISPATCH_AVERAGE_DEPTH_CUE, 32u, true},
        {XG_MODEL_FT4_RAW_DISPATCH_FARTHEST_DEPTH_CUE, 8u, true},
    };
    uint32_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        XgModelFt4RawSource source = source_fixture();
        XgModelFt4RawRecord record;

        source.dispatch_mode = cases[index].mode;
        CHECK(xg_model_ft4_raw_build(&source, &record) ==
              XG_MODEL_FT4_RAW_OK);
        CHECK(record.counter_incremented);
        CHECK(record.accepted);
        CHECK(record.ordering_bucket == cases[index].bucket);
        CHECK(record.primitive.material.raw_texture ==
              !cases[index].depth_cued);
        if (cases[index].depth_cued)
            CHECK(record.material_word == UINT32_C(0x2c445566));
        else
            CHECK(record.material_word == source.material_word);
    }
    return 1;
}

static int test_rejects_culled_and_invalid_sources(void) {
    XgModelFt4RawSource source = source_fixture();
    XgModelFt4RawRecord record;

    source.vertices[1] = (XgHost3dVector){ -64, 64, 0, 0u };
    source.vertices[2] = (XgHost3dVector){ 64, -64, 0, 0u };
    CHECK(xg_model_ft4_raw_build(&source, &record) == XG_MODEL_FT4_RAW_OK);
    CHECK(record.nclip < 0);
    CHECK(!record.passed_screen_cull);
    CHECK(!record.accepted);

    source = source_fixture();
    source.material.raw_texture = false;
    CHECK(xg_model_ft4_raw_build(&source, &record) ==
          XG_MODEL_FT4_RAW_INVALID_SOURCE);
    return 1;
}

static int test_native_view_widens_host_screen_cull(void) {
    XgModelFt4RawSource source = source_fixture();
    XgModelFt4RawRecord record;

    source.projection.translation[0] = 400;
    xg_host_3d_configure_native_view(0, 0);
    CHECK(xg_model_ft4_raw_build(&source, &record) == XG_MODEL_FT4_RAW_OK);
    CHECK(!record.passed_screen_cull);

    xg_host_3d_configure_native_view(1, 54 << 16);
    CHECK(xg_model_ft4_raw_build(&source, &record) == XG_MODEL_FT4_RAW_OK);
    CHECK(record.passed_screen_cull);
    CHECK(record.accepted);
    CHECK(record.primitive.triangles[0].vertices[0].native_view_position);
    xg_host_3d_configure_native_view(0, 0);
    return 1;
}

int main(void) {
    return test_builds_raw_ft4() && test_dispatch_modes() &&
           test_rejects_culled_and_invalid_sources() &&
           test_native_view_widens_host_screen_cull()
        ? 0 : 1;
}
