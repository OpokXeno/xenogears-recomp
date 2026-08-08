#ifndef XG_WORLD_MODELS_H
#define XG_WORLD_MODELS_H

#include "xg_host_3d.h"
#include "xg_model_ft4_raw.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_MODELS_PRODUCER_SIZE = 0x40cu,
    XG_WORLD_MODELS_RESIDENT_DISPATCH_SIZE = 0x1ccu,
    XG_WORLD_MODELS_RECORD_STRIDE = 0x54u,
    XG_WORLD_MODELS_DISPATCH_SELECTOR_COUNT = 10u,
    XG_WORLD_MODELS_DISPATCH_MODE_COUNT = 6u,
    XG_WORLD_MODELS_PACKET_STRIDE = 0x28u,
    XG_WORLD_MODELS_COARSE_DEPTH_LIMIT = 0x0d80u,

    XG_WORLD_MODELS_RECORD_STATE_OFFSET = 0x00u,
    XG_WORLD_MODELS_RECORD_RESOURCE_INDEX_OFFSET = 0x02u,
    XG_WORLD_MODELS_RECORD_DISPATCH_SELECTOR_OFFSET = 0x04u,
    XG_WORLD_MODELS_RECORD_POSITION_X_OFFSET = 0x08u,
    XG_WORLD_MODELS_RECORD_POSITION_Y_OFFSET = 0x0cu,
    XG_WORLD_MODELS_RECORD_STORED_Z_OFFSET = 0x10u,
    XG_WORLD_MODELS_RECORD_MATRIX_OFFSET = 0x20u,
    XG_WORLD_MODELS_RECORD_MODEL_HEADER_OFFSET = 0x40u,
    XG_WORLD_MODELS_RECORD_AUXILIARY_OFFSET = 0x44u,
    XG_WORLD_MODELS_RECORD_PACKET_BASE_0_OFFSET = 0x48u,
    XG_WORLD_MODELS_RECORD_PACKET_BASE_1_OFFSET = 0x4cu,
    XG_WORLD_MODELS_RECORD_TRANSFORM_CHAIN_OFFSET = 0x50u,

    XG_WORLD_MODELS_NODE_POSITION_X_OFFSET = 0x08u,
    XG_WORLD_MODELS_NODE_POSITION_Y_OFFSET = 0x0cu,
    XG_WORLD_MODELS_NODE_STORED_Z_OFFSET = 0x10u,
    XG_WORLD_MODELS_NODE_MATRIX_OFFSET = 0x20u,
    XG_WORLD_MODELS_NODE_WRITEBACK_X_OFFSET = 0x34u,
    XG_WORLD_MODELS_NODE_WRITEBACK_Y_OFFSET = 0x38u,
    XG_WORLD_MODELS_NODE_WRITEBACK_Z_OFFSET = 0x3cu,
    XG_WORLD_MODELS_NODE_NEXT_OFFSET = 0x50u,

    XG_WORLD_MODELS_MODEL_PRIMITIVE_COUNT_OFFSET = 0x04u,
    XG_WORLD_MODELS_MODEL_GROUP_COUNT_OFFSET = 0x06u,
    XG_WORLD_MODELS_MODEL_VERTEX_BASE_OFFSET = 0x08u,
    XG_WORLD_MODELS_MODEL_TOPOLOGY_BASE_OFFSET = 0x10u,
    XG_WORLD_MODELS_MODEL_MATERIAL_BASE_OFFSET = 0x14u,
    XG_WORLD_MODELS_MODEL_BOUNDS_MIN_OFFSET = 0x20u,
    XG_WORLD_MODELS_MODEL_BOUNDS_MAX_OFFSET = 0x28u,
    XG_WORLD_MODELS_MODEL_PACKET_CAPACITY_OFFSET = 0x34u,
    XG_WORLD_MODELS_CONTEXT_OT_OFFSET = 0x70u,
};

#define XG_WORLD_MODELS_PRODUCER_ENTRY UINT32_C(0x800848f4)
#define XG_WORLD_MODELS_RESIDENT_DISPATCH_ENTRY UINT32_C(0x8002c700)
#define XG_WORLD_MODELS_RECORD_BASE_GLOBAL UINT32_C(0x8009c620)
#define XG_WORLD_MODELS_RECORD_COUNT_GLOBAL UINT32_C(0x8009d7e0)
#define XG_WORLD_MODELS_CAMERA_X_GLOBAL UINT32_C(0x8009be28)
#define XG_WORLD_MODELS_CAMERA_Z_GLOBAL UINT32_C(0x8009be30)
#define XG_WORLD_MODELS_CAMERA_MATRIX_GLOBAL UINT32_C(0x8009c808)
#define XG_WORLD_MODELS_BUFFER_INDEX_GLOBAL UINT32_C(0x8009d7f0)
#define XG_WORLD_MODELS_DISPATCH_TABLE_GLOBAL UINT32_C(0x8009ad2c)
#define XG_WORLD_MODELS_CONTEXT_POINTER_GLOBAL UINT32_C(0x8009be3c)
#define XG_WORLD_MODELS_WRAP_X_GLOBAL UINT32_C(0x8009d160)
#define XG_WORLD_MODELS_WRAP_Z_GLOBAL UINT32_C(0x8009d2b4)
#define XG_WORLD_MODELS_SCREEN_RIGHT_GLOBAL UINT32_C(0x800500f8)

