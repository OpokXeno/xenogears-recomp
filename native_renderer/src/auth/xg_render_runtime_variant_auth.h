#ifndef XG_RENDER_RUNTIME_VARIANT_AUTH_H
#define XG_RENDER_RUNTIME_VARIANT_AUTH_H

#include "xg_render_auth_candidate_types.h"
#include "xg_render_instrumentation_types.h"
#include "xg_render_invalidation_event.h"
#include "xg_render_source_types.h"
#include "xg_render_runtime_variants_generated.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderRuntimeVariantModelDispatchContract {
    const uint32_t *instructions;
    uint32_t instruction_count;
    uint32_t matrix_stack_offset;
} XgRenderRuntimeVariantModelDispatchContract;

void xg_render_runtime_variant_reset(void);
void xg_render_runtime_variant_classify_code_write(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
void xg_render_runtime_variant_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
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
uint32_t xg_render_runtime_variant_model_dispatch_contract_count(void);
bool xg_render_runtime_variant_model_dispatch_contract_at(
    uint32_t index, XgRenderRuntimeVariantModelDispatchContract *out_contract);
bool xg_render_runtime_variant_artifact_candidate_authorizes_pc(
    const PsxXgRenderAuthCandidate *candidate, uint32_t pc);
bool xg_render_authoritative_overlay_artifact_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate);
bool xg_render_authoritative_overlay_artifact_candidate_authorizes_pc(
    const PsxXgRenderAuthCandidate *candidate, uint32_t pc);
uint32_t xg_render_runtime_variant_descriptor_code_write_overlap_mask(
    uint32_t write_address, uint32_t write_size);
void xg_render_runtime_variant_register_descriptor_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
bool xg_render_runtime_variant_source_site_lookup(
    uint32_t pc, uint32_t instruction_word,
    PsxXgRenderSourceSiteMetadata *out_metadata);
bool xg_render_runtime_variant_source_pc_relevant(uint32_t pc);
bool xg_render_runtime_variant_native_cutover_lookup(
    uint32_t pc, uint32_t instruction_word,
    XgRenderRuntimeVariantCutoverHandler *out_handler,
    uint32_t *out_continuation);
bool xg_render_runtime_variant_native_cutover_contract_lookup(
    uint32_t pc, uint32_t instruction_word,
    XgRenderRuntimeVariantCutover *out_cutover);
bool xg_render_runtime_variant_native_dispatch_pc_relevant(uint32_t pc);
bool xg_render_runtime_variant_hook_relevant(uint32_t hook, uint32_t pc);

#endif
