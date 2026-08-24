#include "xg_field_character_adapter.h"

#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

static XgFieldCharacterCapture fixture(void) {
    XgFieldCharacterCapture capture = { 0 };

    capture.vertices[0] = (XgFieldCharacterCaptureVertex){ -17, 23, 4u, 8u };
    capture.vertices[1] = (XgFieldCharacterCaptureVertex){ 31, -9, 48u, 12u };
    capture.vertices[2] = (XgFieldCharacterCaptureVertex){ 7, 55, 19u, 63u };
    capture.vertices[3] = (XgFieldCharacterCaptureVertex){ 47, 51, 71u, 67u };
    capture.red = 0x40u;
    capture.green = 0x60u;
    capture.blue = 0x80u;
    capture.semi_transparent = 1u;
    capture.tpage = 0x153u;
    capture.clut_x = 32u;
    capture.clut_y = 7u;
    capture.draw_area_right = 319u;
    capture.draw_area_bottom = 239u;
    capture.draw_offset_x = -12;
    capture.draw_offset_y = 14;
    capture.texture_window_mask_x = 1u;
    capture.texture_window_mask_y = 2u;
    capture.texture_window_offset_x = 3u;
    capture.texture_window_offset_y = 4u;
    capture.dither = 1u;
    capture.mask_check = 1u;
    return capture;
}

static int test_builds_finalized_semantic_candidate(void) {
    XgFieldCharacterCapture capture = fixture();
    XgFieldCharacterCapture unchanged = capture;
    XgFieldCharacterCandidate candidate;

    memset(&candidate, 0xa5, sizeof(candidate));
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_OK);
    CHECK(memcmp(&capture, &unchanged, sizeof(capture)) == 0);
    CHECK(candidate.finalized == 1u);
    CHECK(candidate.vertices[0].x == -17 * INT32_C(65536));
    CHECK(candidate.vertices[1].y == -9 * INT32_C(65536));
    CHECK(candidate.vertices[2].u == 19 * INT32_C(65536));
    CHECK(candidate.vertices[3].v == 67 * INT32_C(65536));
    CHECK(candidate.vertices[0].red == capture.red);
    CHECK(candidate.vertices[1].green == capture.green);
    CHECK(candidate.vertices[2].blue == capture.blue);
    CHECK(candidate.tpage == capture.tpage);
    CHECK(candidate.texture_page_x == 3u);
    CHECK(candidate.texture_page_y == 1u);
    CHECK(candidate.texture_depth == 2u);
    CHECK(candidate.blend_mode == 2u);
    CHECK(candidate.semi_transparent == 1u);
    CHECK(candidate.clut_x == capture.clut_x);
    CHECK(candidate.clut_y == capture.clut_y);
    return 1;
}

static int test_rejects_non_representable_material_state(void) {
    XgFieldCharacterCapture capture = fixture();
    XgFieldCharacterCandidate candidate;
    XgFieldCharacterCandidate zero = { 0 };

    capture.clut_x = 17u;
    memset(&candidate, 0xa5, sizeof(candidate));
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_INVALID_CAPTURE);
    CHECK(memcmp(&candidate, &zero, sizeof(candidate)) == 0);
    capture = fixture();
    capture.tpage = 0x180u;
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_INVALID_CAPTURE);
    capture = fixture();
    capture.draw_area_left = 100u;
    capture.draw_area_right = 99u;
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_INVALID_CAPTURE);
    capture = fixture();
    capture.mask_set = 2u;
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_INVALID_CAPTURE);
    return 1;
}

static int test_context_changes_are_derived_only_from_capture(void) {
    XgFieldCharacterCapture captures[4];
    XgFieldCharacterCandidate candidates[4];
    size_t index;

    for (index = 0u; index < 4u; ++index)
        captures[index] = fixture();
    captures[0].vertices[0].x += 3;       /* movement */
    captures[1].vertices[1].y -= 5;       /* camera */
    captures[2].vertices[2].u += 7u;      /* overlap/animation */
    captures[3].clut_y += 1u;             /* dialog transition palette */
    for (index = 0u; index < 4u; ++index) {
        CHECK(xg_field_character_adapter_build(&captures[index],
                                               &candidates[index]) ==
              XG_FIELD_CHARACTER_ADAPTER_OK);
        CHECK(candidates[index].finalized == 1u);
    }
    CHECK(candidates[0].vertices[0].x != candidates[1].vertices[0].x);
    CHECK(candidates[1].vertices[1].y != candidates[0].vertices[1].y);
    CHECK(candidates[2].vertices[2].u != candidates[0].vertices[2].u);
    CHECK(candidates[3].clut_y != candidates[0].clut_y);
    return 1;
}

static int test_primitive_uses_ps1_quad_split_order(void) {
    XgFieldCharacterCapture capture = fixture();
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;

    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_OK);
    CHECK(xg_field_character_adapter_build_primitive(&candidate, &primitive) ==
          XG_FIELD_CHARACTER_ADAPTER_OK);
    CHECK(primitive.triangle_count == 2u);
    CHECK(primitive.triangles[1].vertices[0].x ==
          (int32_t)capture.vertices[2].x * INT32_C(65536));
    CHECK(primitive.triangles[1].vertices[1].x ==
          (int32_t)capture.vertices[1].x * INT32_C(65536));
    CHECK(primitive.triangles[1].vertices[2].x ==
          (int32_t)capture.vertices[3].x * INT32_C(65536));
    return 1;
}

static int test_argument_failures(void) {
    XgFieldCharacterCapture capture = fixture();
    XgFieldCharacterCandidate candidate;

    CHECK(xg_field_character_adapter_build(NULL, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_INVALID_ARGUMENT);
    CHECK(xg_field_character_adapter_build(&capture, NULL) ==
          XG_FIELD_CHARACTER_ADAPTER_INVALID_ARGUMENT);
    return 1;
}

int main(void) {
    return !(test_builds_finalized_semantic_candidate() &&
              test_rejects_non_representable_material_state() &&
              test_context_changes_are_derived_only_from_capture() &&
              test_primitive_uses_ps1_quad_split_order() &&
              test_argument_failures());
}
