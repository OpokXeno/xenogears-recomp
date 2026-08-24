#ifndef XG_WORLD_MODELS_NATIVE_H
#define XG_WORLD_MODELS_NATIVE_H

#include "xg_world_models.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT = 17,
    XG_WORLD_MODELS_PRIMITIVE_TOPOLOGY_WORD_COUNT = 4,
    XG_WORLD_MODELS_MAX_ATTRIBUTE_WORD_COUNT = 3,
    XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT = 13,
    XG_WORLD_MODELS_OT_BUCKET_COUNT = 0x1000,
    XG_WORLD_MODELS_MAX_CONTROL_COMMANDS_PER_PRIMITIVE = 32,
};

#define XG_WORLD_MODELS_PRODUCER_CONTINUATION_0 UINT32_C(0x80071ac8)
#define XG_WORLD_MODELS_PRODUCER_CONTINUATION_1 UINT32_C(0x80076a7c)
#define XG_WORLD_MODELS_PRODUCER_CONTINUATION_2 UINT32_C(0x800779ac)
#define XG_WORLD_MODELS_PRODUCER_CONTINUATION_3 UINT32_C(0x800789a8)

typedef enum XgWorldModelsNativeResult {
    XG_WORLD_MODELS_NATIVE_OK = 0,
    XG_WORLD_MODELS_NATIVE_INVALID_ARGUMENT,
    XG_WORLD_MODELS_NATIVE_UNAUTHENTICATED,
    XG_WORLD_MODELS_NATIVE_READ_FAILED,
    XG_WORLD_MODELS_NATIVE_SOURCE_MISMATCH,
    XG_WORLD_MODELS_NATIVE_FORBIDDEN_RANGE,
    XG_WORLD_MODELS_NATIVE_CAPACITY_EXCEEDED,
    XG_WORLD_MODELS_NATIVE_BUILD_FAILED,
    XG_WORLD_MODELS_NATIVE_INCOMPLETE_OUTPUT,
} XgWorldModelsNativeResult;

typedef enum XgWorldModelsNativeRangeKind {
    XG_WORLD_MODELS_NATIVE_RANGE_RECORDS = 0,
    XG_WORLD_MODELS_NATIVE_RANGE_TRANSFORM_NODE,
    XG_WORLD_MODELS_NATIVE_RANGE_MODEL_HEADER,
    XG_WORLD_MODELS_NATIVE_RANGE_TOPOLOGY,
    XG_WORLD_MODELS_NATIVE_RANGE_ATTRIBUTE,
    XG_WORLD_MODELS_NATIVE_RANGE_VERTEX,
    XG_WORLD_MODELS_NATIVE_RANGE_AUXILIARY_VERTEX,
    XG_WORLD_MODELS_NATIVE_RANGE_PACKET_OUTPUT,
    XG_WORLD_MODELS_NATIVE_RANGE_ORDERING_TABLE_OUTPUT,
} XgWorldModelsNativeRangeKind;

typedef bool (*XgWorldModelsNativeReadU8)(void *context, uint32_t address,
                                          uint8_t *out_value);
typedef bool (*XgWorldModelsNativeReadU16)(void *context, uint32_t address,
                                           uint16_t *out_value);
typedef bool (*XgWorldModelsNativeReadU32)(void *context, uint32_t address,
                                           uint32_t *out_value);
typedef bool (*XgWorldModelsNativeReadPacketTemplate)(
    void *context, uint32_t model_header_address, uint32_t packet_base_address,
    uint32_t packet_address, uint32_t attribute_address,
    uint8_t primitive_family, uint32_t *out_words, uint8_t word_count,
    uint64_t *out_resource_epoch);
typedef bool (*XgWorldModelsNativeAuthorizeRange)(
    void *context, XgWorldModelsNativeRangeKind kind, uint32_t address,
    uint32_t size);

typedef struct XgWorldModelsNativeAuthenticatedReader {
    void *context;
    XgWorldModelsNativeReadU8 read_u8;
    XgWorldModelsNativeReadU16 read_u16;
    XgWorldModelsNativeReadU32 read_u32;
    XgWorldModelsNativeReadPacketTemplate read_packet_template;
    XgWorldModelsNativeAuthorizeRange authorize_range;
    uint64_t authentication_generation;
    bool authenticated;
} XgWorldModelsNativeAuthenticatedReader;

typedef struct XgWorldModelsNativeRasterState {
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    bool dither;
    bool mask_set;
    bool mask_check;
} XgWorldModelsNativeRasterState;

typedef struct XgWorldModelsNativeRequest {
    uint64_t authentication_generation;
    uint32_t entry_pc;
    uint32_t caller_return;
    uint32_t guest_xclip_bound;
    XgWorldModelsGteState gte;
    XgWorldModelsNativeRasterState raster;
    bool projection_state_authenticated;
    bool lighting_state_authenticated;
} XgWorldModelsNativeRequest;

