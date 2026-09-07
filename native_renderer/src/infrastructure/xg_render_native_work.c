#include "xg_render_native_work.h"
#include "xg_render_semantic_presentation.h"
#include "xg_render_submission.h"

#include <stdlib.h>

#define NATIVE_WORK_UPLOAD_CAPACITY 32u
/* One device-stream ID, not an authenticated game artifact identity. */
#define NATIVE_WORK_UPLOAD_RESOURCE_ID UINT64_C(0x58474e415456524d)

static XgRenderNativeWorkServices g_services;
static XgRenderSourceBuilder g_builder;
static XgRenderSourceCommitHandle g_pending;
static uint64_t g_epoch;
static uint64_t g_receipt;
static uint32_t g_scene_generation;
static uint32_t g_operation_count;
static uint32_t g_upload_count;
static uint32_t g_temporal_count;
static bool g_building;
static bool g_sealed;
static bool g_pending_boundary;

static bool lane_ready(XgRenderPresentationDiagnostics *out_diagnostics) {
    XgRenderPresentationDiagnostics diagnostics;
    if (!xg_render_native_work_enabled()) return false;
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    if (!diagnostics.publication_open || diagnostics.epoch_terminal ||
        diagnostics.source_lane_blocked ||
        (g_epoch != 0u && g_epoch != diagnostics.presentation_epoch))
        return false;
    g_epoch = diagnostics.presentation_epoch;
    if (out_diagnostics != NULL) *out_diagnostics = diagnostics;
    return true;
}

static bool wait_for_worker(void) {
    return lane_ready(NULL) && g_services.wait != NULL &&
        g_services.wait(g_services.user_data) && lane_ready(NULL);
}

static bool publish_pending(void) {
    while (g_sealed) {
        XgRenderTimelineResult result;
        if (!lane_ready(NULL)) return false;
        result = xg_render_source_queue_publish(g_pending);
        if (result == XG_RENDER_TIMELINE_OK) {
            g_sealed = false;
            g_operation_count = 0u;
            g_upload_count = 0u;
            g_temporal_count = 0u;
            g_services.notify(g_services.user_data);
            return lane_ready(NULL);
        }
        if (result != XG_RENDER_TIMELINE_WOULD_BLOCK || !wait_for_worker())
            return false;
    }
    return true;
}

static bool begin_chunk(uint64_t guest_cycle) {
    if (!lane_ready(NULL) || !publish_pending()) return false;
    if (g_building) return true;
    for (;;) {
        XgRenderSourceFrameDescription description = {0};
        XgPresentationIdentity identity;
        XgRenderSourceCommitResult result;
        if (!lane_ready(NULL)) return false;
        if (!g_services.describe(&description) ||
            xg_render_timeline_next_work_identity(
                description.scene_generation, guest_cycle, &identity) !=
                XG_RENDER_TIMELINE_OK || identity.presentation_epoch != g_epoch)
            return false;
        if (!xg_render_source_queue_has_capacity()) {
            if (!wait_for_worker()) return false;
            continue;
        }
        result = xg_render_source_commit_begin(
            &identity, &description.scene, &description.display,
            description.source_interval_vblanks, description.discontinuity,
            false, &g_builder);
        if (result == XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED) {
            if (!wait_for_worker()) return false;
            continue;
        }
        if (result != XG_RENDER_SOURCE_COMMIT_OK) return false;
        g_building = true;
        g_scene_generation = description.scene_generation;
        return xg_render_source_commit_set_native_work(g_builder, false) ==
            XG_RENDER_SOURCE_COMMIT_OK;
    }
}

static bool append_operation(const XgRenderNativeOperation *operation,
                              uint64_t guest_cycle) {
    XgRenderSourceCommitResult result;
    if (!begin_chunk(guest_cycle)) return false;
    result = xg_render_source_commit_append_native_operation(g_builder, operation);
    if (result == XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED &&
        (g_operation_count != 0u || g_temporal_count != 0u)) {
        if (!xg_render_native_work_flush(false, guest_cycle) ||
            !begin_chunk(guest_cycle))
            return false;
        result = xg_render_source_commit_append_native_operation(
            g_builder, operation);
    }
    if (result != XG_RENDER_SOURCE_COMMIT_OK) return false;
    ++g_operation_count;
    if (operation->kind == XG_RENDER_NATIVE_OPERATION_UPLOAD) ++g_upload_count;
    return g_services.service == NULL || g_services.service(g_services.user_data);
}

