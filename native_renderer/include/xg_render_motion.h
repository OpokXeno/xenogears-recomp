#ifndef XG_RENDER_MOTION_H
#define XG_RENDER_MOTION_H

#include "xg_host_3d_types.h"
#include "xg_render_resource_repository.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XG_RENDER_MOTION_NODE_CAPACITY 64u
#define XG_RENDER_MOTION_VERSION 2u

typedef struct XgRenderMotionTrs {
    double translation[3];
    double rotation[4]; /* Unit quaternion, xyzw. */
    double scale[3];
} XgRenderMotionTrs;

typedef enum XgRenderMotionPolicy {
    XG_RENDER_MOTION_LOCAL_TRS = 0,
    /* inv(parent local S) * R * S, without scaling local translation. */
    XG_RENDER_MOTION_CANCEL_PARENT_SCALE = 1,
} XgRenderMotionPolicy;

typedef struct XgRenderMotionNode {
    uint32_t id;
    int32_t parent; /* -1 or an earlier node index. */
    XgRenderMotionPolicy policy;
    uint32_t translation_s16; /* Canonical signed GTE RHS input, not raw MATRIX.t. */
    XgRenderMotionTrs local;
    /* Exact final GTE input, after the producer's Q12/translation stages.
     * Only drawable nodes need this anchor; it is not a decomposed matrix. */
    uint32_t source_matrix_valid;
    XgHost3dMatrix source_model_to_view;
} XgRenderMotionNode;

typedef struct XgRenderMotionSource {
    XgRenderResourceProvenance provenance;
    uint64_t presentation_epoch;
    uint64_t scene_generation;
    uint64_t continuity_generation;
    uint64_t source_update; /* Guest source update, NOT host presentation/chunk. */
    bool discontinuity;
} XgRenderMotionSource;

typedef enum XgRenderMotionEvent {
    XG_MOTION_SOURCE_REQUEST = 0,
    XG_MOTION_SOURCE_MODE_REJECT,
    XG_MOTION_SOURCE_DISPATCH_MISSING,
    XG_MOTION_SOURCE_OPCODE_REJECT,
    XG_MOTION_SOURCE_CAPABILITY_REJECT,
    XG_MOTION_SOURCE_READY,
    XG_MOTION_CANDIDATE_SEEN,
    XG_MOTION_CANDIDATE_IDENTITY_REJECT,
    XG_MOTION_CANDIDATE_REGISTERED,
    XG_MOTION_FIELD_CAPTURE,
    XG_MOTION_FIELD_CALLER_REJECT,
    XG_MOTION_FIELD_UNSUPPORTED,
    XG_MOTION_FIELD_MATRIX_REJECT,
    XG_MOTION_PUBLISH_ATTEMPT,
    XG_MOTION_PUBLISHED,
    XG_MOTION_BIND_ATTEMPT,
    XG_MOTION_BIND_MISSING,
    XG_MOTION_BIND_IDENTITY_REJECT,
    XG_MOTION_BIND_RESOURCE_REJECT,
    XG_MOTION_BOUND,
    XG_MOTION_INVALIDATE_CALL,
    XG_MOTION_INVALIDATE_SKIPPED,
    XG_MOTION_PUBLISH_IMPORT_REJECT,
    XG_MOTION_BIND_GEOMETRY_UNSUPPORTED,
    XG_MOTION_TRANSLATION_CANONICALIZED,
    XG_MOTION_TRANSLATION_STAGE_DISCRETE,
    XG_MOTION_EVENT_COUNT,
} XgRenderMotionEvent;

typedef struct XgRenderMotionDiagnostics {
    uint64_t events[XG_MOTION_EVENT_COUNT];
    uint32_t last_pc[XG_MOTION_EVENT_COUNT];
    uint32_t active_instances;
    uint32_t active_commands;
} XgRenderMotionDiagnostics;

/* Guest-owner diagnostics and loader-backed source authority. This authority
 * never opens/rearms the legacy whole-scene capture/return protocol. */
void xg_render_motion_note(XgRenderMotionEvent event, uint32_t pc);
void xg_render_motion_diagnostics(XgRenderMotionDiagnostics *out);
bool psx_xg_render_motion_source(uint32_t pc, XgRenderMotionSource *out);

