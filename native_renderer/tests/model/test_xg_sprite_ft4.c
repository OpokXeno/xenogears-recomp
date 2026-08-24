#include "xg_sprite_ft4.h"

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

static XgSpriteFt4Source source_fixture(void) {
    XgSpriteFt4Source source = { 0 };

    source.vertices[0] = (XgHost3dVector){ -32, -48, 0, 0u };
    source.vertices[1] = (XgHost3dVector){ 32, -48, 0, 0u };
    source.vertices[2] = (XgHost3dVector){ -32, 48, 0, 0u };
    source.vertices[3] = (XgHost3dVector){ 32, 48, 0, 0u };
    source.projection.rotation[0][0] = 4096;
    source.projection.rotation[1][1] = 4096;
    source.projection.rotation[2][2] = 4096;
    source.projection.translation[2] = 512;
    source.projection.screen_offset_x = 160 << 16;
    source.projection.screen_offset_y = 120 << 16;
    source.projection.projection_distance = 256u;
    source.material.draw_area_right = 319u;
    source.material.draw_area_bottom = 239u;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = true;
    source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    source.color[0] = 0x80u;
    source.color[1] = 0x80u;
    source.color[2] = 0x80u;
    source.uv[1][0] = 31u;
    source.uv[2][1] = 47u;
    source.packet_vertex_for_projection[0] = 0u;
    source.packet_vertex_for_projection[1] = 1u;
    source.packet_vertex_for_projection[2] = 3u;
    source.packet_vertex_for_projection[3] = 2u;
    return source;
}

static int test_projects_without_culling(void) {
    XgSpriteFt4Source source = source_fixture();
    XgSpriteFt4Record record;

    CHECK(xg_sprite_ft4_build(&source, &record) == XG_SPRITE_FT4_OK);
    CHECK(record.vertices[0].x == 144);
    CHECK(record.vertices[0].y == 96);
    CHECK(record.vertices[2].x == 176);
    CHECK(record.vertices[3].x == 144);
    CHECK(record.vertices[3].y == 144);
    CHECK(record.fourth_depth == 128u);
    CHECK(record.primitive.triangles[0].vertices[1].u == 31 * 65536);
    CHECK(record.primitive.triangles[1].vertices[0].v == 47 * 65536);
    CHECK(!record.primitive.triangles[0].vertices[0].projective_position);
    return 1;
}

static int test_rejects_non_ft4_material(void) {
    XgSpriteFt4Source source = source_fixture();
    XgSpriteFt4Record record;

    source.material.textured = false;
    CHECK(xg_sprite_ft4_build(&source, &record) ==
          XG_SPRITE_FT4_INVALID_SOURCE);
    return 1;
}

static int test_maps_uv_with_guest_horizontal_edge_rules(void) {
    uint8_t uv[4][2] = { 0 };

    CHECK(xg_sprite_ft4_map_uv(24u, 193u, 4u, 4u, false, uv) ==
          XG_SPRITE_FT4_OK);
    CHECK(uv[0][0] == 24u && uv[1][0] == 27u);
    CHECK(uv[0][1] == 193u && uv[2][1] == 196u);

    CHECK(xg_sprite_ft4_map_uv(24u, 193u, 4u, 4u, true, uv) ==
          XG_SPRITE_FT4_OK);
    CHECK(uv[0][0] == 23u && uv[1][0] == 26u);
    CHECK(uv[0][1] == 193u && uv[2][1] == 196u);

    CHECK(xg_sprite_ft4_map_uv(0u, 193u, 4u, 4u, true, uv) ==
          XG_SPRITE_FT4_OK);
    CHECK(uv[0][0] == 0u && uv[1][0] == 2u);
    CHECK(xg_sprite_ft4_map_uv(0u, 0u, 0u, 0u, false, NULL) ==
          XG_SPRITE_FT4_INVALID_ARGUMENT);
    return 1;
}

static int test_selects_descriptor_relative_ot_bucket(void) {
    uint32_t address = 0u;

    CHECK(xg_sprite_ft4_select_ot_address(
              UINT32_C(0x8006101c), 0u, 3u, &address) == XG_SPRITE_FT4_OK);
    CHECK(address == UINT32_C(0x8006101c));
    CHECK(xg_sprite_ft4_select_ot_address(
              UINT32_C(0x8006101c), UINT32_C(0x08000000), 3u, &address) ==
          XG_SPRITE_FT4_OK);
    CHECK(address == UINT32_C(0x80061010));
    CHECK(xg_sprite_ft4_select_ot_address(
              4u, UINT32_C(0x08000000), 7u, &address) ==
          XG_SPRITE_FT4_INVALID_SOURCE);
    CHECK(xg_sprite_ft4_select_ot_address(0u, 0u, 0u, NULL) ==
          XG_SPRITE_FT4_INVALID_ARGUMENT);
    return 1;
}

int main(void) {
    return test_projects_without_culling() &&
        test_rejects_non_ft4_material() &&
        test_maps_uv_with_guest_horizontal_edge_rules() &&
        test_selects_descriptor_relative_ot_bucket() ? 0 : 1;
}