bool xg_render_native_work_configure(const XgRenderNativeWorkServices *services) {
    if (services == NULL) {
        g_services = (XgRenderNativeWorkServices){0};
        return true;
    }
    if (services->describe == NULL || services->notify == NULL) return false;
    g_services = *services;
    return true;
}

bool xg_render_native_work_enabled(void) {
    return g_services.describe != NULL;
}

void xg_render_native_work_snapshot(XgRenderNativeWorkSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    *out_snapshot = (XgRenderNativeWorkSnapshot){
        .buffered_operations = g_operation_count,
        .buffered_uploads = g_upload_count,
        .building = g_building,
        .sealed = g_sealed,
    };
}

bool xg_render_native_work_operation(const XgRenderNativeOperation *operation,
                                     uint64_t guest_cycle) {
    return operation != NULL && append_operation(operation, guest_cycle);
}

bool xg_render_native_work_draw(const GpuRenderSemantic *semantic,
                                uint64_t guest_cycle) {
    XgRenderNativeOperation operation = {.kind = XG_RENDER_NATIVE_OPERATION_DRAW};
    if (semantic == NULL) return false;
    operation.semantic = *semantic;
    (void)xg_render_motion_bind_command(semantic->submission_command_id, &operation);
    (void)xg_render_submission_temporal_binding(semantic, &operation.temporal);
    return append_operation(&operation, guest_cycle);
}

bool xg_render_native_work_temporal_coverage(
    uint32_t scope, const XgRenderTemporalComponent *components, uint32_t component_count,
    const XgRenderTemporalSample *samples, uint32_t sample_count,
    uint64_t guest_cycle, XgSemanticResourceRef *out) {
    XgRenderPresentationDiagnostics timeline;
    XgSemanticResourceRef ref;
    if (!out || !begin_chunk(guest_cycle) || !lane_ready(&timeline) ||
        timeline.guest_vblank_sequence == UINT64_MAX) return false;
    const XgPresentationIdentity identity = {
        .presentation_epoch = g_epoch,
        .source_sequence = timeline.source_sequence,
        .guest_vblank_sequence = timeline.guest_vblank_sequence,
        .guest_cycle = guest_cycle,
        .scene_generation = g_scene_generation,
    };
    if (!xg_render_temporal_coverage_create(&identity, timeline.guest_vblank_sequence + 1,
            scope, components, component_count, samples, sample_count, &ref)) return false;
    XgRenderSourceCommitResult result = xg_render_source_commit_append_temporal_coverage(g_builder, ref);
    if (result == XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED &&
        xg_render_native_work_flush(false, guest_cycle) && begin_chunk(guest_cycle))
        result = xg_render_source_commit_append_temporal_coverage(g_builder, ref);
    if (result != XG_RENDER_SOURCE_COMMIT_OK) {
        (void)xg_render_resource_release((XgRenderResourceHandle){ref.resource_id, ref.generation});
        return false;
    }
    ++g_temporal_count;
    *out = ref;
    return true;
}

