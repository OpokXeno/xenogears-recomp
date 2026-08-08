#include "xg_render_private_compare.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

static XgFieldCharacterCapture fixture(uint32_t sequence) {
    XgFieldCharacterCapture capture = { 0 };

    capture.vertices[0] = (XgFieldCharacterCaptureVertex){
        (int16_t)(-100 + (int32_t)(sequence % 50u)),
        (int16_t)(20 + (int32_t)(sequence % 30u)),
        (uint8_t)sequence,
        (uint8_t)(sequence >> 1u),
    };
    capture.vertices[1] = (XgFieldCharacterCaptureVertex){
        (int16_t)(capture.vertices[0].x + 16),
        (int16_t)(capture.vertices[0].y + 2),
        (uint8_t)(sequence + 16u),
        (uint8_t)(sequence + 2u),
    };
    capture.vertices[2] = (XgFieldCharacterCaptureVertex){
        (int16_t)(capture.vertices[0].x + 8),
        (int16_t)(capture.vertices[0].y + 32),
        (uint8_t)(sequence + 8u),
        (uint8_t)(sequence + 32u),
    };
    capture.vertices[3] = (XgFieldCharacterCaptureVertex){
        (int16_t)(capture.vertices[0].x + 24),
        (int16_t)(capture.vertices[0].y + 34),
        (uint8_t)(sequence + 24u),
        (uint8_t)(sequence + 34u),
    };
    capture.red = (uint8_t)(0x40u + sequence);
    capture.green = (uint8_t)(0x60u + sequence);
    capture.blue = (uint8_t)(0x80u + sequence);
    capture.semi_transparent = 1u;
    capture.tpage = (uint16_t)(0x100u | (sequence & 15u));
    capture.clut_x = (uint16_t)((sequence & 31u) * 16u);
    capture.clut_y = (uint16_t)(sequence & 0x1ffu);
    capture.draw_area_right = 319u;
    capture.draw_area_bottom = 239u;
    capture.draw_offset_x = -16;
    capture.draw_offset_y = 8;
    capture.texture_window_mask_x = 1u;
    capture.texture_window_mask_y = 2u;
    capture.texture_window_offset_x = 3u;
    capture.texture_window_offset_y = 4u;
    capture.dither = 1u;
    capture.mask_check = 1u;
    return capture;
}

static uint32_t xy_word(const XgFieldCharacterCaptureVertex *vertex) {
    return (uint32_t)(uint16_t)vertex->x |
           ((uint32_t)(uint16_t)vertex->y << 16u);
}

static uint32_t uv_word(const XgFieldCharacterCaptureVertex *vertex,
                        uint16_t upper) {
    return vertex->u | ((uint32_t)vertex->v << 8u) | ((uint32_t)upper << 16u);
}

static void packet_for(const XgFieldCharacterCapture *capture,
                       uint32_t words[XG_RENDER_PRIVATE_FT4_WORD_COUNT]) {
    const uint16_t clut =
        (uint16_t)((capture->clut_x >> 4u) | (capture->clut_y << 6u));

    words[0] = UINT32_C(0x2e000000) | ((uint32_t)capture->blue << 16u) |
               ((uint32_t)capture->green << 8u) | capture->red;
    words[1] = xy_word(&capture->vertices[0]);
    words[2] = uv_word(&capture->vertices[0], clut);
    words[3] = xy_word(&capture->vertices[1]);
    words[4] = uv_word(&capture->vertices[1], capture->tpage);
    words[5] = xy_word(&capture->vertices[2]);
    words[6] = uv_word(&capture->vertices[2], 0u);
    words[7] = xy_word(&capture->vertices[3]);
    words[8] = uv_word(&capture->vertices[3], 0u);
}

static XgRenderPrivateDrawState draw_state_for(
    const XgFieldCharacterCapture *capture) {
    XgRenderPrivateDrawState state = { 0 };

    state.draw_area_left = capture->draw_area_left;
    state.draw_area_top = capture->draw_area_top;
    state.draw_area_right = capture->draw_area_right;
    state.draw_area_bottom = capture->draw_area_bottom;
    state.draw_offset_x = capture->draw_offset_x;
    state.draw_offset_y = capture->draw_offset_y;
    state.texture_window_mask_x = capture->texture_window_mask_x;
    state.texture_window_mask_y = capture->texture_window_mask_y;
    state.texture_window_offset_x = capture->texture_window_offset_x;
    state.texture_window_offset_y = capture->texture_window_offset_y;
    state.dither = capture->dither;
    state.mask_set = capture->mask_set;
    state.mask_check = capture->mask_check;
    return state;
}

static XgRenderPrivateOriginalPacket original_for(
    const XgFieldCharacterCapture *capture,
    const uint32_t *words,
    uint32_t bucket) {
    XgRenderPrivateOriginalPacket original = { 0 };

    original.words = words;
    original.word_count = XG_RENDER_PRIVATE_FT4_WORD_COUNT;
    original.ot_bucket = bucket;
    original.draw_state = draw_state_for(capture);
    return original;
}

static uint64_t candidate_digest(const XgFieldCharacterCandidate *candidate) {
    const uint8_t *bytes = (const uint8_t *)candidate;
    uint64_t digest = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0u; index < sizeof(*candidate); ++index) {
        digest ^= bytes[index];
        digest *= UINT64_C(1099511628211);
    }
    return digest;
}

