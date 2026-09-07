#include "xg_render_ui_layout.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

static const XgRenderGlyphMetric metrics[] = {
    { .glyph_id = 1u, .atlas_x = 0u, .atlas_y = 0u,
      .width = 4u, .height = 6u, .bearing_y = -5, .advance_x = 5 },
    { .glyph_id = 2u, .atlas_x = 4u, .atlas_y = 0u,
      .width = 6u, .height = 6u, .bearing_x = 1,
      .bearing_y = -5, .advance_x = 7 },
    { .glyph_id = 3u, .atlas_x = 10u, .atlas_y = 0u,
      .width = 3u, .height = 6u, .bearing_y = -5, .advance_x = 4 },
};

static XgRenderUiLayoutRequest fixture_request(void) {
    static const uint32_t glyph_ids[] = { 1u, 2u, 1u, 3u };
    static const XgRenderUiResolvedGlyphStyle styles[] = {
        { .color = UINT32_C(0xff102030), .palette_index = 2u },
        { .color = UINT32_C(0xff405060), .palette_index = 3u,
          .shadow_color = UINT32_C(0x80202020), .shadow_palette_index = 9u,
          .shadow_offset_x = 1, .shadow_offset_y = 1,
          .shadow_enabled = 1u },
        { .color = UINT32_C(0xff708090), .palette_index = 4u },
        { .color = UINT32_C(0xffa0b0c0), .palette_index = 5u },
    };
    static const XgRenderUiResolvedLine lines[] = {
        { .first_glyph = 0u, .glyph_count = 2u },
        { .first_glyph = 2u, .glyph_count = 2u },
    };
    XgRenderUiLayoutRequest request = {
        .owner = {
            .owner_domain = XG_RENDER_UI_OWNER_FIELD_DIALOGUE,
            .owner_root = UINT32_C(0x8008004c),
            .adapter_kind = 17u,
            .owner_text_id = UINT64_C(0x100000002),
        },
        .metrics = metrics,
        .metric_count = sizeof(metrics) / sizeof(metrics[0]),
        .atlas_width = 64u,
        .atlas_height = 32u,
        .glyph_ids = glyph_ids,
        .styles = styles,
        .glyph_count = sizeof(glyph_ids) / sizeof(glyph_ids[0]),
        .reveal_count = 3u,
        .lines = lines,
        .line_count = sizeof(lines) / sizeof(lines[0]),
        .origin_x = 10,
        .origin_y = 20,
        .layout_width = 20,
        .line_height = 8,
        .scale_fp16 = XG_RENDER_UI_LAYOUT_FIXED_ONE,
        .alignment = XG_RENDER_UI_TEXT_ALIGN_LEFT,
        .clip = { .left = 0, .top = 0, .right = 100, .bottom = 100 },
    };
    return request;
}

static void test_multiline_reveal_shadow_and_determinism(void) {
    XgRenderUiLayoutRequest request = fixture_request();
    XgRenderUiLayoutPlacement placements[4];
    XgRenderUiLayoutPlacement repeated_placements[4];
    XgRenderUiLayoutLineRecord lines[2];
    XgRenderUiLayoutLineRecord repeated_lines[2];
    XgRenderUiLayoutOutput output;
    XgRenderUiLayoutOutput repeated;

    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &output) ==
           XG_RENDER_UI_LAYOUT_OK);
    assert(output.total_glyph_count == 4u);
    assert(output.revealed_glyph_count == 3u);
    assert(output.placement_count == 3u);
    assert(output.line_count == 2u);
    assert(output.content_width == 12);
    assert(output.content_height == 16);
    assert(output.digest == UINT64_C(6422170775023476148));
    assert(lines[0].x == 10 && lines[0].baseline_y == 20);
    assert(lines[0].width == 12 && lines[0].placement_count == 2u);
    assert(lines[0].revealed_glyph_count == 2u);
    assert(lines[1].x == 10 && lines[1].baseline_y == 28);
    assert(lines[1].width == 9 && lines[1].placement_count == 1u);
    assert(lines[1].revealed_glyph_count == 1u);
    assert(placements[1].layer_mask ==
           (XG_RENDER_UI_LAYOUT_LAYER_PRIMARY |
            XG_RENDER_UI_LAYOUT_LAYER_SHADOW));
    assert(placements[1].palette_index == 3u);
    assert(placements[1].shadow_palette_index == 9u);
    assert(placements[1].primary.x == 16 && placements[1].primary.y == 15);
    assert(placements[1].shadow.x == 17 && placements[1].shadow.y == 16);

    assert(xg_render_ui_layout_build(
               &request, repeated_placements, 4u, repeated_lines, 2u,
               &repeated) == XG_RENDER_UI_LAYOUT_OK);
    assert(output.digest == repeated.digest);
    assert(repeated_placements[1].primary.x == placements[1].primary.x);
    assert(repeated_placements[1].shadow_uv.left_fp16 ==
           placements[1].shadow_uv.left_fp16);
    assert(repeated_lines[1].advance_x_fp16 == lines[1].advance_x_fp16);
}

static void test_alignment(void) {
    XgRenderUiLayoutRequest request = fixture_request();
    XgRenderUiLayoutPlacement placements[4];
    XgRenderUiLayoutLineRecord lines[2];
    XgRenderUiLayoutOutput output;

    request.alignment = XG_RENDER_UI_TEXT_ALIGN_CENTER;
    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &output) ==
           XG_RENDER_UI_LAYOUT_OK);
    assert(lines[0].x == 14);
    assert(lines[1].x == 15);
    assert(placements[0].primary.x == 14);

    request.alignment = XG_RENDER_UI_TEXT_ALIGN_RIGHT;
    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &output) ==
           XG_RENDER_UI_LAYOUT_OK);
    assert(lines[0].x == 18);
    assert(lines[1].x == 21);
}

