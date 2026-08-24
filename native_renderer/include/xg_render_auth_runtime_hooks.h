#ifndef XG_RENDER_AUTH_RUNTIME_HOOKS_H
#define XG_RENDER_AUTH_RUNTIME_HOOKS_H

#include "psx_xg_render_auth_hook_types.h"
#include "xg_render_source_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

#ifdef __cplusplus
extern "C" {
#endif

/* Legacy writable ABI. Prefer psx_xg_render_auth_cold_enabled() for reads;
 * this remains the canonical cold-hook enable state for existing consumers. */
extern bool g_psx_xg_render_auth_cold_enabled;

bool psx_xg_render_auth_cold_enabled(void);
bool psx_xg_render_auth_cold_hook_relevant(uint32_t hook, uint32_t pc,
                                           uint32_t instruction_word);
void psx_xg_render_auth_cold_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word);
void psx_xg_render_auth_warm_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word);
bool psx_xg_render_auth_source_site_lookup(
    uint32_t pc, uint32_t instruction_word,
    PsxXgRenderSourceSiteMetadata *out_metadata);
bool psx_xg_render_auth_cold_source_pc_relevant(uint32_t pc);

enum {
    PSX_XG_RENDER_COLD_ENTRY = 1u << 0,
    PSX_XG_RENDER_COLD_CAPTURE = 1u << 1,
    PSX_XG_RENDER_COLD_SOURCE = 1u << 2,
    PSX_XG_RENDER_COLD_NATIVE_PRE = 1u << 3,
    PSX_XG_RENDER_COLD_NATIVE_POST = 1u << 4,
    PSX_XG_RENDER_COLD_OVERLAY = 1u << 5,
};

uint32_t psx_xg_render_auth_cold_instruction_flags(
    uint32_t pc, uint32_t instruction_word);
bool psx_xg_render_auth_cold_source_observe(
    PsxXgRenderSourceStage stage, uint32_t pc, uint32_t instruction_word,
    uint32_t auxiliary);
bool psx_xg_render_auth_cold_source_observe_cpu(
    CPUState *cpu, PsxXgRenderSourceStage stage, uint32_t pc,
    uint32_t instruction_word, uint32_t auxiliary);
bool psx_xg_render_auth_resident_ft4_observe(
    CPUState *cpu, uint32_t stage, uint32_t pc, uint32_t instruction_word);
bool psx_xg_render_auth_native_cutover_pc_relevant(uint32_t pc);
bool psx_xg_render_auth_overlay_cutover_relevant(
    uint32_t pc, uint32_t instruction_word);
bool psx_xg_render_auth_native_cutover_post_pc_relevant(uint32_t pc);
bool psx_xg_render_auth_native_ft4_bypass(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word);
bool psx_xg_render_auth_ft4_geometry_pop(
    PsxXgRenderFt4Geometry *out_geometry);
/* Legacy facade retained for source and binary compatibility. */
void psx_xg_render_auth_capture_model_ft3_link(CPUState *cpu);
void psx_xg_render_auth_capture_clear_tile(CPUState *cpu);
void psx_xg_render_auth_capture_logo_sprite(
    uint32_t command_address, uint8_t color);
void psx_xg_render_auth_capture_tile_write(
    CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
    uint8_t color);

#ifdef __cplusplus
}
#endif

#endif
