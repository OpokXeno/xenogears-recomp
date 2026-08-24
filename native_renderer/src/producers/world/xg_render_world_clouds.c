#include "xg_render_world_pending_services.h"

#include "gpu.h"
#include "xg_field_render_services.h"
#include "xg_render_primitive_utils.h"
#include "xg_world_clouds_shadow.h"
#include "xg_world_clouds_source_capture.h"

#include <limits.h>
#include <string.h>

typedef struct XgRenderWorldCloudsPending {
    XgWorldCloudRecord records[XG_WORLD_CLOUD_PACKET_CAPACITY];
    XgWorldCloudRecord temporal_records[XG_WORLD_CLOUD_TEMPORAL_CAPACITY];
    XgWorldCloudPosition stepped_positions[XG_WORLD_CLOUD_COUNT];
    uint32_t expected_packets[XG_WORLD_CLOUD_PACKET_CAPACITY]
                             [XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT];
    uint32_t expected_ot[XG_WORLD_CLOUD_OT_BUCKET_COUNT];
    uint64_t authentication_generation;
    uint32_t entry_stack_pointer;
    uint32_t position_base;
    uint32_t packet_base;
    uint32_t ot_base;
    uint32_t record_count;
    uint32_t temporal_record_count;
    uint32_t expected_attempts;
    CPUState *owner_cpu;
    uint32_t depth;
    bool poisoned;
    bool valid;
} XgRenderWorldCloudsPending;

static XgRenderWorldCloudsPending pending;

