#include "xg_render_native_mesh.h"

#include "psx_gte_divide.h"
#include "xg_host_3d.h"
#include "xg_render_resource_repository.h"

#include <stdlib.h>
#include <string.h>

static bool import_blob(uint64_t tag, uint64_t owner_generation, const void *bytes,
                        size_t byte_count, XgSemanticResourceRef *out) {
    static uint64_t serial, receipt;
    if (!owner_generation || serial == UINT64_MAX || receipt == UINT64_MAX) return false;
    XgRenderResourceImport import = {0};
    XgRenderResourceCapabilityMetadata metadata = {0};
    const uint64_t key[4] = {tag, owner_generation, ++serial, byte_count};
    memcpy(import.identity.bytes, key, sizeof(key));
    const uint64_t id = xg_render_resource_digest(key, sizeof(key));
    for (uint32_t i = 0; i < 8; ++i) import.identity.bytes[i] = (uint8_t)(id >> (i * 8));
    import.resource_id = id;
    import.kind = XG_RENDER_RESOURCE_MODEL;
    import.owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE;
    import.owner_generation = owner_generation;
    import.state = XG_RENDER_RESOURCE_NATIVE_OWNED;
    import.bytes = bytes;
    import.byte_count = byte_count;
    import.content_digest = xg_render_resource_digest(bytes, byte_count);
    metadata.kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE;
    metadata.receipt = ++receipt;
    metadata.lifetime = XG_RENDER_RESOURCE_CAPABILITY_TIMELINE;
    metadata.owner_kind = import.owner_kind;
    metadata.owner_generation = import.owner_generation;
    metadata.source.source_class = XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE;
    metadata.source.identity = import.identity;
    metadata.source.range_size = import.byte_count;
    metadata.source.range_content_digest = import.content_digest;
    bool ok = false;
    if (xg_render_resource_capability_register(&metadata, &import.provenance) ==
        XG_RENDER_RESOURCE_CAPABILITY_OK) {
        XgRenderResourceHandle handle;
        if (xg_render_resource_import_native(&import, &handle) == XG_RENDER_RESOURCE_OK) {
            ok = xg_render_resource_acquire_snapshot(handle, import.content_digest) ==
                 XG_RENDER_RESOURCE_OK;
            (void)xg_render_resource_retire_current(handle);
            if (ok)
                *out = (XgSemanticResourceRef){handle.resource_id, handle.generation,
                                               import.content_digest};
        }
        (void)xg_render_resource_capability_retire(import.provenance);
    }
    return ok;
}

static bool view_blob(XgSemanticResourceRef ref, const void **bytes, size_t *byte_count) {
    XgRenderResourceView view;
    if (!ref.resource_id || !ref.generation ||
        xg_render_resource_view((XgRenderResourceHandle){ref.resource_id, ref.generation},
                                &view) != XG_RENDER_RESOURCE_OK ||
        !view.bytes || view.content_digest != ref.content_digest)
        return false;
    *bytes = view.bytes;
    *byte_count = view.byte_count;
    return true;
}

bool xg_render_native_mesh_geometry_create(uint64_t owner_generation,
                                           const XgRenderNativeMeshPoint *points,
                                           uint32_t point_count,
                                           const XgRenderNativeMeshFace *faces,
                                           uint32_t face_count, XgSemanticResourceRef *out) {
    if (!out || !points || !faces || !point_count || !face_count ||
        point_count > XG_RENDER_NATIVE_MESH_POINT_CAPACITY ||
        face_count > XG_RENDER_NATIVE_MESH_FACE_CAPACITY)
        return false;
    for (uint32_t i = 0; i < face_count; ++i)
        for (unsigned k = 0; k < 3u; ++k)
            if (faces[i].points[k] >= point_count) return false;
    const size_t bytes = sizeof(XgRenderNativeMeshGeometry) +
        (size_t)point_count * sizeof(*points) + (size_t)face_count * sizeof(*faces);
    XgRenderNativeMeshGeometry *geometry = malloc(bytes);
    if (!geometry) return false;
    *geometry = (XgRenderNativeMeshGeometry){.version = XG_RENDER_NATIVE_MESH_VERSION,
        .point_count = point_count, .face_count = face_count};
    memcpy(geometry + 1, points, (size_t)point_count * sizeof(*points));
    memcpy((uint8_t *)(geometry + 1) + (size_t)point_count * sizeof(*points), faces,
           (size_t)face_count * sizeof(*faces));
    const bool ok = import_blob(UINT64_C(0x584748534d474547) /* "XGHSMGEG" */,
                                owner_generation, geometry, bytes, out);
    free(geometry);
    return ok;
}

