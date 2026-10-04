#include "xg_render_model_kernel.h"

#include "cpu_state.h"
#include "xg_field_render_services.h"
#include "xg_host_3d.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_resource_repository.h"
#include "xg_render_producer_lifecycle.h"
#include "xg_render_runtime_host_services.h"
#include "xg_render_submission.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static XgHost3dProjectedVertex *projected;
static XgHost3dVector *locals;
static uint32_t projected_capacity;

/* An identity can be drawn more than once per source update (afterimages,
 * reflections), each with its own matrix. Number the repeats in draw order so
 * every occurrence keeps its own entity across frames. */
#define RIGID_OCCURRENCE_SLOTS 1024u
static struct { uint32_t identity; uint32_t count; uint64_t update; }
    rigid_occurrences[RIGID_OCCURRENCE_SLOTS];

static uint32_t rigid_occurrence(uint32_t identity, uint64_t update) {
    uint32_t slot = (identity >> 2) * 2654435761u % RIGID_OCCURRENCE_SLOTS;
    for (uint32_t probe = 0; probe < RIGID_OCCURRENCE_SLOTS; ++probe) {
        __typeof__(rigid_occurrences[0]) *entry = &rigid_occurrences[slot];
        if (entry->update != update || entry->identity == identity) {
            if (entry->update != update) *entry = (__typeof__(*entry)){identity, 0u, update};
            return entry->count++;
        }
        slot = (slot + 1u) % RIGID_OCCURRENCE_SLOTS;
    }
    return UINT32_MAX;
}

bool xg_render_rigid_pose_publish(const XgRenderRigidPoseRequest *request,
                                  const XgHost3dProjection *projection,
                                  XgRenderMotionSource *source,
                                  XgRenderMotionRef *out) {
    XgRenderMotionPose pose = {0};
    XgHost3dMatrix view = {0}, camera = {0};
    if (!request || !projection || !source || !out || !request->identity ||
        !projection->projection_distance ||
        !psx_xg_render_motion_source(request->source_pc, source))
        return false;
    const uint32_t identity = request->identity;
    memcpy(view.rotation, projection->rotation, sizeof(view.rotation));
    memcpy(view.translation, projection->translation, sizeof(view.translation));
    if (request->world_space) {
        /* R * (local - offset) + T: the offset moves into the translation,
         * rounded to the integer the matrix holds. Endpoints stay anchored
         * to the exact guest projection; only phase deltas see the rounding. */
        for (unsigned r = 0; r < 3; ++r) {
            int64_t moved = 0;
            for (unsigned c = 0; c < 3; ++c)
                moved += (int64_t)view.rotation[r][c] * request->local_offset[c];
            view.translation[r] -= (int32_t)llround((double)moved / 4096.0);
        }
        if (!xg_render_motion_camera_from_view(&view, &pose.camera)) return false;
        pose.nodes[0].local.rotation[3] = 1.0;
        for (unsigned axis = 0; axis < 3; ++axis) pose.nodes[0].local.scale[axis] = 1.0;
    } else {
        for (unsigned axis = 0; axis < 3; ++axis) camera.rotation[axis][axis] = 4096;
        if (!xg_render_motion_camera_from_view(&camera, &pose.camera) ||
            !xg_render_motion_decompose(&view, &pose.nodes[0].local))
            return false;
    }
    const uint32_t occurrence = rigid_occurrence(identity, source->source_update);
    if (occurrence > 0xffffu) return false;
    pose.node_count = 1;
    pose.translation_stage = XG_RENDER_MOTION_TRANSLATION_AFFINE;
    pose.entity_id = request->entity_tag | ((uint64_t)occurrence << 32u) | identity;
    pose.camera_id = request->camera_id;
    pose.geometry_id = xg_render_resource_digest(request->key, sizeof(request->key));
    pose.geometry_generation = 1;
    pose.geometry_scale = 1;
    pose.screen_offset[0] = projection->screen_offset_x / 65536.0;
    pose.screen_offset[1] = projection->screen_offset_y / 65536.0;
    pose.projection_distance = projection->projection_distance;
    pose.nodes[0].id = identity;
    pose.nodes[0].parent = -1;
    pose.nodes[0].policy = XG_RENDER_MOTION_LOCAL_TRS;
    pose.nodes[0].source_matrix_valid = 1;
    pose.nodes[0].source_model_to_view = view;
    if (!xg_render_motion_publish(source, &pose, out)) return false;
    return xg_render_motion_watch(*out, request->watch[0][0], request->watch[0][1]) &&
           xg_render_motion_watch(*out, request->watch[1][0], request->watch[1][1]);
}

