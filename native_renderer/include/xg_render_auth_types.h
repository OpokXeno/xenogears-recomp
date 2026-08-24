#ifndef XG_RENDER_AUTH_TYPES_H
#define XG_RENDER_AUTH_TYPES_H

#include "guest_render_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    XG_RENDER_AUTH_HOOK_STAGE_COUNT = 3,
};

typedef enum XgRenderAuthTier {
    XG_RENDER_AUTH_TIER_STATIC = 0,
    XG_RENDER_AUTH_TIER_COLD_INTERPRETER = 1,
    XG_RENDER_AUTH_TIER_WARM_NATIVE = 2,
} XgRenderAuthTier;

typedef enum XgRenderAuthReason {
    XG_RENDER_AUTH_REJECT_NONE = 0,
    XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH = 1,
    XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH = 2,
    XG_RENDER_AUTH_REJECT_CACHE_IDENTITY_MISMATCH = 3,
    XG_RENDER_AUTH_REJECT_HOOK_SEQUENCE = 4,
    XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION = 5,
    XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR = 6,
    XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE = 7,
} XgRenderAuthReason;

typedef XgRenderAuthReason XgRenderAuthRejectReason;

typedef struct XgRenderAuthDigest {
    uint8_t bytes[32];
} XgRenderAuthDigest;

typedef struct XgRenderAuthIdentity {
    uint32_t namespace_id;
    XgRenderAuthDigest full_sha256;
} XgRenderAuthIdentity;

typedef enum XgRenderAuthHook {
    XG_RENDER_AUTH_HOOK_ENTRY = 0,
    XG_RENDER_AUTH_HOOK_CAPTURE_SITE = 1,
    XG_RENDER_AUTH_HOOK_RETURN = 2,
    XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR = 3,
} XgRenderAuthHook;

typedef struct XgRenderAuthLogicalIdentity {
    uint32_t producer_record_id;
    uint32_t site_record_id;
    uint32_t producer_entry;
    uint32_t capture_site;
    uint32_t return_site;
    uint32_t static_callee;
    XgRenderAuthIdentity field_image_identity;
    XgRenderAuthIdentity static_game_identity;
    XgRenderAuthIdentity runtime_game_identity;
    XgRenderAuthDigest manifest_identity;
    GuestRenderVisualStateId state_id;
} XgRenderAuthLogicalIdentity;

typedef struct XgRenderAuthSnapshot {
    GuestRenderRenderMode effective_render_mode;
    XgRenderAuthReason reject_reason;
    XgRenderAuthIdentity field_image_identity;
    XgRenderAuthLogicalIdentity logical_identity;
    size_t producer_begin_count;
    size_t native_item_count;
    size_t hook_count;
    XgRenderAuthHook hook_sequence[XG_RENDER_AUTH_HOOK_STAGE_COUNT];
    size_t trace_count;
    uint64_t next_trace_sequence;
    bool scene_aborted;
    bool ir_usable;
    bool native_use_permitted;
} XgRenderAuthSnapshot;

#endif
