#include "xg_render_source_frame.h"

#include "xg_render_resource_repository.h"
#include "xg_render_semantic_presentation.h"
#include "xg_render_source_commit.h"
#include "xg_render_surface_graph.h"

#include <stdatomic.h>
#include <string.h>

typedef struct XgRenderPendingSourceFrame {
    XgRenderSourceFrameDescription description;
    XgSemanticPassRecord passes[XG_RENDER_SCENE_PASS_CAPACITY];
    XgSemanticDrawRecord draws[XG_RENDER_SCENE_DRAW_CAPACITY];
    XgSemanticResourceRef resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY];
    XgSemanticUiNodeRecord ui_nodes[XG_RENDER_SCENE_UI_NODE_CAPACITY];
    XgSemanticUiGlyphRunRecord
        ui_glyph_runs[XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY];
    XgSemanticUiGlyphPlacementRecord
        ui_glyph_placements[XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY];
    uint32_t pass_count;
    uint32_t draw_count;
    uint32_t resource_count;
    uint32_t edge_count;
    uint32_t ui_node_count;
    uint32_t ui_glyph_run_count;
    uint32_t ui_glyph_placement_count;
    bool active;
    bool complete;
    bool blocked;
} XgRenderPendingSourceFrame;

static XgRenderPendingSourceFrame g_frame;
static atomic_flag g_frame_lock = ATOMIC_FLAG_INIT;
static uint64_t g_last_published_vblank;
static uint64_t g_presentation_epoch;
static uint64_t g_reset_generation;
static uint32_t g_last_scene_generation;
static bool g_force_discontinuity = true;
static XgRenderSourceFrameHostCallbacks g_host_callbacks;
static bool g_host_callbacks_configured;

static void frame_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_frame_lock,
                                              memory_order_acquire)) {
    }
}

static void frame_unlock(void) {
    atomic_flag_clear_explicit(&g_frame_lock, memory_order_release);
}

static void frame_release_resources(void) {
    uint32_t index;

    for (index = 0u; index < g_frame.resource_count; ++index)
        (void)xg_render_resource_release((XgRenderResourceHandle){
            g_frame.resources[index].resource_id,
            g_frame.resources[index].generation,
        });
}

static void frame_clear(void) {
    frame_release_resources();
    memset(&g_frame, 0, sizeof(g_frame));
}

static void frame_reconcile_timeline(void) {
    XgRenderPresentationDiagnostics diagnostics;

    xg_render_semantic_presentation_diagnostics(&diagnostics);
    if (g_presentation_epoch != 0u &&
        g_presentation_epoch != diagnostics.presentation_epoch) {
        ++g_reset_generation;
        frame_clear();
        g_last_published_vblank = 0u;
        g_last_scene_generation = 0u;
        g_force_discontinuity = true;
    }
    g_presentation_epoch = diagnostics.presentation_epoch;
}

static bool order_less(const XgSemanticOrderKey *left,
                       const XgSemanticOrderKey *right) {
    if (left->pass_id != right->pass_id) return left->pass_id < right->pass_id;
    if (left->layer != right->layer) return left->layer < right->layer;
    if (left->authored_depth != right->authored_depth)
        return left->authored_depth < right->authored_depth;
    if (left->insertion_ordinal != right->insertion_ordinal)
        return left->insertion_ordinal < right->insertion_ordinal;
    return left->split_ordinal < right->split_ordinal;
}

static bool frame_description_equal(
        const XgRenderSourceFrameDescription *left,
        const XgRenderSourceFrameDescription *right) {
    return left->scene.disc_id == right->scene.disc_id &&
        left->scene.executable_identity == right->scene.executable_identity &&
        left->scene.primary_overlay_identity ==
            right->scene.primary_overlay_identity &&
        left->scene.companion_set_identity ==
            right->scene.companion_set_identity &&
        left->scene.authored_scene_id == right->scene.authored_scene_id &&
        left->scene.authored_submode == right->scene.authored_submode &&
        left->scene.module == right->scene.module &&
        left->display.width == right->display.width &&
        left->display.height == right->display.height &&
        left->display.display_x == right->display.display_x &&
        left->display.display_y == right->display.display_y &&
        left->display.aspect_num == right->display.aspect_num &&
        left->display.aspect_den == right->display.aspect_den &&
        left->display.depth24 == right->display.depth24 &&
        left->display.interlaced == right->display.interlaced &&
        left->display.disabled == right->display.disabled &&
        left->display.native_width == right->display.native_width &&
        left->display.native_height == right->display.native_height &&
        left->display.native_offset_x == right->display.native_offset_x &&
        left->display.temporal_hz == right->display.temporal_hz &&
        (left->display.render_scale ? left->display.render_scale : 1u) ==
            (right->display.render_scale ? right->display.render_scale : 1u) &&
        left->scene_generation == right->scene_generation &&
        left->source_interval_vblanks == right->source_interval_vblanks &&
        left->discontinuity == right->discontinuity &&
        left->temporally_eligible == right->temporally_eligible;
}

