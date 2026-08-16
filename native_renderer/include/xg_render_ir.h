#ifndef XG_RENDER_IR_H
#define XG_RENDER_IR_H

#include "guest_render_bridge.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XG_RENDER_IR_ITEM_CAPACITY
#define XG_RENDER_IR_ITEM_CAPACITY 4096u
#endif

enum {
    XG_RENDER_IR_FIXED_FRACTION_BITS = 16,
    XG_RENDER_IR_TRIANGLE_CAPACITY = 2,
    XG_RENDER_IR_SERIALIZATION_VERSION = 1,
    XG_RENDER_IR_SERIALIZED_HEADER_SIZE = 40,
    XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE = 64,
    XG_RENDER_IR_SERIALIZED_NATIVE_SIZE = 240,
};

#define XG_RENDER_IR_NO_PACKET_ADDRESS UINT32_MAX
#define XG_RENDER_IR_TAG_LINK_END UINT32_C(0x00ffffff)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderIr XgRenderIr;
typedef int32_t XgRenderIrFixed16_16;
typedef GuestRenderProducerHandle XgRenderIrProvenanceKey;

typedef enum XgRenderIrResult {
    XG_RENDER_IR_OK = 0,
    XG_RENDER_IR_INVALID_ARGUMENT = 1,
    XG_RENDER_IR_INVALID_TRANSITION = 2,
    XG_RENDER_IR_TRANSACTION_REJECTED = 3,
    XG_RENDER_IR_CAPACITY_EXCEEDED = 4,
    XG_RENDER_IR_STALE_VRAM_SERIAL = 5,
    XG_RENDER_IR_INVALID_ITEM = 6,
    XG_RENDER_IR_INVALID_ORDER = 7,
    XG_RENDER_IR_INVALID_LINKAGE = 8,
    XG_RENDER_IR_FROZEN = 9,
    XG_RENDER_IR_NOT_FINALIZED = 10,
    XG_RENDER_IR_OUT_OF_RANGE = 11,
    XG_RENDER_IR_BUFFER_TOO_SMALL = 12,
} XgRenderIrResult;

typedef enum XgRenderIrPhase {
    XG_RENDER_IR_EMPTY = 0,
    XG_RENDER_IR_BUILDING = 1,
    XG_RENDER_IR_FINALIZED = 2,
    XG_RENDER_IR_REJECTED = 3,
} XgRenderIrPhase;

typedef enum XgRenderIrRejectReason {
    XG_RENDER_IR_REJECT_NONE = 0,
    XG_RENDER_IR_REJECT_CAPACITY = 1,
    XG_RENDER_IR_REJECT_STALE_VRAM_SERIAL = 2,
    XG_RENDER_IR_REJECT_INVALID_ITEM = 3,
    XG_RENDER_IR_REJECT_INVALID_ORDER = 4,
    XG_RENDER_IR_REJECT_INVALID_LINKAGE = 5,
    XG_RENDER_IR_REJECT_VISUAL_STATE_MISMATCH = 6,
    XG_RENDER_IR_REJECT_POST_FINALIZE_MUTATION = 7,
} XgRenderIrRejectReason;

typedef enum XgRenderIrItemKind {
    XG_RENDER_IR_ITEM_COMPATIBILITY = 0,
    XG_RENDER_IR_ITEM_NATIVE = 1,
} XgRenderIrItemKind;

typedef enum XgRenderIrTextureDepth {
    XG_RENDER_IR_TEXTURE_4_BIT = 0,
    XG_RENDER_IR_TEXTURE_8_BIT = 1,
    XG_RENDER_IR_TEXTURE_15_BIT = 2,
} XgRenderIrTextureDepth;

typedef enum XgRenderIrBlendMode {
    XG_RENDER_IR_BLEND_AVERAGE = 0,
    XG_RENDER_IR_BLEND_ADD = 1,
    XG_RENDER_IR_BLEND_SUBTRACT = 2,
    XG_RENDER_IR_BLEND_ADD_QUARTER = 3,
} XgRenderIrBlendMode;

typedef enum XgRenderIrShading {
    XG_RENDER_IR_SHADING_FLAT = 0,
    XG_RENDER_IR_SHADING_GOURAUD = 1,
} XgRenderIrShading;

typedef struct XgRenderIrOrdering {
    uint32_t ot_bucket;
    uint32_t final_ordinal;
    uint32_t packet_guest_address;
    uint32_t predecessor_guest_address;
    uint32_t successor_guest_address;
    uint32_t tag_link_guest_address;
    uint8_t tag_payload_word_count;
} XgRenderIrOrdering;

typedef struct XgRenderIrItemBase {
    XgRenderIrOrdering ordering;
    bool has_provenance;
    XgRenderIrProvenanceKey provenance_key;
    uint32_t source_primitive_index;
} XgRenderIrItemBase;

