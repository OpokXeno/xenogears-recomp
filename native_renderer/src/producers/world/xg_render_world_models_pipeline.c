#include "xg_render_world_models_pipeline.h"

#include "gpu.h"
#include "xg_render_backend.h"
#include "xg_world_models_native.h"

#include <limits.h>
#include <string.h>

enum {
    XG_RENDER_WORLD_MODEL_RECORD_CAPACITY = 256u,
    XG_RENDER_WORLD_MODEL_NODE_CAPACITY = 1024u,
    XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY = 256u,
    XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY = 4096u,
    XG_RENDER_WORLD_MODEL_ANCHOR_CAPACITY =
        XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY * 4u,
};

enum {
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PRECONDITION = 1u,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CALLER_RETURN,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CONTRACT,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_GLOBAL_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_NODE_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CULLED_DISPATCH,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_DISPATCH_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_SOURCE_IDENTITY,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_MISSING,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_INACTIVE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_OWNER,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_EPOCH,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_MODEL,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_PACKET_BASE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_ORDINAL,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_ATTRIBUTE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_FAMILY,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_WORD_COUNT,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_PACKET,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_WORD_COUNT,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_COUNTER,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PACKET_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_MASK,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_ACCEPTED,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_TAG,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PACKET_WORD,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OT_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PRIMITIVE_TOTAL,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_COMMIT_OVERFLOW,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_COMMIT_TOTAL,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_NATIVE_RESULT_BASE = 0x100u,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_BUILD_RESULT_BASE = 0x200u,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_READ_BASE = 0x300u,
};

typedef struct XgRenderWorldModelsNativeState {
    XgWorldModelsRecordSource
        record_sources[XG_RENDER_WORLD_MODEL_RECORD_CAPACITY];
    XgWorldModelsTransformNodeSource
        transform_nodes[XG_RENDER_WORLD_MODEL_NODE_CAPACITY];
    XgWorldModelsRecordOutput records[XG_RENDER_WORLD_MODEL_RECORD_CAPACITY];
    XgWorldModelsNodeSideEffect
        node_side_effects[XG_RENDER_WORLD_MODEL_NODE_CAPACITY];
    XgWorldModelsNativeDispatch
        dispatches[XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY];
    XgWorldModelsNativeDispatchOutput
        dispatch_outputs[XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY];
    XgWorldModelsNativePrimitiveSource
        primitives[XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY];
    XgWorldModelsNativePrimitiveOutput
        outputs[XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY];
    GpuRenderInterpolationVertexAnchor
        anchors[XG_RENDER_WORLD_MODEL_ANCHOR_CAPACITY];
    uint32_t anchor_seen[UINT16_MAX / 32u + 1u];
    const XgRenderWorldModelTemplate
        *templates[XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY];
    XgWorldModelsNativePreparation preparation;
    XgWorldModelsNativeCommit expected_commit;
    uint32_t ot_heads[XG_WORLD_MODELS_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_MODELS_OT_BUCKET_COUNT];
    uint64_t authentication_generation;
    uint32_t entry_stack_pointer;
    uint32_t accepted_count;
    CPUState *owner_cpu;
    const XgRenderWorldModelsPipelineServices *services;
    bool valid;
} XgRenderWorldModelsNativeState;

static XgRenderWorldModelsNativeState native_state;
static PsxXgRenderWorldNativeSnapshot snapshot;

