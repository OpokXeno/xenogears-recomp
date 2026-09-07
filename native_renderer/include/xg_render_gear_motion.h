#ifndef XG_RENDER_GEAR_MOTION_H
#define XG_RENDER_GEAR_MOTION_H

#include "xg_render_motion.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUState CPUState;

typedef struct XgRenderGearMotionServices {
    bool (*source)(uint32_t pc, XgRenderMotionSource *out);
    bool (*range)(uint32_t address, uint32_t size, uint32_t alignment, bool scratch);
    void (*watch)(uint32_t address, uint32_t size);
} XgRenderGearMotionServices;

typedef enum XgRenderGearMotionReject {
    XG_RENDER_GEAR_MOTION_AUTHORITY = 0,
    XG_RENDER_GEAR_MOTION_RANGE,
    XG_RENDER_GEAR_MOTION_HIERARCHY,
    XG_RENDER_GEAR_MOTION_ROTATION,
    XG_RENDER_GEAR_MOTION_REBUILD,
    XG_RENDER_GEAR_MOTION_ATTACHMENT,
    XG_RENDER_GEAR_MOTION_BILLBOARD,
    XG_RENDER_GEAR_MOTION_GEOMETRY,
    XG_RENDER_GEAR_MOTION_PUBLICATION,
    XG_RENDER_GEAR_MOTION_SCOPE,
    XG_RENDER_GEAR_MOTION_REJECT_COUNT,
} XgRenderGearMotionReject;

typedef struct XgRenderGearMotionDiagnostics {
    uint64_t rebuild_entries;
    uint64_t consumed_poses;
    uint64_t published_poses;
    uint64_t bound_parts;
    uint64_t last_consumed_revision;
    uint64_t rejected[XG_RENDER_GEAR_MOTION_REJECT_COUNT];
    uint32_t last_skeleton;
    uint32_t last_joint;
    int32_t last_overall_scale;
} XgRenderGearMotionDiagnostics;

/* Guest-owner observations only. Rebuild entry copies consumed Euler/T/S;
 * rebuild exit verifies the resulting local/accumulated matrices. Render entry
 * validates ALL pieces before publishing, not only the first visible hand. */
void xg_render_gear_motion_observe(CPUState *cpu, uint32_t pc,
                                   const XgRenderGearMotionServices *services);
bool xg_render_gear_motion_bind(CPUState *cpu, XgRenderMotionRef *out_pose, uint32_t *out_part);
void xg_render_gear_motion_reset(void);
void xg_render_gear_motion_invalidate(uint32_t address, uint32_t size);
void xg_render_gear_motion_diagnostics(XgRenderGearMotionDiagnostics *out);

#ifdef __cplusplus
}
#endif
#endif
