#ifndef XG_RENDER_SCENE_SNAPSHOT_H
#define XG_RENDER_SCENE_SNAPSHOT_H

#include "xg_render_ir.h"
#include "xg_render_motion.h"
#include "xg_render_resource_repository.h"
#include "xg_render_ui_scene.h"
#include "gpu_render.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef XG_RENDER_SCENE_PASS_CAPACITY
#define XG_RENDER_SCENE_PASS_CAPACITY 32u
#endif

#ifndef XG_RENDER_SCENE_DRAW_CAPACITY
#define XG_RENDER_SCENE_DRAW_CAPACITY XG_RENDER_IR_ITEM_CAPACITY
#endif

#ifndef XG_RENDER_SCENE_RESOURCE_CAPACITY
#define XG_RENDER_SCENE_RESOURCE_CAPACITY 256u
#endif

#ifndef XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY
#define XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY 64u
#endif

#ifndef XG_RENDER_NATIVE_OPERATION_CAPACITY
#define XG_RENDER_NATIVE_OPERATION_CAPACITY XG_RENDER_SCENE_DRAW_CAPACITY
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgPresentationIdentity {
    uint64_t presentation_epoch;
    uint64_t source_sequence;
    uint64_t guest_vblank_sequence;
    uint64_t guest_cycle;
    uint32_t scene_generation;
} XgPresentationIdentity;

typedef enum XgSemanticModuleKind {
    XG_SEMANTIC_MODULE_RESIDENT = 0,
    XG_SEMANTIC_MODULE_FIELD,
    XG_SEMANTIC_MODULE_WORLD,
    XG_SEMANTIC_MODULE_BATTLE,
    XG_SEMANTIC_MODULE_BATTLING,
    XG_SEMANTIC_MODULE_MENU,
    XG_SEMANTIC_MODULE_MOVIE,
} XgSemanticModuleKind;

typedef struct XgSemanticSceneIdentity {
    uint32_t disc_id;
    uint64_t executable_identity;
    uint64_t primary_overlay_identity;
    uint64_t companion_set_identity;
    uint32_t authored_scene_id;
    uint32_t authored_submode;
    XgSemanticModuleKind module;
} XgSemanticSceneIdentity;

#define XG_SEMANTIC_RENDER_SCALE_MAX 8u
typedef struct XgSemanticDisplayState {
    uint16_t width;
    uint16_t height;
    uint16_t display_x;
    uint16_t display_y;
    uint16_t aspect_num;
    uint16_t aspect_den;
    bool depth24;
    bool interlaced;
    bool disabled;
    /* Optional captured host layout; all zero means canonical-only. Width is
     * the expanded surface width, height is the configured reference height
     * (not active scanlines), and offset is the symmetric horizontal margin.
     * canonical_width = native_width - 2*native_offset_x. */
    uint16_t native_width;
    uint16_t native_height;
    uint16_t native_offset_x;
    /* Captured requested presentation rate, not the monitor refresh rate.
     * Zero disables source-motion smoothing without changing Native geometry. */
    uint16_t temporal_hz;
    /* Captured host raster scale; 0 and 1 both mean 1x. Guest coordinates,
     * VRAM dimensions and display timing are never multiplied by this value. */
    uint16_t render_scale;
    /* Captured host override. False preserves the guest GP0(E1) dither bit;
     * true suppresses ordered dithering in endpoints and interpolated phases. */
    bool dithering_disabled;
    /* Captured host option. True enables the per-pixel Native depth test for
     * producer-classified 3D draws in VIEW/phase planes; guest VRAM never. */
    bool native_depth_test;
} XgSemanticDisplayState;

typedef enum XgSemanticPassLoadOperation {
    XG_SEMANTIC_PASS_LOAD = 0,
    XG_SEMANTIC_PASS_CLEAR,
    XG_SEMANTIC_PASS_DISCARD,
} XgSemanticPassLoadOperation;

typedef struct XgSemanticPassRecord {
    uint32_t pass_id;
    uint64_t target_surface_id;
    uint64_t target_generation;
    uint32_t dependency_mask;
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    XgSemanticPassLoadOperation load_operation;
    bool store;
    bool presentation_output;
} XgSemanticPassRecord;

typedef struct XgSemanticResourceRef {
    uint64_t resource_id;
    uint64_t generation;
    uint64_t content_digest;
} XgSemanticResourceRef;

#define XG_RENDER_TEMPORAL_COVERAGE_VERSION UINT32_C(0x54435601)
/* World decorations can contain 25 lists of 512 independent quads. Bounds
 * validate dynamically sized publications; these are not fixed allocations. */
#define XG_RENDER_TEMPORAL_COMPONENT_CAPACITY 16384u
#define XG_RENDER_TEMPORAL_SAMPLE_CAPACITY 65536u

/* Producer-declared atomic geometry, not a packet, material or visibility island.
 * scene_id is the vertex namespace (it need not equal presentation scene_generation).
 * geometry_id changes on source topology/resource replacement, never camera motion. */
typedef struct XgRenderTemporalComponent {
    uint64_t component_id;
    uint64_t geometry_id;
    uint64_t scene_id;
    uint32_t producer_id;
} XgRenderTemporalComponent;

/* Raw producer projection, BEFORE GP0 destination relocation. Native coordinates
 * include the captured host margin. Only position/projection/vertex identity fields
 * are retained; UV, lighting and raster attributes are not temporal authority. */
typedef struct XgRenderTemporalSample {
    uint64_t component_id;
    GpuRenderSemanticVertex vertex;
} XgRenderTemporalSample;

