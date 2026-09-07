#ifndef XG_RENDER_BATTLE_FX_H
#define XG_RENDER_BATTLE_FX_H

#include "xg_render_ir.h"
#include "xg_host_3d.h"

#include <stdbool.h>
#include <stdint.h>

#define XG_RENDER_BATTLE_FX_RIPPLE_TRIANGLES 560u
#define XG_RENDER_BATTLE_FX_RIPPLE_OVERLAY_IDENTITY \
    UINT64_C(0x5c65f607264d841d)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderBattleFxKind {
    XG_RENDER_BATTLE_FX_RIPPLE_DISSOLVE = 5,
} XgRenderBattleFxKind;

typedef enum XgRenderBattleFxResult {
    XG_RENDER_BATTLE_FX_OK = 0,
    XG_RENDER_BATTLE_FX_INVALID_ARGUMENT,
    XG_RENDER_BATTLE_FX_RESOURCE_FAILED = 8,
    XG_RENDER_BATTLE_FX_EMIT_FAILED,
} XgRenderBattleFxResult;

typedef struct XgRenderBattleFxOwnerIdentity {
    XgRenderBattleFxKind kind;
    uint64_t executable_identity;
    uint64_t overlay_identity;
    uint64_t authentication_receipt;
    uint64_t owner_generation;
} XgRenderBattleFxOwnerIdentity;

typedef struct CPUState CPUState;

typedef struct XgRenderBattleRipplePrimitive {
    XgRenderIrNativePrimitive primitive;
    /* Start of POLY_GT3, not its command word. Acceptance keys packet + 4. */
    uint32_t packet_address;
    uint32_t source_primitive_index;
    uint32_t ot_bucket;
} XgRenderBattleRipplePrimitive;

typedef struct XgRenderBattleRippleCaptureServices {
    void *context;
    /* Artifact-specific authority and owned source bounds, including the
     * resident trig table. Mapping checks alone do not authenticate a source. */
    bool (*authorize)(void *context, uint32_t pc, uint32_t instruction_word,
                       const XgRenderBattleFxOwnerIdentity *owner);
    bool (*source_range_valid)(void *context, uint32_t address,
                               uint32_t byte_count, uint32_t alignment);
    /* Only inherited DQA/DQB are used; pose/projection are rebuilt from authored
     * state. Never supply projected vertices or target packet payloads. */
    bool (*capture_projection)(void *context, XgHost3dProjection *out_projection);
    bool (*capture_draw_state)(void *context, XgRenderIrMaterialState *out_state);
    /* Copy to private pending storage; enqueue at the authenticated return.
     * Resource resolution and ordering belong to accepted GPU operations. */
    bool (*publish_captures)(void *context, const XgRenderBattleFxOwnerIdentity *owner,
                            const XgRenderBattleRipplePrimitive *captures, uint32_t count);
} XgRenderBattleRippleCaptureServices;

/* Pre-instruction hook at 801fc11c / 27bdff78, Ripple image only. Capture after
 * its authored update, before DrawFramebufferRadialRippleDissolve. Packet bases
 * are mesh + 0x64 + index*0x7c + 4 + parity*0x28. No packet payload reads. */
XgRenderBattleFxResult xg_render_battle_fx_capture_ripple(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    const XgRenderBattleFxOwnerIdentity *owner,
    const XgRenderBattleRippleCaptureServices *services);

#ifdef __cplusplus
}
#endif

#endif
