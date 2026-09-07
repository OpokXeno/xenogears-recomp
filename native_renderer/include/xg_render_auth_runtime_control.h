#ifndef XG_RENDER_AUTH_RUNTIME_CONTROL_H
#define XG_RENDER_AUTH_RUNTIME_CONTROL_H

#include "guest_render_types.h"
#include "native_render_mode_types.h"
#include "xg_render_semantic_presentation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GpuRenderSemantic GpuRenderSemantic;
typedef struct GpuVramEvent GpuVramEvent;
typedef struct PsxXgRenderCheckpointRestore PsxXgRenderCheckpointRestore;
typedef struct XgRenderSourceFrameDescription XgRenderSourceFrameDescription;

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*PsxXgRenderPresentationGate)(
    GuestRenderRenderMode requested_mode,
    NativeRenderPresentationSnapshot *out_snapshot,
    void *user_data);
typedef int (*PsxXgRenderExecPhaseExchange)(int phase);

bool psx_xg_render_auth_configure(
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
bool psx_xg_render_auth_movie_owner_active(void);
void psx_xg_render_auth_scene_boundary(void);
void psx_xg_render_auth_scene_boundary_after_timeline_invalidation(void);
void psx_xg_render_auth_before_gpu_submission(void);
void psx_xg_render_auth_note_gpu_semantic_current(
    const GpuRenderSemantic *semantic);
bool psx_xg_render_auth_describe_native_work(
    XgRenderSourceFrameDescription *description);
bool psx_xg_render_auth_accept_native_draw(const GpuRenderSemantic *semantic);
void psx_xg_render_auth_set_native_work_mode(bool enabled);
void psx_xg_render_auth_complete_gpu_source_frame(void);
bool psx_xg_render_auth_note_vram_event(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle,
    const GpuVramEvent *event);
size_t psx_xg_render_auth_checkpoint_size(void);
bool psx_xg_render_auth_checkpoint_write(
    void *out_checkpoint, size_t checkpoint_size);
bool psx_xg_render_auth_checkpoint_prepare(
    const void *checkpoint, size_t checkpoint_size,
    PsxXgRenderCheckpointRestore **out_restore);
void psx_xg_render_auth_checkpoint_commit(
    PsxXgRenderCheckpointRestore *restore);
/* Boot-state commit pre-applies the destructive restore-timeline boundary;
 * the following public GPU restore event records it without invalidating the
 * newly installed checkpoint a second time. */
void psx_xg_render_auth_checkpoint_commit_boot_restore(
    PsxXgRenderCheckpointRestore *restore);
void psx_xg_render_auth_checkpoint_cancel(
    PsxXgRenderCheckpointRestore *restore);
bool psx_xg_render_auth_checkpoint_restore(
    const void *checkpoint, size_t checkpoint_size);
uint32_t psx_xg_render_auth_checkpoint_failure_stage(void);
bool psx_xg_render_auth_note_vram_upload(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    const uint16_t *pixels, size_t pixel_count);
bool psx_xg_render_auth_note_vram_readback(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    const uint16_t *pixels, size_t pixel_count);
bool psx_xg_render_auth_note_vram_readback_digest(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    size_t pixel_count, uint64_t content_digest);
bool psx_xg_render_auth_note_vram_move(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    const uint16_t *pixels, size_t pixel_count);
bool psx_xg_render_auth_note_vram_clear(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    const uint16_t *pixels, size_t pixel_count);
bool psx_xg_render_auth_source_boundary(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle);
uint64_t psx_xg_render_auth_timeline_invalidate(
    XgRenderTimelineInvalidationReason reason);
bool psx_xg_render_auth_prepare_ui_ot(uint32_t start_addr);
void psx_xg_render_auth_complete_ordering_table(
    uint32_t start_addr, uint32_t transferred_words);
void psx_xg_render_auth_source_reset(void);
void psx_xg_render_auth_ft4_geometry_enable(bool enabled);
void psx_xg_render_auth_producer_family_enable(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
