#ifndef XG_RENDER_NATIVE_MESH_H
#define XG_RENDER_NATIVE_MESH_H

#include <stdbool.h>
#include <stdint.h>

#include "xg_host_3d_types.h"
#include "xg_render_ir.h"
#include "xg_render_scene_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A Native mesh: many triangles under one material and one rigid pose,
 * carried by a single MESH operation instead of one DRAW per polygon.
 *
 * Geometry (rarely rebuilt): pose LOCAL points and faces (three points and
 * the packet UV byte pair of each corner). Instance (per source frame): the
 * visible faces under the GTE state the guest projects them with. Expanding
 * an instance yields, per face, exactly what a DRAW of that GTE-projected,
 * depth-cued triangle would materialize, so endpoints and phases equal the
 * per-polygon path; only immutable, compact resources cross the commit. */
#define XG_RENDER_NATIVE_MESH_VERSION 3u
#define XG_RENDER_NATIVE_MESH_POINT_CAPACITY 65536u
#define XG_RENDER_NATIVE_MESH_FACE_CAPACITY 65536u

typedef struct XgRenderNativeMeshPoint {
    XgHost3dVector local;   /* pose LOCAL position */
    uint32_t vertex_id;
} XgRenderNativeMeshPoint;

typedef struct XgRenderNativeMeshFace {
    uint16_t points[3];
    uint8_t u[3], v[3];     /* GPU texel coordinates per corner */
} XgRenderNativeMeshFace;

typedef struct XgRenderNativeMeshGeometry {
    uint32_t version;
    uint32_t point_count;
    uint32_t face_count;
    uint32_t reserved;
    /* XgRenderNativeMeshPoint points[point_count];
     * XgRenderNativeMeshFace faces[face_count]; */
} XgRenderNativeMeshGeometry;

typedef struct XgRenderNativeMeshInstance {
    uint32_t version;
    uint32_t face_count;
    XgSemanticResourceRef geometry;
    /* The GTE state: a point projects as point.local + local_offset. */
    XgHost3dProjection projection;
    int32_t local_offset[3];
    /* DPCS: RGBC (code byte kept) towards the far colour by the IR0 of each
     * face's last corner, the vertex its RTPT leaves IR0 for. */
    uint32_t rgbc;
    int32_t far_color[3];
    uint32_t extra_count;
    /* uint16_t faces[face_count];
     * (8-byte aligned) XgRenderNativeMeshExtra extras[extra_count]; */
} XgRenderNativeMeshInstance;

/* A triangle with its own pose LOCAL points (not geometry faces), e.g. a
 * face clipped at the near plane: projected like points; DPCS by the IR0
 * for screen depth sz, the vertex its RTPT would leave IR0 for. */
typedef struct XgRenderNativeMeshExtra {
    XgHost3dVector local[3];
    uint32_t vertex_ids[3];
    uint8_t u[3], v[3];
    uint16_t sz;
    uint16_t flags; /* XG_RENDER_NATIVE_MESH_PHASE_ONLY */
    uint16_t reserved;
} XgRenderNativeMeshExtra;
/* Drawn by interpolated phases only, never at the endpoint: a face the
 * previous camera saw that this one does not (behind it or turned away). */
#define XG_RENDER_NATIVE_MESH_PHASE_ONLY 1u

/* One expanded face: the materialized triangle plus its pose LOCAL corners. */
typedef struct XgRenderNativeMeshTriangle {
    XgRenderIrTriangle triangle;
    XgHost3dVector local[3];
    uint32_t vertex_ids[3];
    uint32_t flags; /* XG_RENDER_NATIVE_MESH_PHASE_ONLY */
} XgRenderNativeMeshTriangle;

static inline const XgRenderNativeMeshPoint *xg_render_native_mesh_points(
        const XgRenderNativeMeshGeometry *geometry) {
    return (const XgRenderNativeMeshPoint *)(const void *)(geometry + 1);
}
static inline const XgRenderNativeMeshFace *xg_render_native_mesh_faces(
        const XgRenderNativeMeshGeometry *geometry) {
    return (const XgRenderNativeMeshFace *)(const void *)
        (xg_render_native_mesh_points(geometry) + geometry->point_count);
}
static inline const uint16_t *xg_render_native_mesh_instance_faces(
        const XgRenderNativeMeshInstance *instance) {
    return (const uint16_t *)(const void *)(instance + 1);
}
static inline size_t xg_render_native_mesh_extras_offset(uint32_t face_count) {
    return (sizeof(XgRenderNativeMeshInstance) + (size_t)face_count * sizeof(uint16_t) + 7u) & ~(size_t)7u;
}
static inline const XgRenderNativeMeshExtra *xg_render_native_mesh_instance_extras(
        const XgRenderNativeMeshInstance *instance) {
    return (const XgRenderNativeMeshExtra *)(const void *)((const uint8_t *)instance +
        xg_render_native_mesh_extras_offset(instance->face_count));
}
/* Faces, then extras: the triangles an expansion yields. */
static inline uint32_t xg_render_native_mesh_instance_triangles(
        const XgRenderNativeMeshInstance *instance) {
    return instance->face_count + instance->extra_count;
}

/* Each create imports an immutable resource; *out holds one snapshot
 * retain the caller releases (xg_render_resource_release) when done. */
bool xg_render_native_mesh_geometry_create(uint64_t owner_generation,
                                           const XgRenderNativeMeshPoint *points,
                                           uint32_t point_count,
                                           const XgRenderNativeMeshFace *faces,
                                           uint32_t face_count, XgSemanticResourceRef *out);
bool xg_render_native_mesh_instance_create(uint64_t owner_generation,
                                           const XgRenderNativeMeshInstance *header,
                                           const uint16_t *faces,
                                           const XgRenderNativeMeshExtra *extras,
                                           XgSemanticResourceRef *out);
/* Borrowed views of retained resources; false when absent or inconsistent. */
bool xg_render_native_mesh_geometry_view(XgSemanticResourceRef ref,
                                         const XgRenderNativeMeshGeometry **out);
bool xg_render_native_mesh_instance_view(XgSemanticResourceRef ref,
                                         const XgRenderNativeMeshInstance **out);
/* The instance's faces then its extras as triangles, in instance order; out
 * must hold xg_render_native_mesh_instance_triangles. False if a point does
 * not project (an instance only lists what its producer projected under the
 * same state). */
bool xg_render_native_mesh_expand(const XgRenderNativeMeshInstance *instance,
                                  const XgRenderNativeMeshGeometry *geometry,
                                  XgRenderNativeMeshTriangle *out);

/* The 16.16 vertex a DRAW materializes from a GTE-projected point, with its
 * packet UV byte pair and colour. Shared by producers and expansion. */
void xg_render_native_mesh_vertex(XgRenderIrVertex *out,
                                  const XgHost3dProjectedVertex *screen,
                                  uint8_t u, uint8_t v, uint32_t color);
/* GTE DPCS of rgbc towards far_color by the IR0 for screen depth sz. */
uint32_t xg_render_native_mesh_depth_cue(const XgHost3dProjection *projection,
                                         uint32_t rgbc, const int32_t far_color[3],
                                         uint16_t sz);

#ifdef __cplusplus
}
#endif

#endif