static bool services_valid(
        const XgRenderWorldModelsPipelineServices *services) {
    return services != NULL && services->repository != NULL &&
        services->cutover_ready != NULL &&
        services->authentication_generation != NULL &&
        services->authorize_guest_range != NULL &&
        services->stack_address_is_valid != NULL &&
        services->interpolation_scene_generation != NULL &&
        services->screen_x_cull_margin != NULL &&
        services->begin_submission != NULL && services->stage_native != NULL &&
        services->stage_temporal != NULL && services->abort_submission != NULL;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static uint32_t normalized_word_address(uint32_t address) {
    return address & UINT32_C(0x001ffffc);
}

static int16_t low_s16(uint32_t value) {
    return (int16_t)(uint16_t)value;
}

static bool read_u8(void *context, uint32_t address, uint8_t *out_value) {
    const XgRenderWorldModelRepositoryReaderContext *reader = context;

    if (reader == NULL || reader->cpu == NULL ||
        reader->cpu->read_byte == NULL || out_value == NULL ||
        reader->services == NULL ||
        !reader->services->authorize_guest_range(address, 1u, 1u, true))
        return false;
    *out_value = reader->cpu->read_byte(address);
    return true;
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    const XgRenderWorldModelRepositoryReaderContext *reader = context;

    if (reader == NULL || reader->cpu == NULL ||
        reader->cpu->read_half == NULL || out_value == NULL ||
        reader->services == NULL ||
        !reader->services->authorize_guest_range(address, 2u, 2u, true))
        return false;
    *out_value = reader->cpu->read_half(address);
    return true;
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
    const XgRenderWorldModelRepositoryReaderContext *reader = context;

    if (reader == NULL || reader->cpu == NULL ||
        reader->cpu->read_word == NULL || out_value == NULL ||
        reader->services == NULL ||
        !reader->services->authorize_guest_range(address, 4u, 4u, true))
        return false;
    *out_value = reader->cpu->read_word(address);
    return true;
}

static bool authorize_range(
        void *context, XgWorldModelsNativeRangeKind kind,
        uint32_t address, uint32_t size) {
    const XgRenderWorldModelRepositoryReaderContext *reader = context;
    uint32_t alignment;

    if (reader == NULL || reader->services == NULL) return false;
    switch (kind) {
    case XG_WORLD_MODELS_NATIVE_RANGE_RECORDS:
    case XG_WORLD_MODELS_NATIVE_RANGE_TRANSFORM_NODE:
    case XG_WORLD_MODELS_NATIVE_RANGE_MODEL_HEADER:
    case XG_WORLD_MODELS_NATIVE_RANGE_PACKET_OUTPUT:
    case XG_WORLD_MODELS_NATIVE_RANGE_ORDERING_TABLE_OUTPUT:
    case XG_WORLD_MODELS_NATIVE_RANGE_ATTRIBUTE:
    case XG_WORLD_MODELS_NATIVE_RANGE_VERTEX:
    case XG_WORLD_MODELS_NATIVE_RANGE_AUXILIARY_VERTEX:
        alignment = 4u;
        break;
    case XG_WORLD_MODELS_NATIVE_RANGE_TOPOLOGY:
        alignment = 2u;
        break;
    default:
        return false;
    }
    return reader->services->authorize_guest_range(
        address, size, alignment, false);
}

static void capture_gte_state(
        const CPUState *cpu, XgWorldModelsGteState *gte) {
    *gte = (XgWorldModelsGteState){
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .depth_cue_a = low_s16(cpu->gte_ctrl[27]),
        .depth_cue_b = (int32_t)cpu->gte_ctrl[28],
        .average_z_scale3 = low_s16(cpu->gte_ctrl[29]),
        .average_z_scale4 = low_s16(cpu->gte_ctrl[30]),
        .far_color = {
            (int32_t)cpu->gte_ctrl[21],
            (int32_t)cpu->gte_ctrl[22],
            (int32_t)cpu->gte_ctrl[23],
        },
    };
}

static bool begin_submission(void *context) {
    const XgRenderWorldModelsNativeState *workspace = context;

    return workspace != NULL && workspace->services != NULL &&
        workspace->services->begin_submission();
}

static bool stage_primitive(
        void *context, const XgRenderIrNativePrimitive *primitive,
        uint32_t packet_address, uint32_t primitive_index) {
    const XgRenderWorldModelsNativeState *workspace = context;
    const XgWorldModelsNativePrimitiveSource *source;

    if (workspace == NULL || workspace->services == NULL ||
        primitive_index >= workspace->preparation.primitive_count)
        return false;
    source = &workspace->primitives[primitive_index];
    if (source->source_index >
        (UINT32_MAX - source->primitive_index) / 4096u)
        return false;
    return workspace->services->stage_native(
        primitive, packet_address,
        UINT32_C(0x66000000) |
            ((packet_address & UINT32_C(0x001ffffc)) >> 2u),
        source->model_header_address & UINT32_C(0x1fffffff),
        source->source_index * 4096u + source->primitive_index);
}

static const XgRenderIrVertex *source_vertex(
        const XgWorldModelsNativePrimitiveOutput *output,
        uint32_t source_vertex_index) {
    if (output == NULL || source_vertex_index >= XG_HOST_3D_VERTEX_COUNT ||
        output->primitive.triangle_count == 0u)
        return NULL;
    if (source_vertex_index < 3u)
        return &output->primitive.triangles[0].vertices[source_vertex_index];
    if (output->primitive.triangle_count < 2u) return NULL;
    return &output->primitive.triangles[1].vertices[2];
}

static bool collect_interpolation_anchors(
        XgRenderWorldModelsNativeState *workspace, uint64_t scene_id,
        uint32_t *out_anchor_count, uint32_t *out_failure_detail) {
    uint32_t anchor_count = 0u;

    if (out_failure_detail != NULL) *out_failure_detail = 0u;
    if (workspace == NULL || scene_id == 0u || out_anchor_count == NULL ||
        out_failure_detail == NULL)
        return false;
    *out_anchor_count = 0u;
    for (uint32_t dispatch_index = 0u;
         dispatch_index < workspace->preparation.dispatch_count;
         ++dispatch_index) {
        const XgWorldModelsNativeDispatch *dispatch =
            &workspace->dispatches[dispatch_index];

        if (dispatch->primitive_start > workspace->preparation.primitive_count ||
            dispatch->primitive_count >
                workspace->preparation.primitive_count -
                    dispatch->primitive_start) {
            *out_failure_detail = 1u;
            return false;
        }
        memset(workspace->anchor_seen, 0, sizeof(workspace->anchor_seen));
        for (uint32_t primitive_offset = 0u;
             primitive_offset < dispatch->primitive_count;
             ++primitive_offset) {
            const uint32_t primitive_index =
                dispatch->primitive_start + primitive_offset;
            const XgWorldModelsNativePrimitiveSource *source =
                &workspace->primitives[primitive_index];
            const XgWorldModelsNativePrimitiveOutput *output =
                &workspace->outputs[primitive_index];

            if (source->dispatch_index != dispatch_index) {
                *out_failure_detail = 2u;
                return false;
            }
            for (uint32_t source_vertex_index = 0u;
                 source_vertex_index < source->vertex_count;
                 ++source_vertex_index) {
                const uint32_t topology_vertex =
                    source->topology[source_vertex_index];
                const uint32_t word = topology_vertex / 32u;
                const uint32_t mask =
                    UINT32_C(1) << (topology_vertex % 32u);

                if ((workspace->anchor_seen[word] & mask) != 0u) continue;
                if (anchor_count == XG_RENDER_WORLD_MODEL_ANCHOR_CAPACITY) {
                    *out_anchor_count = anchor_count;
                    *out_failure_detail = 3u;
                    return false;
                }
                const XgRenderIrVertex *vertex =
                    source_vertex(output, source_vertex_index);
                if (vertex == NULL) {
                    *out_anchor_count = anchor_count;
                    *out_failure_detail = 4u;
                    return false;
                }
                const XgRenderBackendStatus status =
                    xg_render_backend_translate_anchor(
                        &output->primitive.material, vertex, scene_id,
                        source->model_header_address & UINT32_C(0x1fffffff),
                        &workspace->anchors[anchor_count]);
                if (status != XG_RENDER_BACKEND_OK) {
                    *out_anchor_count = anchor_count;
                    *out_failure_detail =
                        UINT32_C(0x100) + (uint32_t)status;
                    return false;
                }
                workspace->anchor_seen[word] |= mask;
                ++anchor_count;
            }
        }
    }
    *out_anchor_count = anchor_count;
    return true;
}

static bool snapshot_can_record(uint32_t primitive_count) {
    return snapshot.native_cutover_count != UINT64_MAX &&
        snapshot.native_primitive_count <= UINT64_MAX - primitive_count;
}

static void record_failure(
        PsxXgRenderWorldNativeFailureStage stage, uint32_t detail,
        uint32_t anchor_count) {
    if (snapshot.native_failure_count == 0u) {
        snapshot.first_failure_stage = (uint32_t)stage;
        snapshot.first_failure_detail = detail;
        snapshot.first_anchor_count = anchor_count;
    }
    if (snapshot.native_failure_count != UINT64_MAX)
        ++snapshot.native_failure_count;
    snapshot.last_failure_stage = (uint32_t)stage;
    snapshot.last_failure_detail = detail;
    snapshot.last_anchor_count = anchor_count;
}

bool xg_render_world_models_prepare(
        CPUState *cpu, const XgRenderWorldModelsPipelineServices *services) {
    XgRenderWorldModelsNativeState *workspace = &native_state;
    XgWorldModelsNativePreparation preparation;
    XgWorldModelsNativeCommit commit = {0};
    XgWorldModelsNativeRequest request = {0};
    XgWorldModelsNativeAuthenticatedReader reader;
    XgWorldModelsNativeWorkspace native_workspace;
    XgWorldModelsNativeResult native_result;
    XgRenderWorldModelRepositoryReaderContext reader_context;
    GpuDrawState draw = {0};
    uint64_t generation;
    uint32_t accepted_count = 0u;
    uint32_t failure_detail =
        XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PRECONDITION;
    uint32_t primitive_index;

    if (!services_valid(services) || !services->cutover_ready() ||
        workspace->valid || cpu == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || cpu->read_byte == NULL ||
        !services->stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] < 0x40u ||
        !services->authorize_guest_range(
            cpu->gpr[29] - 0x40u, 0x40u, 4u, false) ||
        !services->authentication_generation(&generation)) {
        record_failure(PSX_XG_RENDER_WORLD_NATIVE_FAILURE_PREPARE,
                       failure_detail, 0u);
        return false;
    }
    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CALLER_RETURN;
    if (!physical_address_equals(
            cpu->gpr[31], XG_WORLD_MODELS_PRODUCER_CONTINUATION_0) &&
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_MODELS_PRODUCER_CONTINUATION_1) &&
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_MODELS_PRODUCER_CONTINUATION_2) &&
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_MODELS_PRODUCER_CONTINUATION_3))
        goto fail;

    gpu_get_draw_state(&draw);
    request.authentication_generation = generation;
    request.entry_pc = XG_WORLD_MODELS_PRODUCER_ENTRY;
    request.caller_return = cpu->gpr[31];
    request.guest_xclip_bound = psx_ws_xclip_bound(
        cpu->read_word(XG_WORLD_MODELS_SCREEN_RIGHT_GLOBAL));
    capture_gte_state(cpu, &request.gte);
    request.raster = (XgWorldModelsNativeRasterState){
        .draw_area_left = draw.left,
        .draw_area_top = draw.top,
        .draw_area_right = draw.right,
        .draw_area_bottom = draw.bottom,
        .draw_offset_x = draw.offset_x,
        .draw_offset_y = draw.offset_y,
        .texture_window_mask_x = draw.texture_window_mask_x,
        .texture_window_mask_y = draw.texture_window_mask_y,
        .texture_window_offset_x = draw.texture_window_offset_x,
        .texture_window_offset_y = draw.texture_window_offset_y,
        .dither = draw.dither,
        .mask_set = draw.mask_set,
        .mask_check = draw.mask_check,
    };
    request.projection_state_authenticated = true;
    request.lighting_state_authenticated = true;
    reader_context = (XgRenderWorldModelRepositoryReaderContext){
        .cpu = cpu,
        .services = services->repository,
    };
    reader = (XgWorldModelsNativeAuthenticatedReader){
        .context = &reader_context,
        .read_u8 = read_u8,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .read_packet_template =
            xg_render_world_model_repository_read_packet_template,
        .authorize_range = authorize_range,
        .authentication_generation = generation,
        .authenticated = true,
    };
    native_workspace = (XgWorldModelsNativeWorkspace){
        .record_sources = workspace->record_sources,
        .record_capacity = XG_RENDER_WORLD_MODEL_RECORD_CAPACITY,
        .transform_nodes = workspace->transform_nodes,
        .transform_node_capacity = XG_RENDER_WORLD_MODEL_NODE_CAPACITY,
    };
    memset(workspace->ot_touched, 0, sizeof(workspace->ot_touched));
    xg_render_world_model_repository_clear_template_read_failure();
    native_result = xg_world_models_native_prepare(
        &request, &reader, &native_workspace, workspace->records,
        XG_RENDER_WORLD_MODEL_RECORD_CAPACITY, workspace->node_side_effects,
        XG_RENDER_WORLD_MODEL_NODE_CAPACITY, workspace->dispatches,
        XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY, workspace->primitives,
        XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY, &preparation);
    if (native_result != XG_WORLD_MODELS_NATIVE_OK) {
        const uint32_t template_failure =
            xg_render_world_model_repository_template_read_failure();
        failure_detail = template_failure != 0u
            ? XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_READ_BASE +
                  template_failure
            : XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_NATIVE_RESULT_BASE +
                  (uint32_t)native_result;
        goto fail;
    }
    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CONTRACT;
    if (!preparation.authenticated || !preparation.sealed ||
        preparation.authentication_generation != generation ||
        preparation.record_count > XG_RENDER_WORLD_MODEL_RECORD_CAPACITY ||
        preparation.transform_node_count > XG_RENDER_WORLD_MODEL_NODE_CAPACITY ||
        preparation.dispatch_count > XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY ||
        preparation.primitive_count > XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY ||
        !physical_address_equals(preparation.continuation_pc, cpu->gpr[31]))
        goto fail;

    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_GLOBAL_RANGE;
    if (!services->authorize_guest_range(
            XG_WORLD_MODELS_SCALE_X_SCRATCH, 12u, 4u, true) ||
        !services->authorize_guest_range(
            XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH, 6u, 2u, true) ||
        !services->authorize_guest_range(
            XG_WORLD_MODELS_RESIDENT_CULL_MODE_GLOBAL, 4u, 4u, false) ||
        !services->authorize_guest_range(
            XG_WORLD_MODELS_RESIDENT_VERTEX_TOTAL_GLOBAL, 4u, 4u, false) ||
        !services->authorize_guest_range(
            XG_WORLD_MODELS_RESIDENT_EMITTED_COUNT_GLOBAL, 4u, 4u, false))
        goto fail;
    for (uint32_t index = 0u;
         index < preparation.transform_node_count; ++index) {
        failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_NODE_RANGE;
        if (!services->authorize_guest_range(
                workspace->node_side_effects[index].guest_address +
                    XG_WORLD_MODELS_NODE_WRITEBACK_X_OFFSET,
                12u, 4u, false))
            goto fail;
    }

    primitive_index = 0u;
    for (uint32_t dispatch_index = 0u;
         dispatch_index < preparation.dispatch_count; ++dispatch_index) {
        const XgWorldModelsNativeDispatch *dispatch =
            &workspace->dispatches[dispatch_index];
        XgWorldModelsNativeDispatchOutput *dispatch_output =
            &workspace->dispatch_outputs[dispatch_index];

        *dispatch_output = (XgWorldModelsNativeDispatchOutput){0};
        dispatch_output->source_index = dispatch->source_index;
        dispatch_output->bounds_accepted = dispatch->bounds_accepted;
        dispatch_output->guest_bounds_accepted = dispatch->guest_bounds_accepted;
        if (!dispatch->bounds_accepted && !dispatch->guest_bounds_accepted) {
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CULLED_DISPATCH;
            if (dispatch->primitive_count != 0u) goto fail;
            continue;
        }
        failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_DISPATCH_RANGE;
        if (dispatch->primitive_start != primitive_index ||
            dispatch->primitive_count >
                preparation.primitive_count - primitive_index)
            goto fail;
        for (uint32_t dispatch_primitive = 0u;
             dispatch_primitive < dispatch->primitive_count;
             ++dispatch_primitive, ++primitive_index) {
            const XgWorldModelsNativePrimitiveSource *source =
                &workspace->primitives[primitive_index];
            XgWorldModelsNativePrimitiveOutput *output =
                &workspace->outputs[primitive_index];
            const XgRenderWorldModelTemplate *template_entry;

            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_SOURCE_IDENTITY;
            if (source->dispatch_index != dispatch_index ||
                source->primitive_index != dispatch_primitive)
                goto fail;
            native_result = xg_world_models_native_build_primitive(
                &preparation, generation, source, output);
            if (native_result != XG_WORLD_MODELS_NATIVE_OK) {
                failure_detail =
                    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_BUILD_RESULT_BASE +
                    (uint32_t)native_result;
                goto fail;
            }
            if (!dispatch->bounds_accepted) {
                output->passed_screen_cull = false;
                output->accepted = false;
                output->ordering_table_written = false;
            }
            if (!dispatch->guest_bounds_accepted) {
                output->counter_incremented = false;
                output->guest_ordering_table_written = false;
                output->guest_packet_word_write_mask = 0u;
            }
            template_entry = xg_render_world_model_repository_find_template(
                source->packet_address);
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_MISSING;
            if (template_entry == NULL) goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_INACTIVE;
            if (!template_entry->active) goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_OWNER;
            if (template_entry->owner_cpu != cpu) goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_EPOCH;
            if (template_entry->resource_epoch != source->resource_epoch)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_MODEL;
            if (template_entry->model_address !=
                    normalized_word_address(source->model_header_address))
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_PACKET_BASE;
            if (template_entry->packet_base !=
                    normalized_word_address(source->packet_base_address))
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_ORDINAL;
            if (template_entry->primitive_ordinal != source->primitive_index)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_ATTRIBUTE;
            if (template_entry->attribute_address !=
                    normalized_word_address(source->attribute_address))
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_FAMILY;
            if (template_entry->primitive_family != source->primitive_family)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_WORD_COUNT;
            if (template_entry->word_count != source->packet_word_count)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_PACKET;
            if (output->packet_address != source->packet_address) goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_WORD_COUNT;
            if (output->packet_word_count != source->packet_word_count)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_COUNTER;
            if (output->guest_ordering_table_written &&
                !output->counter_incremented)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PACKET_RANGE;
            if (!services->authorize_guest_range(
                    source->packet_address,
                    (uint32_t)source->packet_word_count * 4u, 4u, false))
                goto fail;
            workspace->templates[primitive_index] = template_entry;
            const uint32_t allowed_mask =
                (UINT32_C(1) << source->packet_word_count) - 1u;
            failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_MASK;
            if ((output->packet_word_write_mask & ~allowed_mask) != 0u)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_ACCEPTED;
            if (output->accepted != output->ordering_table_written) goto fail;
            failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_TAG;
            if ((output->accepted || output->guest_ordering_table_written) &&
                (output->packet_word_write_mask & 1u) == 0u)
                goto fail;
            for (uint32_t word = 0u;
                 word < source->packet_word_count; ++word) {
                failure_detail =
                    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PACKET_WORD;
                if (cpu->read_word(source->packet_address + word * 4u) !=
                    template_entry->words[word])
                    goto fail;
            }
            ++dispatch_output->processed_primitive_count;
            dispatch_output->emitted_count_delta +=
                output->counter_incremented ? 1u : 0u;
            dispatch_output->packet_cursor_masked |=
                output->packet_cursor_masked;
            dispatch_output->primitive_family_mask |=
                UINT32_C(1) << source->primitive_family;
            if (output->packet_word_write_mask != 0u)
                ++dispatch_output->packet_side_effect_primitive_count;
            if (output->accepted) {
                ++dispatch_output->accepted_primitive_count;
                ++accepted_count;
            }
            if (output->guest_ordering_table_written) {
                const uint32_t bucket = output->ordering_bucket;
                const uint32_t ot_address =
                    dispatch->call.ordering_table_address + bucket * 4u;

                failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OT_RANGE;
                if (bucket >= XG_WORLD_MODELS_OT_BUCKET_COUNT ||
                    !services->authorize_guest_range(
                        ot_address, 4u, 4u, false))
                    goto fail;
                if (!workspace->ot_touched[bucket]) {
                    workspace->ot_heads[bucket] = cpu->read_word(ot_address);
                    workspace->ot_touched[bucket] = true;
                }
            }
        }
        dispatch_output->packet_cursor_after = dispatch->packet_cursor_after;
        if (dispatch_output->packet_cursor_masked)
            dispatch_output->packet_cursor_after &= UINT32_C(0x00ffffff);
        dispatch_output->group_cursor_after = dispatch->group_cursor_after;
    }
    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PRIMITIVE_TOTAL;
    if (primitive_index != preparation.primitive_count ||
        !snapshot_can_record(accepted_count))
        goto fail;

    commit.entry_side_effects = preparation.world.entry_side_effects;
    commit.completed_dispatch_count = preparation.dispatch_count;
    for (uint32_t dispatch_index = 0u;
         dispatch_index < preparation.dispatch_count; ++dispatch_index) {
        const XgWorldModelsNativeDispatch *dispatch =
            &workspace->dispatches[dispatch_index];
        const XgWorldModelsNativeDispatchOutput *output =
            &workspace->dispatch_outputs[dispatch_index];

        if (!dispatch->bounds_accepted && !dispatch->guest_bounds_accepted)
            continue;
        failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_COMMIT_OVERFLOW;
        if ((dispatch->guest_bounds_accepted &&
             commit.resident_vertex_total > UINT32_MAX -
                 dispatch->model.vertex_count) ||
            commit.resident_emitted_count > UINT32_MAX -
                output->emitted_count_delta ||
            commit.processed_primitive_count > UINT32_MAX -
                output->processed_primitive_count ||
            commit.accepted_primitive_count > UINT32_MAX -
                output->accepted_primitive_count)
            goto fail;
        if (dispatch->guest_bounds_accepted)
            commit.resident_vertex_total += dispatch->model.vertex_count;
        commit.resident_emitted_count += output->emitted_count_delta;
        commit.processed_primitive_count += output->processed_primitive_count;
        commit.accepted_primitive_count += output->accepted_primitive_count;
        if (dispatch->guest_bounds_accepted) {
            commit.resident_packet_cursor = output->packet_cursor_after;
            commit.resident_group_cursor = output->group_cursor_after;
            commit.resident_model_0c = dispatch->model.auxiliary_vertex_base;
            commit.resident_vertex_base = dispatch->model.vertex_base;
            commit.resident_ot_base = dispatch->call.ordering_table_address;
            commit.resident_model_18 = dispatch->model.model_18;
            commit.resident_dispatch_globals_written = true;
        }
    }
    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_COMMIT_TOTAL;
    if (commit.processed_primitive_count != preparation.primitive_count ||
        commit.accepted_primitive_count != accepted_count)
        goto fail;

    for (primitive_index = 0u;
         primitive_index < preparation.primitive_count; ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[primitive_index];
        XgWorldModelsNativePrimitiveOutput *output =
            &workspace->outputs[primitive_index];

        if (output->guest_ordering_table_written) {
            const XgWorldModelsNativeDispatch *dispatch =
                &workspace->dispatches[source->dispatch_index];
            const uint32_t bucket = output->ordering_bucket;
            const uint32_t previous_head = workspace->ot_heads[bucket];

            output->packet_words[0] =
                (output->packet_words[0] & UINT32_C(0xff000000)) |
                (previous_head & UINT32_C(0x00ffffff));
            workspace->ot_heads[bucket] =
                (previous_head & UINT32_C(0xff000000)) |
                (source->packet_address & UINT32_C(0x00ffffff));
            (void)dispatch;
        }
    }
    commit.authentication_generation = generation;
    commit.continuation_pc = preparation.continuation_pc;
    commit.entry_side_effects.resident_vertex_total =
        commit.resident_vertex_total;
    commit.entry_side_effects.resident_emitted_count =
        commit.resident_emitted_count;
    commit.authenticated = true;
    commit.sealed = true;
    workspace->preparation = preparation;
    workspace->expected_commit = commit;
    workspace->authentication_generation = generation;
    workspace->entry_stack_pointer = cpu->gpr[29];
    workspace->accepted_count = accepted_count;
    workspace->owner_cpu = cpu;
    workspace->services = services;
    workspace->valid = true;
    return true;