void xg_render_projected_vertex_semantic(GpuRenderSemanticVertex *target,
                                         const XgHost3dProjectedVertex *source) {
    target->x = (int32_t)source->x * 65536;
    target->y = (int32_t)source->y * 65536;
    target->native_view_x = source->native_view_x_16_16;
    target->native_view_y = source->native_view_y_16_16;
    target->native_view_position = source->native_view_position &&
        source->projective_view_z > 0;
    target->native_view_depth = source->native_view_depth_q12;
    target->projective_view_x = source->projective_view_x;
    target->projective_view_y = source->projective_view_y;
    target->projective_view_z = source->projective_view_z;
    target->projective_offset_x = source->projective_offset_x_16_16;
    target->projective_offset_y = source->projective_offset_y_16_16;
    target->projective_native_offset_x = source->projective_native_offset_x_16_16;
    target->projective_native_offset_y = source->projective_native_offset_y_16_16;
    target->projective_distance = source->projective_distance;
    target->projective_position = source->projective_position;
}

bool xg_render_rigid_stage_ft4(const XgHost3dProjection *projection,
                               uint32_t packet, const XgHost3dVector local[4],
                               const XgHost3dVector *motion_local,
                               const uint32_t ids[4], uint32_t identity,
                               uint32_t primitive_id,
                               const XgRenderMotionRef *motion,
                               uint64_t continuity_generation,
                               XgRenderDepthFamily depth_family) {
    static const uint8_t split[2][3] = {{0, 1, 2}, {2, 1, 3}};
    XgHost3dProjectedVertex screen[4];
    uint32_t flags;
    for (unsigned v = 0; v < 4u; ++v)
        if (!xg_host_3d_rtps(projection, &local[v], &screen[v], &flags))
            return false;
    const uint32_t command_id = (packet & 0x1fffffffu) + 4u;
    GpuRenderSemantic semantic = {0};
    if (motion)
        xg_render_semantic_set_interpolation_identity(
            &semantic, continuity_generation, identity, primitive_id);
    semantic.material.textured = 1;
    semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    semantic.triangle_count = 2;
    XgRenderMotionDrawBinding binding = {.triangle_count = 2, .native_exact = 1u};
    if (motion) binding.motion = *motion;
    for (unsigned t = 0; t < 2u; ++t) {
        semantic.triangles[t].split_index = (uint8_t)t;
        semantic.triangles[t].split_count = 2;
        for (unsigned v = 0; v < 3u; ++v) {
            const unsigned corner = split[t][v];
            GpuRenderSemanticVertex *target = &semantic.triangles[t].vertices[v];
            xg_render_projected_vertex_semantic(target, &screen[corner]);
            if (motion) {
                target->interpolation_group_id = identity;
                target->interpolation_vertex_id = ids[corner];
                target->interpolation_vertex_identity_valid = 1u;
            }
            binding.local[t][v] = motion_local ? motion_local[corner] : local[corner];
            binding.vertex_ids[t][v] = ids[corner];
        }
    }
    xg_render_depth_policy_stamp_semantic(&semantic, depth_family);
    if (xg_render_submission_stage_exact((GpuRenderTransactionId){0}, command_id,
                                         &semantic) != GUEST_RENDER_TRANSACTION_OK)
        return false;
    (void)xg_render_motion_register_command(command_id, motion ? &binding : NULL,
                                            motion ? identity : 0u,
                                            motion ? primitive_id : 0u);
    return true;
}

