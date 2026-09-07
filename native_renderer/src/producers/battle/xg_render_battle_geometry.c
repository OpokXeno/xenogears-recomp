#include "xg_render_battle_geometry.h"

#include "cpu_state.h"
#include "xg_field_render_services.h"
#include "xg_host_3d.h"
#include "xg_render_producer_lifecycle.h"
#include "xg_render_runtime_host_services.h"
#include "xg_render_submission.h"

#include <stdlib.h>

static XgHost3dProjectedVertex *projected;
static uint32_t projected_capacity;
static volatile struct {
    uint64_t attempts, completed, rejected, bound_polygons, projected_vertices;
    uint64_t native_polygons, capacity_rejected;
    uint32_t last_model, last_mode, last_family, last_packet;
} battle_geometry_diagnostics;

bool xg_render_battle_geometry_capture(
        const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle) {
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
    XgHost3dProjection projection;
    uint32_t model, vertex_base, topology, packet, packet_bytes, vertex_count, group_count;

    ++battle_geometry_diagnostics.attempts;
    if (!cpu || !lifecycle || !lifecycle->guest_data_range_is_valid ||
        !xg_render_submission_native_work_mode() ||
        !xg_render_runtime_host_services(&host) || !host.read_word ||
        !host.native_text_authorizes_pc ||
        !host.native_text_authorizes_pc(0x8002c700u) ||
        !xg_render_battle_geometry_authorizes_call(cpu->gpr[31] - 8u))
        goto reject;
    model = cpu->gpr[4];
    packet = cpu->gpr[5];
    battle_geometry_diagnostics.last_model = model;
    battle_geometry_diagnostics.last_mode = cpu->gpr[7];
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
        if (!xg_host_3d_rtps(&projection, &local, &projected[v], &flags)) goto reject;
    }
    battle_geometry_diagnostics.projected_vertices += vertex_count;
    for (uint32_t group = 0; group < group_count; ++group) {
        if (!lifecycle->guest_data_range_is_valid(topology, 4u, 4u, false)) goto reject;
        const uint32_t header = host.read_word(topology);
        const uint32_t family = header & 0xffu, count = header >> 16u;
        battle_geometry_diagnostics.last_family = family;
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
                    target->x = (int32_t)source->x * 65536;
                    target->y = (int32_t)source->y * 65536;
                    target->native_view_x = source->native_view_x_16_16;
                    target->native_view_y = source->native_view_y_16_16;
                    target->native_view_position = source->native_view_position &&
                        source->projective_view_z > 0;
                    target->projective_view_x = source->projective_view_x;
                    target->projective_view_y = source->projective_view_y;
                    target->projective_view_z = source->projective_view_z;
                    target->projective_offset_x = source->projective_offset_x_16_16;
                    target->projective_offset_y = source->projective_offset_y_16_16;
                    target->projective_native_offset_x = source->projective_native_offset_x_16_16;
                    target->projective_native_offset_y = source->projective_native_offset_y_16_16;
                    target->projective_distance = source->projective_distance;
                    target->projective_position = source->projective_position;
                    native &= target->native_view_position != 0u;
                }
            }
            battle_geometry_diagnostics.last_packet = packet;
            /* A binding is NOT a draw. GPU acceptance validates canonical XY
             * and layout, supplies final material, and retains real OT order.
             * A culled/unlinked slot can never inject a draw at frame end. */
            if (xg_render_submission_stage_exact((GpuRenderTransactionId){0},
                    (packet & 0x1fffffffu) + 4u, &semantic) != GUEST_RENDER_TRANSACTION_OK) {
                ++battle_geometry_diagnostics.capacity_rejected;
                goto reject;
            }
            ++battle_geometry_diagnostics.bound_polygons;
            battle_geometry_diagnostics.native_polygons += native;
            packet += packet_sizes[family];
            packet_bytes -= packet_sizes[family];
        }
        topology += 4u + count * 8u;
    }
    ++battle_geometry_diagnostics.completed;
    return true;
reject:
    ++battle_geometry_diagnostics.rejected;
    return false;
}