bool xg_render_native_mesh_instance_create(uint64_t owner_generation,
                                           const XgRenderNativeMeshInstance *header,
                                           const uint16_t *faces,
                                           const XgRenderNativeMeshExtra *extras,
                                           XgSemanticResourceRef *out) {
    if (!out || !header || (header->face_count && !faces) || (header->extra_count && !extras) ||
        !xg_render_native_mesh_instance_triangles(header) ||
        header->face_count > XG_RENDER_NATIVE_MESH_FACE_CAPACITY ||
        header->extra_count > XG_RENDER_NATIVE_MESH_FACE_CAPACITY ||
        !header->geometry.resource_id || !header->projection.projection_distance)
        return false;
    const size_t extras_offset = xg_render_native_mesh_extras_offset(header->face_count);
    const size_t bytes = extras_offset + (size_t)header->extra_count * sizeof(*extras);
    XgRenderNativeMeshInstance *instance = calloc(1u, bytes);
    if (!instance) return false;
    *instance = *header;
    instance->version = XG_RENDER_NATIVE_MESH_VERSION;
    if (header->face_count) memcpy(instance + 1, faces, (size_t)header->face_count * sizeof(*faces));
    if (header->extra_count)
        memcpy((uint8_t *)instance + extras_offset, extras, (size_t)header->extra_count * sizeof(*extras));
    const bool ok = import_blob(UINT64_C(0x584748534d494e53) /* "XGHSMINS" */,
                                owner_generation, instance, bytes, out);
    free(instance);
    return ok;
}

bool xg_render_native_mesh_geometry_view(XgSemanticResourceRef ref,
                                         const XgRenderNativeMeshGeometry **out) {
    const void *bytes;
    size_t byte_count;
    if (!out || !view_blob(ref, &bytes, &byte_count) ||
        byte_count < sizeof(XgRenderNativeMeshGeometry))
        return false;
    const XgRenderNativeMeshGeometry *geometry = bytes;
    if (geometry->version != XG_RENDER_NATIVE_MESH_VERSION || !geometry->point_count ||
        !geometry->face_count || geometry->point_count > XG_RENDER_NATIVE_MESH_POINT_CAPACITY ||
        geometry->face_count > XG_RENDER_NATIVE_MESH_FACE_CAPACITY ||
        byte_count != sizeof(*geometry) +
            (size_t)geometry->point_count * sizeof(XgRenderNativeMeshPoint) +
            (size_t)geometry->face_count * sizeof(XgRenderNativeMeshFace))
        return false;
    *out = geometry;
    return true;
}

bool xg_render_native_mesh_instance_view(XgSemanticResourceRef ref,
                                         const XgRenderNativeMeshInstance **out) {
    const void *bytes;
    size_t byte_count;
    if (!out || !view_blob(ref, &bytes, &byte_count) ||
        byte_count < sizeof(XgRenderNativeMeshInstance))
        return false;
    const XgRenderNativeMeshInstance *instance = bytes;
    if (instance->version != XG_RENDER_NATIVE_MESH_VERSION ||
        !xg_render_native_mesh_instance_triangles(instance) ||
        instance->face_count > XG_RENDER_NATIVE_MESH_FACE_CAPACITY ||
        instance->extra_count > XG_RENDER_NATIVE_MESH_FACE_CAPACITY ||
        byte_count != xg_render_native_mesh_extras_offset(instance->face_count) +
            (size_t)instance->extra_count * sizeof(XgRenderNativeMeshExtra))
        return false;
    *out = instance;
    return true;
}

