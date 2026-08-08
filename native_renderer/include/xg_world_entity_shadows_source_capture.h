#ifndef XG_WORLD_ENTITY_SHADOWS_SOURCE_CAPTURE_H
#define XG_WORLD_ENTITY_SHADOWS_SOURCE_CAPTURE_H

#include "xg_world_entity_shadows.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  XG_WORLD_ENTITY_SHADOWS_MAX_AUTHENTICATED_READS = 147,
};

typedef enum XgWorldEntityShadowsSourceRangeKind {
  XG_WORLD_ENTITY_SHADOWS_SOURCE_PENDING_LIST = 0,
  XG_WORLD_ENTITY_SHADOWS_SOURCE_TERRAIN_CHUNK,
} XgWorldEntityShadowsSourceRangeKind;

typedef bool (*XgWorldEntityShadowsReadU8)(void *context, uint32_t address,
                                           uint8_t *out_value);
typedef bool (*XgWorldEntityShadowsReadU16)(void *context, uint32_t address,
                                            uint16_t *out_value);
typedef bool (*XgWorldEntityShadowsReadU32)(void *context, uint32_t address,
                                            uint32_t *out_value);
typedef bool (*XgWorldEntityShadowsAuthorizeSourceRange)(
    void *context, XgWorldEntityShadowsSourceRangeKind kind, uint32_t address,
    uint32_t size);

typedef struct XgWorldEntityShadowsAuthenticatedReader {
  void *context;
  XgWorldEntityShadowsReadU8 read_u8;
  XgWorldEntityShadowsReadU16 read_u16;
  XgWorldEntityShadowsReadU32 read_u32;
  XgWorldEntityShadowsAuthorizeSourceRange authorize_source_range;
  uint64_t authentication_generation;
  bool authenticated;
} XgWorldEntityShadowsAuthenticatedReader;

typedef struct XgWorldEntityShadowsRasterState {
  uint16_t draw_area_left;
  uint16_t draw_area_top;
  uint16_t draw_area_right;
  uint16_t draw_area_bottom;
  int16_t draw_offset_x;
  int16_t draw_offset_y;
  bool dither;
  bool mask_set;
  bool mask_check;
} XgWorldEntityShadowsRasterState;

typedef struct XgWorldEntityShadowsCaptureRequest {
  uint64_t authentication_generation;
  uint32_t producer_callsite;
  int32_t screen_offset_x;
  int32_t screen_offset_y;
  uint16_t projection_distance;
  XgWorldEntityShadowsRasterState raster;
  bool projection_state_authenticated;
} XgWorldEntityShadowsCaptureRequest;

typedef struct XgWorldEntityShadowsCapture {
  XgWorldEntityShadowsSource source;
  uint64_t authentication_generation;
  uint32_t authenticated_read_count;
  uint32_t authenticated_read_bytes;
  uint32_t pending_list_address;
  uint32_t producer_callsite;
  bool authenticated;
  bool sealed;
} XgWorldEntityShadowsCapture;

typedef struct XgWorldEntityShadowsNativePreparation {
  XgWorldEntityShadowsSideEffects side_effects;
  uint64_t authentication_generation;
  uint32_t record_count;
  uint32_t accepted_record_count;
  uint32_t continuation_pc;
  bool authenticated;
  bool sealed;
} XgWorldEntityShadowsNativePreparation;

typedef enum XgWorldEntityShadowsCaptureResult {
  XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK = 0,
  XG_WORLD_ENTITY_SHADOWS_CAPTURE_INVALID_ARGUMENT,
  XG_WORLD_ENTITY_SHADOWS_CAPTURE_UNAUTHENTICATED,
  XG_WORLD_ENTITY_SHADOWS_CAPTURE_READ_FAILED,
  XG_WORLD_ENTITY_SHADOWS_CAPTURE_SOURCE_MISMATCH,
  XG_WORLD_ENTITY_SHADOWS_CAPTURE_FORBIDDEN_RANGE,
} XgWorldEntityShadowsCaptureResult;

XgWorldEntityShadowsCaptureResult xg_world_entity_shadows_source_capture(
    const XgWorldEntityShadowsCaptureRequest *request,
    const XgWorldEntityShadowsAuthenticatedReader *reader,
    XgWorldEntityShadowsCapture *out_capture);

/* Authenticated value-only boundary for a pre-body Native return cutover.
 * On success, apply records in source order. Unmasked packet words and pending
 * entries remain unchanged; side_effects authorizes the count write. No field
 * in out_preparation is authoritative after a non-OK result. */
XgWorldEntityShadowsResult xg_world_entity_shadows_prepare_native_cutover(
    const XgWorldEntityShadowsCapture *capture,
    uint64_t authentication_generation, XgWorldEntityShadowRecord *records,
    uint32_t record_capacity,
    XgWorldEntityShadowsNativePreparation *out_preparation);

#ifdef __cplusplus
}
#endif

#endif
