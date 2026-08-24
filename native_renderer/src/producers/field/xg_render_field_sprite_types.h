#ifndef XG_RENDER_FIELD_SPRITE_TYPES_H
#define XG_RENDER_FIELD_SPRITE_TYPES_H

#include "xg_render_ir.h"
#include "xg_render_producer_lifecycle.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum XgRenderFieldSpriteOverlayKind {
    XG_RENDER_FIELD_SPRITE_OVERLAY_FIELD,
    XG_RENDER_FIELD_SPRITE_OVERLAY_PROJECTED_2E,
} XgRenderFieldSpriteOverlayKind;

typedef struct XgRenderFieldSpriteOverlayPublication {
    XgRenderIrNativePrimitive primitive;
    XgRenderProducerLifecycle lifecycle;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t family;
    bool interpolation_identity_valid;
    bool material_ready;
    XgRenderFieldSpriteOverlayKind kind;
} XgRenderFieldSpriteOverlayPublication;

typedef struct XgRenderFieldSpriteTemplateInput {
    XgRenderIrNativePrimitive primitive;
    XgRenderProducerLifecycle lifecycle;
    uint32_t packet_address;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint32_t xy[4];
    uint16_t uv[4];
    uint16_t tpage;
    uint16_t clut;
    bool interpolation_identity_valid;
} XgRenderFieldSpriteTemplateInput;

#endif