static int test_canonical_packet_publishes_semantic_primitive(void) {
    const uint32_t bucket = 73u;
    XgFieldCharacterCapture capture = fixture(5u);
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    uint32_t words[XG_RENDER_PRIVATE_FT4_WORD_COUNT];
    XgRenderPrivateOriginalPacket original;

    packet_for(&capture, words);
    original = original_for(&capture, words, bucket);
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_OK);
    CHECK(xg_render_private_compare_field_character(&candidate, &original, bucket,
                                                    &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_EQUAL);
    CHECK(primitive.triangle_count == 2u);
    CHECK(primitive.material.textured);
    CHECK(!primitive.material.raw_texture);
    CHECK(primitive.material.semi_transparent);
    CHECK(primitive.material.shading == XG_RENDER_IR_SHADING_FLAT);
    CHECK(primitive.material.clut_x == capture.clut_x);
    CHECK(primitive.triangles[0].vertices[2].x ==
          (int32_t)capture.vertices[2].x * INT32_C(65536));
    CHECK(primitive.triangles[1].vertices[0].x ==
          (int32_t)capture.vertices[2].x * INT32_C(65536));
    CHECK(primitive.triangles[1].vertices[1].x ==
          (int32_t)capture.vertices[1].x * INT32_C(65536));
    CHECK(primitive.triangles[1].vertices[2].x ==
          (int32_t)capture.vertices[3].x * INT32_C(65536));
    words[6] |= UINT32_C(0xa5a50000);
    words[8] |= UINT32_C(0x5a5a0000);
    CHECK(xg_render_private_compare_field_character(&candidate, &original, bucket,
                                                    &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_EQUAL);
    return 1;
}

static int test_mismatches_fail_closed_without_output(void) {
    const uint32_t bucket = 91u;
    XgFieldCharacterCapture capture = fixture(7u);
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    XgRenderIrNativePrimitive zero = { 0 };
    uint32_t words[XG_RENDER_PRIVATE_FT4_WORD_COUNT];
    XgRenderPrivateOriginalPacket original;

    packet_for(&capture, words);
    original = original_for(&capture, words, bucket);
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_OK);

    words[6] ^= UINT32_C(0x00000001);
    memset(&primitive, 0xa5, sizeof(primitive));
    CHECK(xg_render_private_compare_field_character(&candidate, &original, bucket,
                                                    &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_PACKET_MISMATCH);
    CHECK(memcmp(&primitive, &zero, sizeof(primitive)) == 0);
    words[6] ^= UINT32_C(0x00000001);

    CHECK(xg_render_private_compare_field_character(&candidate, &original,
                                                    bucket + 1u, &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_OT_BUCKET_MISMATCH);
    original.draw_state.mask_set ^= 1u;
    CHECK(xg_render_private_compare_field_character(&candidate, &original, bucket,
                                                    &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_DRAW_STATE_MISMATCH);
    original.draw_state = draw_state_for(&capture);
    original.word_count = 8u;
    CHECK(xg_render_private_compare_field_character(&candidate, &original, bucket,
                                                    &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_PACKET_LENGTH_MISMATCH);
    original.word_count = XG_RENDER_PRIVATE_FT4_WORD_COUNT;
    original.words = NULL;
    CHECK(xg_render_private_compare_field_character(&candidate, &original, bucket,
                                                    &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_ORIGINAL_UNAVAILABLE);
    original.words = words;
    candidate.tpage ^= 1u;
    CHECK(xg_render_private_compare_field_character(&candidate, &original, bucket,
                                                    &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_CANDIDATE_INVALID);
    candidate.tpage ^= 1u;
    candidate.finalized = 0u;
    CHECK(xg_render_private_compare_field_character(&candidate, &original, bucket,
                                                    &primitive) ==
          XG_RENDER_PRIVATE_COMPARE_CANDIDATE_NOT_FINALIZED);
    return 1;
}

static int test_original_poison_cannot_change_finalized_candidate(void) {
    XgFieldCharacterCapture capture = fixture(11u);
    XgFieldCharacterCandidate candidate;
    uint32_t words[XG_RENDER_PRIVATE_FT4_WORD_COUNT];
    uint64_t before;

    packet_for(&capture, words);
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_OK);
    before = candidate_digest(&candidate);
    memset(words, 0xa5, sizeof(words));
    CHECK(candidate_digest(&candidate) == before);
    return 1;
}

static int test_ten_thousand_packet_exact_candidates(void) {
    uint32_t sequence;

    for (sequence = 0u; sequence < 10000u; ++sequence) {
        const uint32_t bucket = 100u + (sequence & 31u);
        XgFieldCharacterCapture capture = fixture(sequence);
        XgFieldCharacterCandidate candidate;
        XgRenderIrNativePrimitive primitive;
        uint32_t words[XG_RENDER_PRIVATE_FT4_WORD_COUNT];
        XgRenderPrivateOriginalPacket original;

        packet_for(&capture, words);
        original = original_for(&capture, words, bucket);
        CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
              XG_FIELD_CHARACTER_ADAPTER_OK);
        CHECK(xg_render_private_compare_field_character(&candidate, &original,
                                                        bucket, &primitive) ==
              XG_RENDER_PRIVATE_COMPARE_EQUAL);
    }
    return 1;
}

int main(void) {
    return !(test_canonical_packet_publishes_semantic_primitive() &&
             test_mismatches_fail_closed_without_output() &&
             test_original_poison_cannot_change_finalized_candidate() &&
             test_ten_thousand_packet_exact_candidates());
}