typedef struct XgWorldModelsNativeWorkspace {
    XgWorldModelsRecordSource *record_sources;
    uint32_t record_capacity;
    XgWorldModelsTransformNodeSource *transform_nodes;
    uint32_t transform_node_capacity;
} XgWorldModelsNativeWorkspace;

typedef struct XgWorldModelsNativeModelSource {
    uint32_t model_header_address;
    uint32_t vertex_base;
    uint32_t auxiliary_vertex_base;
    uint32_t topology_base;
    uint32_t material_base;
    uint32_t model_18;
    uint32_t packet_capacity_bytes;
    XgHost3dVector bounds_min;
    XgHost3dVector bounds_max;
    uint16_t vertex_count;
    uint16_t group_count;
} XgWorldModelsNativeModelSource;

typedef struct XgWorldModelsNativeDispatch {
    XgWorldModelsNativeModelSource model;
    XgWorldModelsResidentCall call;
    uint32_t source_index;
    uint32_t primitive_start;
    uint32_t primitive_count;
    uint32_t packet_cursor_after;
    uint32_t group_cursor_after;
    uint32_t primitive_family_mask;
    bool bounds_accepted;
    bool guest_bounds_accepted;
} XgWorldModelsNativeDispatch;

typedef struct XgWorldModelsNativePrimitiveSource {
    XgHost3dProjection projection;
    XgHost3dVector vertices[XG_HOST_3D_VERTEX_COUNT];
    XgHost3dVector auxiliary_vertices[XG_HOST_3D_VERTEX_COUNT];
    uint32_t attribute_words[XG_WORLD_MODELS_MAX_ATTRIBUTE_WORD_COUNT];
    uint32_t template_words[XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT];
    uint64_t resource_epoch;
    uint32_t model_header_address;
    uint32_t packet_base_address;
    uint32_t packet_address;
    uint32_t attribute_address;
    uint32_t source_index;
    uint32_t dispatch_index;
    uint32_t primitive_index;
    uint16_t topology[XG_WORLD_MODELS_PRIMITIVE_TOPOLOGY_WORD_COUNT];
    uint16_t tpage;
    uint16_t clut;
    uint8_t primitive_family;
    uint8_t dispatch_mode;
    uint8_t vertex_count;
    uint8_t attribute_size;
    uint8_t packet_word_count;
} XgWorldModelsNativePrimitiveSource;

typedef struct XgWorldModelsNativeAnchorSource {
    XgHost3dProjection projection;
    XgHost3dVector vertex;
    uint32_t model_header_address;
    uint32_t source_index;
    uint16_t vertex_index;
} XgWorldModelsNativeAnchorSource;

typedef struct XgWorldModelsNativePreparation {
    XgWorldModelsBuildSummary world;
    XgWorldModelsGteState gte;
    XgWorldModelsNativeRasterState raster;
    const XgWorldModelsNativeDispatch *sealed_dispatches;
    const XgWorldModelsNativePrimitiveSource *sealed_primitives;
    const XgWorldModelsNativeAnchorSource *sealed_anchor_sources;
    uint64_t dispatch_digest;
    uint64_t primitive_digest;
    uint64_t anchor_digest;
    uint64_t authentication_generation;
    uint32_t continuation_pc;
    uint32_t record_count;
    uint32_t transform_node_count;
    uint32_t dispatch_count;
    uint32_t primitive_count;
    uint32_t anchor_count;
    uint32_t primitive_family_mask;
    uint32_t dispatch_mode_mask;
    uint32_t ordering_shift;
    uint32_t screen_right;
    uint32_t packed_screen_bottom;
    uint32_t depth_cue_color_word;
    uint32_t guest_xclip_bound;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    bool primitive_digest_validated;
    bool anchor_digest_validated;
    bool authenticated;
    bool sealed;
    bool consumed;
} XgWorldModelsNativePreparation;

typedef struct XgWorldModelsNativePrimitiveOutput {
    XgRenderIrNativePrimitive primitive;
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    uint32_t packet_words[XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT];
    uint32_t packet_word_write_mask;
    /* Subset of packet writes produced by the effective guest cull. */
    uint32_t guest_packet_word_write_mask;
    int32_t nclip;
    uint32_t projection_flags;
    uint32_t ordering_depth;
    uint32_t ordering_bucket;
    uint32_t packet_address;
    uint8_t packet_word_count;
    bool passed_screen_cull;
    /* Guest-visible counter and packet side effects use the guest xclip bound. */
    bool counter_incremented;
    bool accepted;
    bool ordering_table_written;
    bool guest_ordering_table_written;
    bool packet_cursor_masked;
    bool primitive_staged;
    bool packet_side_effects_staged;
} XgWorldModelsNativePrimitiveOutput;