void xg_render_native_mesh_vertex(XgRenderIrVertex *out,
                                  const XgHost3dProjectedVertex *screen,
                                  uint8_t u, uint8_t v, uint32_t color) {
    /* xg_render_projected_vertex_semantic's mapping, as materialized. */
    *out = (XgRenderIrVertex){
        .x = (int32_t)screen->x * 65536, .y = (int32_t)screen->y * 65536,
        .u = (int32_t)u << 16, .v = (int32_t)v << 16,
        .r = (uint8_t)color, .g = (uint8_t)(color >> 8), .b = (uint8_t)(color >> 16),
        .native_view_x = screen->native_view_x_16_16,
        .native_view_y = screen->native_view_y_16_16,
        /* Even behind the camera: the continuous Native projection is the
         * one phases move from, and the clip replaces such corners. */
        .native_view_position = screen->native_view_position != 0u,
        .native_view_depth = screen->native_view_depth_q12,
        .projective_view_x = screen->projective_view_x,
        .projective_view_y = screen->projective_view_y,
        .projective_view_z = screen->projective_view_z,
        .projective_offset_x = screen->projective_offset_x_16_16,
        .projective_offset_y = screen->projective_offset_y_16_16,
        .projective_native_offset_x = screen->projective_native_offset_x_16_16,
        .projective_native_offset_y = screen->projective_native_offset_y_16_16,
        .projective_distance = screen->projective_distance,
        .projective_position = screen->projective_position != 0u,
    };
}