#define XG_WORLD_MODELS_SCALE_X_SCRATCH UINT32_C(0x1f800010)
#define XG_WORLD_MODELS_SCALE_Y_SCRATCH UINT32_C(0x1f800014)
#define XG_WORLD_MODELS_SCALE_Z_SCRATCH UINT32_C(0x1f800018)
#define XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH UINT32_C(0x1f8000a0)
#define XG_WORLD_MODELS_RESIDENT_CULL_MODE_GLOBAL UINT32_C(0x80050104)
#define XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL UINT32_C(0x80059424)
#define XG_WORLD_MODELS_RESIDENT_GROUP_CURSOR_GLOBAL UINT32_C(0x80059528)
#define XG_WORLD_MODELS_RESIDENT_MODEL_0C_GLOBAL UINT32_C(0x8005952c)
#define XG_WORLD_MODELS_RESIDENT_VERTEX_BASE_GLOBAL UINT32_C(0x8005953c)
#define XG_WORLD_MODELS_RESIDENT_OT_BASE_GLOBAL UINT32_C(0x80059568)
#define XG_WORLD_MODELS_RESIDENT_EMITTED_COUNT_GLOBAL UINT32_C(0x80059578)
#define XG_WORLD_MODELS_RESIDENT_MODEL_18_GLOBAL UINT32_C(0x80059498)
#define XG_WORLD_MODELS_RESIDENT_VERTEX_TOTAL_GLOBAL UINT32_C(0x800595c0)

typedef enum XgWorldModelsResult {
    XG_WORLD_MODELS_OK = 0,
    XG_WORLD_MODELS_INVALID_ARGUMENT,
    XG_WORLD_MODELS_INVALID_SOURCE,
    XG_WORLD_MODELS_CAPACITY_EXCEEDED,
    XG_WORLD_MODELS_BUILD_FAILED,
} XgWorldModelsResult;

typedef enum XgWorldModelsDisposition {
    XG_WORLD_MODELS_INACTIVE = 0,
    XG_WORLD_MODELS_COARSE_FLAG_REJECTED,
    XG_WORLD_MODELS_COARSE_DEPTH_REJECTED,
    XG_WORLD_MODELS_RESIDENT_DISPATCH,
} XgWorldModelsDisposition;

typedef struct XgWorldModelsGteState {
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    uint16_t projection_distance;
    int16_t depth_cue_a;
    int32_t depth_cue_b;
    int16_t average_z_scale3;
    int16_t average_z_scale4;
    int32_t far_color[3];
} XgWorldModelsGteState;

/* Nodes are supplied in the exact order followed through guest offset 0x50. */
typedef struct XgWorldModelsTransformNodeSource {
    uint32_t guest_address;
    int32_t position_x;
    int32_t position_y;
    int32_t stored_z;
    XgHost3dMatrix matrix;
} XgWorldModelsTransformNodeSource;

typedef struct XgWorldModelsRecordSource {
    int16_t state;
    int16_t dispatch_selector;
    int32_t position_x;
    int32_t position_y;
    int32_t stored_z;
    XgHost3dMatrix matrix;
    uint32_t model_header_address;
    uint32_t packet_base[2];
    const XgWorldModelsTransformNodeSource *transform_nodes;
    uint32_t transform_node_count;
} XgWorldModelsRecordSource;

/* This contract contains pre-GTE values only: no packet, OT, or GTE result. */
typedef struct XgWorldModelsSource {
    const XgWorldModelsRecordSource *records;
    int16_t record_count;
    uint16_t buffer_index;
    int32_t camera_x_12_12;
    int32_t camera_z_12_12;
    int32_t wrap_x;
    int32_t wrap_z;
    XgHost3dMatrix camera_matrix;
    XgWorldModelsGteState gte;
    uint32_t ordering_table_address;
} XgWorldModelsSource;

typedef struct XgWorldModelsEntrySideEffects {
    uint32_t scratch_scale[3];
    uint32_t resident_cull_mode;
    uint32_t resident_vertex_total;
    uint32_t resident_emitted_count;
    int16_t coarse_origin[3];
} XgWorldModelsEntrySideEffects;

typedef struct XgWorldModelsNodeSideEffect {
    uint32_t guest_address;
    int32_t translation[3];
} XgWorldModelsNodeSideEffect;

/* This is the handoff boundary. Resident bounds tests, command dispatch,
 * globals, packet writes, and OT writes are intentionally not emulated. */
typedef struct XgWorldModelsResidentCall {
    uint32_t model_header_address;
    uint32_t packet_base_address;
    uint32_t ordering_table_address;
    uint8_t dispatch_mode;
} XgWorldModelsResidentCall;

typedef struct XgWorldModelsRecordOutput {
    XgHost3dMatrix object_to_view;
    XgHost3dProjection projection;
    XgWorldModelsResidentCall resident_call;
    uint32_t source_index;
    uint32_t coarse_flags;
    uint16_t coarse_depth;
    XgWorldModelsDisposition disposition;
} XgWorldModelsRecordOutput;

typedef struct XgWorldModelsBuildSummary {
    XgWorldModelsEntrySideEffects entry_side_effects;
    uint32_t record_count;
    uint32_t node_side_effect_count;
    uint32_t resident_dispatch_count;
} XgWorldModelsBuildSummary;

XgWorldModelsResult xg_world_models_build(
    const XgWorldModelsSource *source,
    XgWorldModelsRecordOutput *records,
    uint32_t record_capacity,
    XgWorldModelsNodeSideEffect *node_side_effects,
    uint32_t node_side_effect_capacity,
    XgWorldModelsBuildSummary *summary);

/* Projection, dispatch mode, and packet_address in source_values are replaced
 * with values derived by the world wrapper. */
XgModelFt4RawResult xg_world_models_build_raw_ft4(
    const XgWorldModelsRecordOutput *world_record,
    uint32_t primitive_index,
    const XgModelFt4RawSource *source_values,
    XgModelFt4RawRecord *record);

#ifdef __cplusplus
}
#endif

#endif