static bool resource_ref_equal(const XgSemanticResourceRef *left,
                               const XgSemanticResourceRef *right) {
    return left->resource_id == right->resource_id &&
        left->generation == right->generation &&
        left->content_digest == right->content_digest;
}

static bool resource_handle_equal(const XgSemanticResourceRef *left,
                                  const XgSemanticResourceRef *right) {
    return left->resource_id == right->resource_id &&
        left->generation == right->generation;
}

static bool surface_edge_equal(const XgSemanticSurfaceEdge *left,
                                const XgSemanticSurfaceEdge *right) {
    return left->source_surface_id == right->source_surface_id &&
        left->source_generation == right->source_generation &&
        left->target_surface_id == right->target_surface_id &&
        left->target_generation == right->target_generation &&
        left->kind == right->kind &&
        left->order.pass_id == right->order.pass_id &&
        left->order.layer == right->order.layer &&
        left->order.authored_depth == right->order.authored_depth &&
        left->order.insertion_ordinal == right->order.insertion_ordinal &&
        left->order.split_ordinal == right->order.split_ordinal &&
        left->source.x == right->source.x &&
        left->source.y == right->source.y &&
        left->source.width == right->source.width &&
        left->source.height == right->source.height &&
        left->destination.x == right->destination.x &&
        left->destination.y == right->destination.y &&
        left->destination.width == right->destination.width &&
        left->destination.height == right->destination.height &&
        left->sample.sampler == right->sample.sampler &&
        left->sample.wrap_u == right->sample.wrap_u &&
        left->sample.wrap_v == right->sample.wrap_v &&
        left->sample.clut_resource_id == right->sample.clut_resource_id &&
        left->sample.clut_generation == right->sample.clut_generation &&
        left->sample.clut_x == right->sample.clut_x &&
        left->sample.clut_y == right->sample.clut_y &&
        left->sample.texture_window_mask_x ==
            right->sample.texture_window_mask_x &&
        left->sample.texture_window_mask_y ==
            right->sample.texture_window_mask_y &&
        left->sample.texture_window_offset_x ==
            right->sample.texture_window_offset_x &&
        left->sample.texture_window_offset_y ==
            right->sample.texture_window_offset_y &&
        left->sample.texture_depth == right->sample.texture_depth &&
        left->sample.blend_mode == right->sample.blend_mode &&
        left->sample.palette_enabled == right->sample.palette_enabled &&
        left->sample.semi_transparent == right->sample.semi_transparent &&
        left->sample.mask_set == right->sample.mask_set &&
        left->sample.mask_check == right->sample.mask_check &&
        left->effect_phase == right->effect_phase &&
        left->effect_phase_count == right->effect_phase_count;
}

static bool resource_owner_compatible(
        const XgRenderResourceView *view, uint32_t scene_generation) {
    if (view->owner_kind == XG_RENDER_RESOURCE_OWNER_BATCH)
        return false;
    if (view->owner_kind == XG_RENDER_RESOURCE_OWNER_SCENE)
        return view->owner_generation == scene_generation;
    if (view->owner_kind == XG_RENDER_RESOURCE_OWNER_SOURCE)
        return view->owner_generation == scene_generation ||
            (view->kind == XG_RENDER_RESOURCE_MOVIE_FRAME &&
             view->retain_count != 0u);
    return view->owner_kind >= XG_RENDER_RESOURCE_OWNER_STATIC &&
        view->owner_kind <= XG_RENDER_RESOURCE_OWNER_MODULE;
}

static XgRenderSourceFrameResult frame_result_from_commit(
        XgRenderSourceCommitResult result) {
    if (result == XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED)
        return XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED;
    if (result == XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT)
        return XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
    if (result == XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION)
        return XG_RENDER_SOURCE_FRAME_INVALID_TRANSITION;
    return XG_RENDER_SOURCE_FRAME_REJECTED;
}

static void sort_draws(void) {
    uint32_t index;

    for (index = 1u; index < g_frame.draw_count; ++index) {
        XgSemanticDrawRecord draw = g_frame.draws[index];
        uint32_t destination = index;
        while (destination != 0u &&
               order_less(&draw.order,
                          &g_frame.draws[destination - 1u].order)) {
            g_frame.draws[destination] = g_frame.draws[destination - 1u];
            --destination;
        }
        g_frame.draws[destination] = draw;
    }
}

