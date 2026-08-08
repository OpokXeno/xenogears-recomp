#include "xg_field_character_source_adapter.h"

#include <stddef.h>
#include <string.h>

static int32_t shift_right_floor_i32(int32_t value, unsigned shift) {
    int64_t magnitude;

    if (shift == 0u || value >= 0) return value / (INT32_C(1) << shift);
    magnitude = -(int64_t)value;
    return (int32_t)(-((magnitude + ((INT64_C(1) << shift) - 1)) >> shift));
}

static int build_projection(const XgFieldCharacterSourceSnapshot *snapshot,
                            XgFieldCharacterSourceDerived *derived) {
    const XgHost3dLongVector seed = { 0, 0, 0x1000 };
    XgHost3dLongVector orientation;
    XgHost3dLongVector cross;
    XgHost3dLongVector first_axis;
    XgHost3dLongVector third_axis;
    XgHost3dLongVector translation;
    XgHost3dLongVector scale;
    XgHost3dMatrix camera = { 0 };
    XgHost3dMatrix actor = { 0 };
    XgHost3dRotAverage4Input input = { 0 };
    uint32_t flags;
    unsigned row;
    unsigned column;

    orientation = (XgHost3dLongVector){ snapshot->actor.orientation[0],
                                       snapshot->actor.orientation[1],
                                       snapshot->actor.orientation[2] };
    if (!xg_host_3d_op12(&seed, &orientation, &cross, &flags) ||
        !xg_host_3d_vector_normal(&cross, &first_axis) ||
        !xg_host_3d_op12(&first_axis, &orientation, &cross, &flags) ||
        !xg_host_3d_vector_normal(&cross, &third_axis))
        return 0;
    for (row = 0u; row < 3u; ++row) {
        for (column = 0u; column < 3u; ++column)
            camera.rotation[row][column] =
                snapshot->source_matrix.rotation[row * 3u + column];
        camera.translation[row] = snapshot->source_matrix.translation[row];
    }
    actor.rotation[0][0] = (int16_t)first_axis.x;
    actor.rotation[0][1] = (int16_t)first_axis.y;
    actor.rotation[0][2] = (int16_t)first_axis.z;
    actor.rotation[1][0] = (int16_t)orientation.x;
    actor.rotation[1][1] = (int16_t)orientation.y;
    actor.rotation[1][2] = (int16_t)orientation.z;
    actor.rotation[2][0] = (int16_t)third_axis.x;
    actor.rotation[2][1] = (int16_t)third_axis.y;
    actor.rotation[2][2] = (int16_t)third_axis.z;
    for (column = 0u; column < 3u; ++column) {
        XgHost3dVector source = { actor.rotation[0][column],
                                  actor.rotation[1][column],
                                  actor.rotation[2][column], 0u };
        XgHost3dVector output;

        if (!xg_host_3d_rtir(&camera, &source, &output, &flags)) return 0;
        derived->object_to_view.rotation[0][column] = output.x;
        derived->object_to_view.rotation[1][column] = output.y;
        derived->object_to_view.rotation[2][column] = output.z;
    }
    translation = (XgHost3dLongVector){ snapshot->actor.world_offset[0],
                                        snapshot->actor.world_offset[1],
                                        snapshot->actor.world_offset[2] };
    if (!xg_host_3d_rt(&camera, &translation, &translation, &flags)) return 0;
    derived->object_to_view.translation[0] = translation.x;
    derived->object_to_view.translation[1] = translation.y;
    derived->object_to_view.translation[2] = translation.z;
    scale.x = shift_right_floor_i32(
        (int32_t)snapshot->actor.shadow_scale[0] *
            snapshot->units.scale_numerator,
        snapshot->actor.scale_shift);
    scale.y = shift_right_floor_i32(
        (int32_t)snapshot->actor.shadow_scale[1] *
            snapshot->units.scale_numerator,
        snapshot->actor.scale_shift);
    scale.z = shift_right_floor_i32(
        (int32_t)snapshot->actor.shadow_scale[2] *
            snapshot->units.scale_numerator,
        snapshot->actor.scale_shift);
    if (!xg_host_3d_scale_matrix(&derived->object_to_view, &scale)) return 0;

    for (row = 0u; row < 3u; ++row) {
        for (column = 0u; column < 3u; ++column)
            input.projection.rotation[row][column] =
                derived->object_to_view.rotation[row][column];
        input.projection.translation[row] =
            derived->object_to_view.translation[row];
    }
    input.projection.screen_offset_x = snapshot->projection.screen_offset_x;
    input.projection.screen_offset_y = snapshot->projection.screen_offset_y;
    input.projection.projection_distance =
        snapshot->projection.projection_distance;
    input.projection.average_z_scale4 =
        snapshot->projection.average_z_scale4;
    for (row = 0u; row < XG_HOST_3D_VERTEX_COUNT; ++row) {
        input.vertices[row] = (XgHost3dVector){
            snapshot->model_vertices[row].x,
            snapshot->model_vertices[row].y,
            snapshot->model_vertices[row].z,
            snapshot->model_vertices[row].pad,
        };
    }
    if (!xg_host_3d_rot_average4(&input, &derived->projection)) return 0;
    derived->ordering_bucket =
        derived->projection.ordering_depth >> snapshot->ordering.ordering_shift;
    return 1;
}