static uint8_t depth_cue_channel(uint8_t color, int32_t far_color, int16_t ir0) {
    int32_t step = far_color - (int32_t)color * 16;
    step = step < -32768 ? -32768 : step > 32767 ? 32767 : step;
    const int64_t value = (int64_t)color * 16 * 4096 + (int64_t)ir0 * step;
    const int32_t mac = value >= 0 ? (int32_t)(value / 4096)
                                   : (int32_t)(-(((-value) + 4095) / 4096));
    const int32_t c = mac >= 0 ? mac / 16 : -(((-mac) + 15) / 16);
    return (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
}

uint32_t xg_render_native_mesh_depth_cue(const XgHost3dProjection *projection,
                                         uint32_t rgbc, const int32_t far_color[3],
                                         uint16_t sz) {
    int overflow = 0;
    const int32_t scale = psx_gte_divide(projection->projection_distance, sz, &overflow);
    const int64_t mac0 = (int64_t)projection->depth_cue_a * scale + projection->depth_cue_b;
    const int64_t shifted = mac0 >= 0 ? mac0 >> 12 : -((-mac0 + 4095) >> 12);
    const int32_t wrapped = (int32_t)(uint32_t)(uint64_t)shifted;
    const int16_t ir0 = (int16_t)(wrapped < 0 ? 0 : wrapped > 0x1000 ? 0x1000 : wrapped);
    return (rgbc & 0xff000000u) |
        depth_cue_channel((uint8_t)rgbc, far_color[0], ir0) |
        (uint32_t)depth_cue_channel((uint8_t)(rgbc >> 8), far_color[1], ir0) << 8 |
        (uint32_t)depth_cue_channel((uint8_t)(rgbc >> 16), far_color[2], ir0) << 16;
}

bool xg_render_native_mesh_expand(const XgRenderNativeMeshInstance *instance,
                                  const XgRenderNativeMeshGeometry *geometry,
                                  XgRenderNativeMeshTriangle *out) {
    /* Per-thread projected-point cache, stamped per expansion. */
    static _Thread_local XgHost3dProjectedVertex *projected;
    static _Thread_local uint32_t *stamps, capacity, stamp;
    if (!instance || !geometry || !out) return false;
    if (geometry->point_count > capacity) {
        XgHost3dProjectedVertex *grown_projected =
            realloc(projected, (size_t)geometry->point_count * sizeof(*projected));
        if (!grown_projected) return false;
        projected = grown_projected;
        uint32_t *grown_stamps = realloc(stamps, (size_t)geometry->point_count * sizeof(*stamps));
        if (!grown_stamps) return false;
        memset(grown_stamps + capacity, 0, (size_t)(geometry->point_count - capacity) * sizeof(*stamps));
        stamps = grown_stamps;
        capacity = geometry->point_count;
    }
    if (++stamp == 0u) {
        memset(stamps, 0, (size_t)capacity * sizeof(*stamps));
        stamp = 1u;
    }
    const XgRenderNativeMeshPoint *points = xg_render_native_mesh_points(geometry);
    const XgRenderNativeMeshFace *faces = xg_render_native_mesh_faces(geometry);
    const uint16_t *visible = xg_render_native_mesh_instance_faces(instance);
    for (uint32_t i = 0; i < instance->face_count; ++i) {
        if (visible[i] >= geometry->face_count) return false;
        const XgRenderNativeMeshFace *face = &faces[visible[i]];
        XgRenderNativeMeshTriangle *triangle = &out[i];
        for (unsigned k = 0; k < 3u; ++k) {
            const uint32_t p = face->points[k];
            if (stamps[p] != stamp) {
                const int32_t x = points[p].local.x + instance->local_offset[0];
                const int32_t y = points[p].local.y + instance->local_offset[1];
                const int32_t z = points[p].local.z + instance->local_offset[2];
                if (x < -32768 || x > 32767 || y < -32768 || y > 32767 ||
                    z < -32768 || z > 32767) return false;
                const XgHost3dVector local = {(int16_t)x, (int16_t)y, (int16_t)z, 0};
                uint32_t flags;
                if (!xg_host_3d_rtps(&instance->projection, &local, &projected[p], &flags))
                    return false;
                stamps[p] = stamp;
            }
        }
        const uint32_t color = xg_render_native_mesh_depth_cue(
            &instance->projection, instance->rgbc, instance->far_color,
            projected[face->points[2]].z);
        triangle->triangle.split_index = 0u;
        triangle->triangle.split_count = 1u;
        triangle->flags = 0u;
        for (unsigned k = 0; k < 3u; ++k) {
            const XgRenderNativeMeshPoint *point = &points[face->points[k]];
            xg_render_native_mesh_vertex(&triangle->triangle.vertices[k],
                                         &projected[face->points[k]], face->u[k], face->v[k],
                                         color);
            triangle->local[k] = point->local;
            triangle->vertex_ids[k] = point->vertex_id;
        }
    }
    const XgRenderNativeMeshExtra *extras = xg_render_native_mesh_instance_extras(instance);
    for (uint32_t i = 0; i < instance->extra_count; ++i) {
        const XgRenderNativeMeshExtra *extra = &extras[i];
        XgRenderNativeMeshTriangle *triangle = &out[instance->face_count + i];
        const uint32_t color = xg_render_native_mesh_depth_cue(
            &instance->projection, instance->rgbc, instance->far_color, extra->sz);
        triangle->triangle.split_index = 0u;
        triangle->triangle.split_count = 1u;
        triangle->flags = extra->flags & XG_RENDER_NATIVE_MESH_PHASE_ONLY;
        for (unsigned k = 0; k < 3u; ++k) {
            const int32_t x = extra->local[k].x + instance->local_offset[0];
            const int32_t y = extra->local[k].y + instance->local_offset[1];
            const int32_t z = extra->local[k].z + instance->local_offset[2];
            if (x < -32768 || x > 32767 || y < -32768 || y > 32767 ||
                z < -32768 || z > 32767) return false;
            const XgHost3dVector local = {(int16_t)x, (int16_t)y, (int16_t)z, 0};
            XgHost3dProjectedVertex screen;
            uint32_t flags;
            if (!xg_host_3d_rtps(&instance->projection, &local, &screen, &flags)) return false;
            xg_render_native_mesh_vertex(&triangle->triangle.vertices[k], &screen,
                                         extra->u[k], extra->v[k], color);
            triangle->local[k] = extra->local[k];
            triangle->vertex_ids[k] = extra->vertex_ids[k];
        }
    }
    return true;
}