bool xg_render_model_kernel_capture(const CPUState *cpu,
                                    const XgRenderProducerLifecycleServices *lifecycle,
                                    const XgRenderModelKernelCall *call,
                                    volatile XgRenderModelKernelStats *stats) {
    /* slus_006.64 dispatch LUT 8004fe50: 17 polygon rows, topology stride 8.
     * Bit 3 selects quads, bit 1 Gouraud, bit 0 texture; bit 2 is the material
     * initialization variant. Row 16 is environment-mapped FT3. All six
     * depth/lighting modes share this layout. */
    static const uint8_t packet_sizes[17] = {
        20, 32, 28, 40, 20, 32, 28, 40,
        24, 40, 36, 52, 24, 40, 36, 52,
        32,
    };
    static const uint8_t split[2][3] = {{0, 1, 2}, {2, 1, 3}};
    XgRenderRuntimeHostServices host;
    /* Zeroed: the GTE capture fills only the canonical fields, and a stale
     * native_transform_valid would project every vertex through garbage. */
    XgHost3dProjection projection = {0};
    uint32_t model, vertex_base, topology, packet, packet_bytes, vertex_count, group_count;

    if (!stats || !call) return false;
    ++stats->attempts;
    if (!cpu || !lifecycle || !lifecycle->guest_data_range_is_valid ||
        !xg_render_submission_native_work_mode() ||
        !xg_render_runtime_host_services(&host) || !host.read_word ||
        !host.native_text_authorizes_pc ||
        !host.native_text_authorizes_pc(0x8002c700u))
        goto reject;
    model = cpu->gpr[4];
    packet = cpu->gpr[5];
    stats->last_model = model;
    stats->last_mode = cpu->gpr[7];
    if (cpu->gpr[7] > 5u ||
        !lifecycle->guest_data_range_is_valid(model, 0x38u, 4u, false))
        goto reject;
    vertex_count = host.read_word(model) >> 16u;
    group_count = host.read_word(model + 4u) >> 16u;
    vertex_base = host.read_word(model + 8u);
    topology = host.read_word(model + 0x10u);
    packet_bytes = host.read_word(model + 0x34u);
    if (!vertex_count || !group_count || !packet_bytes ||
        !lifecycle->guest_data_range_is_valid(vertex_base, vertex_count * 8u, 4u, false) ||
        !lifecycle->guest_data_range_is_valid(packet, packet_bytes, 4u, false))
        goto reject;
    if (vertex_count > projected_capacity) {
        XgHost3dProjectedVertex *grown = realloc(projected,
            (size_t)vertex_count * sizeof(*projected));
        if (!grown) goto reject;
        projected = grown;
        XgHost3dVector *grown_locals = realloc(locals, (size_t)vertex_count * sizeof(*locals));
        if (!grown_locals) goto reject;
        locals = grown_locals;
        projected_capacity = vertex_count;
    }
    xg_render_runtime_capture_shadow_projection(cpu, &projection);
    /* Project each shared vertex once, before the guest bounding-box cull.
     * Host reads do not charge guest cycles or execute any source instructions. */
    for (uint32_t v = 0; v < vertex_count; ++v) {
        const uint32_t xy = host.read_word(vertex_base + v * 8u);
        const uint32_t z = host.read_word(vertex_base + v * 8u + 4u);
        const XgHost3dVector local = {(int16_t)xy, (int16_t)(xy >> 16u), (int16_t)z, 0};
        uint32_t flags;
        locals[v] = local;
        if (!xg_host_3d_rtps(&projection, &local, &projected[v], &flags)) goto reject;
    }
    stats->projected_vertices += vertex_count;
    const uint32_t identity = call->identity;
    XgRenderMotionSource motion_source = {0};
    XgRenderMotionRef motion = {0};
    if (identity) {
        const XgRenderRigidPoseRequest request = {
            .entity_tag = call->entity_tag, .camera_id = call->camera_id,
            .source_pc = call->source_pc, .identity = identity,
            .key = {model, vertex_base, topology, vertex_count},
            .watch = {{model, 0x38u}, {vertex_base, vertex_count * 8u}},
        };
        if (xg_render_rigid_pose_publish(&request, &projection, &motion_source, &motion))
            ++stats->motion_published;
        else
            ++stats->motion_rejected;
    }
    for (uint32_t group = 0; group < group_count; ++group) {
        if (!lifecycle->guest_data_range_is_valid(topology, 4u, 4u, false)) goto reject;
        const uint32_t header = host.read_word(topology);
        const uint32_t family = header & 0xffu, count = header >> 16u;
        stats->last_family = family;
        if (family >= 17u || count > 0x7fffu ||
            !lifecycle->guest_data_range_is_valid(topology, 4u + count * 8u, 4u, false) ||
            count > packet_bytes / packet_sizes[family] ||
            host.read_word(0x8004fe6cu + family * 40u) != 8u ||
            host.read_word(0x8004fe74u + family * 40u) != packet_sizes[family])
            goto reject;
        const uint32_t corners = family & 8u ? 4u : 3u;
        for (uint32_t p = 0; p < count; ++p) {
            const uint32_t first = host.read_word(topology + 4u + p * 8u);
            const uint32_t second = host.read_word(topology + 8u + p * 8u);
            const uint32_t indices[4] = {first & 0xffffu, first >> 16u,
                                        second & 0xffffu, second >> 16u};
            GpuRenderSemantic semantic = {0};
            bool native = true;
            for (uint32_t v = 0; v < corners; ++v)
                if (indices[v] >= vertex_count) goto reject;
            const uint32_t command_id = (packet & 0x1fffffffu) + 4u;
            const uint32_t primitive_id = ((topology + 4u + p * 8u) & 0x1fffffffu) |
                                          (corners == 3u ? 1u : 0u);
            if (motion.handle.resource_id)
                xg_render_semantic_set_interpolation_identity(
                    &semantic, motion_source.continuity_generation, identity, primitive_id);
            semantic.material.textured = (family & 1u) != 0u || family == 16u;
            semantic.material.shading = family & 2u
                ? GPU_RENDER_SHADING_GOURAUD : GPU_RENDER_SHADING_FLAT;
            semantic.triangle_count = corners - 2u;
            for (uint32_t t = 0; t < semantic.triangle_count; ++t) {
                semantic.triangles[t].split_index = (uint8_t)t;
                semantic.triangles[t].split_count = (uint8_t)semantic.triangle_count;
                for (uint32_t v = 0; v < 3u; ++v) {
                    const XgHost3dProjectedVertex *source = &projected[indices[split[t][v]]];
                    GpuRenderSemanticVertex *target = &semantic.triangles[t].vertices[v];
                    xg_render_projected_vertex_semantic(target, source);
                    if (motion.handle.resource_id) {
                        target->interpolation_group_id = identity;
                        target->interpolation_vertex_id = indices[split[t][v]];
                        target->interpolation_vertex_identity_valid = 1u;
                    }
                    native &= target->native_view_position != 0u;
                }
            }
            xg_render_depth_policy_stamp_semantic(&semantic, call->depth_family);
            stats->last_packet = packet;
            /* A binding is NOT a draw. GPU acceptance validates canonical XY
             * and layout, supplies final material, and retains real OT order.
             * A culled/unlinked slot can never inject a draw at frame end. */
            if (xg_render_submission_stage_exact((GpuRenderTransactionId){0},
                    command_id, &semantic) != GUEST_RENDER_TRANSACTION_OK) {
                ++stats->capacity_rejected;
                goto reject;
            }
            /* Packet slots are reused every frame: always replace or forget
             * this slot's LOCAL binding so a stale pose can never apply. */
            if (motion.handle.resource_id) {
                XgRenderMotionDrawBinding binding = {.motion = motion,
                    .triangle_count = semantic.triangle_count, .native_exact = 1u};
                for (uint32_t t = 0; t < binding.triangle_count; ++t)
                    for (uint32_t v = 0; v < 3u; ++v) {
                        binding.local[t][v] = locals[indices[split[t][v]]];
                        binding.local[t][v].pad = 0;
                        binding.vertex_ids[t][v] = indices[split[t][v]];
                    }
                if (xg_render_motion_register_command(command_id, &binding, identity, primitive_id))
                    ++stats->motion_bound;
            } else
                (void)xg_render_motion_register_command(command_id, NULL, 0u, 0u);
            ++stats->bound_polygons;
            stats->native_polygons += native;
            packet += packet_sizes[family];
            packet_bytes -= packet_sizes[family];
        }
        topology += 4u + count * 8u;
    }
    ++stats->completed;
    return true;
reject:
    ++stats->rejected;
    return false;
}