fail:
    record_failure(PSX_XG_RENDER_WORLD_NATIVE_FAILURE_PREPARE,
                   failure_detail, 0u);
    return false;
}

bool xg_render_world_models_commit(
        CPUState *cpu, const XgRenderWorldModelsPipelineServices *services) {
    XgRenderWorldModelsNativeState *workspace = &native_state;
    XgWorldModelsNativePreparation *preparation = &workspace->preparation;
    const XgWorldModelsNativeCommit *expected = &workspace->expected_commit;
    XgWorldModelsNativeCommit commit;
    XgWorldModelsNativeResult native_result;
    uint64_t generation;
    uint32_t anchor_count = 0u;
    uint32_t failure_detail = 1u;
    PsxXgRenderWorldNativeFailureStage failure_stage =
        PSX_XG_RENDER_WORLD_NATIVE_FAILURE_COMMIT_PRECONDITION;

    if (!services_valid(services) || !workspace->valid ||
        workspace->owner_cpu != cpu)
        goto fail;
    failure_detail = 2u;
    if (!services->cutover_ready()) goto fail;
    failure_detail = 3u;
    if (!services->authentication_generation(&generation)) goto fail;
    failure_detail = 4u;
    if (generation != workspace->authentication_generation) goto fail;
    failure_detail = 5u;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL)
        goto fail;
    failure_detail = 6u;
    if (workspace->entry_stack_pointer < 0x40u) goto fail;
    failure_detail = 7u;
    if (cpu->gpr[29] != workspace->entry_stack_pointer - 0x40u) goto fail;
    failure_detail = 8u;
    if (!physical_address_equals(
            cpu->read_word(cpu->gpr[29] + 0x38u), expected->continuation_pc))
        goto fail;
    failure_detail = 9u;
    if (cpu->read_word(XG_WORLD_MODELS_SCALE_X_SCRATCH) !=
            expected->entry_side_effects.scratch_scale[0] ||
        cpu->read_word(XG_WORLD_MODELS_SCALE_Y_SCRATCH) !=
            expected->entry_side_effects.scratch_scale[1] ||
        cpu->read_word(XG_WORLD_MODELS_SCALE_Z_SCRATCH) !=
            expected->entry_side_effects.scratch_scale[2] ||
        cpu->read_half(XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH) !=
            (uint16_t)expected->entry_side_effects.coarse_origin[0] ||
        cpu->read_half(XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH + 2u) !=
            (uint16_t)expected->entry_side_effects.coarse_origin[1] ||
        cpu->read_half(XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH + 4u) !=
            (uint16_t)expected->entry_side_effects.coarse_origin[2] ||
        cpu->read_word(XG_WORLD_MODELS_RESIDENT_CULL_MODE_GLOBAL) !=
            expected->entry_side_effects.resident_cull_mode ||
        cpu->read_word(XG_WORLD_MODELS_RESIDENT_VERTEX_TOTAL_GLOBAL) !=
            expected->resident_vertex_total ||
        cpu->read_word(XG_WORLD_MODELS_RESIDENT_EMITTED_COUNT_GLOBAL) !=
            expected->resident_emitted_count)
        goto fail;

    for (uint32_t index = 0u;
         index < preparation->transform_node_count; ++index) {
        const XgWorldModelsNodeSideEffect *effect =
            &workspace->node_side_effects[index];

        for (uint32_t component = 0u; component < 3u; ++component) {
            if (cpu->read_word(
                    effect->guest_address +
                        XG_WORLD_MODELS_NODE_WRITEBACK_X_OFFSET +
                        component * 4u) !=
                (uint32_t)effect->translation[component])
                goto fail;
        }
    }
    for (uint32_t primitive_index = 0u;
         primitive_index < preparation->primitive_count; ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[primitive_index];
        const XgWorldModelsNativePrimitiveOutput *output =
            &workspace->outputs[primitive_index];
        const XgRenderWorldModelTemplate *template_entry =
            workspace->templates[primitive_index];

        if (template_entry == NULL || !template_entry->active ||
            template_entry->owner_cpu != cpu ||
            template_entry->resource_epoch != source->resource_epoch)
            goto fail;
        for (uint32_t word = 0u;
             word < source->packet_word_count; ++word) {
            const uint32_t expected_word =
                (output->guest_packet_word_write_mask &
                 (UINT32_C(1) << word)) != 0u
                    ? output->packet_words[word]
                    : template_entry->words[word];

            if (cpu->read_word(source->packet_address + word * 4u) !=
                expected_word)
                goto fail;
        }
    }
    for (uint32_t index = 0u; index < XG_WORLD_MODELS_OT_BUCKET_COUNT; ++index) {
        if (workspace->ot_touched[index] &&
            cpu->read_word(expected->resident_ot_base + index * 4u) !=
                workspace->ot_heads[index])
            goto fail;
    }
    if (expected->resident_dispatch_globals_written &&
        (cpu->read_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL) !=
             expected->resident_packet_cursor ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_GROUP_CURSOR_GLOBAL) !=
             expected->resident_group_cursor ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_MODEL_0C_GLOBAL) !=
             expected->resident_model_0c ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_VERTEX_BASE_GLOBAL) !=
             expected->resident_vertex_base ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_OT_BASE_GLOBAL) !=
             expected->resident_ot_base ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_MODEL_18_GLOBAL) !=
             expected->resident_model_18))
        goto fail;

    if (!collect_interpolation_anchors(
            workspace, services->interpolation_scene_generation(),
            &anchor_count, &failure_detail)) {
        failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_ANCHOR_COLLECTION;
        goto fail;
    }
    workspace->services = services;
    native_result = xg_world_models_native_finalize(
        preparation, generation, workspace->dispatches,
        workspace->dispatch_outputs, preparation->dispatch_count,
        workspace->primitives, workspace->outputs,
        preparation->primitive_count, workspace,
        begin_submission, stage_primitive, &commit);
    if (native_result != XG_WORLD_MODELS_NATIVE_OK ||
        !commit.authenticated || !commit.sealed ||
        commit.authentication_generation != generation ||
        commit.accepted_primitive_count != workspace->accepted_count ||
        commit.resident_vertex_total != expected->resident_vertex_total ||
        commit.resident_emitted_count != expected->resident_emitted_count) {
        failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_FINALIZE;
        failure_detail = (uint32_t)native_result;
        goto fail;
    }
    for (uint32_t primitive_index = 0u;
         primitive_index < preparation->primitive_count; ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[primitive_index];
        const XgWorldModelsNativePrimitiveOutput *output =
            &workspace->outputs[primitive_index];
        const uint8_t mode = source->dispatch_mode;
        const int32_t margin = services->screen_x_cull_margin();
        const uint32_t interpolation_primitive_id =
            source->source_index * 4096u + source->primitive_index;
        const bool average = source->primitive_family == 16u ||
            mode == 0u || mode == 4u;
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_FRONT_FACE |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = margin > 0
                ? -margin * INT32_C(65536) : INT32_MIN,
            .screen_top = 0,
            .screen_right_exclusive =
                ((int32_t)preparation->screen_right + margin) *
                    INT32_C(65536),
            .screen_bottom_exclusive =
                (int32_t)(preparation->packed_screen_bottom >> 16u) *
                    INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x10000,
            .depth_mode = average ? GPU_RENDER_TEMPORAL_DEPTH_AVERAGE :
                ((mode == 2u || mode == 5u)
                    ? GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM
                    : GPU_RENDER_TEMPORAL_DEPTH_MINIMUM),
            .front_face = GPU_RENDER_TEMPORAL_FRONT_POSITIVE,
            .ordering_depth_shift = (uint8_t)(average
                ? preparation->ordering_shift
                : ((preparation->ordering_shift + 2u) & 31u)),
        };

        if (output->accepted || output->primitive.triangle_count == 0u)
            continue;
        if (!services->stage_temporal(
                &output->primitive,
                source->model_header_address & UINT32_C(0x1fffffff),
                interpolation_primitive_id, &policy)) {
            failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_FINALIZE;
            failure_detail = UINT32_C(0x1000) + primitive_index;
            goto fail;
        }
    }
    const GpuRenderTransactionStatus transaction_status = anchor_count != 0u
        ? gr_record_interpolation_anchors(workspace->anchors, anchor_count)
        : GPU_RENDER_TRANSACTION_OK;
    if (transaction_status != GPU_RENDER_TRANSACTION_OK) {
        failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_ANCHOR_RECORD;
        failure_detail = (uint32_t)transaction_status;
        goto fail;
    }

    for (uint32_t primitive_index = 0u;
         primitive_index < preparation->primitive_count; ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[primitive_index];
        const XgRenderWorldModelTemplate *template_entry =
            workspace->templates[primitive_index];
        const XgWorldModelsNativePrimitiveOutput *output =
            &workspace->outputs[primitive_index];

        if (!xg_render_world_model_repository_update_template_words(
                source->packet_address, cpu, template_entry->resource_epoch,
                output->packet_words, source->packet_word_count,
                output->guest_packet_word_write_mask)) {
            failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_FINALIZE;
            failure_detail = UINT32_C(0x2000) + primitive_index;
            goto fail;
        }
    }
    ++snapshot.native_cutover_count;
    snapshot.native_primitive_count += workspace->accepted_count;
    snapshot.last_anchor_count = anchor_count;
    xg_render_world_models_clear_pending();
    return true;

