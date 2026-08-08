#ifndef XG_WORLD_MINIMAP_H
#define XG_WORLD_MINIMAP_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_MINIMAP_TRIANGLE_COUNT = 4,
    XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT = 3,
    XG_WORLD_MINIMAP_MARKER_CAPACITY = 32,
    XG_WORLD_MINIMAP_ORDER_EVENT_CAPACITY = 38,
};

#define XG_WORLD_MINIMAP_NO_ORDER_EVENT UINT32_MAX
#define XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS UINT32_C(0x1f8000b8)
#define XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS UINT32_C(0x1f8000f0)
#define XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS UINT32_C(0x1f800104)

typedef enum XgWorldMinimapResult {
    XG_WORLD_MINIMAP_OK = 0,
    XG_WORLD_MINIMAP_INVALID_ARGUMENT,
    XG_WORLD_MINIMAP_INVALID_SOURCE,
    XG_WORLD_MINIMAP_BUILD_FAILED,
} XgWorldMinimapResult;

typedef enum XgWorldMinimapMarkerCoordinateRule {
    XG_WORLD_MINIMAP_MARKER_OFFSET = 0,
    XG_WORLD_MINIMAP_MARKER_RESIDENT_SCALED = 1,
} XgWorldMinimapMarkerCoordinateRule;

typedef enum XgWorldMinimapOrderKind {
    XG_WORLD_MINIMAP_ORDER_TRIANGLE = 0,
    XG_WORLD_MINIMAP_ORDER_DRAW_MODE,
    XG_WORLD_MINIMAP_ORDER_MARKER,
    XG_WORLD_MINIMAP_ORDER_PANEL,
} XgWorldMinimapOrderKind;

typedef struct XgWorldMinimapRasterState {
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    bool dither;
    bool mask_set;
    bool mask_check;
} XgWorldMinimapRasterState;

typedef struct XgWorldMinimapMarkerSource {
    uint16_t raw_x;
    uint16_t raw_y;
} XgWorldMinimapMarkerSource;

typedef struct XgWorldMinimapSource {
    XgHost3dVector triangles[XG_WORLD_MINIMAP_TRIANGLE_COUNT]
                                [XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT];
    XgWorldMinimapMarkerSource markers[XG_WORLD_MINIMAP_MARKER_CAPACITY];
    XgWorldMinimapRasterState raster;
    int32_t world_x_20_12;
    int32_t world_z_20_12;
    int32_t translation_z;
    int32_t vertical_offset;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    uint32_t marker_mask;
    uint32_t buffer_index;
    uint16_t projection_distance;
    uint16_t angle;
    int16_t sine;
    int16_t cosine;
} XgWorldMinimapSource;

typedef struct XgWorldMinimapScratchUpdates {
    uint32_t angle_address;
    uint32_t rotation_address;
    uint32_t translation_address;
    uint16_t angle_x;
    uint16_t angle_y;
    uint16_t angle_z;
    int16_t rotation[3][3];
    int32_t translation[3];
} XgWorldMinimapScratchUpdates;

typedef struct XgWorldMinimapTriangleRecord {
    XgRenderIrNativePrimitive primitive;
    int16_t screen_xy[XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT][2];
    uint32_t packet_address;
    uint32_t screen_xy_address[XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT];
    bool submitted;
} XgWorldMinimapTriangleRecord;

typedef struct XgWorldMinimapMarkerRecord {
    XgRenderIrNativePrimitive primitive;
    uint32_t packet_address;
    uint32_t screen_xy_address;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    XgWorldMinimapMarkerCoordinateRule coordinate_rule;
    bool active;
} XgWorldMinimapMarkerRecord;

typedef struct XgWorldMinimapPanelRecord {
    XgRenderIrNativePrimitive primitive;
    uint32_t packet_address;
    int16_t screen_xy[XG_HOST_3D_VERTEX_COUNT][2];
    uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2];
} XgWorldMinimapPanelRecord;

typedef struct XgWorldMinimapDrawModeRecord {
    uint32_t packet_address;
    uint32_t command_word;
    uint8_t payload_word_count;
} XgWorldMinimapDrawModeRecord;

typedef struct XgWorldMinimapOrderEvent {
    XgWorldMinimapOrderKind kind;
    uint32_t packet_address;
    /* Target addPrim call order. */
    uint32_t insertion_ordinal;
    /* Final traversal order, where zero is the producer's new OT head. */
    uint32_t final_chain_ordinal;
    /* Prior producer event linked by the tag, or the preexisting OT tail. */
    uint32_t successor_event_index;
    uint8_t source_index;
    uint8_t payload_word_count;
} XgWorldMinimapOrderEvent;

typedef struct XgWorldMinimapBuildOutput {
    XgHost3dProjection projection;
    XgWorldMinimapScratchUpdates scratch;
    XgWorldMinimapTriangleRecord
        triangles[XG_WORLD_MINIMAP_TRIANGLE_COUNT];
    XgWorldMinimapMarkerRecord markers[XG_WORLD_MINIMAP_MARKER_CAPACITY];
    XgWorldMinimapPanelRecord panel;
    XgWorldMinimapDrawModeRecord draw_mode;
    XgWorldMinimapOrderEvent
        ordering[XG_WORLD_MINIMAP_ORDER_EVENT_CAPACITY];
    uint32_t ordering_count;
    uint32_t active_marker_count;
    /* The first inserted triangle retains the caller-owned prior OT head. */
    bool requires_external_ot_tail;
} XgWorldMinimapBuildOutput;

XgWorldMinimapResult xg_world_minimap_build(
    const XgWorldMinimapSource *source,
    XgWorldMinimapBuildOutput *output);

#ifdef __cplusplus
}
#endif

#endif
