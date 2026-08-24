#ifndef XG_RENDER_AUTH_H
#define XG_RENDER_AUTH_H

#include "guest_render_types.h"
#include "xg_render_auth_types.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XG_RENDER_AUTH_TRACE_CAPACITY
#define XG_RENDER_AUTH_TRACE_CAPACITY 64u
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderAuth XgRenderAuth;

enum {
    XG_RENDER_AUTH_PRODUCER_RECORD_ID = 3,
    XG_RENDER_AUTH_SITE_RECORD_ID = 4,
};

typedef struct XgRenderAuthValidation {
    uint32_t base_crc32;
    uint32_t range_crc32;
    uint64_t code_page_generation;
    XgRenderAuthDigest instruction_window_digest;
    uint32_t instruction_window_start;
    uint32_t instruction_window_size;
    uint32_t caller_site;
    uint32_t callee_entry;
    uint32_t return_site;
    uint32_t required_jal_opcode;
    uint32_t jal_target;
    uint32_t required_delay_slot_instructions;
    bool delay_slot_complete;
    bool delay_slot_is_control_transfer;
    XgRenderAuthDigest manifest_digest;
} XgRenderAuthValidation;

typedef struct XgRenderAuthCacheIdentity {
    XgRenderAuthDigest codegen_digest;
    XgRenderAuthDigest manifest_digest;
} XgRenderAuthCacheIdentity;

typedef struct XgRenderAuthProfile {
    uint32_t producer_record_id;
    uint32_t site_record_id;
    uint32_t producer_entry;
    XgRenderAuthIdentity static_game_identity;
    XgRenderAuthIdentity field_image_identity;
    XgRenderAuthValidation validation;
    XgRenderAuthCacheIdentity cache_identity;
} XgRenderAuthProfile;

typedef enum XgRenderAuthResult {
    XG_RENDER_AUTH_OK = 0,
    XG_RENDER_AUTH_INVALID_ARGUMENT = 1,
    XG_RENDER_AUTH_INVALID_TRANSITION = 2,
    XG_RENDER_AUTH_REJECTED = 3,
} XgRenderAuthResult;

typedef enum XgRenderAuthEventMode {
    XG_RENDER_AUTH_EVENT_ACCEPTED_HOOK = 0,
    XG_RENDER_AUTH_EVENT_REJECTED_HOOK = 1,
    XG_RENDER_AUTH_EVENT_CODE_PAGE_MUTATION = 2,
} XgRenderAuthEventMode;

typedef struct XgRenderAuthExecution {
    XgRenderAuthTier tier;
    XgRenderAuthHook hook;
    uint32_t observed_producer_entry;
    XgRenderAuthIdentity static_game_identity;
    XgRenderAuthIdentity runtime_game_identity;
    XgRenderAuthIdentity field_image_identity;
    XgRenderAuthValidation validation;
    XgRenderAuthCacheIdentity cache_identity;
} XgRenderAuthExecution;

typedef struct XgRenderAuthDecision {
    GuestRenderRenderMode effective_render_mode;
    XgRenderAuthReason reject_reason;
    bool native_use_permitted;
} XgRenderAuthDecision;

typedef struct XgRenderAuthTraceEvent {
    uint64_t sequence;
    GuestRenderVisualStateId state_id;
    uint32_t producer_record_id;
    uint32_t site_record_id;
    uint32_t producer_entry;
    uint32_t capture_site;
    uint32_t return_site;
    uint32_t static_callee;
    XgRenderAuthTier tier;
    XgRenderAuthHook hook;
    XgRenderAuthReason reason;
    XgRenderAuthEventMode event_mode;
    GuestRenderRenderMode effective_render_mode;
} XgRenderAuthTraceEvent;

typedef struct XgRenderAuthTraceSnapshot {
    uint64_t next_sequence;
    size_t count;
    XgRenderAuthTraceEvent events[XG_RENDER_AUTH_TRACE_CAPACITY];
} XgRenderAuthTraceSnapshot;

XgRenderAuthResult xg_render_auth_process_owner(XgRenderAuth **out_auth);
XgRenderAuthResult xg_render_auth_scene_begin(
    XgRenderAuth *auth,
    GuestRenderVisualStateId state_id,
    const XgRenderAuthProfile *profile);
XgRenderAuthResult xg_render_auth_scene_reset(XgRenderAuth *auth);
XgRenderAuthResult xg_render_auth_observe_hook(
    XgRenderAuth *auth,
    const XgRenderAuthExecution *execution,
    XgRenderAuthDecision *out_decision);
/* Binds one exact packet after the authenticated capture gate. */
XgRenderAuthResult xg_render_auth_bind_packet(
    XgRenderAuth *auth,
    uint32_t packet_address,
    uint32_t source_primitive_index);
/* Binds an exact candidate before the remaining auth hooks. The bridge cannot
 * present this state until the producer reaches authenticated finalization. */
XgRenderAuthResult xg_render_auth_bind_provisional_packet(
    XgRenderAuth *auth,
    uint32_t packet_address,
    uint32_t source_primitive_index);
XgRenderAuthResult xg_render_auth_append_native_insertion(
    XgRenderAuth *auth,
    uint32_t packet_address,
    uint32_t source_primitive_index,
    uint32_t ot_bucket,
    uint8_t tag_payload_word_count,
    const XgRenderIrNativePrimitive *primitive,
    bool provisional);
XgRenderAuthResult xg_render_auth_note_code_page_mutation(
    XgRenderAuth *auth,
    uint64_t previous_generation,
    uint64_t current_generation);
XgRenderAuthResult xg_render_auth_abort(XgRenderAuth *auth,
                                        XgRenderAuthReason reason);
XgRenderAuthResult xg_render_auth_snapshot(
    const XgRenderAuth *auth,
    XgRenderAuthSnapshot *out_snapshot);
XgRenderAuthResult xg_render_auth_native_item_get(
    const XgRenderAuth *auth, size_t index, XgRenderIrNativeItem *out_item);
XgRenderAuthResult xg_render_auth_trace_snapshot(
    const XgRenderAuth *auth,
    XgRenderAuthTraceSnapshot *out_snapshot);
size_t xg_render_auth_trace_capacity(void);
const char *xg_render_auth_reason_name(uint32_t reason);

#ifdef __cplusplus
}
#endif

#endif
