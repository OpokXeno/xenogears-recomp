#include "xg_world_models.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static const uint8_t dispatch_modes[XG_WORLD_MODELS_DISPATCH_SELECTOR_COUNT] = {
    4u, 4u, 5u, 5u, 0u, 0u, 2u, 2u, 3u, 3u,
};

static int32_t wrap_i32(uint32_t value) {
    if (value <= INT32_MAX) return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int32_t add_i32(int32_t left, int32_t right) {
    return wrap_i32((uint32_t)left + (uint32_t)right);
}

static int32_t subtract_i32(int32_t left, int32_t right) {
    return wrap_i32((uint32_t)left - (uint32_t)right);
}

static int32_t negate_i32(int32_t value) {
    return wrap_i32(0u - (uint32_t)value);
}

static int32_t shift_right_12_floor(int32_t value) {
    if (value >= 0) return value / 4096;
    return (int32_t)(-((-(int64_t)value + 4095) / 4096));
}

static int32_t wrap_world_coordinate(int32_t value, int32_t wrap) {
    const int32_t span = wrap_i32((uint32_t)wrap << 11u);

    if (value < -0x4000) return add_i32(value, span);
    if (value > 0x4000) return subtract_i32(value, span);
    return value;
}

static void set_projection(const XgWorldModelsSource *source,
                           const XgHost3dMatrix *matrix,
                           XgHost3dProjection *projection) {
    memset(projection, 0, sizeof(*projection));
    memcpy(projection->rotation, matrix->rotation,
           sizeof(projection->rotation));
    memcpy(projection->translation, matrix->translation,
           sizeof(projection->translation));
    projection->screen_offset_x = source->gte.screen_offset_x;
    projection->screen_offset_y = source->gte.screen_offset_y;
    projection->projection_distance = source->gte.projection_distance;
    projection->depth_cue_a = source->gte.depth_cue_a;
    projection->depth_cue_b = source->gte.depth_cue_b;
    projection->average_z_scale4 = source->gte.average_z_scale4;
}

static XgWorldModelsResult validate_source(
    const XgWorldModelsSource *source,
    XgWorldModelsRecordOutput *records,
    uint32_t record_capacity,
    XgWorldModelsNodeSideEffect *node_side_effects,
    uint32_t node_side_effect_capacity,
    uint32_t *active_node_count) {
    uint32_t index;
    uint32_t count = 0u;

    if (source == NULL || active_node_count == NULL)
        return XG_WORLD_MODELS_INVALID_ARGUMENT;
    if (source->record_count <= 0) {
        *active_node_count = 0u;
        return XG_WORLD_MODELS_OK;
    }
    if (source->records == NULL || records == NULL)
        return XG_WORLD_MODELS_INVALID_ARGUMENT;
    if ((uint32_t)source->record_count > record_capacity)
        return XG_WORLD_MODELS_CAPACITY_EXCEEDED;
    if (source->buffer_index > 1u) return XG_WORLD_MODELS_INVALID_SOURCE;

    for (index = 0u; index < (uint32_t)source->record_count; ++index) {
        const XgWorldModelsRecordSource *record = &source->records[index];

        if (record->state != 0) continue;
        if (record->transform_node_count != 0u &&
            record->transform_nodes == NULL)
            return XG_WORLD_MODELS_INVALID_ARGUMENT;
        if (UINT32_MAX - count < record->transform_node_count)
            return XG_WORLD_MODELS_INVALID_SOURCE;
        count += record->transform_node_count;
    }
    if (count > node_side_effect_capacity)
        return XG_WORLD_MODELS_CAPACITY_EXCEEDED;
    if (count != 0u && node_side_effects == NULL)
        return XG_WORLD_MODELS_INVALID_ARGUMENT;
    *active_node_count = count;
    return XG_WORLD_MODELS_OK;
}

XgWorldModelsResult xg_world_models_build(
    const XgWorldModelsSource *source,
    XgWorldModelsRecordOutput *records,
    uint32_t record_capacity,
    XgWorldModelsNodeSideEffect *node_side_effects,
    uint32_t node_side_effect_capacity,
    XgWorldModelsBuildSummary *summary) {
    const XgHost3dLongVector half_scale = { 0x800, 0x800, 0x800 };
    const XgHost3dVector origin = { 0, 0, 0, 0u };
    XgWorldModelsResult validation;
    uint32_t active_node_count;
    uint32_t node_output = 0u;
    uint32_t index;

    if (summary == NULL) return XG_WORLD_MODELS_INVALID_ARGUMENT;
    memset(summary, 0, sizeof(*summary));
    validation = validate_source(source, records, record_capacity,
                                 node_side_effects,
                                 node_side_effect_capacity,
                                 &active_node_count);
    if (validation != XG_WORLD_MODELS_OK) return validation;
    (void)active_node_count;

    summary->entry_side_effects.scratch_scale[0] = 0x800u;
    summary->entry_side_effects.scratch_scale[1] = 0x800u;
    summary->entry_side_effects.scratch_scale[2] = 0x800u;
    summary->entry_side_effects.resident_cull_mode = 3u;
    if (source->record_count <= 0) return XG_WORLD_MODELS_OK;

    memset(records, 0,
           sizeof(*records) * (size_t)(uint32_t)source->record_count);
    summary->record_count = (uint32_t)source->record_count;

    for (index = 0u; index < (uint32_t)source->record_count; ++index) {
        const XgWorldModelsRecordSource *record = &source->records[index];
        XgWorldModelsRecordOutput *output = &records[index];
        XgHost3dMatrix transform;
        XgHost3dMatrix composed;
        XgHost3dProjectedVertex projected_origin;
        const uint32_t coarse_depth_limit =
            xg_host_3d_native_view_depth_limit(
                XG_WORLD_MODELS_COARSE_DEPTH_LIMIT);
        uint32_t node;
        uint32_t flags;
        int32_t relative_x;
        int32_t relative_z;

        output->source_index = index;
        output->disposition = XG_WORLD_MODELS_INACTIVE;
        if (record->state != 0) continue;

        transform = record->matrix;
        transform.translation[0] = record->position_x;
        transform.translation[1] = record->position_y;
        transform.translation[2] = negate_i32(record->stored_z);
        for (node = 0u; node < record->transform_node_count; ++node) {
            const XgWorldModelsTransformNodeSource *source_node =
                &record->transform_nodes[node];
            XgHost3dMatrix node_transform = source_node->matrix;
            XgWorldModelsNodeSideEffect *effect =
                &node_side_effects[node_output++];

            node_transform.translation[0] = source_node->position_x;
            node_transform.translation[1] = source_node->position_y;
            node_transform.translation[2] = negate_i32(source_node->stored_z);
            effect->guest_address = source_node->guest_address;
            memcpy(effect->translation, node_transform.translation,
                   sizeof(effect->translation));
            summary->node_side_effect_count = node_output;
            if (!xg_host_3d_comp_matrix(&node_transform, &transform,
                                        &composed))
                return XG_WORLD_MODELS_BUILD_FAILED;
            transform = composed;
        }

        relative_x = subtract_i32(
            transform.translation[0],
            shift_right_12_floor(source->camera_x_12_12));
        relative_z = subtract_i32(
            negate_i32(shift_right_12_floor(source->camera_z_12_12)),
            transform.translation[2]);
        relative_x = wrap_world_coordinate(relative_x, source->wrap_x);
        relative_z = wrap_world_coordinate(relative_z, source->wrap_z);
        transform.translation[0] = relative_x;
        transform.translation[2] = negate_i32(relative_z);
        if (!xg_host_3d_scale_matrix(&transform, &half_scale) ||
            !xg_host_3d_comp_matrix(&source->camera_matrix, &transform,
                                    &output->object_to_view))
            return XG_WORLD_MODELS_BUILD_FAILED;

        set_projection(source, &output->object_to_view, &output->projection);
        if (!xg_host_3d_rtps(&output->projection, &origin,
                             &projected_origin, &flags))
            return XG_WORLD_MODELS_BUILD_FAILED;
        output->coarse_flags = flags;
        output->coarse_depth = projected_origin.z;
        if ((flags & UINT32_C(0x80000000)) != 0u) {
            output->disposition = XG_WORLD_MODELS_COARSE_FLAG_REJECTED;
            continue;
        }
        if (projected_origin.z >= coarse_depth_limit) {
            output->disposition = XG_WORLD_MODELS_COARSE_DEPTH_REJECTED;
            continue;
        }
        if (record->dispatch_selector < 0 ||
            (uint16_t)record->dispatch_selector >=
                (uint16_t)XG_WORLD_MODELS_DISPATCH_SELECTOR_COUNT)
            return XG_WORLD_MODELS_INVALID_SOURCE;

        output->resident_call.model_header_address =
            record->model_header_address;
        output->resident_call.packet_base_address =
            record->packet_base[source->buffer_index];
        output->resident_call.ordering_table_address =
            source->ordering_table_address;
        output->resident_call.dispatch_mode =
            dispatch_modes[record->dispatch_selector];
        output->disposition = XG_WORLD_MODELS_RESIDENT_DISPATCH;
        ++summary->resident_dispatch_count;
    }
    return XG_WORLD_MODELS_OK;
}

XgModelFt4RawResult xg_world_models_build_raw_ft4(
    const XgWorldModelsRecordOutput *world_record,
    uint32_t primitive_index,
    const XgModelFt4RawSource *source_values,
    XgModelFt4RawRecord *record) {
    XgModelFt4RawSource adapted;
    uint32_t packet_offset;

    if (world_record == NULL || source_values == NULL || record == NULL)
        return XG_MODEL_FT4_RAW_INVALID_ARGUMENT;
    if (world_record->disposition != XG_WORLD_MODELS_RESIDENT_DISPATCH ||
        primitive_index > UINT32_MAX / XG_WORLD_MODELS_PACKET_STRIDE)
        return XG_MODEL_FT4_RAW_INVALID_SOURCE;
    packet_offset = primitive_index * XG_WORLD_MODELS_PACKET_STRIDE;
    if (world_record->resident_call.packet_base_address >
        UINT32_MAX - packet_offset)
        return XG_MODEL_FT4_RAW_INVALID_SOURCE;

    adapted = *source_values;
    adapted.projection = world_record->projection;
    adapted.dispatch_mode = world_record->resident_call.dispatch_mode;
    adapted.packet_address =
        world_record->resident_call.packet_base_address + packet_offset;
    return xg_model_ft4_raw_build(&adapted, record);
}
