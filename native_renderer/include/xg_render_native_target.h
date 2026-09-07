#ifndef XG_RENDER_NATIVE_TARGET_H
#define XG_RENDER_NATIVE_TARGET_H

#include "xg_render_scene_snapshot.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUState CPUState;

typedef struct XgRenderNativeTargetRect {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} XgRenderNativeTargetRect;

typedef struct XgRenderNativeTargetServices {
    /* Full resident CFG authority, not an instruction-only signature check. */
    bool (*native_text_authorizes_pc)(uint32_t entry);
    bool (*guest_data_range_is_valid)(uint32_t address, uint32_t size,
                                     uint32_t alignment, bool allow_scratchpad);
    /* Nonzero while the source is available. Scene changes do not expire SDK
     * packets; current resident authority and packet bytes are checked at take. */
    uint64_t (*generation)(void);
    /* Observation read, without charging guest CPU memory-access cycles. */
    uint32_t (*read_word)(uint32_t address);
} XgRenderNativeTargetServices;

/* Guest-owner thread only. Observes SetDrawEnv/SetDrawEnv2 before execution:
 * a0 is the DR_ENV tag, a1 the authored DRAWENV. No destination payload reads. */
bool xg_render_native_target_observe(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    const XgRenderNativeTargetServices *services);

/* Called only for an accepted E3 with a normalized RAM command ID. Validates the
 * constructed SDK E3/E4/E5 payload against the captured declaration, then copies a
 * pointer-free TARGET operation; false leaves out_operation unchanged. Despite
 * its name this retains the mapping for repeated submissions of the DR_ENV.
 * Scene transitions retain unchanged packets owned by authenticated resident
 * code. Recycled/mutated packets lose the mapping. E3/E4 without this metadata
 * remain material scissor changes, not targets. */
bool xg_render_native_target_take(uint32_t command_id,
                                XgRenderNativeOperation *out_operation);
void xg_render_native_target_reset(void);

#ifdef __cplusplus
}
#endif

#endif
