#ifndef XG_WORLD_DECORATIONS_H
#define XG_WORLD_DECORATIONS_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_DECORATIONS_POSITION_CAPACITY = 512,
    XG_WORLD_DECORATIONS_PACKET_CAPACITY = 512,
    XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT = 16,
    XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT = 4,
    XG_WORLD_DECORATIONS_FT4_PAYLOAD_WORD_COUNT = 9,
};

typedef enum XgWorldDecorationsResult {
    XG_WORLD_DECORATIONS_OK = 0,
    XG_WORLD_DECORATIONS_INVALID_ARGUMENT,
    XG_WORLD_DECORATIONS_INVALID_SOURCE,
    XG_WORLD_DECORATIONS_CAPACITY_EXCEEDED,
    XG_WORLD_DECORATIONS_BUILD_FAILED,
} XgWorldDecorationsResult;

typedef struct XgWorldDecorationsPosition {
    int16_t x;
    int16_t y;
    int16_t z;
    uint16_t pad;
} XgWorldDecorationsPosition;

typedef struct XgWorldDecorationsSource {
    XgWorldDecorationsPosition
        positions[XG_WORLD_DECORATIONS_POSITION_CAPACITY];
    XgHost3dVector vertices[XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT];
    XgHost3dMatrix camera_matrix;
    XgHost3dMatrix decoration_matrix;
    XgRenderIrMaterialState material;
    uint16_t depth_clut[XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT];
    uint8_t uv[XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT][2];
    uint32_t position_count;
    int32_t camera_origin_x;
    int32_t camera_origin_z;
    int32_t wrap_x;
    int32_t wrap_z;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint16_t projection_distance;
    int16_t depth_cue_a;
    int32_t depth_cue_b;
} XgWorldDecorationsSource;

typedef struct XgWorldDecorationsRecord {
    XgRenderIrNativePrimitive primitive;
    XgHost3dProjectedVertex ft4_vertices[XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT];
    /* Derived FT4 payload only; the DMA tag and OT link are intentionally
     * outside this value contract. */
    uint32_t ft4_payload_words[XG_WORLD_DECORATIONS_FT4_PAYLOAD_WORD_COUNT];
    uint32_t rtpt_flags;
    uint32_t rtps_flags;
    uint32_t ordering_bucket;
    uint32_t source_index;
    uint32_t packet_index;
    int16_t depth_cue;
    uint16_t third_depth;
    uint16_t clut;
    uint8_t tag_payload_word_count;
} XgWorldDecorationsRecord;

/* Appends records at *in_out_packet_count, matching the producer's shared
 * packet-count side effect across repeated helper calls. */
XgWorldDecorationsResult xg_world_decorations_build(
    const XgWorldDecorationsSource *source, XgWorldDecorationsRecord *records,
    uint32_t record_capacity, uint32_t *in_out_packet_count);

#ifdef __cplusplus
}
#endif

#endif