bool xg_render_native_work_flush(bool display_boundary, uint64_t guest_cycle) {
    XgRenderSourceCommitResult result;
    XgPresentationIdentity identity;
    if (!lane_ready(NULL)) return false;
    if (g_sealed) {
        const bool was_boundary = g_pending_boundary;
        if (!publish_pending()) return false;
        if (!display_boundary || was_boundary) return true;
    }
    if (!g_building) {
        if (!display_boundary) return true;
        if (!begin_chunk(guest_cycle)) return false;
    }
    if (display_boundary) {
        XgRenderSourceFrameDescription description = {0};
        const XgSemanticDisplayState *display = &description.display;
        if (!g_services.describe(&description) ||
            description.scene_generation != g_scene_generation ||
            display->display_x >= 1024u || display->display_y >= 512u ||
            (!display->disabled &&
             (display->width == 0u || display->width > 1024u ||
              display->height == 0u || display->height > 512u ||
              display->aspect_num == 0u || display->aspect_den == 0u)) ||
            xg_render_source_commit_set_native_display(g_builder, display) !=
                XG_RENDER_SOURCE_COMMIT_OK)
            return false;
    }
    if (xg_render_source_commit_set_native_work(g_builder, display_boundary) !=
        XG_RENDER_SOURCE_COMMIT_OK)
        return false;
    if (xg_render_timeline_next_work_identity(
            g_scene_generation, guest_cycle, &identity) != XG_RENDER_TIMELINE_OK ||
        xg_render_source_commit_set_native_identity(g_builder, &identity) !=
            XG_RENDER_SOURCE_COMMIT_OK)
        return false;
    result = xg_render_source_commit_seal(g_builder, &g_pending);
    g_building = false;
    if (result != XG_RENDER_SOURCE_COMMIT_OK) return false;
    g_sealed = true;
    g_pending_boundary = display_boundary;
    return publish_pending();
}

bool xg_render_native_work_drain(uint64_t guest_cycle) {
    if (!xg_render_native_work_flush(false, guest_cycle)) return false;
    for (;;) {
        XgRenderPresentationDiagnostics diagnostics;
        if (!lane_ready(&diagnostics)) return false;
        if (diagnostics.source_queue_depth == 0u && !diagnostics.worker_busy)
            return true;
        if (!wait_for_worker()) return false;
    }
}

void xg_render_native_work_cancel_pending(void) {
    if (g_building) xg_render_source_commit_cancel(g_builder);
    if (g_sealed) (void)xg_render_source_commit_retire(g_pending);
    g_building = false;
    g_sealed = false;
    g_pending_boundary = false;
    g_operation_count = 0u;
    g_upload_count = 0u;
    g_temporal_count = 0u;
    g_scene_generation = 0u;
    g_epoch = 0u;
}