static bool services_valid(const XgRenderWorldPendingServices *services) {
    return services != NULL && services->cutover_ready != NULL &&
        services->authentication_generation != NULL &&
        services->authorize_guest_range != NULL &&
        services->screen_x_cull_margin != NULL &&
        services->begin_submission != NULL && services->stage_native != NULL &&
        services->stage_temporal != NULL && services->abort_submission != NULL &&
        services->coordinator_in_progress != NULL &&
        services->coordinator_begin != NULL &&
        services->coordinator_end != NULL &&
        services->coordinator_fail != NULL &&
        services->coordinator_failed != NULL;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool read_u16(void *opaque, uint32_t address, uint16_t *out_value) {
    CPUState *cpu = opaque;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_u32(void *opaque, uint32_t address, uint32_t *out_value) {
    CPUState *cpu = opaque;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static uint32_t capture_build(
        CPUState *cpu, uint64_t authentication_generation,
        const XgRenderWorldPendingServices *services,
        XgWorldCloudsCapture *capture, XgWorldCloudRecord *records,
        uint32_t *out_count, XgWorldCloudPosition *stepped_positions,
        XgWorldCloudsBuildStats *out_stats,
        XgWorldCloudRecord *temporal_records, uint32_t *out_temporal_count) {
    XgWorldCloudsCaptureRequest request = {0};
    XgWorldCloudsAuthenticatedReader reader = {0};
    GpuDrawState draw = {0};

    if (cpu == NULL || capture == NULL || records == NULL ||
        out_count == NULL || stepped_positions == NULL || out_stats == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        authentication_generation == 0u)
        return 1u;
    gpu_get_draw_state(&draw);
    request = (XgWorldCloudsCaptureRequest){
        .authentication_generation = authentication_generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = services->screen_x_cull_margin(),
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .texture_window_mask_x = draw.texture_window_mask_x,
            .texture_window_mask_y = draw.texture_window_mask_y,
            .texture_window_offset_x = draw.texture_window_offset_x,
            .texture_window_offset_y = draw.texture_window_offset_y,
            .dither = draw.dither != 0u,
            .mask_set = draw.mask_set != 0u,
            .mask_check = draw.mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldCloudsAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = authentication_generation,
        .authenticated = true,
    };
    if (xg_world_clouds_source_capture(&request, &reader, capture) !=
        XG_WORLD_CLOUDS_CAPTURE_OK)
        return 1u;
    if (xg_world_clouds_build_with_temporal(
            &capture->source, records, XG_WORLD_CLOUD_PACKET_CAPACITY,
            out_count, out_stats, temporal_records,
            XG_WORLD_CLOUD_TEMPORAL_CAPACITY, out_temporal_count) !=
        XG_WORLD_CLOUDS_OK)
        return 2u;
    memcpy(stepped_positions, capture->source.positions,
           sizeof(capture->source.positions));
    if (xg_world_clouds_step_positions(
            stepped_positions, capture->source.velocities) !=
        XG_WORLD_CLOUDS_OK)
        return 2u;
    return 0u;
}

void xg_render_world_clouds_clear_pending(void) {
    if (!pending.valid && !pending.poisoned) return;
    memset(&pending, 0, sizeof(pending));
}

static void poison(const XgRenderWorldPendingServices *services) {
    pending.valid = false;
    pending.poisoned = true;
    if (services != NULL && services->coordinator_fail != NULL)
        services->coordinator_fail();
}

static void expected_packet(
        uint32_t index, const XgWorldCloudRecord *record, uint32_t tag) {
    uint32_t *words = pending.expected_packets[index];
    uint32_t vertex;

    words[0] = tag;
    words[1] = record->material_word;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        words[2u + vertex * 2u] =
            xg_render_projected_xy(&record->vertices[vertex]);
        words[3u + vertex * 2u] =
            (words[3u + vertex * 2u] & UINT32_C(0xffff0000)) |
            record->uv[vertex];
    }
    words[3] = (words[3] & UINT32_C(0x0000ffff)) |
        ((uint32_t)record->clut << 16u);
    words[5] = (words[5] & UINT32_C(0x0000ffff)) |
        ((uint32_t)record->tpage << 16u);
}

void xg_render_world_clouds_prepare(
        CPUState *cpu, const XgRenderWorldPendingServices *services) {
    enum {
        CLOUD_BUFFER_INDEX = 0x8009d7f0u,
        CLOUD_PACKET_BASES = 0x8009d7f8u,
        CLOUD_CONTEXT = 0x8009be3cu,
        CLOUD_CALLBACK_POINTER = 0x8009cd40u,
        CLOUD_CALLBACK_PHYSICAL = 0x00086700u,
        CLOUD_PACKET_STRIDE = 0x28u,
        CLOUD_POSITION_STRIDE = 0x10u,
    };
    XgWorldCloudsCapture capture;
    XgWorldCloudsBuildStats stats = {0};
    uint64_t generation;
    uint32_t context_address;
    uint32_t buffer_index;
    uint32_t index;
    uint32_t word;

    if (!services_valid(services)) return;
    if (pending.depth != 0u) {
        if (pending.depth != UINT32_MAX) ++pending.depth;
        poison(services);
        return;
    }
    xg_render_world_clouds_clear_pending();
    pending.owner_cpu = cpu;
    pending.depth = 1u;
    if (!services->cutover_ready() ||
        !services->authentication_generation(&generation)) {
        poison(services);
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_CLOUDS_SHADOW_FULL_RETURN) ||
        !physical_address_equals(
            cpu->read_word(CLOUD_CALLBACK_POINTER), CLOUD_CALLBACK_PHYSICAL) ||
        capture_build(
            cpu, generation, services, &capture, pending.records,
            &pending.record_count, pending.stepped_positions, &stats,
            pending.temporal_records, &pending.temporal_record_count) != 0u) {
        poison(services);
        return;
    }

    buffer_index = cpu->read_word(CLOUD_BUFFER_INDEX);
    if (buffer_index >= 2u) {
        poison(services);
        return;
    }
    pending.packet_base =
        cpu->read_word(CLOUD_PACKET_BASES + buffer_index * 4u);
    if (!services->authorize_guest_range(
            pending.packet_base,
            XG_WORLD_CLOUD_PACKET_CAPACITY * CLOUD_PACKET_STRIDE, 4u, false) ||
        !services->authorize_guest_range(
            capture.position_array_address,
            XG_WORLD_CLOUD_COUNT * CLOUD_POSITION_STRIDE, 4u, false) ||
        cpu->gpr[29] < 0x50u ||
        !services->authorize_guest_range(
            cpu->gpr[29] - 0x50u, 0x50u, 4u, false)) {
        poison(services);
        return;
    }
    context_address = cpu->read_word(CLOUD_CONTEXT);
    if (context_address > UINT32_MAX - 0x70u ||
        !xg_render_runtime_word_address_is_valid(context_address + 0x70u)) {
        poison(services);
        return;
    }
    pending.ot_base = cpu->read_word(context_address + 0x70u);
    if (!services->authorize_guest_range(
            pending.ot_base, XG_WORLD_CLOUD_OT_BUCKET_COUNT * 4u, 4u, false)) {
        poison(services);
        return;
    }

    pending.authentication_generation = generation;
    pending.entry_stack_pointer = cpu->gpr[29];
    pending.position_base = capture.position_array_address;
    pending.expected_attempts = stats.quad_attempt_count -
        stats.far_preinsert_depth_stops - stats.far_postinsert_depth_stops;
    for (index = 0u; index < XG_WORLD_CLOUD_OT_BUCKET_COUNT; ++index)
        pending.expected_ot[index] =
            cpu->read_word(pending.ot_base + index * 4u);
    for (index = 0u; index < XG_WORLD_CLOUD_PACKET_CAPACITY; ++index) {
        const uint32_t packet =
            pending.packet_base + index * CLOUD_PACKET_STRIDE;

        for (word = 0u;
             word < XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT; ++word)
            pending.expected_packets[index][word] =
                cpu->read_word(packet + word * 4u);
    }
    for (index = 0u; index < pending.record_count; ++index) {
        const XgWorldCloudRecord *record = &pending.records[index];
        const uint32_t packet =
            pending.packet_base + index * CLOUD_PACKET_STRIDE;
        const uint32_t bucket = record->ordering_bucket;

        if (bucket >= XG_WORLD_CLOUD_OT_BUCKET_COUNT ||
            !xg_render_runtime_word_address_is_valid(packet) ||
            !xg_render_runtime_word_address_is_valid(
                packet + CLOUD_PACKET_STRIDE - 4u)) {
            poison(services);
            return;
        }
        if ((pending.expected_packets[index][0] >> 24u) != 9u) {
            poison(services);
            return;
        }
        expected_packet(
            index, record, UINT32_C(0x09000000) |
                (pending.expected_ot[bucket] & UINT32_C(0x00ffffff)));
        pending.expected_ot[bucket] = packet & UINT32_C(0x00ffffff);
    }
    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        const uint32_t address =
            capture.position_array_address + index * CLOUD_POSITION_STRIDE;

        if (!xg_render_runtime_word_address_is_valid(address) ||
            !xg_render_runtime_word_address_is_valid(address + 8u)) {
            poison(services);
            return;
        }
    }
    pending.valid = true;
}

