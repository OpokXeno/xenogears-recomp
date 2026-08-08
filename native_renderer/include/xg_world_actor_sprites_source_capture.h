#ifndef XG_WORLD_ACTOR_SPRITES_SOURCE_CAPTURE_H
#define XG_WORLD_ACTOR_SPRITES_SOURCE_CAPTURE_H

#include "xg_world_actor_sprites.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_READS = 861,
    XG_WORLD_ACTOR_SPRITES_NATIVE_MAX_AUTHENTICATED_BYTES = 1726,
};

typedef enum XgWorldActorSpritesSourceRangeKind {
    XG_WORLD_ACTOR_SPRITES_SOURCE_ACTOR = 0,
    XG_WORLD_ACTOR_SPRITES_SOURCE_DATA,
    XG_WORLD_ACTOR_SPRITES_SOURCE_DESCRIPTORS,
    XG_WORLD_ACTOR_SPRITES_SOURCE_PARTS,
    XG_WORLD_ACTOR_SPRITES_SOURCE_CONTEXT,
    XG_WORLD_ACTOR_SPRITES_SOURCE_TRIG_TABLE,
    XG_WORLD_ACTOR_SPRITES_SOURCE_CAMERA,
    XG_WORLD_ACTOR_SPRITES_SOURCE_SCRATCH_TEMPLATE,
    XG_WORLD_ACTOR_SPRITES_SOURCE_GLOBAL,
} XgWorldActorSpritesSourceRangeKind;

typedef bool (*XgWorldActorSpritesReadU8)(void *context, uint32_t address,
                                         uint8_t *out_value);
typedef bool (*XgWorldActorSpritesReadU16)(void *context, uint32_t address,
                                          uint16_t *out_value);
typedef bool (*XgWorldActorSpritesReadU32)(void *context, uint32_t address,
                                          uint32_t *out_value);
typedef bool (*XgWorldActorSpritesAuthorizeSourceRange)(
    void *context, XgWorldActorSpritesSourceRangeKind kind, uint32_t address,
    uint32_t size);

typedef struct XgWorldActorSpritesAuthenticatedReader {
    void *context;
    XgWorldActorSpritesReadU8 read_u8;
    XgWorldActorSpritesReadU16 read_u16;
    XgWorldActorSpritesReadU32 read_u32;
    XgWorldActorSpritesAuthorizeSourceRange authorize_source_range;
    uint64_t authentication_generation;
    bool authenticated;
} XgWorldActorSpritesAuthenticatedReader;

typedef struct XgWorldActorSpritesRasterState {
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
} XgWorldActorSpritesRasterState;

typedef struct XgWorldActorSpritesNativeRequest {
    uint64_t authentication_generation;
    uint32_t resident_entry_pc;
    uint32_t prepared_seam_pc;
    uint32_t resident_caller_return;
    uint32_t actor_address;
    uint32_t ordering_table_address;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    uint16_t projection_distance;
    int16_t depth_cue_a;
    int32_t depth_cue_b;
    int16_t average_z_scale4;
    XgWorldActorSpritesRasterState raster;
    bool resident_context_authenticated;
    bool projection_state_authenticated;
} XgWorldActorSpritesNativeRequest;

typedef struct XgWorldActorSpritesScratchOutput {
    XgHost3dVector vertices[XG_HOST_3D_VERTEX_COUNT];
    uint32_t address;
    uint8_t component_write_mask;
    bool written;
} XgWorldActorSpritesScratchOutput;

typedef struct XgWorldActorSpritesNativePreparation {
    XgWorldActorSpritesScratchOutput body_scratch;
    XgWorldActorSpritesScratchOutput shadow_scratch;
    uint64_t authentication_generation;
    uint32_t actor_address;
    uint32_t data_address;
    uint32_t descriptor_address;
    uint32_t initial_packet_cursor;
    uint32_t final_packet_cursor;
    uint32_t packet_limit;
    uint32_t record_count;
    uint32_t body_record_count;
    uint32_t shadow_record_count;
    uint32_t continuation_pc;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    bool packet_cursor_written;
    bool authenticated;
    bool sealed;
} XgWorldActorSpritesNativePreparation;

typedef enum XgWorldActorSpritesNativeResult {
    XG_WORLD_ACTOR_SPRITES_NATIVE_OK = 0,
    XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_ARGUMENT,
    XG_WORLD_ACTOR_SPRITES_NATIVE_UNAUTHENTICATED,
    XG_WORLD_ACTOR_SPRITES_NATIVE_READ_FAILED,
    XG_WORLD_ACTOR_SPRITES_NATIVE_SOURCE_MISMATCH,
    XG_WORLD_ACTOR_SPRITES_NATIVE_FORBIDDEN_RANGE,
    XG_WORLD_ACTOR_SPRITES_NATIVE_BUILD_FAILED,
    XG_WORLD_ACTOR_SPRITES_NATIVE_INVALID_OUTPUT,
} XgWorldActorSpritesNativeResult;

/* Authenticated preparation boundary at 0x8001e2b4, after the resident setup
 * call. Original remains authoritative for CPU/GTE/timing and guest writes;
 * the Native commit at 0x8001e2e0 must match every prepared result before any
 * semantic primitive is staged. Success returns body records followed by
 * optional ground-shadow records;
 * either family may be absent when its independent Original capacity gate
 * rejects the complete descriptor count.
 * Each record supplies the exact nine FT4 payload words and bit masks, packet
 * address, and OT address. A commit must verify Original's merged payload,
 * 0x09 DMA tag, OT linkage, scratch halfwords, and final packet cursor before
 * staging the corresponding Native primitive.
 * No output is authoritative after a non-OK result, and no guest state may be
 * changed before the complete preflight and staging transaction succeeds. */
XgWorldActorSpritesNativeResult xg_world_actor_sprites_native_prepare(
    const XgWorldActorSpritesNativeRequest *request,
    const XgWorldActorSpritesAuthenticatedReader *reader,
    XgWorldActorSpriteRecord *records, uint32_t record_capacity,
    XgWorldActorSpritesNativePreparation *out_preparation);

#ifdef __cplusplus
}
#endif

#endif
