#include "xg_model_ft4_raw.h"

#include "xg_render_quad_builder.h"

#include <limits.h>
#include <string.h>

static int32_t nclip(const XgHost3dProjectedVertex vertices[4]) {
    return (int32_t)vertices[0].x * vertices[1].y +
        (int32_t)vertices[1].x * vertices[2].y +
        (int32_t)vertices[2].x * vertices[0].y -
        (int32_t)vertices[0].x * vertices[2].y -
        (int32_t)vertices[1].x * vertices[0].y -
        (int32_t)vertices[2].x * vertices[1].y;
}

static uint32_t packed_xy(const XgHost3dProjectedVertex *vertex) {
    return (uint16_t)vertex->x | ((uint32_t)(uint16_t)vertex->y << 16u);
}

static int dispatch_mode_is_valid(uint8_t mode) {
    return mode == XG_MODEL_FT4_RAW_DISPATCH_AVERAGE ||
        mode == XG_MODEL_FT4_RAW_DISPATCH_FARTHEST ||
        mode == XG_MODEL_FT4_RAW_DISPATCH_NEAREST ||
        mode == XG_MODEL_FT4_RAW_DISPATCH_AVERAGE_DEPTH_CUE ||
        mode == XG_MODEL_FT4_RAW_DISPATCH_FARTHEST_DEPTH_CUE;
}

static int16_t clamp_i16(int32_t value) {
    if (value < INT16_MIN) return INT16_MIN;
    if (value > INT16_MAX) return INT16_MAX;
    return (int16_t)value;
}

static uint8_t clamp_color(int32_t value) {
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

static uint8_t depth_cue_channel(uint8_t color, int32_t far_color,
                                 int16_t depth_cue) {
    const int16_t step = clamp_i16(far_color - (int32_t)color * 16);
    const int64_t base = (int64_t)color * 16 * 4096;
    const int64_t value = base + (int64_t)depth_cue * step;
    const int32_t mac = value >= 0
        ? (int32_t)(value / 4096)
        : (int32_t)(-(((-value) + 4095) / 4096));

    return clamp_color(mac >= 0 ? mac / 16 : -(((-mac) + 15) / 16));
}

static uint16_t selected_depth(const XgHost3dRotAverage4Output *output,
                               uint8_t mode) {
    uint16_t depth = output->vertices[0].z;
    uint32_t vertex;

    for (vertex = 1u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        if (mode == XG_MODEL_FT4_RAW_DISPATCH_FARTHEST ||
            mode == XG_MODEL_FT4_RAW_DISPATCH_FARTHEST_DEPTH_CUE) {
            if (output->vertices[vertex].z > depth)
                depth = output->vertices[vertex].z;
        } else if (output->vertices[vertex].z < depth) {
            depth = output->vertices[vertex].z;
        }
    }
    return depth;
}

XgModelFt4RawResult xg_model_ft4_raw_build(
    const XgModelFt4RawSource *source, XgModelFt4RawRecord *record) {
    XgHost3dRotAverage4Input input = { 0 };
    XgHost3dRotAverage4Output output;
    XgRenderQuadSource quad = { 0 };
    bool all_below;
    bool all_left;
    bool all_right;
    const int32_t native_margin = source != NULL &&
            source->screen_x_cull_margin > 0
        ? source->screen_x_cull_margin
        : xg_host_3d_native_view_margin();
    uint16_t insertion_depth;
    uint32_t vertex;

    if (source == NULL || record == NULL)
        return XG_MODEL_FT4_RAW_INVALID_ARGUMENT;
    memset(record, 0, sizeof(*record));
    if (source->ordering_shift > 31u ||
        !dispatch_mode_is_valid(source->dispatch_mode) ||
        source->material.shading != XG_RENDER_IR_SHADING_FLAT ||
        !source->material.textured || !source->material.raw_texture ||
        source->material.texture_depth > XG_RENDER_IR_TEXTURE_15_BIT)
        return XG_MODEL_FT4_RAW_INVALID_SOURCE;

    memcpy(input.vertices, source->vertices, sizeof(input.vertices));
    input.projection = source->projection;
    if (!xg_host_3d_rot_average4(&input, &output))
        return XG_MODEL_FT4_RAW_BUILD_FAILED;
    memcpy(record->vertices, output.vertices, sizeof(record->vertices));
    record->projection_flags = output.projection_flags;
    record->nclip = nclip(output.vertices);
    if (source->dispatch_mode == XG_MODEL_FT4_RAW_DISPATCH_AVERAGE ||
        source->dispatch_mode ==
            XG_MODEL_FT4_RAW_DISPATCH_AVERAGE_DEPTH_CUE) {
        insertion_depth = output.ordering_depth;
        record->ordering_bucket =
            output.ordering_depth >> source->ordering_shift;
    } else {
        insertion_depth = selected_depth(&output, source->dispatch_mode);
        record->ordering_bucket = insertion_depth >>
            ((source->ordering_shift + 2u) & 31u);
    }
    all_below = true;
    all_left = true;
    all_right = true;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        all_below &= packed_xy(&output.vertices[vertex]) >=
            source->packed_screen_bottom;
        if (native_margin > 0) {
            all_left &= (int32_t)output.vertices[vertex].x < -native_margin;
            all_right &= (int32_t)output.vertices[vertex].x >=
                (int32_t)source->screen_right + native_margin;
        } else {
            all_left = false;
            all_right &= (uint16_t)output.vertices[vertex].x >=
                source->screen_right;
        }
    }
    record->passed_screen_cull = (int32_t)record->projection_flags >= 0 &&
        record->nclip > 0 && !all_below && !all_left && !all_right;
    record->counter_incremented = record->passed_screen_cull;
    record->accepted = record->counter_incremented && insertion_depth != 0u;

    record->material_word = source->material_word;
    quad.material = source->material;
    if (source->dispatch_mode ==
            XG_MODEL_FT4_RAW_DISPATCH_AVERAGE_DEPTH_CUE ||
        source->dispatch_mode ==
            XG_MODEL_FT4_RAW_DISPATCH_FARTHEST_DEPTH_CUE) {
        const uint8_t red = depth_cue_channel(
            (uint8_t)source->depth_cue_color_word, source->far_color[0],
            output.depth_cue);
        const uint8_t green = depth_cue_channel(
            (uint8_t)(source->depth_cue_color_word >> 8u),
            source->far_color[1],
            output.depth_cue);
        const uint8_t blue = depth_cue_channel(
            (uint8_t)(source->depth_cue_color_word >> 16u),
            source->far_color[2],
            output.depth_cue);

        record->material_word =
            (source->material_word & UINT32_C(0xfe000000)) |
            red | ((uint32_t)green << 8u) | ((uint32_t)blue << 16u);
        quad.material.raw_texture = false;
    }
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        quad.vertices[vertex] = (XgRenderQuadSourceVertex){
            .u = source->uv[vertex][0],
            .v = source->uv[vertex][1],
            .red = (uint8_t)record->material_word,
            .green = (uint8_t)(record->material_word >> 8u),
            .blue = (uint8_t)(record->material_word >> 16u),
        };
        xg_render_quad_set_projected_position(
            &quad.vertices[vertex], &output.vertices[vertex]);
    }
    if (xg_render_quad_build_primitive(&quad, &record->primitive) !=
        XG_RENDER_QUAD_BUILDER_OK)
        return XG_MODEL_FT4_RAW_BUILD_FAILED;
    return XG_MODEL_FT4_RAW_OK;
}