typedef struct XgRenderTemporalCoverageHeader {
    uint32_t version;
    uint32_t producer_scope;
    XgPresentationIdentity identity;
    uint64_t source_update;
    uint32_t component_count;
    uint32_t sample_count;
} XgRenderTemporalCoverageHeader;

/* Borrowed view of a retained immutable MODEL resource. Arrays follow header in
 * storage, sorted by component_id and (component_id, group_id, vertex_id).
 * Each publication REPLACES the entire producer_scope, including empty coverage. */
typedef struct XgRenderTemporalCoverageView {
    const XgRenderTemporalCoverageHeader *header;
    const XgRenderTemporalComponent *components;
    const XgRenderTemporalSample *samples;
} XgRenderTemporalCoverageView;

typedef struct XgRenderTemporalBinding {
    XgSemanticResourceRef coverage;
    uint64_t component_id;
} XgRenderTemporalBinding;

/* FIFO metadata event at the gap BEFORE native_operations[before_operation].
 * An index equal to native_operation_count is the trailing gap. This is not a
 * GPU operation. Keep publications separate from the commit's unique retains. */
typedef struct XgRenderTemporalPublication {
    XgSemanticResourceRef coverage;
    uint32_t before_operation;
} XgRenderTemporalPublication;

typedef struct XgSemanticDrawRecord {
    XgSemanticOrderKey order;
    XgRenderIrProvenanceKey provenance;
    uint32_t source_primitive_index;
    uint64_t texture_resource_id;
    uint64_t texture_generation;
    uint64_t clut_resource_id;
    uint64_t clut_generation;
    bool has_provenance;
    bool interpolable;
    uint64_t interpolation_id;
    XgRenderIrNativePrimitive primitive;
    /* Material remains in primitive.material for either topology. Triangles
     * are stored only in primitive; line draws have primitive.triangle_count=0. */
    GpuRenderSemanticTopology topology;
    uint8_t line_count;
    GpuRenderSemanticLine lines[GPU_RENDER_SEMANTIC_LINE_CAPACITY];
    uint8_t screen_space_2d;
    uint8_t aa_exempt;
    uint8_t native_view_effect;
    uint16_t native_view_effect_index;
    /* Host HD texture replacement chosen for this draw (valid == 0: none). */
    GpuRenderHdTexture hd_texture;
} XgSemanticDrawRecord;

typedef enum XgRenderNativeOperationKind {
    XG_RENDER_NATIVE_OPERATION_DRAW = 0,
    XG_RENDER_NATIVE_OPERATION_UPLOAD,
    XG_RENDER_NATIVE_OPERATION_COPY,
    XG_RENDER_NATIVE_OPERATION_FILL,
    XG_RENDER_NATIVE_OPERATION_TARGET,
} XgRenderNativeOperationKind;

/* Pointer-free work for the native VRAM FIFO, not a scene fragment. Coordinates
 * and nonzero extents are decoded VRAM words/rows (1024x512); rectangles may
 * wrap. DRAW uses semantic.material masks. Other operations use mask_set/check.
 * UPLOAD reads a matching RGB555/CLUT555 resource from its first row, respecting
 * descriptor.row_pitch. COPY reads native VRAM at execution, not a host image.
 * TARGET selects an explicitly captured, non-wrapping framebuffer rectangle
 * using dst_x/dst_y/width/height only; it is not inferred from a draw scissor. */
typedef struct XgRenderNativeOperation {
    XgRenderNativeOperationKind kind;
    GpuRenderSemantic semantic;
    /* Optional presentation-only LOCAL geometry and shared immutable pose.
     * FIFO/endpoint execution always uses semantic unchanged. */
    XgRenderMotionDrawBinding motion;
    XgRenderTemporalBinding temporal;
    uint16_t src_x;
    uint16_t src_y;
    uint16_t dst_x;
    uint16_t dst_y;
    uint16_t width;
    uint16_t height;
    uint16_t fill_color;
    bool mask_set;
    bool mask_check;
    XgSemanticResourceRef upload;
    /* DRAW only: host HD texture replacement decided at submission. */
    GpuRenderHdTexture hd_texture;
} XgRenderNativeOperation;

typedef enum XgSemanticSurfaceEdgeKind {
    XG_SEMANTIC_SURFACE_COPY = 0,
    XG_SEMANTIC_SURFACE_SAMPLE,
    XG_SEMANTIC_SURFACE_FEEDBACK,
    XG_SEMANTIC_SURFACE_MOVIE,
} XgSemanticSurfaceEdgeKind;

typedef struct XgSemanticSurfaceRect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} XgSemanticSurfaceRect;

typedef struct XgSemanticSurfaceSampleState {
    XgRenderResourceSampler sampler;
    XgRenderResourceWrap wrap_u;
    XgRenderResourceWrap wrap_v;
    uint64_t clut_resource_id;
    uint64_t clut_generation;
    uint16_t clut_x;
    uint16_t clut_y;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    XgRenderIrTextureDepth texture_depth;
    XgRenderIrBlendMode blend_mode;
    bool palette_enabled;
    bool semi_transparent;
    bool mask_set;
    bool mask_check;
} XgSemanticSurfaceSampleState;

typedef struct XgSemanticSurfaceEdge {
    uint64_t source_surface_id;
    uint64_t source_generation;
    uint64_t target_surface_id;
    uint64_t target_generation;
    XgSemanticSurfaceEdgeKind kind;
    XgSemanticOrderKey order;
    XgSemanticSurfaceRect source;
    XgSemanticSurfaceRect destination;
    XgSemanticSurfaceSampleState sample;
    uint32_t effect_phase;
    uint32_t effect_phase_count;
} XgSemanticSurfaceEdge;

#ifdef __cplusplus
}
#endif

#endif
