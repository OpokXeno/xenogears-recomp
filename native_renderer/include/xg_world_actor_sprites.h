#ifndef XG_WORLD_ACTOR_SPRITES_H
#define XG_WORLD_ACTOR_SPRITES_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"
#include "xg_sprite_ft4.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_ACTOR_SPRITES_ACTOR_CAPACITY = 64,
    XG_WORLD_ACTOR_SPRITES_DESCRIPTOR_CAPACITY = 63,
    XG_WORLD_ACTOR_SPRITES_PART_CAPACITY = 8,
    XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY =
        XG_WORLD_ACTOR_SPRITES_DESCRIPTOR_CAPACITY * 2,
    XG_WORLD_ACTOR_SPRITES_RECORD_CAPACITY =
        XG_WORLD_ACTOR_SPRITES_ACTOR_CAPACITY *
        XG_WORLD_ACTOR_SPRITES_DESCRIPTOR_CAPACITY * 2,
    XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE = 0x28,
    XG_WORLD_ACTOR_SPRITE_PACKET_WORD_COUNT = 10,
    XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT = 9,
    XG_WORLD_ACTOR_SPRITE_TAG_PAYLOAD_WORD_COUNT = 9,
    XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT = 0x1000,
    XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_X = 1,
    XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_Y = 2,
    XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_Z = 4,
};

#define XG_WORLD_ACTOR_SPRITES_WORLD_ENTRY UINT32_C(0x80085cdc)
#define XG_WORLD_ACTOR_SPRITES_WORLD_CALLER_RETURN UINT32_C(0x80071ab0)
#define XG_WORLD_ACTOR_SPRITES_RESIDENT_ENTRY UINT32_C(0x8001e298)
#define XG_WORLD_ACTOR_SPRITES_RESIDENT_CALLER_RETURN UINT32_C(0x80085eac)
#define XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM UINT32_C(0x8001e2b4)
#define XG_WORLD_ACTOR_SPRITES_CONTINUATION UINT32_C(0x8001e2e0)
#define XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR UINT32_C(0x80059580)
#define XG_WORLD_ACTOR_SPRITES_PACKET_LIMIT UINT32_C(0x80059534)
#define XG_WORLD_ACTOR_SPRITES_BODY_SCRATCH UINT32_C(0x8004fb98)
#define XG_WORLD_ACTOR_SPRITES_SHADOW_SCRATCH UINT32_C(0x8004fad8)

typedef enum XgWorldActorSpritesResult {
    XG_WORLD_ACTOR_SPRITES_OK = 0,
    XG_WORLD_ACTOR_SPRITES_INVALID_ARGUMENT,
    XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE,
    XG_WORLD_ACTOR_SPRITES_CAPACITY_EXCEEDED,
    XG_WORLD_ACTOR_SPRITES_BUILD_FAILED,
} XgWorldActorSpritesResult;

typedef enum XgWorldActorSpriteFamily {
    XG_WORLD_ACTOR_SPRITE_BODY = 0,
    XG_WORLD_ACTOR_SPRITE_SHADOW = 1,
} XgWorldActorSpriteFamily;

typedef struct XgWorldActorSpriteDescriptor {
    int16_t x;
    int16_t y;
    uint8_t u;
    uint8_t v;
    uint8_t width;
    uint8_t height;
    int8_t extent_x_adjust;
    int8_t extent_y_adjust;
    uint16_t tpage;
    uint16_t clut;
    uint32_t material_word;
    uint8_t part;
    bool reverse_x;
    bool reverse_y;
} XgWorldActorSpriteDescriptor;

typedef struct XgWorldActorSpritePartTransform {
    XgHost3dMatrix rotation;
    int32_t offset_x;
    int32_t offset_y;
    bool enabled;
} XgWorldActorSpritePartTransform;

typedef struct XgWorldActorSpriteActorSource {
    int32_t position[3];
    XgHost3dMatrix resolved_sprite_matrix;
    XgWorldActorSpritePartTransform
        parts[XG_WORLD_ACTOR_SPRITES_PART_CAPACITY];
    XgHost3dVector body_vertex_template[XG_HOST_3D_VERTEX_COUNT];
    XgHost3dVector shadow_vertex_template[XG_HOST_3D_VERTEX_COUNT];
    const XgWorldActorSpriteDescriptor *descriptors;
    uint32_t descriptor_count;
    int16_t scale;
    int16_t shadow_y;
    int8_t origin_x;
    int8_t origin_y;
    uint8_t scale_shift;
    uint8_t hidden_part_mask;
    bool active;
    bool sprite_present;
    bool origin_mirror_x;
    bool flip_x;
    bool flip_y;
    bool shadow_enabled;
    bool per_part_ordering;
    bool has_part_transforms;
} XgWorldActorSpriteActorSource;

typedef struct XgWorldActorSpritesValueSource {
    XgHost3dProjection camera_projection;
    XgRenderIrMaterialState material_template;
    const XgWorldActorSpriteActorSource *actors;
    uint32_t actor_count;
    int32_t camera_origin_x;
    int32_t camera_origin_z;
    int32_t wrap_x;
    int32_t wrap_z;
} XgWorldActorSpritesValueSource;

typedef struct XgWorldActorSpritesAdaptedActor {
    XgHost3dVector actor_position;
    XgHost3dProjection body_projection;
    XgHost3dProjection shadow_projection;
    uint32_t depth_projection_flags;
    uint32_t ordering_bucket;
    uint16_t depth;
    bool accepted;
} XgWorldActorSpritesAdaptedActor;

typedef struct XgWorldActorSpriteRecord {
    XgSpriteFt4Record sprite;
    XgHost3dVector source_vertices[XG_HOST_3D_VERTEX_COUNT];
    uint32_t
        packet_payload_words[XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT];
    uint32_t packet_payload_write_masks
        [XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT];
    uint32_t packet_address;
    uint32_t ordering_table_address;
    uint32_t actor_index;
    uint32_t descriptor_index;
    uint32_t ordering_bucket;
    uint32_t material_word;
    uint16_t tpage;
    uint16_t clut;
    uint8_t part;
    uint8_t tag_payload_word_count;
    uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2];
    XgWorldActorSpriteFamily family;
} XgWorldActorSpriteRecord;

/* resolved_sprite_matrix is the value after the resident preparation call at
 * 0x8001e2ac. The adapter never reads packets, ordering tables, or GTE output. */
XgWorldActorSpritesResult xg_world_actor_sprites_adapt_actor(
    const XgWorldActorSpritesValueSource *source,
    uint32_t actor_index,
    XgWorldActorSpritesAdaptedActor *out_actor);

XgWorldActorSpritesResult xg_world_actor_sprites_build(
    const XgWorldActorSpritesValueSource *source,
    XgWorldActorSpriteRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count);

#ifdef __cplusplus
}
#endif

#endif
