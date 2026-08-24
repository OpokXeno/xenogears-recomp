#ifndef XG_RENDER_AUTH_RUNTIME_INVALIDATION_H
#define XG_RENDER_AUTH_RUNTIME_INVALIDATION_H

#include "xg_render_auth_candidate_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void psx_xg_render_auth_note_code_write(uint64_t previous_generation,
                                        uint64_t current_generation,
                                        uint32_t guest_pc,
                                        uint32_t write_size);
void psx_xg_render_auth_loader_mismatch(uint32_t pc);
void psx_xg_render_auth_native_bad_entry(uint32_t owner, uint32_t pc);
void psx_xg_render_auth_note_artifact_candidate(
    const PsxXgRenderAuthCandidate *candidate);
void psx_xg_render_auth_note_candidate_dispatch(
    const PsxXgRenderAuthCandidate *candidate);

#ifdef __cplusplus
}
#endif

#endif