typedef struct XgRenderIrMaterialState {
    uint16_t tpage;
    uint16_t texture_page_x;
    uint16_t texture_page_y;
    uint16_t clut_x;
    uint16_t clut_y;
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    XgRenderIrTextureDepth texture_depth;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    XgRenderIrShading shading;
    bool textured;
    bool raw_texture;
    bool semi_transparent;
    XgRenderIrBlendMode blend_mode;
    bool dither;
    bool mask_set;
    bool mask_check;
} XgRenderIrMaterialState;

typedef struct XgRenderIrVertex {
    XgRenderIrFixed16_16 x;
    XgRenderIrFixed16_16 y;
    XgRenderIrFixed16_16 u;
    XgRenderIrFixed16_16 v;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    XgRenderIrFixed16_16 native_view_x;
    XgRenderIrFixed16_16 native_view_y;
    bool native_view_position;
    int32_t projective_view_x;
    int32_t projective_view_y;
    int32_t projective_view_z;
    XgRenderIrFixed16_16 projective_offset_x;
    XgRenderIrFixed16_16 projective_offset_y;
    XgRenderIrFixed16_16 projective_native_offset_x;
    XgRenderIrFixed16_16 projective_native_offset_y;
    uint16_t projective_distance;
    bool projective_position;
    uint32_t interpolation_group_id;
    uint32_t interpolation_vertex_id;
    bool interpolation_vertex_identity_valid;
} XgRenderIrVertex;

typedef struct XgRenderIrTriangle {
    uint8_t split_index;
    uint8_t split_count;
    XgRenderIrVertex vertices[3];
} XgRenderIrTriangle;

typedef struct XgRenderIrNativePrimitive {
    XgRenderIrMaterialState material;
    uint8_t triangle_count;
    XgRenderIrTriangle triangles[XG_RENDER_IR_TRIANGLE_CAPACITY];
} XgRenderIrNativePrimitive;

typedef struct XgRenderIrCompatibilityItem {
    XgRenderIrItemBase base;
} XgRenderIrCompatibilityItem;

typedef struct XgRenderIrNativeItem {
    XgRenderIrItemBase base;
    union {
        XgRenderIrNativePrimitive primitive;
        XgRenderIrNativePrimitive native;
    };
} XgRenderIrNativeItem;

typedef struct XgRenderIrItem {
    XgRenderIrItemKind kind;
    XgRenderIrItemBase base;
    XgRenderIrNativePrimitive native;
} XgRenderIrItem;

typedef struct XgRenderIrSnapshot {
    GuestRenderVisualStateId state_id;
    uint64_t vram_mutation_serial;
    size_t item_count;
    size_t compatibility_count;
    size_t native_count;
    XgRenderIrPhase phase;
    XgRenderIrRejectReason reject_reason;
    bool usable;
} XgRenderIrSnapshot;

XgRenderIrResult xg_render_ir_process_owner(XgRenderIr **out_ir);
XgRenderIrResult xg_render_ir_reset(XgRenderIr *ir);
XgRenderIrResult xg_render_ir_begin(XgRenderIr *ir,
                                    GuestRenderVisualStateId state_id,
                                    uint64_t vram_mutation_serial);
XgRenderIrResult xg_render_ir_append_compatibility(
    XgRenderIr *ir,
    const XgRenderIrCompatibilityItem *item);
XgRenderIrResult xg_render_ir_append_native(XgRenderIr *ir,
                                             const XgRenderIrNativeItem *item);
XgRenderIrResult xg_render_ir_append_native_insertion(
    XgRenderIr *ir,
    const XgRenderIrNativeItem *item,
    uint32_t ot_bucket,
    uint32_t packet_guest_address,
    uint8_t tag_payload_word_count);
XgRenderIrResult xg_render_ir_finalize(XgRenderIr *ir,
                                       GuestRenderVisualStateId state_id,
                                       uint64_t current_vram_mutation_serial);
XgRenderIrResult xg_render_ir_snapshot(const XgRenderIr *ir,
                                       XgRenderIrSnapshot *out_snapshot);
XgRenderIrResult xg_render_ir_item_get(const XgRenderIr *ir,
                                       size_t index,
                                       XgRenderIrItem *out_item);
XgRenderIrResult xg_render_ir_serialized_size(const XgRenderIr *ir,
                                              size_t *out_size);
XgRenderIrResult xg_render_ir_serialize_normalized(const XgRenderIr *ir,
                                                    uint8_t *out_bytes,
                                                    size_t out_capacity,
                                                    size_t *out_size);
size_t xg_render_ir_item_capacity(void);
const char *xg_render_ir_reject_reason_name(uint32_t reason);

#ifdef XG_RENDER_IR_TESTING
void xg_render_ir_test_reset_process_owner(void);
XgRenderIrResult xg_render_ir_test_stored_item_get(const XgRenderIr *ir,
                                                    size_t index,
                                                    XgRenderIrItem *out_item);
#endif

#ifdef __cplusplus
}
#endif

#endif