fail:
    record_failure(failure_stage, failure_detail, anchor_count);
    if (services_valid(services)) services->abort_submission();
    xg_render_world_models_clear_pending();
    xg_render_world_model_repository_invalidate();
    return false;
}

void xg_render_world_models_clear_pending(void) {
    if (native_state.valid) native_state.valid = false;
}

void xg_render_world_models_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    PsxXgRenderWorldNativeSnapshot repository = {0};

    if (out_snapshot == NULL) return;
    xg_render_world_model_repository_snapshot(&repository);
    *out_snapshot = snapshot;
    out_snapshot->packet_copy_begin_count = repository.packet_copy_begin_count;
    out_snapshot->packet_copy_finish_count = repository.packet_copy_finish_count;
    out_snapshot->packet_copy_template_count =
        repository.packet_copy_template_count;
    out_snapshot->packet_copy_failure_detail =
        repository.packet_copy_failure_detail;
    out_snapshot->packet_copy_last_destination =
        repository.packet_copy_last_destination;
    out_snapshot->packet_copy_last_source = repository.packet_copy_last_source;
    out_snapshot->packet_copy_last_size = repository.packet_copy_last_size;
    out_snapshot->packet_copy_range_count = repository.packet_copy_range_count;
    out_snapshot->first_missing_model_address =
        repository.first_missing_model_address;
    out_snapshot->first_missing_packet_base =
        repository.first_missing_packet_base;
    out_snapshot->first_missing_packet_address =
        repository.first_missing_packet_address;
    out_snapshot->first_missing_copy_range_kind =
        repository.first_missing_copy_range_kind;
    out_snapshot->first_missing_copy_range_index =
        repository.first_missing_copy_range_index;
}

