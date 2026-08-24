#ifndef XG_RENDER_AUTH_RUNTIME_CONTROL_H
#define XG_RENDER_AUTH_RUNTIME_CONTROL_H

#include "guest_render_types.h"
#include "native_render_mode_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct GpuRenderSemantic GpuRenderSemantic;

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*PsxXgRenderPresentationGate)(
    GuestRenderRenderMode requested_mode,
    NativeRenderPresentationSnapshot *out_snapshot,
    void *user_data);
typedef int (*PsxXgRenderExecPhaseExchange)(int phase);

bool psx_xg_render_auth_configure(
    GuestRenderTimingMode requested_timing_mode,
    GuestRenderRenderMode requested_render_mode,
    PsxXgRenderPresentationGate presentation_gate,
    void *presentation_user_data);
void psx_xg_render_auth_reset(void);
bool psx_xg_render_auth_configure_native_view(
    bool enabled, uint16_t aspect_num, uint16_t aspect_den,
    uint16_t canonical_width, uint16_t canonical_height);
void psx_xg_render_auth_set_terrain_temporal_coverage(bool enabled);
void psx_xg_render_auth_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
void psx_xg_render_auth_set_exec_phase_exchange(
    PsxXgRenderExecPhaseExchange exchange);
void psx_xg_render_auth_cold_enable(bool enabled);
void psx_xg_render_auth_scene_boundary(void);
void psx_xg_render_auth_before_gpu_submission(void);
void psx_xg_render_auth_note_gpu_semantic_current(
    const GpuRenderSemantic *semantic);
void psx_xg_render_auth_complete_gpu_source_frame(void);
bool psx_xg_render_auth_prepare_ui_ot(uint32_t start_addr);
void psx_xg_render_auth_source_reset(void);
void psx_xg_render_auth_ft4_geometry_enable(bool enabled);
void psx_xg_render_auth_producer_family_enable(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
