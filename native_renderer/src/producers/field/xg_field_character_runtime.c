#include "xg_field_character_runtime.h"

#include "cpu_state.h"
#include "gpu.h"
#include "xg_field_character_adapter.h"
#include "xg_field_character_capture.h"
#include "xg_field_character_source_adapter.h"
#include "xg_render_ir.h"
#include "xg_render_private_compare.h"

#include <stddef.h>
#include <string.h>

static bool read_semantic_u16(void *context, uint32_t address,
                              uint16_t *out_value) {
    CPUState *cpu = (CPUState *)context;

    if (!cpu || !cpu->read_half || !out_value) return false;
    *out_value = cpu->read_half(address);
    return true;
}

XgFieldCharacterRuntimeResult xg_field_character_runtime_build_candidate(
    const XgFieldCharacterSourceSnapshot *snapshot,
    XgFieldCharacterRuntimeCandidate *out_candidate) {
    XgFieldCharacterSourceAdapterResult result;

    if (snapshot == NULL || out_candidate == NULL)
        return XG_FIELD_CHARACTER_RUNTIME_INVALID_ARGUMENT;
    memset(out_candidate, 0, sizeof(*out_candidate));
    result = xg_field_character_source_adapter_build(
        snapshot, &out_candidate->candidate, &out_candidate->source_derived);
    if (result == XG_FIELD_CHARACTER_SOURCE_ADAPTER_OK) {
        out_candidate->draw_area_left = snapshot->raster.draw_area_left;
        out_candidate->draw_area_top = snapshot->raster.draw_area_top;
        out_candidate->draw_area_right = snapshot->raster.draw_area_right;
        out_candidate->draw_area_bottom = snapshot->raster.draw_area_bottom;
        out_candidate->draw_offset_x = snapshot->raster.draw_offset_x;
        out_candidate->draw_offset_y = snapshot->raster.draw_offset_y;
        out_candidate->texture_window_mask_x =
            snapshot->raster.texture_window_mask_x;
        out_candidate->texture_window_mask_y =
            snapshot->raster.texture_window_mask_y;
        out_candidate->texture_window_offset_x =
            snapshot->raster.texture_window_offset_x;
        out_candidate->texture_window_offset_y =
            snapshot->raster.texture_window_offset_y;
        out_candidate->dither = snapshot->raster.dither;
        out_candidate->mask_set = snapshot->raster.mask_set;
        out_candidate->mask_check = snapshot->raster.mask_check;
        out_candidate->valid = 1u;
        return XG_FIELD_CHARACTER_RUNTIME_OK;
    }
    if (result == XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_TRANSFORM ||
        result == XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_MATERIAL ||
        result == XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_ORDERING)
        return XG_FIELD_CHARACTER_RUNTIME_INCOMPLETE_SOURCE;
    if (result == XG_FIELD_CHARACTER_SOURCE_ADAPTER_INVALID_ARGUMENT)
        return XG_FIELD_CHARACTER_RUNTIME_INVALID_ARGUMENT;
    return XG_FIELD_CHARACTER_RUNTIME_ADAPTER_FAILED;
}

