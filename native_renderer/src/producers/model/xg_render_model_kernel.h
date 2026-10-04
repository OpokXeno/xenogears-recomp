#ifndef XG_RENDER_MODEL_KERNEL_H
#define XG_RENDER_MODEL_KERNEL_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu_render.h"
#include "xg_host_3d_types.h"
#include "xg_render_depth_policy.h"
#include "xg_render_motion.h"

typedef struct CPUState CPUState;
typedef struct XgRenderProducerLifecycleServices XgRenderProducerLifecycleServices;

/* Shared by the overlay producers that draw through the resident model
 * kernel or project their own rigid geometry with the GTE (Battle, Battling).
 * Each producer keeps its own identities, authority and diagnostics. */

/* One rigid motion node: the exact GTE object-to-view matrix under an
 * identity camera. Interpolating it carries the object's motion and the
 * camera, and its endpoints stay anchored to the GTE. A source can draw one
 * identity several times per update (afterimages); repeats are numbered in
 * draw order. entity_tag and camera_id keep each producer's poses apart. */
typedef struct XgRenderRigidPoseRequest {
    uint64_t entity_tag;
    uint64_t camera_id;
    uint32_t source_pc;
    uint32_t identity;
    uint32_t key[4];
    uint32_t watch[2][2];
    /* World-fixed geometry the guest feeds camera-relative (its locals are
     * world - camera): bound locals are guest locals + local_offset, a fixed
     * world origin, and the pose is the GTE camera itself. The camera then
     * interpolates as a camera (eye and rotation), translation included,
     * instead of stepping with the locals. A world pose owns its camera_id. */
    bool world_space;
    int32_t local_offset[3];
} XgRenderRigidPoseRequest;

bool xg_render_rigid_pose_publish(const XgRenderRigidPoseRequest *request,
                                  const XgHost3dProjection *projection,
                                  XgRenderMotionSource *source,
                                  XgRenderMotionRef *out);

/* Host projection of one vertex as a semantic vertex (canonical XY plus the
 * native view and projective fields). */
void xg_render_projected_vertex_semantic(GpuRenderSemanticVertex *target,
                                         const XgHost3dProjectedVertex *source);

/* Bind one guest POLY_FT4 (corner order v0..v3, split {0,1,2},{2,1,3}) to the
 * host projection of its four local vertices, and to a rigid pose when
 * motion is not NULL, with motion_local[4] (world-space poses) or local as
 * the pose's locals. A binding is not a draw: GPU acceptance validates it. */
bool xg_render_rigid_stage_ft4(const XgHost3dProjection *projection,
                               uint32_t packet, const XgHost3dVector local[4],
                               const XgHost3dVector *motion_local,
                               const uint32_t vertex_ids[4], uint32_t identity,
                               uint32_t primitive_id,
                               const XgRenderMotionRef *motion,
                               uint64_t continuity_generation,
                               XgRenderDepthFamily depth_family);

/* SubmitModelPacket (0x8002c700) entry, after the caller authorized the call
 * site: bind every packet the kernel can emit for the model in a0/a1. */
typedef struct XgRenderModelKernelCall {
    uint64_t entity_tag;
    uint64_t camera_id;
    uint32_t source_pc;     /* call site inside a registered motion owner */
    uint32_t identity;      /* stable per part; 0 = no motion */
    XgRenderDepthFamily depth_family;
} XgRenderModelKernelCall;

typedef struct XgRenderModelKernelStats {
    uint64_t attempts, completed, rejected, bound_polygons, projected_vertices;
    uint64_t native_polygons, capacity_rejected;
    uint64_t motion_published, motion_rejected, motion_bound;
    uint32_t last_model, last_mode, last_family, last_packet;
} XgRenderModelKernelStats;

bool xg_render_model_kernel_capture(const CPUState *cpu,
                                    const XgRenderProducerLifecycleServices *lifecycle,
                                    const XgRenderModelKernelCall *call,
                                    volatile XgRenderModelKernelStats *stats);

#endif
