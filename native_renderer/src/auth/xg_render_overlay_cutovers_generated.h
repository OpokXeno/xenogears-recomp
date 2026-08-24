#ifndef XG_RENDER_OVERLAY_CUTOVERS_GENERATED_H
#define XG_RENDER_OVERLAY_CUTOVERS_GENERATED_H

#include "xg_render_auth_candidate_types.h"

#include <stdbool.h>
#include <stdint.h>

bool xg_render_overlay_cutover_relevant(
    uint32_t pc, uint32_t instruction_word);
bool xg_render_overlay_cutover_pc_relevant(uint32_t pc);
uint32_t xg_render_overlay_cutover_site_count(void);
bool xg_render_overlay_cutover_site_at(
    uint32_t index, uint32_t *out_pc, uint32_t *out_instruction);
bool xg_render_overlay_artifact_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate);
bool xg_render_overlay_artifact_candidate_authorizes_pc(
    const PsxXgRenderAuthCandidate *candidate, uint32_t pc);

#endif
