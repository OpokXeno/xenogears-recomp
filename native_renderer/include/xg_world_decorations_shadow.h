#ifndef XG_WORLD_DECORATIONS_SHADOW_H
#define XG_WORLD_DECORATIONS_SHADOW_H

#include "cpu_state.h"
#include "gpu.h"
#include "xg_world_decorations_source_capture.h"
#include "xg_world_decorations_snapshot_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XG_WORLD_DECORATIONS_SHADOW_OUTER_BEGIN_PC UINT32_C(0x8008615c)
#define XG_WORLD_DECORATIONS_SHADOW_HELPER_BEGIN_PC UINT32_C(0x80099bfc)
#define XG_WORLD_DECORATIONS_SHADOW_HELPER_FINISH_PC UINT32_C(0x80099e78)
#define XG_WORLD_DECORATIONS_SHADOW_OUTER_FINISH_PC UINT32_C(0x800863bc)
#define XG_WORLD_DECORATIONS_SHADOW_OUTER_RETURN UINT32_C(0x80071ab8)
#define XG_WORLD_DECORATIONS_SHADOW_HELPER_RETURN UINT32_C(0x8008639c)
#define XG_WORLD_DECORATIONS_SHADOW_BUFFER_ADDRESS UINT32_C(0x8009d7f0)
#define XG_WORLD_DECORATIONS_SHADOW_PACKET_BASES UINT32_C(0x8009d7e8)
#define XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT UINT32_C(0x8009be04)
#define XG_WORLD_DECORATIONS_SHADOW_CONTEXT UINT32_C(0x8009be3c)

enum {
    XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE = 0x28,
    XG_WORLD_DECORATIONS_SHADOW_HELPER_FRAME_SIZE = 8,
    XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE = 0x28,
    XG_WORLD_DECORATIONS_SHADOW_OT_CAPACITY = 0x1000,
};

typedef enum XgWorldDecorationsShadowResult {
    XG_WORLD_DECORATIONS_SHADOW_OK = 0,
    XG_WORLD_DECORATIONS_SHADOW_INVALID_ARGUMENT,
    XG_WORLD_DECORATIONS_SHADOW_INVALID_STATE,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKED,
} XgWorldDecorationsShadowResult;

typedef enum XgWorldDecorationsShadowMismatch {
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_NONE = 0,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_ARGUMENT = 1u << 0,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_BUFFER = 1u << 1,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_COUNT = 1u << 2,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_PAYLOAD = 1u << 3,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_GEOMETRY = 1u << 4,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_TAG = 1u << 5,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_OT = 1u << 6,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_A3 = 1u << 7,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_S0 = 1u << 8,
    XG_WORLD_DECORATIONS_SHADOW_MISMATCH_OUTER = 1u << 9,
} XgWorldDecorationsShadowMismatch;

/* A value-only view of registers and authenticated guest reads at one hook.
 * There is deliberately no write callback: the shadow observer cannot mutate
 * guest state. */
typedef struct XgWorldDecorationsShadowObservation {
    void *read_context;
    XgWorldDecorationsReadU16 read_u16;
    XgWorldDecorationsReadU32 read_u32;
    uint64_t authentication_generation;
    uint32_t pc;
    uint32_t return_address;
    uint32_t stack_pointer;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t s0;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint16_t projection_distance;
    XgWorldDecorationsRasterState raster;
    bool authenticated;
    bool helper_arguments_authenticated;
    bool projection_state_authenticated;
} XgWorldDecorationsShadowObservation;

typedef struct XgWorldDecorationsShadowOtExpectation {
    uint32_t bucket;
    uint32_t address;
    uint32_t last_packet;
} XgWorldDecorationsShadowOtExpectation;

typedef struct XgWorldDecorationsShadow {
    XgWorldDecorationsShadowSnapshot snapshot;
    XgWorldDecorationsRecord records[XG_WORLD_DECORATIONS_PACKET_CAPACITY];
    uint32_t packet_addresses[XG_WORLD_DECORATIONS_PACKET_CAPACITY];
    uint32_t expected_tags[XG_WORLD_DECORATIONS_PACKET_CAPACITY];
    uint32_t initial_payload_words[XG_WORLD_DECORATIONS_PACKET_CAPACITY]
                                  [XG_WORLD_DECORATIONS_FT4_PAYLOAD_WORD_COUNT];
    XgWorldDecorationsShadowOtExpectation
        touched_ot[XG_WORLD_DECORATIONS_PACKET_CAPACITY];
    void *read_context;
    XgWorldDecorationsReadU16 read_u16;
    XgWorldDecorationsReadU32 read_u32;
    uint64_t authentication_generation;
    uint32_t outer_entry_stack_pointer;
    uint32_t helper_entry_stack_pointer;
    uint32_t packet_base;
    uint32_t ot_base;
    uint32_t expected_count;
    uint32_t helper_first_count;
    uint32_t touched_ot_count;
    bool helper_matches;
    bool outer_matches;
} XgWorldDecorationsShadow;

/* Runtime-facing singleton observer. */
void xg_world_decorations_shadow_reset(void);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_outer_begin(
    CPUState *cpu, uint64_t authentication_generation,
    const GpuDrawState *draw_state, int32_t screen_x_cull_margin);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_helper_begin(
    CPUState *cpu);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_helper_finish(
    CPUState *cpu);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_outer_finish(
    CPUState *cpu);
void xg_world_decorations_shadow_lifecycle_invalidate(void);
void xg_world_decorations_shadow_lifecycle_block(void);
bool xg_world_decorations_shadow_record_native_cutover(
    uint32_t primitive_count);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_snapshot(
    XgWorldDecorationsShadowSnapshot *out_snapshot);

/* Value-only instance API used by focused tests and offline observers. */
void xg_world_decorations_shadow_state_reset(
    XgWorldDecorationsShadow *shadow);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_outer_begin(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_helper_begin(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_helper_finish(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_outer_finish(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation);

XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_block(
    XgWorldDecorationsShadow *shadow,
    XgWorldDecorationsShadowBlocker blocker, uint32_t detail);
XgWorldDecorationsShadowResult
xg_world_decorations_shadow_state_lifecycle_invalidate(
    XgWorldDecorationsShadow *shadow, uint32_t detail);
XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_snapshot(
    const XgWorldDecorationsShadow *shadow,
    XgWorldDecorationsShadowSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif
