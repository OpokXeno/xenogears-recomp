#include "xg_world_models.h"
#include "xg_world_models_native.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 0;                                                           \
        }                                                                       \
    } while (0)

static XgHost3dMatrix identity_matrix(void) {
    XgHost3dMatrix matrix = { 0 };

    matrix.rotation[0][0] = 4096;
    matrix.rotation[1][1] = 4096;
    matrix.rotation[2][2] = 4096;
    return matrix;
}

static XgWorldModelsSource source_fixture(
    const XgWorldModelsRecordSource *records, int16_t record_count) {
    XgWorldModelsSource source = { 0 };

    source.records = records;
    source.record_count = record_count;
    source.buffer_index = 1u;
    source.wrap_x = 20;
    source.wrap_z = 20;
    source.camera_matrix = identity_matrix();
    source.gte.screen_offset_x = 160 << 16;
    source.gte.screen_offset_y = 120 << 16;
    source.gte.projection_distance = 256u;
    source.gte.average_z_scale4 = 1024;
    source.ordering_table_address = UINT32_C(0x800b2000);
    return source;
}

static XgWorldModelsRecordSource record_fixture(void) {
    XgWorldModelsRecordSource record = { 0 };

    record.dispatch_selector = 4;
    record.position_x = 100;
    record.stored_z = -512;
    record.matrix = identity_matrix();
    record.matrix.translation[0] = 0x11111111;
    record.matrix.translation[1] = 0x22222222;
    record.matrix.translation[2] = 0x33333333;
    record.model_header_address = UINT32_C(0x800a1000);
    record.packet_base[0] = UINT32_C(0x800b0000);
    record.packet_base[1] = UINT32_C(0x800b1000);
    return record;
}