typedef struct XgRenderMotionRef {
    XgRenderResourceHandle handle;
    uint64_t digest;
} XgRenderMotionRef;

/* Guest-owner endpoint refinement, independent of rigid temporal eligibility.
 * Apply the owned pose to a current LOCAL vertex while preserving viewport
 * offsets in its continuous source-projected Native Q16 position. Deforming
 * models can use this without binding their vertices to a rigid motion curve. */
bool xg_render_motion_refine_native_vertex(XgRenderMotionRef ref, uint32_t part,
    const XgHost3dVector *local, int32_t *native_x, int32_t *native_y,
    int32_t *native_depth_q12);

typedef enum XgRenderMotionTranslationStage {
    XG_RENDER_MOTION_TRANSLATION_AFFINE = 0,
    /* camera * s16(parent accumulated) * s16(leaf local). */
    XG_RENDER_MOTION_TRANSLATION_FIELD,
    /* (camera * s16(root)) * s16(bone-relative accumulated). */
    XG_RENDER_MOTION_TRANSLATION_GEAR,
    /* Parent chain is composed leaf-first; camera RHS is narrowed only AFTER
     * camera-relative subtraction and the authored World wrapping operation. */
    XG_RENDER_MOTION_TRANSLATION_WORLD,
} XgRenderMotionTranslationStage;

/* Resource storage is always sizeof(XgRenderMotionPose), including zeroed
 * inactive nodes. This fixed array is not a flexible-array payload. */
typedef struct XgRenderMotionPose {
    uint32_t version;
    uint32_t node_count;
    uint64_t presentation_epoch;
    uint64_t scene_generation;
    uint64_t continuity_generation; /* Source camera/teleport/interpolation reset epoch. */
    uint64_t source_update;
    uint64_t entity_id;
    uint64_t geometry_id;
    uint64_t geometry_generation;
    uint64_t camera_id;
    uint32_t discontinuity;
    uint32_t world_wrapped;
    XgRenderMotionTranslationStage translation_stage;
    /* Camera-to-world TRS (inverse of the captured view); projection offsets
     * are pixels, not Q16. Uniform inverse scale preserves source camera zoom. */
    XgRenderMotionTrs camera;
    double screen_offset[2];
    double projection_distance;
    /* World subtracts these coordinates BEFORE camera_matrix and wraps once.
     * Its 0.5 geometry scale affects the linear part, never translation. */
    double camera_origin[3];
    double wrap_span[3];
    double geometry_scale;
    /* Host depth test only (never positions): view Z added per view-Y unit
     * relative to a node's origin. A view-space billboard anchored at its feet
     * then takes the depth of an upright figure; zero for real geometry. */
    double upright_depth_slope;
    XgRenderMotionNode nodes[XG_RENDER_MOTION_NODE_CAPACITY];
} XgRenderMotionPose;

typedef struct XgRenderMotionDrawBinding {
    XgRenderMotionRef motion;
    uint32_t motion_part_index;
    uint32_t triangle_count;
    XgHost3dVector local[2][3];
    uint32_t vertex_ids[2][3];
} XgRenderMotionDrawBinding;

typedef struct XgRenderMotionTransform {
    double camera[3][4];
    double screen_offset[2];
    double projection_distance;
    double upright_depth_slope; /* XgRenderMotionPose.upright_depth_slope */
    double model_to_view[XG_RENDER_MOTION_NODE_CAPACITY][3][4];
} XgRenderMotionTransform;

typedef struct XgRenderMotionEvaluation {
    XgRenderMotionRef current;
    uint32_t node_count;
    bool interpolated;
    double alpha;
    XgRenderMotionTransform phase;
    XgRenderMotionTransform endpoints[2];
    /* Coherent Native camera/model basis. Canonical matrices above keep the
     * source GTE anchors; Native endpoints and phases share this precise basis. */
    XgRenderMotionTransform native_phase;
    XgRenderMotionTransform native_current;
    XgHost3dProjection source_projection[2][XG_RENDER_MOTION_NODE_CAPACITY];
} XgRenderMotionEvaluation;