static void test_scaled_clipping_adjusts_uv(void) {
    XgRenderUiLayoutRequest request = fixture_request();
    XgRenderUiLayoutPlacement placement[1];
    XgRenderUiLayoutLineRecord line;
    XgRenderUiLayoutOutput output;
    const uint32_t glyph_id = 1u;
    const XgRenderUiResolvedGlyphStyle style = {
        .color = UINT32_C(0xffffffff),
        .shadow_color = UINT32_C(0xff000000),
        .palette_index = 7u,
        .shadow_palette_index = 8u,
        .shadow_offset_x = 1,
        .shadow_enabled = 1u,
    };
    const XgRenderUiResolvedLine resolved_line = {
        .first_glyph = 0u, .glyph_count = 1u,
    };

    request.glyph_ids = &glyph_id;
    request.styles = &style;
    request.glyph_count = 1u;
    request.reveal_count = 1u;
    request.lines = &resolved_line;
    request.line_count = 1u;
    request.origin_x = -4;
    request.origin_y = 5;
    request.scale_fp16 = 2 * XG_RENDER_UI_LAYOUT_FIXED_ONE;
    request.line_height = 6;
    request.clip = (XgRenderUiLayoutRect){
        .left = 0, .top = 0, .right = 5, .bottom = 10,
    };
    assert(xg_render_ui_layout_build(
               &request, placement, 1u, &line, 1u, &output) ==
           XG_RENDER_UI_LAYOUT_OK);
    assert(output.placement_count == 1u);
    assert(placement[0].primary.x == 0);
    assert(placement[0].primary.width == 4u);
    assert(placement[0].primary_uv.left_fp16 == UINT64_C(2) * 65536u);
    assert(placement[0].primary_uv.right_fp16 == UINT64_C(4) * 65536u);
    assert((placement[0].layer_mask & XG_RENDER_UI_LAYOUT_LAYER_SHADOW) != 0u);
    assert(placement[0].shadow.x == 0);
    assert(placement[0].shadow_uv.left_fp16 == UINT64_C(1) * 65536u);
}

static void test_owner_specific_input_is_preserved_and_hashed(void) {
    XgRenderUiLayoutRequest request = fixture_request();
    XgRenderUiLayoutPlacement placements[4];
    XgRenderUiLayoutLineRecord lines[2];
    XgRenderUiLayoutOutput first;
    XgRenderUiLayoutOutput second;

    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &first) ==
           XG_RENDER_UI_LAYOUT_OK);
    request.owner.owner_domain = XG_RENDER_UI_OWNER_GENERAL_MENU;
    request.owner.owner_root = UINT32_C(0x80100000);
    request.owner.adapter_kind = 99u;
    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &second) ==
           XG_RENDER_UI_LAYOUT_OK);
    assert(second.owner.owner_domain == XG_RENDER_UI_OWNER_GENERAL_MENU);
    assert(second.owner.adapter_kind == 99u);
    assert(first.digest != second.digest);
}

static void test_errors_capacity_and_hidden_validation(void) {
    XgRenderUiLayoutRequest request = fixture_request();
    XgRenderUiLayoutPlacement placements[4];
    XgRenderUiLayoutLineRecord lines[2];
    XgRenderUiLayoutOutput output = {0};
    uint32_t invalid_ids[] = { 1u, 2u, 1u, 99u };
    XgRenderGlyphMetric invalid_metrics[3];
    XgRenderUiResolvedLine invalid_lines[2];

    assert(xg_render_ui_layout_build(
               &request, placements, 2u, lines, 2u, &output) ==
           XG_RENDER_UI_LAYOUT_CAPACITY_EXCEEDED);
    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 1u, &output) ==
           XG_RENDER_UI_LAYOUT_CAPACITY_EXCEEDED);

    request.glyph_ids = invalid_ids;
    request.reveal_count = 1u;
    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &output) ==
           XG_RENDER_UI_LAYOUT_GLYPH_NOT_FOUND);

    request = fixture_request();
    request.origin_x = INT32_MAX;
    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &output) ==
           XG_RENDER_UI_LAYOUT_OVERFLOW);

    request = fixture_request();
    memcpy(invalid_metrics, metrics, sizeof(invalid_metrics));
    invalid_metrics[2].atlas_x = 63u;
    request.metrics = invalid_metrics;
    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &output) ==
           XG_RENDER_UI_LAYOUT_OUT_OF_BOUNDS);

    request = fixture_request();
    memcpy(invalid_lines, request.lines, sizeof(invalid_lines));
    invalid_lines[0].first_glyph = 1u;
    request.lines = invalid_lines;
    assert(xg_render_ui_layout_build(
               &request, placements, 4u, lines, 2u, &output) ==
           XG_RENDER_UI_LAYOUT_INVALID_ARGUMENT);
}

int main(void) {
    test_multiline_reveal_shadow_and_determinism();
    test_alignment();
    test_scaled_clipping_adjusts_uv();
    test_owner_specific_input_is_preserved_and_hashed();
    test_errors_capacity_and_hidden_validation();
    return 0;
}