static int test_builds_pre_dispatch_contract_and_side_effects(void) {
    XgWorldModelsTransformNodeSource node = {
        .guest_address = UINT32_C(0x800a5000),
        .position_x = 20,
        .position_y = 30,
        .stored_z = 10,
    };
    XgWorldModelsRecordSource sources[4];
    XgWorldModelsSource source;
    XgWorldModelsRecordOutput outputs[4];
    XgWorldModelsNodeSideEffect effects[2];
    XgWorldModelsBuildSummary summary;

    node.matrix = identity_matrix();
    node.matrix.rotation[0][0] = 0;
    node.matrix.rotation[0][1] = -4096;
    node.matrix.rotation[1][0] = 4096;
    node.matrix.rotation[1][1] = 0;
    sources[0] = record_fixture();
    sources[0].transform_nodes = &node;
    sources[0].transform_node_count = 1u;
    sources[1] = record_fixture();
    sources[1].state = 1;
    sources[1].dispatch_selector = -1;
    sources[1].transform_nodes = &node;
    sources[1].transform_node_count = 1u;
    sources[2] = record_fixture();
    sources[2].position_x = 0;
    sources[2].stored_z = -4000;
    sources[3] = record_fixture();
    sources[3].position_x = 100000;
    source = source_fixture(sources, 4);

    CHECK(xg_world_models_build(&source, outputs, 4u, effects, 2u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(summary.record_count == 4u);
    CHECK(summary.node_side_effect_count == 1u);
    CHECK(summary.resident_dispatch_count == 1u);
    CHECK(summary.entry_side_effects.scratch_scale[0] == 0x800u);
    CHECK(summary.entry_side_effects.scratch_scale[1] == 0x800u);
    CHECK(summary.entry_side_effects.scratch_scale[2] == 0x800u);
    CHECK(summary.entry_side_effects.resident_cull_mode == 3u);
    CHECK(summary.entry_side_effects.resident_vertex_total == 0u);
    CHECK(summary.entry_side_effects.resident_emitted_count == 0u);
    CHECK(summary.entry_side_effects.coarse_origin[0] == 0);
    CHECK(summary.entry_side_effects.coarse_origin[1] == 0);
    CHECK(summary.entry_side_effects.coarse_origin[2] == 0);

    CHECK(effects[0].guest_address == node.guest_address);
    CHECK(effects[0].translation[0] == 20);
    CHECK(effects[0].translation[1] == 30);
    CHECK(effects[0].translation[2] == -10);
    CHECK(outputs[0].disposition == XG_WORLD_MODELS_RESIDENT_DISPATCH);
    CHECK(outputs[0].object_to_view.rotation[0][0] == 0);
    CHECK(outputs[0].object_to_view.rotation[0][1] == -2048);
    CHECK(outputs[0].object_to_view.rotation[1][0] == 2048);
    CHECK(outputs[0].object_to_view.rotation[1][1] == 0);
    CHECK(outputs[0].object_to_view.rotation[2][2] == 2048);
    CHECK(outputs[0].object_to_view.translation[0] == 20);
    CHECK(outputs[0].object_to_view.translation[1] == 130);
    CHECK(outputs[0].object_to_view.translation[2] == 502);
    CHECK(outputs[0].coarse_flags == 0u);
    CHECK(outputs[0].coarse_depth == 502u);
    CHECK(outputs[0].resident_call.model_header_address ==
          UINT32_C(0x800a1000));
    CHECK(outputs[0].resident_call.packet_base_address ==
          UINT32_C(0x800b1000));
    CHECK(outputs[0].resident_call.ordering_table_address ==
          UINT32_C(0x800b2000));
    CHECK(outputs[0].resident_call.dispatch_mode == 0u);
    CHECK(outputs[1].disposition == XG_WORLD_MODELS_INACTIVE);
    CHECK(outputs[2].disposition == XG_WORLD_MODELS_COARSE_DEPTH_REJECTED);
    CHECK(outputs[2].coarse_depth == 4000u);
    CHECK(outputs[3].disposition == XG_WORLD_MODELS_COARSE_FLAG_REJECTED);
    CHECK((outputs[3].coarse_flags & UINT32_C(0x80000000)) != 0u);
    return 1;
}

static int test_wrap_boundaries_and_signed_camera_shift(void) {
    XgWorldModelsRecordSource record = record_fixture();
    XgWorldModelsSource source = source_fixture(&record, 1);
    XgWorldModelsRecordOutput output;
    XgWorldModelsBuildSummary summary;

    record.position_x = 0x4001;
    record.stored_z = 0x4001;
    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(output.object_to_view.translation[0] == -24575);
    CHECK(output.object_to_view.translation[2] == 24575);

    record.position_x = 0x4000;
    record.stored_z = 0x4000;
    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(output.object_to_view.translation[0] == 0x4000);
    CHECK(output.object_to_view.translation[2] == -0x4000);

    record.position_x = 0;
    record.stored_z = -512;
    source.camera_x_12_12 = -1;
    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(output.object_to_view.translation[0] == 1);
    return 1;
}

static int test_native_view_widens_coarse_depth_gate(void) {
    XgWorldModelsRecordSource record = record_fixture();
    XgWorldModelsSource source = source_fixture(&record, 1);
    XgWorldModelsRecordOutput output;
    XgWorldModelsBuildSummary summary;

    record.stored_z = -4000;
    xg_host_3d_configure_native_view_aspect(1, 54 << 16, 16u, 9u);
    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(output.coarse_depth == 4000u);
    CHECK(output.disposition == XG_WORLD_MODELS_RESIDENT_DISPATCH);
    xg_host_3d_configure_native_view(0, 0);
    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(output.disposition == XG_WORLD_MODELS_COARSE_DEPTH_REJECTED);
    return 1;
}

static int test_entry_only_and_validation(void) {
    XgWorldModelsSource source = source_fixture(NULL, -1);
    XgWorldModelsBuildSummary summary;
    XgWorldModelsRecordSource record = record_fixture();
    XgWorldModelsRecordOutput output;
    XgWorldModelsTransformNodeSource node = { 0 };

    CHECK(xg_world_models_build(&source, NULL, 0u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(summary.record_count == 0u);
    CHECK(summary.entry_side_effects.resident_cull_mode == 3u);
    CHECK(summary.entry_side_effects.scratch_scale[2] == 0x800u);

    source = source_fixture(&record, 1);
    CHECK(xg_world_models_build(&source, &output, 0u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_CAPACITY_EXCEEDED);
    source.buffer_index = 2u;
    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_INVALID_SOURCE);
    CHECK(xg_world_models_build(NULL, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_INVALID_ARGUMENT);
    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                NULL) == XG_WORLD_MODELS_INVALID_ARGUMENT);
    source = source_fixture(&record, 1);
    record.transform_nodes = &node;
    record.transform_node_count = 1u;
    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_CAPACITY_EXCEEDED);
    return 1;
}

static int test_dispatch_selector_table(void) {
    static const uint8_t expected_modes[10] = {
        4u, 4u, 5u, 5u, 0u, 0u, 2u, 2u, 3u, 3u,
    };
    XgWorldModelsRecordSource records[10];
    XgWorldModelsRecordOutput outputs[10];
    XgWorldModelsSource source;
    XgWorldModelsBuildSummary summary;
    uint32_t index;

    for (index = 0u; index < 10u; ++index) {
        records[index] = record_fixture();
        records[index].dispatch_selector = (int16_t)index;
    }
    source = source_fixture(records, 10);
    CHECK(xg_world_models_build(&source, outputs, 10u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(summary.resident_dispatch_count == 10u);
    for (index = 0u; index < 10u; ++index) {
        CHECK(outputs[index].disposition ==
              XG_WORLD_MODELS_RESIDENT_DISPATCH);
        CHECK(outputs[index].resident_call.dispatch_mode ==
              expected_modes[index]);
    }
    return 1;
}

static XgModelFt4RawSource raw_ft4_fixture(void) {
    XgModelFt4RawSource source = { 0 };

    source.vertices[0] = (XgHost3dVector){ -32, -32, 0, 0u };
    source.vertices[1] = (XgHost3dVector){ 32, -32, 0, 0u };
    source.vertices[2] = (XgHost3dVector){ -32, 32, 0, 0u };
    source.vertices[3] = (XgHost3dVector){ 32, 32, 0, 0u };
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = true;
    source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    source.uv[1][0] = 31u;
    source.uv[2][1] = 63u;
    source.screen_right = 319;
    source.packed_screen_bottom = UINT32_C(0x00ee0000);
    source.ordering_shift = 4u;
    source.packet_address = UINT32_C(0xdeadbeef);
    return source;
}

static int test_raw_ft4_adapter_delegates_to_shared_kernel(void) {
    XgWorldModelsRecordSource record = record_fixture();
    XgWorldModelsSource source = source_fixture(&record, 1);
    XgWorldModelsRecordOutput output;
    XgWorldModelsBuildSummary summary;
    XgModelFt4RawSource values = raw_ft4_fixture();
    XgModelFt4RawSource direct_source;
    XgModelFt4RawRecord adapted;
    XgModelFt4RawRecord direct;

    CHECK(xg_world_models_build(&source, &output, 1u, NULL, 0u,
                                &summary) == XG_WORLD_MODELS_OK);
    CHECK(output.disposition == XG_WORLD_MODELS_RESIDENT_DISPATCH);
    CHECK(xg_world_models_build_raw_ft4(&output, 2u, &values, &adapted) ==
          XG_MODEL_FT4_RAW_OK);
    direct_source = values;
    direct_source.projection = output.projection;
    direct_source.packet_address = UINT32_C(0x800b1000) + 2u * 0x28u;
    CHECK(xg_model_ft4_raw_build(&direct_source, &direct) ==
          XG_MODEL_FT4_RAW_OK);
    CHECK(memcmp(&adapted, &direct, sizeof(adapted)) == 0);
    CHECK(adapted.accepted);

    output.resident_call.dispatch_mode = 2u;
    CHECK(xg_world_models_build_raw_ft4(&output, 0u, &values, &adapted) ==
          XG_MODEL_FT4_RAW_OK);
    direct_source = values;
    direct_source.projection = output.projection;
    direct_source.dispatch_mode = 2u;
    direct_source.packet_address = UINT32_C(0x800b1000);
    CHECK(xg_model_ft4_raw_build(&direct_source, &direct) ==
          XG_MODEL_FT4_RAW_OK);
    CHECK(memcmp(&adapted, &direct, sizeof(adapted)) == 0);
    CHECK(xg_world_models_build_raw_ft4(NULL, 0u, &values, &adapted) ==
          XG_MODEL_FT4_RAW_INVALID_ARGUMENT);
    return 1;
}

static uint64_t byte_digest(const void *data, uint32_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t digest = UINT64_C(1469598103934665603);
    uint32_t index;

    for (index = 0u; index < size; ++index) {
        digest ^= bytes[index];
        digest *= UINT64_C(1099511628211);
    }
    return digest;
}

static int test_depth_cued_quad_right_cull_writes_first_xy(void) {
    XgWorldModelsNativePrimitiveSource source = {0};
    XgWorldModelsNativePreparation preparation = {0};
    XgWorldModelsNativePrimitiveOutput output;
    uint32_t vertex;

    source.projection.rotation[0][0] = 4096;
    source.projection.rotation[1][1] = 4096;
    source.projection.rotation[2][2] = 4096;
    source.projection.screen_offset_x = 400 << 16;
    source.projection.screen_offset_y = 120 << 16;
    source.projection.projection_distance = 256u;
    source.projection.average_z_scale4 = 1024;
    source.vertices[0] = (XgHost3dVector){-32, -32, 512, 0u};
    source.vertices[1] = (XgHost3dVector){32, -32, 512, 0u};
    source.vertices[2] = (XgHost3dVector){-32, 32, 512, 0u};
    source.vertices[3] = (XgHost3dVector){32, 32, 512, 0u};
    source.attribute_words[0] = UINT32_C(0x2d000000);
    source.template_words[0] = UINT32_C(0x09000000);
    source.template_words[1] = UINT32_C(0x2d808080);
    source.model_header_address = UINT32_C(0x800a1000);
    source.packet_address = UINT32_C(0x800d7344);
    source.source_index = 0u;
    source.topology[0] = 0u;
    source.topology[1] = 1u;
    source.topology[2] = 2u;
    source.topology[3] = 3u;
    source.primitive_family = 13u;
    source.dispatch_mode = 4u;
    source.vertex_count = 4u;
    source.attribute_size = 12u;
    source.packet_word_count = 10u;

    preparation.sealed_primitives = &source;
    preparation.primitive_count = 1u;
    preparation.record_count = 1u;
    preparation.dispatch_count = 1u;
    preparation.primitive_family_mask = UINT32_C(1) << 13u;
    preparation.dispatch_mode_mask = UINT32_C(1) << 4u;
    preparation.ordering_shift = 4u;
    preparation.screen_right = 319u;
    preparation.guest_xclip_bound = 319u;
    preparation.packed_screen_bottom = UINT32_C(0x00ef0000);
    preparation.authentication_generation = 1u;
    preparation.authenticated = true;
    preparation.sealed = true;
    preparation.primitive_digest = byte_digest(&source, sizeof(source));

    CHECK(xg_world_models_native_build_primitive(
              &preparation, 1u, &source, &output) ==
          XG_WORLD_MODELS_NATIVE_OK);
    CHECK(!output.accepted);
    CHECK(!output.passed_screen_cull);
    CHECK(!output.counter_incremented);
    CHECK(!output.ordering_table_written);
    CHECK(output.packet_cursor_masked);
    CHECK(output.packet_word_write_mask == (UINT32_C(1) << 2u));
    CHECK(output.packet_words[2] ==
          ((uint16_t)output.vertices[0].x |
           ((uint32_t)(uint16_t)output.vertices[0].y << 16u)));
    for (vertex = 1u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex)
        CHECK((output.packet_word_write_mask &
               (UINT32_C(1) << (2u + vertex * 2u))) == 0u);
    CHECK(output.guest_packet_word_write_mask == (UINT32_C(1) << 2u));

    xg_host_3d_configure_native_view(1, 54 << 16);
    source.projection.screen_offset_x = 350 << 16;
    preparation.primitive_digest = byte_digest(&source, sizeof(source));
    CHECK(xg_world_models_native_build_primitive(
              &preparation, 1u, &source, &output) ==
          XG_WORLD_MODELS_NATIVE_OK);
    CHECK(output.passed_screen_cull);
    CHECK(output.accepted);
    CHECK(!output.counter_incremented);
    CHECK(output.primitive.triangles[0].vertices[0].projective_position);
    CHECK(output.primitive.triangles[0].vertices[0].projective_view_z ==
          output.vertices[0].projective_view_z);
    CHECK(output.primitive.triangles[0].vertices[0].projective_distance ==
          source.projection.projection_distance);
    CHECK(output.primitive.triangles[0].vertices[0]
              .interpolation_vertex_identity_valid);
    CHECK(output.primitive.triangles[0].vertices[0]
              .interpolation_group_id == UINT32_C(0x640a1000));
    CHECK(output.primitive.triangles[0].vertices[1]
              .interpolation_vertex_id == 1u);
    CHECK(output.primitive.triangles[0].vertices[1]
              .interpolation_vertex_id ==
          output.primitive.triangles[1].vertices[1]
              .interpolation_vertex_id);
    {
        const uint32_t first_group = output.primitive.triangles[0].vertices[0]
            .interpolation_group_id;

        source.model_header_address = UINT32_C(0x800a2000);
        preparation.primitive_digest = byte_digest(&source, sizeof(source));
        CHECK(xg_world_models_native_build_primitive(
                  &preparation, 1u, &source, &output) ==
              XG_WORLD_MODELS_NATIVE_OK);
        CHECK(output.primitive.triangles[0].vertices[0]
                  .interpolation_group_id != first_group);
        source.model_header_address = UINT32_C(0x800a1000);
        preparation.primitive_digest = byte_digest(&source, sizeof(source));
    }
    CHECK((output.packet_word_write_mask & (UINT32_C(1) << 2u)) != 0u);
    CHECK(output.guest_packet_word_write_mask == (UINT32_C(1) << 2u));

    preparation.guest_xclip_bound = INT32_MAX;
    CHECK(xg_world_models_native_build_primitive(
              &preparation, 1u, &source, &output) ==
          XG_WORLD_MODELS_NATIVE_OK);
    CHECK(output.accepted);
    CHECK(output.counter_incremented);
    CHECK(output.guest_ordering_table_written);
    CHECK(output.guest_packet_word_write_mask ==
          output.packet_word_write_mask);

    xg_host_3d_configure_native_view(0, 0);
    source.projection.screen_offset_x = 1010 << 16;
    preparation.primitive_digest = byte_digest(&source, sizeof(source));
    CHECK(xg_world_models_native_build_primitive(
              &preparation, 1u, &source, &output) ==
          XG_WORLD_MODELS_NATIVE_OK);
    CHECK((int32_t)output.projection_flags < 0);
    CHECK(output.nclip > 0);
    CHECK(!output.passed_screen_cull);
    CHECK(!output.accepted);
    CHECK(output.counter_incremented);
    CHECK(output.guest_ordering_table_written);
    CHECK(output.guest_packet_word_write_mask ==
          output.packet_word_write_mask);
    return 1;
}

typedef struct FinalizeProbe {
    uint32_t begin_count;
    uint32_t stage_count;
} FinalizeProbe;

static bool finalize_probe_begin(void *context) {
    FinalizeProbe *probe = (FinalizeProbe *)context;

    ++probe->begin_count;
    return true;
}

static bool finalize_probe_stage(
    void *context, const XgRenderIrNativePrimitive *primitive,
    uint32_t packet_address, uint32_t primitive_index) {
    FinalizeProbe *probe = (FinalizeProbe *)context;

    (void)primitive;
    (void)packet_address;
    (void)primitive_index;
    ++probe->stage_count;
    return true;
}

static int test_native_finalize_tracks_guest_count_separately(void) {
    XgWorldModelsNativePreparation preparation = {0};
    XgWorldModelsNativeDispatch dispatch = {0};
    XgWorldModelsNativeDispatchOutput dispatch_output = {0};
    XgWorldModelsNativePrimitiveSource sources[2] = {{0}};
    XgWorldModelsNativePrimitiveOutput outputs[2] = {{0}};
    XgWorldModelsNativeCommit commit;
    FinalizeProbe probe = {0};
    uint32_t index;

    dispatch.source_index = 7u;
    dispatch.primitive_count = 2u;
    dispatch.packet_cursor_after = UINT32_C(0x800b1020);
    dispatch.group_cursor_after = 3u;
    dispatch.primitive_family_mask = UINT32_C(1) << 4u;
    dispatch.model.vertex_count = 8u;
    dispatch.model.auxiliary_vertex_base = UINT32_C(0x800a2000);
    dispatch.model.vertex_base = UINT32_C(0x800a1000);
    dispatch.model.model_18 = UINT32_C(0x800a3000);
    dispatch.call.ordering_table_address = UINT32_C(0x800b2000);
    dispatch.bounds_accepted = true;
    dispatch.guest_bounds_accepted = true;

    for (index = 0u; index < 2u; ++index) {
        sources[index].packet_address = UINT32_C(0x800b1000) + index * 4u;
        sources[index].dispatch_index = 0u;
        sources[index].primitive_index = index;
        sources[index].primitive_family = 4u;
        sources[index].packet_word_count = 1u;
        outputs[index].packet_address = sources[index].packet_address;
        outputs[index].packet_word_count = 1u;
        outputs[index].packet_word_write_mask = 1u;
        outputs[index].accepted = true;
        outputs[index].ordering_table_written = true;
    }
    outputs[0].guest_packet_word_write_mask = 1u;
    outputs[0].counter_incremented = true;
    outputs[0].guest_ordering_table_written = true;

    dispatch_output.source_index = dispatch.source_index;
    dispatch_output.processed_primitive_count = 2u;
    dispatch_output.accepted_primitive_count = 2u;
    dispatch_output.packet_side_effect_primitive_count = 2u;
    dispatch_output.emitted_count_delta = 1u;
    dispatch_output.primitive_family_mask = dispatch.primitive_family_mask;
    dispatch_output.packet_cursor_after = dispatch.packet_cursor_after;
    dispatch_output.group_cursor_after = dispatch.group_cursor_after;
    dispatch_output.bounds_accepted = true;
    dispatch_output.guest_bounds_accepted = true;

    preparation.sealed_dispatches = &dispatch;
    preparation.sealed_primitives = sources;
    preparation.dispatch_digest = byte_digest(&dispatch, sizeof(dispatch));
    preparation.primitive_digest = byte_digest(sources, sizeof(sources));
    preparation.authentication_generation = 9u;
    preparation.continuation_pc = XG_WORLD_MODELS_PRODUCER_CONTINUATION_0;
    preparation.dispatch_count = 1u;
    preparation.primitive_count = 2u;
    preparation.authenticated = true;
    preparation.sealed = true;

    CHECK(xg_world_models_native_finalize(
              &preparation, 9u, &dispatch, &dispatch_output, 1u, sources,
              outputs, 2u, &probe, finalize_probe_begin,
              finalize_probe_stage, &commit) == XG_WORLD_MODELS_NATIVE_OK);
    CHECK(commit.resident_emitted_count == 1u);
    CHECK(commit.entry_side_effects.resident_emitted_count == 1u);
    CHECK(commit.accepted_primitive_count == 2u);
    CHECK(probe.begin_count == 1u);
    CHECK(probe.stage_count == 2u);
    return 1;
}

int main(void) {
    return test_builds_pre_dispatch_contract_and_side_effects() &&
           test_wrap_boundaries_and_signed_camera_shift() &&
           test_native_view_widens_coarse_depth_gate() &&
           test_entry_only_and_validation() &&
           test_dispatch_selector_table() &&
           test_raw_ft4_adapter_delegates_to_shared_kernel() &&
           test_depth_cued_quad_right_cull_writes_first_xy() &&
           test_native_finalize_tracks_guest_count_separately()
        ? 0 : 1;
}