static void sort_ui_records(void) {
    uint32_t index;

    for (index = 1u; index < g_frame.ui_node_count; ++index) {
        XgSemanticUiNodeRecord node = g_frame.ui_nodes[index];
        uint32_t destination = index;
        while (destination != 0u &&
               order_less(&node.order,
                          &g_frame.ui_nodes[destination - 1u].order)) {
            g_frame.ui_nodes[destination] =
                g_frame.ui_nodes[destination - 1u];
            --destination;
        }
        g_frame.ui_nodes[destination] = node;
    }
    for (index = 1u; index < g_frame.ui_glyph_run_count; ++index) {
        XgSemanticUiGlyphRunRecord run = g_frame.ui_glyph_runs[index];
        uint32_t destination = index;
        while (destination != 0u &&
               order_less(&run.order,
                          &g_frame.ui_glyph_runs[destination - 1u].order)) {
            g_frame.ui_glyph_runs[destination] =
                g_frame.ui_glyph_runs[destination - 1u];
            --destination;
        }
        g_frame.ui_glyph_runs[destination] = run;
    }
}

static XgRenderSourceFrameResult frame_attach_surface_graph(
        uint32_t scene_generation) {
    XgRenderSurfacePublication
        publications[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY];
    size_t publication_count = 0u;
    size_t edge_count = 0u;
    XgSemanticResourceRef acquired[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    bool append_publication[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY] = {false};
    bool append_edge[XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY] = {false};
    uint32_t acquired_count = 0u;
    uint32_t added_publications = 0u;
    uint32_t added_edges = 0u;

    if (xg_render_surface_graph_copy_publications(
            publications, XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY,
            &publication_count) != XG_RENDER_SURFACE_GRAPH_OK ||
        xg_render_surface_graph_copy_edges(
            edges, XG_RENDER_SURFACE_GRAPH_EDGE_CAPACITY,
            &edge_count) != XG_RENDER_SURFACE_GRAPH_OK)
        return XG_RENDER_SOURCE_FRAME_REJECTED;
    for (size_t index = 0u; index < publication_count; ++index) {
        const XgRenderSurfacePublication *publication = &publications[index];
        const XgSemanticResourceRef resource = {
            .resource_id = publication->handle.resource_id,
            .generation = publication->handle.generation,
            .content_digest = publication->content_digest,
        };
        bool duplicate = false;

        if (publication->owner_generation != scene_generation ||
            resource.resource_id == 0u || resource.generation == 0u ||
            resource.content_digest == 0u)
            return XG_RENDER_SOURCE_FRAME_REJECTED;
        for (uint32_t prior = 0u; prior < g_frame.resource_count; ++prior) {
            const XgSemanticResourceRef *existing = &g_frame.resources[prior];
            if (!resource_handle_equal(existing, &resource)) continue;
            if (!resource_ref_equal(existing, &resource))
                return XG_RENDER_SOURCE_FRAME_REJECTED;
            duplicate = true;
            break;
        }
        for (size_t prior = 0u; !duplicate && prior < index; ++prior) {
            const XgSemanticResourceRef existing = {
                .resource_id = publications[prior].handle.resource_id,
                .generation = publications[prior].handle.generation,
                .content_digest = publications[prior].content_digest,
            };
            if (!resource_handle_equal(&existing, &resource)) continue;
            if (!resource_ref_equal(&existing, &resource))
                return XG_RENDER_SOURCE_FRAME_REJECTED;
            duplicate = true;
        }
        if (duplicate) continue;
        append_publication[index] = true;
        added_publications++;
    }
    for (size_t index = 0u; index < edge_count; ++index) {
        bool duplicate = false;
        bool targets_pass = false;

        for (uint32_t pass = 0u; pass < g_frame.pass_count; ++pass)
            if (edges[index].target_surface_id ==
                    g_frame.passes[pass].target_surface_id &&
                edges[index].target_generation ==
                    g_frame.passes[pass].target_generation) {
                targets_pass = true;
                break;
            }
        if (!targets_pass) continue;
        for (uint32_t prior = 0u; prior < g_frame.edge_count; ++prior)
            if (surface_edge_equal(&g_frame.edges[prior], &edges[index])) {
                duplicate = true;
                break;
            }
        for (size_t prior = 0u; !duplicate && prior < index; ++prior)
            if (append_edge[prior] &&
                surface_edge_equal(&edges[prior], &edges[index])) {
                duplicate = true;
                break;
            }
        if (duplicate) continue;
        append_edge[index] = true;
        added_edges++;
    }
    if (added_publications >
            XG_RENDER_SCENE_RESOURCE_CAPACITY - g_frame.resource_count ||
        added_edges >
            XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY - g_frame.edge_count)
        return XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED;

    for (size_t index = 0u; index < publication_count; ++index) {
        const XgRenderSurfacePublication *publication = &publications[index];
        XgSemanticResourceRef resource;
        if (!append_publication[index]) continue;
        resource = (XgSemanticResourceRef){
            .resource_id = publication->handle.resource_id,
            .generation = publication->handle.generation,
            .content_digest = publication->content_digest,
        };
        if (xg_render_resource_acquire_current(
                publication->handle, publication->content_digest) !=
            XG_RENDER_RESOURCE_OK) {
            while (acquired_count != 0u) {
                const XgSemanticResourceRef *rollback =
                    &acquired[--acquired_count];
                (void)xg_render_resource_release((XgRenderResourceHandle){
                    rollback->resource_id, rollback->generation});
            }
            return XG_RENDER_SOURCE_FRAME_REJECTED;
        }
        acquired[acquired_count++] = resource;
    }
    for (size_t index = 0u; index < publication_count; ++index)
        if (append_publication[index])
            g_frame.resources[g_frame.resource_count++] =
                (XgSemanticResourceRef){
                    .resource_id = publications[index].handle.resource_id,
                    .generation = publications[index].handle.generation,
                    .content_digest = publications[index].content_digest,
                };
    for (size_t index = 0u; index < edge_count; ++index)
        if (append_edge[index]) g_frame.edges[g_frame.edge_count++] = edges[index];
    return XG_RENDER_SOURCE_FRAME_OK;
}

static XgRenderSourceFrameResult frame_append(void *destination,
                                               uint32_t *count,
                                               uint32_t capacity,
                                               size_t element_size,
                                               const void *record) {
    if (!g_frame.active || g_frame.complete)
        return XG_RENDER_SOURCE_FRAME_INVALID_TRANSITION;
    if (record == NULL) {
        g_frame.blocked = true;
        return XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
    }
    if (*count >= capacity) {
        g_frame.blocked = true;
        return XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED;
    }
    memcpy((unsigned char *)destination + (size_t)*count * element_size,
           record, element_size);
    ++*count;
    return XG_RENDER_SOURCE_FRAME_OK;
}

void xg_render_source_frame_reset(void) {
    frame_lock();
    ++g_reset_generation;
    frame_clear();
    frame_reconcile_timeline();
    g_last_published_vblank = 0u;
    g_last_scene_generation = 0u;
    g_force_discontinuity = true;
    frame_unlock();
}

void xg_render_source_frame_reject(void) {
    frame_lock();
    frame_reconcile_timeline();
    if (g_frame.active) g_frame.blocked = true;
    frame_unlock();
}

bool xg_render_source_frame_configure_host_callbacks(
        const XgRenderSourceFrameHostCallbacks *callbacks) {
    if (callbacks == NULL || callbacks->sealed_capture == NULL ||
        callbacks->published_notify == NULL)
        return false;

    frame_lock();
    g_host_callbacks = *callbacks;
    g_host_callbacks_configured = true;
    frame_unlock();
    return true;
}

void xg_render_source_frame_clear_host_callbacks(void) {
    frame_lock();
    memset(&g_host_callbacks, 0, sizeof(g_host_callbacks));
    g_host_callbacks_configured = false;
    frame_unlock();
}

XgRenderSourceFrameResult xg_render_source_frame_begin(
        const XgRenderSourceFrameDescription *description) {
    XgRenderSourceFrameResult result = XG_RENDER_SOURCE_FRAME_OK;

    frame_lock();
    frame_reconcile_timeline();
    if (description == NULL || description->scene_generation == 0u) {
        result = XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
    } else if (g_frame.active) {
        result = XG_RENDER_SOURCE_FRAME_INVALID_TRANSITION;
    } else {
        frame_clear();
        g_frame.description = *description;
        g_frame.active = true;
    }
    frame_unlock();
    return result;
}

XgRenderSourceFrameResult xg_render_source_frame_append_fragment(
        const XgRenderSourceFrameDescription *description,
        const XgRenderSourceFrameFragment *fragment) {
    bool append_resource[XG_RENDER_SCENE_RESOURCE_CAPACITY] = {false};
    XgSemanticResourceRef acquired[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgRenderSourceFrameResult result = XG_RENDER_SOURCE_FRAME_OK;
    uint32_t acquired_count = 0u;
    uint32_t added_resource_count = 0u;

    if (description == NULL || fragment == NULL ||
        description->scene_generation == 0u)
        return XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
    if (fragment->pass_count > XG_RENDER_SCENE_PASS_CAPACITY ||
        fragment->draw_count > XG_RENDER_SCENE_DRAW_CAPACITY ||
        fragment->resource_count > XG_RENDER_SCENE_RESOURCE_CAPACITY ||
        fragment->surface_edge_count > XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY ||
        fragment->ui_node_count > XG_RENDER_SCENE_UI_NODE_CAPACITY ||
        fragment->ui_glyph_run_count >
            XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY ||
        fragment->ui_glyph_placement_count >
            XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY)
        return XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED;
    if ((fragment->pass_count != 0u && fragment->passes == NULL) ||
        (fragment->draw_count != 0u && fragment->draws == NULL) ||
        (fragment->resource_count != 0u && fragment->resources == NULL) ||
        (fragment->surface_edge_count != 0u &&
         fragment->surface_edges == NULL) ||
        (fragment->ui_node_count != 0u && fragment->ui_nodes == NULL) ||
        (fragment->ui_glyph_run_count != 0u &&
         fragment->ui_glyph_runs == NULL) ||
        (fragment->ui_glyph_placement_count != 0u &&
         fragment->ui_glyph_placements == NULL))
        return XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;

    frame_lock();
    frame_reconcile_timeline();
    if (g_frame.active && g_frame.complete) {
        result = XG_RENDER_SOURCE_FRAME_INVALID_TRANSITION;
        goto finished;
    }
    if (g_frame.active &&
        !frame_description_equal(&g_frame.description, description)) {
        result = XG_RENDER_SOURCE_FRAME_REJECTED;
        goto finished;
    }
    if (fragment->pass_count >
            XG_RENDER_SCENE_PASS_CAPACITY - g_frame.pass_count ||
        fragment->draw_count >
            XG_RENDER_SCENE_DRAW_CAPACITY - g_frame.draw_count ||
        fragment->surface_edge_count >
            XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY - g_frame.edge_count ||
        fragment->ui_node_count >
            XG_RENDER_SCENE_UI_NODE_CAPACITY - g_frame.ui_node_count ||
        fragment->ui_glyph_run_count >
            XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY -
                g_frame.ui_glyph_run_count ||
        fragment->ui_glyph_placement_count >
            XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY -
                g_frame.ui_glyph_placement_count) {
        result = XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED;
        goto finished;
    }

    for (uint32_t index = 0u; index < fragment->resource_count; ++index) {
        const XgSemanticResourceRef *resource = &fragment->resources[index];
        XgRenderResourceView view;
        bool duplicate = false;

        if (resource->resource_id == 0u || resource->generation == 0u ||
            resource->content_digest == 0u) {
            result = XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
            goto finished;
        }
        for (uint32_t prior = 0u; prior < g_frame.resource_count; ++prior) {
            const XgSemanticResourceRef *existing = &g_frame.resources[prior];
            if (!resource_handle_equal(existing, resource)) continue;
            if (!resource_ref_equal(existing, resource)) {
                result = XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
                goto finished;
            }
            duplicate = true;
            break;
        }
        for (uint32_t prior = 0u; !duplicate && prior < index; ++prior) {
            const XgSemanticResourceRef *existing =
                &fragment->resources[prior];
            if (!resource_handle_equal(existing, resource)) continue;
            if (!resource_ref_equal(existing, resource)) {
                result = XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
                goto finished;
            }
            duplicate = true;
        }
        if (duplicate) continue;
        if (xg_render_resource_view((XgRenderResourceHandle){
                resource->resource_id, resource->generation}, &view) !=
                XG_RENDER_RESOURCE_OK ||
            (!view.current && view.retain_count == 0u) ||
            view.content_digest != resource->content_digest ||
            !resource_owner_compatible(&view,
                                       description->scene_generation)) {
            result = XG_RENDER_SOURCE_FRAME_REJECTED;
            goto finished;
        }
        append_resource[index] = true;
        added_resource_count++;
    }
    if (added_resource_count >
            XG_RENDER_SCENE_RESOURCE_CAPACITY - g_frame.resource_count) {
        result = XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED;
        goto finished;
    }
    for (uint32_t index = 0u; index < fragment->resource_count; ++index) {
        const XgSemanticResourceRef *resource = &fragment->resources[index];
        if (!append_resource[index]) continue;
        if (xg_render_resource_acquire_snapshot((XgRenderResourceHandle){
                resource->resource_id, resource->generation},
                resource->content_digest) != XG_RENDER_RESOURCE_OK) {
            result = XG_RENDER_SOURCE_FRAME_REJECTED;
            goto rollback;
        }
        acquired[acquired_count++] = *resource;
    }

    if (!g_frame.active) {
        g_frame.description = *description;
        g_frame.active = true;
        g_frame.complete = false;
        g_frame.blocked = false;
    }
    if (fragment->pass_count != 0u) {
        memcpy(g_frame.passes + g_frame.pass_count, fragment->passes,
               fragment->pass_count * sizeof(g_frame.passes[0]));
        g_frame.pass_count += fragment->pass_count;
    }
    if (fragment->draw_count != 0u) {
        memcpy(g_frame.draws + g_frame.draw_count, fragment->draws,
               fragment->draw_count * sizeof(g_frame.draws[0]));
        g_frame.draw_count += fragment->draw_count;
    }
    for (uint32_t index = 0u; index < fragment->resource_count; ++index)
        if (append_resource[index])
            g_frame.resources[g_frame.resource_count++] =
                fragment->resources[index];
    if (fragment->surface_edge_count != 0u) {
        memcpy(g_frame.edges + g_frame.edge_count, fragment->surface_edges,
               fragment->surface_edge_count * sizeof(g_frame.edges[0]));
        g_frame.edge_count += fragment->surface_edge_count;
    }
    if (fragment->ui_node_count != 0u) {
        memcpy(g_frame.ui_nodes + g_frame.ui_node_count, fragment->ui_nodes,
               fragment->ui_node_count * sizeof(g_frame.ui_nodes[0]));
        g_frame.ui_node_count += fragment->ui_node_count;
    }
    if (fragment->ui_glyph_run_count != 0u) {
        memcpy(g_frame.ui_glyph_runs + g_frame.ui_glyph_run_count,
               fragment->ui_glyph_runs,
               fragment->ui_glyph_run_count *
                   sizeof(g_frame.ui_glyph_runs[0]));
        g_frame.ui_glyph_run_count += fragment->ui_glyph_run_count;
    }
    if (fragment->ui_glyph_placement_count != 0u) {
        memcpy(g_frame.ui_glyph_placements +
                   g_frame.ui_glyph_placement_count,
               fragment->ui_glyph_placements,
               fragment->ui_glyph_placement_count *
                   sizeof(g_frame.ui_glyph_placements[0]));
        g_frame.ui_glyph_placement_count +=
            fragment->ui_glyph_placement_count;
    }
    goto finished;

rollback:
    while (acquired_count != 0u) {
        const XgSemanticResourceRef *resource = &acquired[--acquired_count];
        (void)xg_render_resource_release((XgRenderResourceHandle){
            resource->resource_id, resource->generation});
    }
finished:
    frame_unlock();
    return result;
}

#define DEFINE_APPEND(name, type, field, count_field, capacity)                 \
XgRenderSourceFrameResult name(const type *record) {                            \
    XgRenderSourceFrameResult result;                                           \
    frame_lock();                                                               \
    frame_reconcile_timeline();                                                  \
    result = frame_append(g_frame.field, &g_frame.count_field, capacity,         \
                          sizeof(g_frame.field[0]), record);                     \
    frame_unlock();                                                             \
    return result;                                                              \
}

DEFINE_APPEND(xg_render_source_frame_append_pass, XgSemanticPassRecord,
              passes, pass_count, XG_RENDER_SCENE_PASS_CAPACITY)
DEFINE_APPEND(xg_render_source_frame_append_draw, XgSemanticDrawRecord,
              draws, draw_count, XG_RENDER_SCENE_DRAW_CAPACITY)
DEFINE_APPEND(xg_render_source_frame_append_surface_edge,
               XgSemanticSurfaceEdge, edges, edge_count,
               XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY)
DEFINE_APPEND(xg_render_source_frame_append_ui_node,
              XgSemanticUiNodeRecord, ui_nodes, ui_node_count,
              XG_RENDER_SCENE_UI_NODE_CAPACITY)
DEFINE_APPEND(xg_render_source_frame_append_ui_glyph_run,
              XgSemanticUiGlyphRunRecord, ui_glyph_runs, ui_glyph_run_count,
              XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY)
DEFINE_APPEND(xg_render_source_frame_append_ui_glyph_placement,
              XgSemanticUiGlyphPlacementRecord, ui_glyph_placements,
              ui_glyph_placement_count,
              XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY)

#undef DEFINE_APPEND

XgRenderSourceFrameResult xg_render_source_frame_append_resource(
        const XgSemanticResourceRef *resource) {
    XgRenderSourceFrameResult result = XG_RENDER_SOURCE_FRAME_OK;
    uint32_t index;

    frame_lock();
    frame_reconcile_timeline();
    if (!g_frame.active || g_frame.complete) {
        result = XG_RENDER_SOURCE_FRAME_INVALID_TRANSITION;
        goto finished;
    }
    if (resource == NULL || resource->resource_id == 0u ||
        resource->generation == 0u || resource->content_digest == 0u) {
        g_frame.blocked = true;
        result = XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
        goto finished;
    }
    for (index = 0u; index < g_frame.resource_count; ++index) {
        const XgSemanticResourceRef *existing = &g_frame.resources[index];

        if (existing->resource_id != resource->resource_id ||
            existing->generation != resource->generation)
            continue;
        if (existing->content_digest != resource->content_digest) {
            g_frame.blocked = true;
            result = XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT;
        }
        goto finished;
    }
    if (g_frame.resource_count >= XG_RENDER_SCENE_RESOURCE_CAPACITY) {
        g_frame.blocked = true;
        result = XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED;
        goto finished;
    }
    if (xg_render_resource_acquire_snapshot(
            (XgRenderResourceHandle){
                resource->resource_id,
                resource->generation,
            }, resource->content_digest) != XG_RENDER_RESOURCE_OK) {
        g_frame.blocked = true;
        result = XG_RENDER_SOURCE_FRAME_REJECTED;
        goto finished;
    }
    g_frame.resources[g_frame.resource_count++] = *resource;

finished:
    frame_unlock();
    return result;
}

XgRenderSourceFrameResult xg_render_source_frame_complete(void) {
    XgRenderSourceFrameResult result = XG_RENDER_SOURCE_FRAME_OK;

    frame_lock();
    frame_reconcile_timeline();
    if (!g_frame.active || g_frame.complete) {
        result = XG_RENDER_SOURCE_FRAME_INVALID_TRANSITION;
    } else if (g_frame.blocked || g_frame.pass_count == 0u) {
        g_frame.blocked = true;
        result = XG_RENDER_SOURCE_FRAME_INCOMPLETE;
    } else {
        sort_draws();
        sort_ui_records();
        g_frame.complete = true;
    }
    frame_unlock();
    return result;
}

XgRenderSourceFrameResult xg_render_source_frame_publish_boundary(void) {
    XgPresentationIdentity identity;
    XgRenderSourceBuilder builder = {0};
    XgRenderSourceCommitHandle commit = {0};
    XgRenderSourceCommitHeader copied_header;
    XgRenderSourceFrameHostCallbacks host_callbacks = {0};
    XgRenderSourceFrameResult result = XG_RENDER_SOURCE_FRAME_REJECTED;
    uint64_t reset_generation;
    uint32_t source_interval_vblanks;
    bool discontinuity;
    bool host_callbacks_configured = false;
    uint32_t index;
    XgRenderSourceCommitResult commit_result;

    frame_lock();
    frame_reconcile_timeline();
    if (!g_frame.active) {
        frame_unlock();
        return XG_RENDER_SOURCE_FRAME_EMPTY;
    }
    if (!g_frame.complete || g_frame.blocked) {
        frame_clear();
        frame_unlock();
        return XG_RENDER_SOURCE_FRAME_INCOMPLETE;
    }
    result = frame_attach_surface_graph(g_frame.description.scene_generation);
    if (result != XG_RENDER_SOURCE_FRAME_OK) goto finished;
    result = XG_RENDER_SOURCE_FRAME_REJECTED;
    discontinuity = g_force_discontinuity ||
        g_frame.description.discontinuity ||
        (g_last_scene_generation != 0u &&
         g_last_scene_generation != g_frame.description.scene_generation);
    if (xg_render_timeline_next_identity(
            g_frame.description.scene_generation, &identity) !=
            XG_RENDER_TIMELINE_OK ||
        identity.presentation_epoch != g_presentation_epoch)
        goto finished;
    source_interval_vblanks = g_frame.description.source_interval_vblanks;
    if (source_interval_vblanks == 0u) {
        source_interval_vblanks =
            g_last_published_vblank != 0u &&
            identity.guest_vblank_sequence > g_last_published_vblank &&
            identity.guest_vblank_sequence - g_last_published_vblank <=
                UINT32_MAX
                ? (uint32_t)(identity.guest_vblank_sequence -
                             g_last_published_vblank)
                : 1u;
    }
    commit_result = xg_render_source_commit_begin(
            &identity, &g_frame.description.scene,
            &g_frame.description.display,
            source_interval_vblanks,
            discontinuity,
            g_frame.description.temporally_eligible, &builder);
    if (commit_result != XG_RENDER_SOURCE_COMMIT_OK) {
        result = frame_result_from_commit(commit_result);
        goto finished;
    }
    for (index = 0u; index < g_frame.pass_count; ++index)
        if ((commit_result = xg_render_source_commit_append_pass(builder,
                &g_frame.passes[index])) != XG_RENDER_SOURCE_COMMIT_OK) {
            result = frame_result_from_commit(commit_result);
            goto cancel;
        }
    for (index = 0u; index < g_frame.resource_count; ++index)
        if ((commit_result = xg_render_source_commit_append_resource(builder,
                &g_frame.resources[index])) != XG_RENDER_SOURCE_COMMIT_OK) {
            result = frame_result_from_commit(commit_result);
            goto cancel;
        }
    for (index = 0u; index < g_frame.draw_count; ++index)
        if ((commit_result = xg_render_source_commit_append_draw(builder,
                &g_frame.draws[index])) != XG_RENDER_SOURCE_COMMIT_OK) {
            result = frame_result_from_commit(commit_result);
            goto cancel;
        }
    for (index = 0u; index < g_frame.edge_count; ++index)
        if ((commit_result = xg_render_source_commit_append_surface_edge(
                builder, &g_frame.edges[index])) !=
                XG_RENDER_SOURCE_COMMIT_OK) {
            result = frame_result_from_commit(commit_result);
            goto cancel;
        }
    for (index = 0u; index < g_frame.ui_node_count; ++index)
        if ((commit_result = xg_render_source_commit_append_ui_node(builder,
                &g_frame.ui_nodes[index])) != XG_RENDER_SOURCE_COMMIT_OK) {
            result = frame_result_from_commit(commit_result);
            goto cancel;
        }
    for (index = 0u; index < g_frame.ui_glyph_run_count; ++index)
        if ((commit_result = xg_render_source_commit_append_ui_glyph_run(
                builder, &g_frame.ui_glyph_runs[index])) !=
                XG_RENDER_SOURCE_COMMIT_OK) {
            result = frame_result_from_commit(commit_result);
            goto cancel;
        }
    for (index = 0u; index < g_frame.ui_glyph_placement_count; ++index)
        if ((commit_result = xg_render_source_commit_append_ui_glyph_placement(
                builder, &g_frame.ui_glyph_placements[index])) !=
                XG_RENDER_SOURCE_COMMIT_OK) {
            result = frame_result_from_commit(commit_result);
            goto cancel;
        }
    commit_result = xg_render_source_commit_seal(builder, &commit);
    if (commit_result != XG_RENDER_SOURCE_COMMIT_OK) {
        result = frame_result_from_commit(commit_result);
        goto finished;
    }
    if (xg_render_source_commit_header_copy(commit, &copied_header) !=
            XG_RENDER_SOURCE_COMMIT_OK) {
        (void)xg_render_source_commit_retire(commit);
        goto finished;
    }
    host_callbacks = g_host_callbacks;
    host_callbacks_configured = g_host_callbacks_configured;
    reset_generation = g_reset_generation;
    frame_clear();
    frame_unlock();

    /* Host callbacks run with no source-frame or source-commit lock held. */
    if (host_callbacks_configured &&
        !host_callbacks.sealed_capture(commit, &copied_header,
                                       host_callbacks.user_data)) {
        (void)xg_render_source_commit_retire(commit);
        return XG_RENDER_SOURCE_FRAME_CAPTURE_FAILED;
    }
    frame_lock();
    frame_reconcile_timeline();
    /* A reset may have cancelled this detached frame during capture. Publish
     * and update cadence under the same lock so resets cannot split them. */
    if (reset_generation != g_reset_generation ||
        xg_render_source_queue_publish(commit) != XG_RENDER_TIMELINE_OK) {
        frame_unlock();
        (void)xg_render_source_commit_retire(commit);
        return XG_RENDER_SOURCE_FRAME_REJECTED;
    }

    g_last_published_vblank = identity.guest_vblank_sequence;
    g_last_scene_generation = identity.scene_generation;
    g_force_discontinuity = false;
    frame_unlock();
    if (host_callbacks_configured)
        host_callbacks.published_notify(host_callbacks.user_data);
    return XG_RENDER_SOURCE_FRAME_OK;

cancel:
    xg_render_source_commit_cancel(builder);
finished:
    frame_clear();
    frame_unlock();
    return result;
}

void xg_render_source_frame_snapshot(XgRenderSourceFrameSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    frame_lock();
    frame_reconcile_timeline();
    *out_snapshot = (XgRenderSourceFrameSnapshot){
        .presentation_epoch = g_presentation_epoch,
        .scene_generation = g_frame.description.scene_generation,
        .pass_count = g_frame.pass_count,
        .draw_count = g_frame.draw_count,
        .resource_count = g_frame.resource_count,
        .surface_edge_count = g_frame.edge_count,
        .ui_node_count = g_frame.ui_node_count,
        .ui_glyph_run_count = g_frame.ui_glyph_run_count,
        .ui_glyph_placement_count = g_frame.ui_glyph_placement_count,
        .active = g_frame.active,
        .complete = g_frame.complete,
        .blocked = g_frame.blocked,
    };
    frame_unlock();
}