void xg_render_world_clouds_commit(
        CPUState *cpu, const XgRenderWorldPendingServices *services) {
    enum {
        CLOUD_PACKET_STRIDE = 0x28u,
        CLOUD_POSITION_STRIDE = 0x10u,
        CLOUD_STACK_FRAME_SIZE = 0x50u,
        CLOUD_SAVED_RA_OFFSET = 0x4cu,
        CLOUD_SCRATCH_EMITTED = 0x1f8002f0u,
        CLOUD_SCRATCH_ATTEMPTS = 0x1f8002f4u,
    };
    XgWorldCloudsShadowSnapshot snapshot;
    uint64_t generation;
    uint32_t index;
    uint32_t word;

    if (!services_valid(services)) return;
    if (pending.depth == 0u) {
        poison(services);
        xg_render_world_clouds_clear_pending();
        return;
    }
    if (pending.depth > 1u) {
        if (cpu == NULL || pending.owner_cpu != cpu ||
            !services->authentication_generation(&generation) ||
            pending.authentication_generation != generation)
            poison(services);
        --pending.depth;
        return;
    }
    if (pending.poisoned || !pending.valid || pending.owner_cpu != cpu ||
        !services->cutover_ready() ||
        !services->authentication_generation(&generation) || cpu == NULL ||
        cpu->read_word == NULL ||
        pending.authentication_generation != generation ||
        pending.entry_stack_pointer < CLOUD_STACK_FRAME_SIZE ||
        cpu->gpr[29] != pending.entry_stack_pointer - CLOUD_STACK_FRAME_SIZE ||
        !services->authorize_guest_range(
            cpu->gpr[29], CLOUD_STACK_FRAME_SIZE, 4u, false) ||
        !services->authorize_guest_range(
            pending.packet_base,
            XG_WORLD_CLOUD_PACKET_CAPACITY * CLOUD_PACKET_STRIDE, 4u, false) ||
        !services->authorize_guest_range(
            pending.ot_base, XG_WORLD_CLOUD_OT_BUCKET_COUNT * 4u, 4u, false) ||
        !services->authorize_guest_range(
            pending.position_base,
            XG_WORLD_CLOUD_COUNT * CLOUD_POSITION_STRIDE, 4u, false) ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + CLOUD_SAVED_RA_OFFSET),
            XG_WORLD_CLOUDS_SHADOW_FULL_RETURN) ||
        cpu->gpr[11] != pending.packet_base +
            pending.record_count * CLOUD_PACKET_STRIDE ||
        cpu->read_word(CLOUD_SCRATCH_EMITTED) != pending.record_count ||
        cpu->read_word(CLOUD_SCRATCH_ATTEMPTS) != pending.expected_attempts) {
        poison(services);
        xg_render_world_clouds_clear_pending();
        return;
    }
    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        const uint32_t address =
            pending.position_base + index * CLOUD_POSITION_STRIDE;

        if (cpu->read_word(address) !=
                (uint32_t)pending.stepped_positions[index].x ||
            cpu->read_word(address + 4u) !=
                (uint32_t)pending.stepped_positions[index].y ||
            cpu->read_word(address + 8u) !=
                (uint32_t)pending.stepped_positions[index].z) {
            poison(services);
            xg_render_world_clouds_clear_pending();
            return;
        }
    }
    for (index = 0u; index < XG_WORLD_CLOUD_PACKET_CAPACITY; ++index) {
        const uint32_t packet =
            pending.packet_base + index * CLOUD_PACKET_STRIDE;

        for (word = 0u;
             word < XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT; ++word) {
            if (cpu->read_word(packet + word * 4u) !=
                pending.expected_packets[index][word]) {
                poison(services);
                xg_render_world_clouds_clear_pending();
                return;
            }
        }
    }
    for (index = 0u; index < XG_WORLD_CLOUD_OT_BUCKET_COUNT; ++index) {
        if (cpu->read_word(pending.ot_base + index * 4u) !=
            pending.expected_ot[index]) {
            poison(services);
            xg_render_world_clouds_clear_pending();
            return;
        }
    }

    xg_world_clouds_shadow_snapshot(&snapshot);
    if (snapshot.native_cutover_count == UINT64_MAX ||
        snapshot.native_primitive_count >
            UINT64_MAX - pending.record_count) {
        poison(services);
        xg_render_world_clouds_clear_pending();
        return;
    }
    if (services->coordinator_in_progress()) {
        poison(services);
        xg_render_world_clouds_clear_pending();
        return;
    }
    if (pending.record_count != 0u && !services->begin_submission()) {
        poison(services);
        xg_render_world_clouds_clear_pending();
        return;
    }
    if (!services->coordinator_begin()) {
        services->abort_submission();
        poison(services);
        xg_render_world_clouds_clear_pending();
        return;
    }
    for (index = 0u; index < pending.record_count; ++index) {
        const XgWorldCloudRecord *record = &pending.records[index];
        const uint32_t packet =
            pending.packet_base + index * CLOUD_PACKET_STRIDE;
        const uint32_t interpolation_primitive_id =
            ((record->source_index * 3u + (uint32_t)record->lod) * 48u) +
            record->lod_quad_index;

        if (!services->stage_native(
                &record->primitive, packet,
                UINT32_C(0x62000000) | index, UINT32_C(0x80086798),
                interpolation_primitive_id)) {
            services->abort_submission();
            services->coordinator_end();
            poison(services);
            xg_render_world_clouds_clear_pending();
            return;
        }
        if (services->coordinator_failed()) {
            services->coordinator_end();
            poison(services);
            xg_render_world_clouds_clear_pending();
            return;
        }
    }
    for (index = 0u; index < pending.temporal_record_count; ++index) {
        const XgWorldCloudRecord *record = &pending.temporal_records[index];
        const uint32_t interpolation_primitive_id =
            ((record->source_index * 3u + (uint32_t)record->lod) * 48u) +
            record->lod_quad_index;
        const int32_t margin = services->screen_x_cull_margin();
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = -margin * INT32_C(65536),
            .screen_top = 0,
            .screen_right_exclusive =
                (320 + margin) * INT32_C(65536),
            .screen_bottom_exclusive = 216 * INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x0d01,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = 4u,
        };

        if (!services->stage_temporal(
                &record->primitive, UINT32_C(0x80086798),
                interpolation_primitive_id, &policy)) {
            services->abort_submission();
            services->coordinator_end();
            poison(services);
            xg_render_world_clouds_clear_pending();
            return;
        }
    }
    services->coordinator_end();
    if (!xg_world_clouds_shadow_record_native_cutover(pending.record_count)) {
        services->abort_submission();
        poison(services);
    }
    xg_render_world_clouds_clear_pending();
}

void xg_render_world_clouds_reset(void) {
    pending = (XgRenderWorldCloudsPending){0};
}

void xg_render_world_clouds_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    const bool semantic_write = event->mutation.semantic_authority_loss ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_SKY) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);

    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST) {
        xg_render_world_clouds_clear_pending();
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
        event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
        (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE && semantic_write)) {
        xg_world_clouds_shadow_invalidate();
        xg_render_world_clouds_clear_pending();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_world_clouds_reset();
        xg_world_clouds_shadow_reset();
    }
}