XgFieldCharacterRuntimeResult xg_field_character_runtime_build_shadow_candidate(
    CPUState *cpu, const PsxXgRenderFt4Geometry *geometry,
    XgFieldCharacterRuntimeCandidate *out_candidate) {
    XgFieldCharacterGeometryInput geometry_input = { 0 };
    XgFieldCharacterDrawStateInput draw_input = { 0 };
    XgFieldCharacterSemanticReader reader = { cpu, read_semantic_u16 };
    XgFieldCharacterCapture capture;
    XgFieldCharacterCaptureResult capture_result;
    GpuDrawState draw;
    size_t index;

    if (!cpu || !geometry || !out_candidate || !cpu->read_half ||
        !geometry->host_transformed)
        return XG_FIELD_CHARACTER_RUNTIME_INVALID_ARGUMENT;
    memset(out_candidate, 0, sizeof(*out_candidate));
    memset(&draw, 0, sizeof(draw));
    gpu_get_draw_state(&draw);
    geometry_input.packet_guest_address = geometry->packet_guest_address;
    geometry_input.semantic_template =
        (XgFieldCharacterSemanticTemplate)geometry->semantic_template;
    for (index = 0u; index < XG_FIELD_CHARACTER_VERTEX_COUNT; ++index) {
        geometry_input.x[index] = geometry->x[index];
        geometry_input.y[index] = geometry->y[index];
    }
    draw_input.draw_area_left = draw.left;
    draw_input.draw_area_top = draw.top;
    draw_input.draw_area_right = draw.right;
    draw_input.draw_area_bottom = draw.bottom;
    draw_input.draw_offset_x = draw.offset_x;
    draw_input.draw_offset_y = draw.offset_y;
    draw_input.texture_window_mask_x = draw.texture_window_mask_x;
    draw_input.texture_window_mask_y = draw.texture_window_mask_y;
    draw_input.texture_window_offset_x = draw.texture_window_offset_x;
    draw_input.texture_window_offset_y = draw.texture_window_offset_y;
    draw_input.dither = draw.dither;
    draw_input.mask_set = draw.mask_set;
    draw_input.mask_check = draw.mask_check;
    capture_result = xg_field_character_capture_build(
        &geometry_input, &draw_input, &reader, &capture);
    if (capture_result == XG_FIELD_CHARACTER_CAPTURE_UNMAPPED_TEMPLATE)
        return XG_FIELD_CHARACTER_RUNTIME_UNMAPPED_TEMPLATE;
    if (capture_result == XG_FIELD_CHARACTER_CAPTURE_SEMANTIC_READ_FAILED)
        return XG_FIELD_CHARACTER_RUNTIME_SEMANTIC_READ_FAILED;
    if (capture_result == XG_FIELD_CHARACTER_CAPTURE_INVALID_MATERIAL)
        return XG_FIELD_CHARACTER_RUNTIME_INVALID_MATERIAL;
    if (capture_result != XG_FIELD_CHARACTER_CAPTURE_OK)
        return XG_FIELD_CHARACTER_RUNTIME_INVALID_ARGUMENT;
    if (xg_field_character_adapter_build(&capture, &out_candidate->candidate) !=
        XG_FIELD_CHARACTER_ADAPTER_OK)
        return XG_FIELD_CHARACTER_RUNTIME_ADAPTER_FAILED;
    out_candidate->packet_guest_address = geometry->packet_guest_address;
    out_candidate->draw_area_left = draw.left;
    out_candidate->draw_area_top = draw.top;
    out_candidate->draw_area_right = draw.right;
    out_candidate->draw_area_bottom = draw.bottom;
    out_candidate->draw_offset_x = draw.offset_x;
    out_candidate->draw_offset_y = draw.offset_y;
    out_candidate->texture_window_mask_x = draw.texture_window_mask_x;
    out_candidate->texture_window_mask_y = draw.texture_window_mask_y;
    out_candidate->texture_window_offset_x = draw.texture_window_offset_x;
    out_candidate->texture_window_offset_y = draw.texture_window_offset_y;
    out_candidate->dither = draw.dither;
    out_candidate->mask_set = draw.mask_set;
    out_candidate->mask_check = draw.mask_check;
    out_candidate->valid = 1u;
    return XG_FIELD_CHARACTER_RUNTIME_OK;
}

XgFieldCharacterRuntimeResult xg_field_character_runtime_compare_original(
    CPUState *cpu, const XgFieldCharacterRuntimeCandidate *runtime_candidate,
    uint32_t original_ot_bucket, uint32_t authenticated_ot_bucket,
    XgRenderIrNativePrimitive *out_primitive, uint32_t *out_compare_result,
    uint32_t *out_mismatch_word, uint32_t *out_mismatch_byte) {
    uint32_t words[XG_RENDER_PRIVATE_FT4_WORD_COUNT];
    XgRenderPrivateOriginalPacket original = { 0 };
    XgRenderPrivateCompareResult compare_result;
    size_t index;

    if (!cpu || !runtime_candidate || !out_primitive || !cpu->read_word ||
        runtime_candidate->valid != 1u)
        return XG_FIELD_CHARACTER_RUNTIME_INVALID_ARGUMENT;
    for (index = 0u; index < XG_RENDER_PRIVATE_FT4_WORD_COUNT; ++index)
        words[index] = cpu->read_word(
            runtime_candidate->packet_guest_address + 4u + (uint32_t)index * 4u);
    original.words = words;
    original.word_count = XG_RENDER_PRIVATE_FT4_WORD_COUNT;
    original.ot_bucket = original_ot_bucket;
    original.draw_state.draw_area_left = runtime_candidate->draw_area_left;
    original.draw_state.draw_area_top = runtime_candidate->draw_area_top;
    original.draw_state.draw_area_right = runtime_candidate->draw_area_right;
    original.draw_state.draw_area_bottom = runtime_candidate->draw_area_bottom;
    original.draw_state.draw_offset_x = runtime_candidate->draw_offset_x;
    original.draw_state.draw_offset_y = runtime_candidate->draw_offset_y;
    original.draw_state.texture_window_mask_x =
        runtime_candidate->texture_window_mask_x;
    original.draw_state.texture_window_mask_y =
        runtime_candidate->texture_window_mask_y;
    original.draw_state.texture_window_offset_x =
        runtime_candidate->texture_window_offset_x;
    original.draw_state.texture_window_offset_y =
        runtime_candidate->texture_window_offset_y;
    original.draw_state.dither = runtime_candidate->dither;
    original.draw_state.mask_set = runtime_candidate->mask_set;
    original.draw_state.mask_check = runtime_candidate->mask_check;
    compare_result = xg_render_private_compare_field_character_detailed(
        &runtime_candidate->candidate, &original, authenticated_ot_bucket,
        out_primitive, out_mismatch_word, out_mismatch_byte);
    if (out_compare_result) *out_compare_result = (uint32_t)compare_result;
    if (compare_result != XG_RENDER_PRIVATE_COMPARE_EQUAL)
        return XG_FIELD_CHARACTER_RUNTIME_COMPARE_FAILED;
    return XG_FIELD_CHARACTER_RUNTIME_OK;
}