static int digest_is_present(const uint8_t *digest) {
    size_t index;

    for (index = 0u; index < XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE; ++index)
        if (digest[index] != 0u) return 1;
    return 0;
}

XgFieldCharacterSourceAdapterResult xg_field_character_source_adapter_build(
    const XgFieldCharacterSourceSnapshot *snapshot,
    XgFieldCharacterCandidate *out_candidate,
    XgFieldCharacterSourceDerived *out_derived) {
    const uint32_t forbidden_reads = snapshot == NULL ? 1u :
        snapshot->absence.packet_arena_read_count |
        snapshot->absence.ot_payload_read_count |
        snapshot->absence.vram_read_count |
        snapshot->absence.post_gte_read_count |
        snapshot->absence.general_guest_read_count;

    XgFieldCharacterSourceDerived derived;
    XgFieldCharacterCapture capture;
    size_t index;

    if (out_candidate == NULL || out_derived == NULL || snapshot == NULL)
        return XG_FIELD_CHARACTER_SOURCE_ADAPTER_INVALID_ARGUMENT;
    memset(out_candidate, 0, sizeof(*out_candidate));
    memset(out_derived, 0, sizeof(*out_derived));
    memset(&derived, 0, sizeof(derived));
    memset(&capture, 0, sizeof(capture));
    if (snapshot->schema_version !=
            XG_FIELD_CHARACTER_SOURCE_SCHEMA_VERSION ||
        snapshot->presence_mask !=
            XG_FIELD_CHARACTER_SOURCE_REQUIRED_PRESENCE ||
        snapshot->identity.producer_entry !=
            XG_FIELD_CHARACTER_SOURCE_PRODUCER_ENTRY ||
        snapshot->identity.producer_record_id == 0u ||
        snapshot->identity.actor_count == 0u ||
        snapshot->identity.actor_index >= snapshot->identity.actor_count ||
        snapshot->identity.model_initializer !=
            XG_FIELD_CHARACTER_SOURCE_MODEL_INITIALIZER_8007AA44 ||
        !digest_is_present(snapshot->identity.game_sha256) ||
        !digest_is_present(snapshot->identity.manifest_sha256) ||
        snapshot->generation.source_generation == 0u ||
        snapshot->generation.visual_state.scene_epoch == 0u ||
        snapshot->absence.authenticated_read_count == 0u ||
        snapshot->absence.authenticated_read_count >
            XG_FIELD_CHARACTER_SOURCE_MAX_AUTHENTICATED_READS ||
        snapshot->absence.authenticated_read_bytes == 0u ||
        snapshot->ordering.ft4_index_from_actor_state != 1u ||
        snapshot->ordering.ot_base_from_producer_argument != 1u ||
        snapshot->source_matrix.source_identity !=
            XG_FIELD_CHARACTER_SOURCE_MATRIX_FIELD_CAMERA_800AFA64 ||
        snapshot->material.source_identity !=
            XG_FIELD_CHARACTER_SOURCE_MATERIAL_INITIALIZER_8007AA44 ||
        snapshot->material.present_mask !=
            XG_FIELD_CHARACTER_SOURCE_MATERIAL_REQUIRED ||
        snapshot->projection.source_identity !=
            XG_FIELD_CHARACTER_SOURCE_PROJECTION_SETUP_80074108 ||
        snapshot->projection.screen_offset_fraction_bits != 16u ||
        snapshot->projection.depth_cue_output_unused != 1u ||
        snapshot->ordering.source_identity !=
            XG_FIELD_CHARACTER_SOURCE_ORDERING_800769EC ||
        snapshot->units.matrix_fraction_bits != 12u ||
        snapshot->units.scale_product_fraction_bits != 12u ||
        snapshot->units.model_axis_seed != 0x1000 ||
        snapshot->units.scale_numerator != 0x0c00 ||
        snapshot->authenticated != 1u || snapshot->sealed != 1u ||
        snapshot->absence.rigid_quad_has_no_pose != 1u ||
        snapshot->absence.lighting_not_used != 1u ||
        snapshot->absence.depth_cue_output_not_consumed != 1u ||
        snapshot->absence.packet_is_sink_only != 1u ||
        forbidden_reads != 0u ||
        !xg_field_character_source_digest_matches(snapshot))
        return XG_FIELD_CHARACTER_SOURCE_ADAPTER_INVALID_SNAPSHOT;
    if (snapshot->culling.producer_disabled != 0u ||
        snapshot->culling.actor_active != 1u ||
        snapshot->culling.state_visible != 1u)
        return XG_FIELD_CHARACTER_SOURCE_ADAPTER_CULLED;
    if ((snapshot->absence.unresolved_mask &
         XG_FIELD_CHARACTER_SOURCE_MATERIAL_UNRESOLVED) != 0u)
        return XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_MATERIAL;
    if ((snapshot->absence.unresolved_mask &
         XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_ACTOR_MODEL_TRANSFORM) != 0u)
        return XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_TRANSFORM;
    if ((snapshot->absence.unresolved_mask &
         (XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_ORDERING_DEPTH |
           XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_ORDERING_BUCKET)) != 0u)
        return XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_ORDERING;
    if (snapshot->absence.unresolved_mask != 0u ||
        !build_projection(snapshot, &derived))
        return XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_TRANSFORM;
    for (index = 0u; index < XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT; ++index) {
        capture.vertices[index].x = derived.projection.vertices[index].x;
        capture.vertices[index].y = derived.projection.vertices[index].y;
        capture.vertices[index].u = snapshot->material.u[index];
        capture.vertices[index].v = snapshot->material.v[index];
    }
    capture.red = snapshot->material.red;
    capture.green = snapshot->material.green;
    capture.blue = snapshot->material.blue;
    capture.tpage = snapshot->material.tpage;
    capture.clut_x = snapshot->material.clut_x;
    capture.clut_y = snapshot->material.clut_y;
    capture.semi_transparent = snapshot->material.semi_transparent;
    capture.draw_area_left = snapshot->raster.draw_area_left;
    capture.draw_area_top = snapshot->raster.draw_area_top;
    capture.draw_area_right = snapshot->raster.draw_area_right;
    capture.draw_area_bottom = snapshot->raster.draw_area_bottom;
    capture.draw_offset_x = snapshot->raster.draw_offset_x;
    capture.draw_offset_y = snapshot->raster.draw_offset_y;
    capture.texture_window_mask_x = snapshot->raster.texture_window_mask_x;
    capture.texture_window_mask_y = snapshot->raster.texture_window_mask_y;
    capture.texture_window_offset_x = snapshot->raster.texture_window_offset_x;
    capture.texture_window_offset_y = snapshot->raster.texture_window_offset_y;
    capture.dither = snapshot->raster.dither;
    capture.mask_set = snapshot->raster.mask_set;
    capture.mask_check = snapshot->raster.mask_check;
    if (xg_field_character_adapter_build(&capture, out_candidate) !=
        XG_FIELD_CHARACTER_ADAPTER_OK)
        return XG_FIELD_CHARACTER_SOURCE_ADAPTER_INVALID_SNAPSHOT;
    *out_derived = derived;
    return XG_FIELD_CHARACTER_SOURCE_ADAPTER_OK;
}