typedef enum XgRenderMotionProjectResult {
    XG_RENDER_MOTION_PROJECTED = 0,
    XG_RENDER_MOTION_ENDPOINT,
    XG_RENDER_MOTION_CLIP_REQUIRED,
    XG_RENDER_MOTION_INVALID,
} XgRenderMotionProjectResult;

struct XgRenderNativeOperation;

/* Q12 source matrix only. Rejects singular/reflected/sheared transforms. */
bool xg_render_motion_decompose(const XgHost3dMatrix *matrix, XgRenderMotionTrs *out);
/* CompMatrix/RT consume the low signed 16 bits of RHS translation. Camera/LHS
 * translation and absolute World coordinates must NOT use this conversion. */
int32_t xg_render_motion_s16_translation(int32_t stored);
bool xg_render_motion_camera_from_view(const XgHost3dMatrix *view, XgRenderMotionTrs *out);
/* Authenticated guest producer only. Copies an immutable pose into repository;
 * repeated identical instance/update publications share the same handle. */
bool xg_render_motion_publish(const XgRenderMotionSource *source, const XgRenderMotionPose *pose,
                              XgRenderMotionRef *out);
/* View is borrowed while the caller owns a snapshot retain (e.g. source commit).
 * Retired generations remain valid; no guest reads or current-generation lookup. */
bool xg_render_motion_view(XgRenderMotionRef ref, const XgRenderMotionPose **out);
bool xg_render_motion_binding_valid(const XgRenderMotionDrawBinding *binding);
/* Guest-owner command cache. packet+4 is the command ID. NULL forgets a command.
 * Geometry is copied now, before the producer can mutate/reuse source RAM. */
bool xg_render_motion_register_command(uint32_t command_id,
                                       const XgRenderMotionDrawBinding *binding,
                                       uint32_t producer_id, uint32_t primitive_id);
void xg_render_motion_forget_range(uint32_t address, uint32_t size);
void xg_render_motion_forget_entity(uint64_t entity_id);
void xg_render_motion_prune_authority(void);
/* Guest source lifetime watches, never packet-output watches. A source mutation
 * retires only producer/cache ownership; already committed poses remain alive. */
bool xg_render_motion_watch(XgRenderMotionRef ref, uint32_t address, uint32_t size);
void xg_render_motion_invalidate_range(uint32_t address, uint32_t size);
void xg_render_motion_reset(void);
/* Called after authenticated command resolution, before source-commit append.
 * A valid binding refines Native XY using fractional LOCAL-to-view projection;
 * canonical guest XY remains unchanged. An absent/incompatible binding leaves
 * ordinary endpoint rendering unchanged. */
bool xg_render_motion_bind_command(uint32_t command_id, struct XgRenderNativeOperation *operation);
/* Consumer: evaluate ONCE per (previous,current,alpha), share across all draws.
 * Incompatible lifecycle/hierarchy selects current without interpolation. */
bool xg_render_motion_evaluate(XgRenderMotionRef previous, XgRenderMotionRef current, double alpha,
                               XgRenderMotionEvaluation *out);
/* Transform LOCAL geometry with a shared endpoint-anchored matrix curve, then
 * anchor each local vertex to exact GTE screen endpoints. Equal local vertices
 * share the result, including aliases in different polygons. Both alpha 0 and 1
 * are evaluated; ENDPOINT is only an incompatible/unpaired lifecycle.
 * screen_delta contains canonical pixel displacements from B plus phase depth;
 * native_delta contains the separate continuous subpixel displacements, plus the
 * view-Z displacement [2] of the same Native transforms (zero when static). Add
 * each to its CURRENT semantic plane so target relocation/viewport offsets stay intact.
 * Nonpositive phase depth uses GTE SZ/divide saturation, as at the endpoints;
 * it is not a geometric near-plane rejection of the bound model. */
XgRenderMotionProjectResult xg_render_motion_project(const XgRenderMotionEvaluation *evaluation,
                                                      const XgRenderMotionDrawBinding *binding,
                                                      double screen_delta[2][3][3],
                                                      double native_delta[2][3][3]);

#ifdef __cplusplus
}
#endif
#endif
