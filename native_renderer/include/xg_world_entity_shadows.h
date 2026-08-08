#ifndef XG_WORLD_ENTITY_SHADOWS_H
#define XG_WORLD_ENTITY_SHADOWS_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY = 16,
  XG_WORLD_ENTITY_SHADOW_RENDERABLE_CAPACITY = 15,
  XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE = 0x28,
  XG_WORLD_ENTITY_SHADOW_PACKET_WORD_COUNT = 10,
  XG_WORLD_ENTITY_SHADOW_PACKET_PAYLOAD_WORDS = 9,
  XG_WORLD_ENTITY_SHADOW_PACKET_BUFFER_COUNT = 2,
  XG_WORLD_ENTITY_SHADOW_PACKET_BUFFER_BYTES = 0x280,
  XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT = 0x100,
  XG_WORLD_ENTITY_SHADOW_UV_COUNT = 4,
  XG_WORLD_ENTITY_SHADOW_PACKET_GEOMETRY_WRITE_MASK = 0x154,
  XG_WORLD_ENTITY_SHADOW_PACKET_ACCEPTED_WRITE_MASK = 0x155,
};

#define XG_WORLD_ENTITY_SHADOWS_ENTRY_PC UINT32_C(0x800747dc)
#define XG_WORLD_ENTITY_SHADOWS_PRODUCER_CALLSITE UINT32_C(0x80071ab8)
#define XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC UINT32_C(0x80071ac0)
#define XG_WORLD_ENTITY_SHADOWS_PENDING_COUNT_ADDRESS UINT32_C(0x8009be38)
#define XG_WORLD_ENTITY_SHADOWS_PACKET_BASES_ADDRESS UINT32_C(0x8009be14)
#define XG_WORLD_ENTITY_SHADOWS_BUFFER_INDEX_ADDRESS UINT32_C(0x8009d7f0)
#define XG_WORLD_ENTITY_SHADOWS_CONTEXT_ADDRESS UINT32_C(0x8009be3c)
#define XG_WORLD_ENTITY_SHADOWS_CONTEXT_OT_OFFSET UINT32_C(0x70)

typedef enum XgWorldEntityShadowsResult {
  XG_WORLD_ENTITY_SHADOWS_OK = 0,
  XG_WORLD_ENTITY_SHADOWS_INVALID_ARGUMENT,
  XG_WORLD_ENTITY_SHADOWS_INVALID_SOURCE,
  XG_WORLD_ENTITY_SHADOWS_CAPACITY_EXCEEDED,
  XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED,
} XgWorldEntityShadowsResult;

typedef enum XgWorldEntityShadowCull {
  XG_WORLD_ENTITY_SHADOW_CULL_NONE = 0,
  XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS,
  XG_WORLD_ENTITY_SHADOW_CULL_MINIMUM_DEPTH,
} XgWorldEntityShadowCull;

/* Exact 8-byte list element consumed at 0x800747dc. */
typedef struct XgWorldEntityShadowPendingEntry {
  int16_t x;
  int16_t type;
  int16_t z;
  uint16_t pad;
} XgWorldEntityShadowPendingEntry;

typedef struct XgWorldEntityShadowPendingList {
  XgWorldEntityShadowPendingEntry entries[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
  uint32_t count;
} XgWorldEntityShadowPendingList;

typedef struct XgWorldEntityShadowSourceEntry {
  XgWorldEntityShadowPendingEntry pending;
  XgHost3dLongVector terrain_normal;
  int32_t terrain_height_fixed;
  uint32_t terrain_chunk_address;
  uint32_t terrain_cell_address;
  uint8_t terrain_heights[5];
} XgWorldEntityShadowSourceEntry;

typedef struct XgWorldEntityShadowsSource {
  XgWorldEntityShadowPendingList pending;
  XgWorldEntityShadowSourceEntry entries[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
  XgHost3dMatrix camera;
  XgHost3dMatrix billboard;
  XgRenderIrMaterialState material;
  int32_t camera_origin_x_fixed;
  int32_t camera_origin_z_fixed;
  int32_t screen_offset_x;
  int32_t screen_offset_y;
  uint16_t projection_distance;
  int16_t type2_sine;
  int16_t type2_cosine;
  bool has_type2;
} XgWorldEntityShadowsSource;

typedef struct XgWorldEntityShadowRecord {
    XgRenderIrNativePrimitive primitive;
    XgHost3dMatrix local_transform;
    XgHost3dMatrix transform;
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
  uint32_t rtpt_flags;
  uint32_t rtps_flags;
  uint32_t ordering_bucket;
  uint32_t source_index;
  uint32_t packet_offset;
  uint32_t material_word;
  /* Bit N authorizes the exact Original write to packet word N. */
  uint16_t packet_word_write_mask;
  uint16_t minimum_depth;
  uint16_t clut;
  uint16_t tpage;
  uint16_t uv[XG_WORLD_ENTITY_SHADOW_UV_COUNT];
  int16_t type;
  XgWorldEntityShadowCull cull;
  /* Original links this packet into ordering_bucket after its packet writes. */
  bool ordering_table_written;
  bool accepted;
} XgWorldEntityShadowRecord;

/* Non-render guest state and packet-cursor effects of 0x800747dc. */
typedef struct XgWorldEntityShadowsSideEffects {
  uint32_t pending_count_before;
  uint32_t pending_count_after;
  uint32_t packet_slots_advanced;
  uint32_t packet_bytes_advanced;
  uint32_t ordering_insertions;
  bool pending_count_written;
  bool pending_entries_unchanged;
} XgWorldEntityShadowsSideEffects;

/* Value-only equivalent of the list mutation at 0x80074794. */
XgWorldEntityShadowsResult
xg_world_entity_shadows_enqueue(const XgWorldEntityShadowPendingList *before,
                                int16_t type, const int32_t position_fixed[3],
                                XgWorldEntityShadowPendingList *after);

XgWorldEntityShadowsResult xg_world_entity_shadows_build(
    const XgWorldEntityShadowsSource *source,
    XgWorldEntityShadowRecord *records, uint32_t record_capacity,
    uint32_t *out_record_count,
    XgWorldEntityShadowsSideEffects *out_side_effects);

#ifdef __cplusplus
}
#endif

#endif