typedef struct XgWorldModelsNativeDispatchOutput {
    uint32_t source_index;
    uint32_t processed_primitive_count;
    uint32_t accepted_primitive_count;
    uint32_t staged_primitive_count;
    uint32_t packet_side_effect_primitive_count;
    uint32_t staged_packet_side_effect_count;
    uint32_t emitted_count_delta;
    uint32_t primitive_family_mask;
    uint32_t packet_cursor_after;
    uint32_t group_cursor_after;
    bool bounds_accepted;
    bool guest_bounds_accepted;
    bool packet_cursor_masked;
} XgWorldModelsNativeDispatchOutput;

typedef struct XgWorldModelsNativeCommit {
    XgWorldModelsEntrySideEffects entry_side_effects;
    uint64_t authentication_generation;
    uint32_t continuation_pc;
    uint32_t resident_vertex_total;
    uint32_t resident_emitted_count;
    uint32_t resident_packet_cursor;
    uint32_t resident_group_cursor;
    uint32_t resident_model_0c;
    uint32_t resident_vertex_base;
    uint32_t resident_ot_base;
    uint32_t resident_model_18;
    uint32_t completed_dispatch_count;
    uint32_t processed_primitive_count;
    uint32_t accepted_primitive_count;
    bool resident_dispatch_globals_written;
    bool authenticated;
    bool sealed;
} XgWorldModelsNativeCommit;

typedef bool (*XgWorldModelsNativeBeginSubmission)(void *context);
typedef bool (*XgWorldModelsNativeStagePrimitive)(
    void *context, const XgRenderIrNativePrimitive *primitive,
    uint32_t packet_address, uint32_t primitive_index);

/* Captures source values without mutating guest state. read_packet_template
 * must return an authenticated semantic sidecar seeded by the packet
 * initializer and maintained only from authorized modeled writes; mutable
 * packet output RAM is not an allowed source. Every caller-owned output array
 * is non-authoritative unless the returned preparation is authenticated and
 * sealed. */
XgWorldModelsNativeResult xg_world_models_native_prepare(
    const XgWorldModelsNativeRequest *request,
    const XgWorldModelsNativeAuthenticatedReader *reader,
    const XgWorldModelsNativeWorkspace *workspace,
    XgWorldModelsRecordOutput *records, uint32_t record_capacity,
    XgWorldModelsNodeSideEffect *node_side_effects,
    uint32_t node_side_effect_capacity,
    XgWorldModelsNativeDispatch *dispatches, uint32_t dispatch_capacity,
    XgWorldModelsNativePrimitiveSource *primitives,
    uint32_t primitive_capacity,
    XgWorldModelsNativeAnchorSource *anchor_sources,
    uint32_t anchor_capacity,
    XgWorldModelsNativePreparation *out_preparation);

XgWorldModelsNativeResult xg_world_models_native_build_primitive(
    XgWorldModelsNativePreparation *preparation,
    uint64_t authentication_generation,
    const XgWorldModelsNativePrimitiveSource *source,
    XgWorldModelsNativePrimitiveOutput *out_primitive);

XgWorldModelsNativeResult xg_world_models_native_build_anchor_vertex(
    XgWorldModelsNativePreparation *preparation,
    uint64_t authentication_generation,
    const XgWorldModelsNativeAnchorSource *source,
    XgRenderIrVertex *out_vertex);

/* Seals guest-visible outputs only after every prepared dispatch and packet
 * write mask has been authenticated, then every accepted semantic primitive
 * has been staged.
 * packet_word_write_mask bit zero authorizes dynamic DMA-tag linking;
 * packet_words[0] is never the link value. packet_cursor_masked is the
 * aggregate OR of primitive outputs. Packet-side-effect staging flags are
 * verification receipts; Original remains the only guest-memory writer. The
 * callbacks are the only authority for presentation-gate admission and
 * semantic staging. Empty submissions do not open a producer transaction. */
XgWorldModelsNativeResult xg_world_models_native_finalize(
    XgWorldModelsNativePreparation *preparation,
    uint64_t authentication_generation,
    const XgWorldModelsNativeDispatch *dispatches,
    XgWorldModelsNativeDispatchOutput *dispatch_outputs,
    uint32_t dispatch_output_count,
    const XgWorldModelsNativePrimitiveSource *primitives,
    XgWorldModelsNativePrimitiveOutput *primitive_outputs,
    uint32_t primitive_output_count, void *submission_context,
    XgWorldModelsNativeBeginSubmission begin_submission,
    XgWorldModelsNativeStagePrimitive stage_primitive,
    XgWorldModelsNativeCommit *out_commit);

#ifdef __cplusplus
}
#endif

#endif