bool xg_render_native_work_vram_event(const GpuVramEvent *event,
                                      uint64_t guest_cycle) {
    XgRenderNativeOperation operation = {0};
    XgRenderResourceImport import = {0};
    XgRenderResourceCapabilityMetadata metadata = {0};
    uint8_t *bytes;
    size_t pixel_count;
    bool success = false;
    if (event == NULL || !xg_render_native_work_enabled()) return false;
    switch (event->operation) {
    case GPU_VRAM_EVENT_READBACK:
    case GPU_VRAM_EVENT_RENDER_TARGET_WRITE:
    case GPU_VRAM_EVENT_SCANOUT:
        return true;
    case GPU_VRAM_EVENT_MOVE:
        operation.kind = XG_RENDER_NATIVE_OPERATION_COPY;
        operation.src_x = event->source_x;
        operation.src_y = event->source_y;
        break;
    case GPU_VRAM_EVENT_CLEAR:
        operation.kind = XG_RENDER_NATIVE_OPERATION_FILL;
        operation.fill_color = event->fill_color;
        break;
    case GPU_VRAM_EVENT_UPLOAD:
    case GPU_VRAM_EVENT_RESTORE:
        operation.kind = XG_RENDER_NATIVE_OPERATION_UPLOAD;
        break;
    default:
        return false;
    }
    operation.dst_x = event->destination_x;
    operation.dst_y = event->destination_y;
    operation.width = event->width;
    operation.height = event->height;
    if (event->operation == GPU_VRAM_EVENT_RESTORE) {
        XgRenderPresentationDiagnostics diagnostics;
        xg_render_semantic_presentation_diagnostics(&diagnostics);
        if (g_epoch != 0u && g_epoch == diagnostics.presentation_epoch)
            return false;
        xg_render_native_work_cancel_pending();
        if (!xg_render_native_work_drain(guest_cycle)) return false;
        operation.dst_x = 0u;
        operation.dst_y = 0u;
        operation.width = 1024u;
        operation.height = 512u;
    }
    if (operation.kind != XG_RENDER_NATIVE_OPERATION_UPLOAD) {
        operation.mask_set = event->mask_set;
        operation.mask_check = event->mask_check;
        return append_operation(&operation, guest_cycle);
    }
    pixel_count = (size_t)operation.width * operation.height;
    if (operation.dst_x >= 1024u || operation.dst_y >= 512u ||
        operation.width == 0u || operation.width > 1024u ||
        operation.height == 0u || operation.height > 512u ||
        event->pixels == NULL || event->pixel_count != pixel_count ||
        !begin_chunk(guest_cycle))
        return false;
    if ((g_upload_count >= NATIVE_WORK_UPLOAD_CAPACITY ||
         g_operation_count >= XG_RENDER_NATIVE_OPERATION_CAPACITY) &&
        (!xg_render_native_work_flush(false, guest_cycle) ||
         !begin_chunk(guest_cycle)))
        return false;
    bytes = malloc(pixel_count * 2u);
    if (bytes == NULL) return false;
    for (size_t index = 0u; index < pixel_count; ++index) {
        bytes[index * 2u] = (uint8_t)event->pixels[index];
        bytes[index * 2u + 1u] = (uint8_t)(event->pixels[index] >> 8u);
    }
    import.resource_id = NATIVE_WORK_UPLOAD_RESOURCE_ID;
    import.kind = XG_RENDER_RESOURCE_TEXTURE;
    import.owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE;
    import.state = XG_RENDER_RESOURCE_NATIVE_OWNED;
    import.bytes = bytes;
    import.byte_count = pixel_count * 2u;
    import.content_digest = xg_render_resource_digest(bytes, import.byte_count);
    import.descriptor = (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = operation.width,
        .height = operation.height,
        .row_pitch = (uint32_t)operation.width * 2u,
        .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
        .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
        .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
    };
    metadata.kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE;
    metadata.lifetime = XG_RENDER_RESOURCE_CAPABILITY_TIMELINE;
    metadata.owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE;
    metadata.source.source_class = XG_RENDER_RESOURCE_SOURCE_VRAM_TRANSFER;
    metadata.source.range_size = import.byte_count;
    metadata.source.range_content_digest = import.content_digest;
    /* Capacity recovery must publish our own retains before waiting for their
     * ACKs. Register again only after draining, and retry at most once. */
    for (unsigned attempt = 0u; attempt < 2u; ++attempt) {
        XgRenderResourceCapabilityResult capability_result;
        XgRenderResourceResult import_result;
        XgRenderResourceHandle handle;
        if (attempt != 0u &&
            (!xg_render_native_work_drain(guest_cycle) || !begin_chunk(guest_cycle)))
            break;
        if (g_receipt == UINT64_MAX) break;
        metadata.receipt = ++g_receipt;
        metadata.owner_generation = g_scene_generation;
        import.owner_generation = g_scene_generation;
        capability_result = xg_render_resource_capability_register(
            &metadata, &import.provenance);
        if (capability_result == XG_RENDER_RESOURCE_CAPABILITY_CAPACITY_EXCEEDED)
            continue;
        if (capability_result != XG_RENDER_RESOURCE_CAPABILITY_OK) break;
        import_result = xg_render_resource_import_native(&import, &handle);
        if (import_result == XG_RENDER_RESOURCE_OK) {
            XgRenderResourceResult retire_result;
            operation.upload = (XgSemanticResourceRef){
                handle.resource_id, handle.generation, import.content_digest,
            };
            success = append_operation(&operation, guest_cycle);
            /* append acquired the snapshot; only that retain survives to ACK. */
            retire_result = xg_render_resource_retire_current(handle);
            capability_result = xg_render_resource_capability_retire(import.provenance);
            success = success && retire_result == XG_RENDER_RESOURCE_OK &&
                capability_result == XG_RENDER_RESOURCE_CAPABILITY_OK;
            break;
        }
        capability_result = xg_render_resource_capability_retire(import.provenance);
        if (import_result != XG_RENDER_RESOURCE_CAPACITY_EXCEEDED ||
            capability_result != XG_RENDER_RESOURCE_CAPABILITY_OK)
            break;
    }
    free(bytes);
    return success;
}
