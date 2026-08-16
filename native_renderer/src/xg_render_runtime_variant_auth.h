#ifndef XG_RENDER_RUNTIME_VARIANT_AUTH_H
#define XG_RENDER_RUNTIME_VARIANT_AUTH_H

#include "xg_render_auth_runtime.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum XgRenderRuntimeVariantEvent {
    XG_RENDER_RUNTIME_VARIANT_IGNORE = 0,
    XG_RENDER_RUNTIME_VARIANT_ACTIVATED,
    XG_RENDER_RUNTIME_VARIANT_ENTRY,
    XG_RENDER_RUNTIME_VARIANT_CAPTURE,
    XG_RENDER_RUNTIME_VARIANT_RETURN,
    XG_RENDER_RUNTIME_VARIANT_CONSUMED,
    XG_RENDER_RUNTIME_VARIANT_REJECT,
} XgRenderRuntimeVariantEvent;

void xg_render_runtime_variant_reset(void);
bool xg_render_runtime_variant_no_gates_enabled(void);
XgRenderRuntimeVariantEvent xg_render_runtime_variant_observe(
    uint32_t hook, uint32_t pc, uint32_t instruction_word,
    uint32_t delay_slot_word, uint32_t return_address,
    uint64_t scene_generation);
bool xg_render_runtime_variant_event_is_exact(
    XgRenderRuntimeVariantEvent event, uint32_t pc);
bool xg_render_runtime_variant_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate);
bool xg_render_runtime_variant_artifact_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate);
bool xg_render_runtime_variant_artifact_candidate_authorizes_pc(
    const PsxXgRenderAuthCandidate *candidate, uint32_t pc);
bool xg_render_authoritative_overlay_artifact_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate);
bool xg_render_authoritative_overlay_artifact_candidate_authorizes_pc(
    const PsxXgRenderAuthCandidate *candidate, uint32_t pc);
bool xg_render_runtime_variant_artifact_contains_pc(uint32_t pc);
bool xg_render_runtime_variant_active_code_write_overlaps(
    uint32_t write_address, uint32_t write_size);
uint32_t xg_render_runtime_variant_code_write_overlap_mask(
    uint32_t write_address, uint32_t write_size);
bool xg_render_runtime_variant_model_ft4_code_write_overlaps(
    uint32_t write_address, uint32_t write_size);
bool xg_render_runtime_variant_sprite_ft4_code_write_overlaps(
    uint32_t write_address, uint32_t write_size);
void xg_render_runtime_variant_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
bool xg_render_runtime_variant_source_site_lookup(
    uint32_t pc, uint32_t instruction_word,
    PsxXgRenderSourceSiteMetadata *out_metadata);
bool xg_render_runtime_variant_source_pc_relevant(uint32_t pc);
bool xg_render_runtime_variant_native_cutover_matches(
    uint32_t pc, uint32_t instruction_word, uint32_t *out_continuation);
bool xg_render_runtime_variant_hook_relevant(uint32_t hook, uint32_t pc);

#endif
