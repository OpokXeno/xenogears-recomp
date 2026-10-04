#include "xg_render_battle_geometry.h"

#include "cpu_state.h"
#include "xg_render_model_kernel.h"

static volatile XgRenderModelKernelStats battle_geometry_diagnostics;

/* Articulated parts (8009f844) and arena models (800a48ec) keep their
 * 0x7c-byte part record in s1 across the draw call: a stable, unique identity
 * per part across frames, unlike the double-buffered packets or the mesh,
 * which identical combatants share. */
static uint32_t battle_part_identity(const CPUState *cpu) {
    const uint32_t caller = (cpu->gpr[31] & 0x1fffffffu) | 0x80000000u;
    if (caller != 0x800a006cu && caller != 0x800a4aecu) return 0u;
    return cpu->gpr[17] & 0x1fffffffu;
}

bool xg_render_battle_geometry_capture(
        const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle) {
    if (!cpu || !xg_render_battle_geometry_authorizes_call(cpu->gpr[31] - 8u)) {
        ++battle_geometry_diagnostics.rejected;
        return false;
    }
    const XgRenderModelKernelCall call = {
        .entity_tag = UINT64_C(0x4254000000000000),
        .camera_id = UINT64_C(0x425443414d455241),
        .source_pc = cpu->gpr[31] - 8u,
        .identity = battle_part_identity(cpu),
        .depth_family = XG_RENDER_DEPTH_FAMILY_BATTLE_GEOMETRY,
    };
    return xg_render_model_kernel_capture(cpu, lifecycle, &call,
                                          &battle_geometry_diagnostics);
}