bool xg_render_world_models_pending_metadata_copy(
        CPUState **out_owner_cpu, uint32_t *out_entry_stack_pointer,
        XgWorldModelsNativePreparation *out_preparation,
        XgWorldModelsNativeCommit *out_commit) {
    if (!native_state.valid || out_owner_cpu == NULL ||
        out_entry_stack_pointer == NULL || out_preparation == NULL ||
        out_commit == NULL)
        return false;
    *out_owner_cpu = native_state.owner_cpu;
    *out_entry_stack_pointer = native_state.entry_stack_pointer;
    *out_preparation = native_state.preparation;
    *out_commit = native_state.expected_commit;
    return true;
}

bool xg_render_world_models_pending_node_copy(
        uint32_t index, XgWorldModelsNodeSideEffect *out_effect) {
    if (!native_state.valid || out_effect == NULL ||
        index >= native_state.preparation.transform_node_count)
        return false;
    *out_effect = native_state.node_side_effects[index];
    return true;
}

bool xg_render_world_models_pending_packet_copy(
        uint32_t index, uint32_t *out_address, uint32_t *out_words,
        uint32_t capacity, uint32_t *out_word_count,
        uint32_t *out_write_mask) {
    const XgWorldModelsNativePrimitiveSource *source;
    const XgWorldModelsNativePrimitiveOutput *output;

    if (!native_state.valid || out_address == NULL || out_words == NULL ||
        out_word_count == NULL || out_write_mask == NULL ||
        index >= native_state.preparation.primitive_count)
        return false;
    source = &native_state.primitives[index];
    output = &native_state.outputs[index];
    if (capacity < source->packet_word_count) return false;
    *out_address = source->packet_address;
    *out_word_count = source->packet_word_count;
    *out_write_mask = output->packet_word_write_mask;
    memcpy(out_words, output->packet_words,
           source->packet_word_count * sizeof(out_words[0]));
    return true;
}

bool xg_render_world_models_pending_ot_copy(
        uint32_t index, uint32_t *out_address, uint32_t *out_value) {
    if (!native_state.valid || out_address == NULL || out_value == NULL ||
        index >= XG_WORLD_MODELS_OT_BUCKET_COUNT ||
        !native_state.ot_touched[index])
        return false;
    *out_address = native_state.expected_commit.resident_ot_base + index * 4u;
    *out_value = native_state.ot_heads[index];
    return true;
}

void xg_render_world_models_reset(void) {
    native_state = (XgRenderWorldModelsNativeState){0};
    snapshot = (PsxXgRenderWorldNativeSnapshot){0};
    xg_render_world_model_repository_reset();
}

void xg_render_world_models_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    const bool semantic_write = event->mutation.semantic_authority_loss ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_SKY) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);

    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
        event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST ||
        event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
        (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE && semantic_write))
        xg_render_world_models_clear_pending();
    else if (event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_render_world_models_reset();
}
