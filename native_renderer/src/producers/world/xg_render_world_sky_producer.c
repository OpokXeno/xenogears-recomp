#include "xg_render_world_sky_producer.h"

#include "gpu.h"
#include "psx_cyc.h"
#include "xg_field_render_services.h"
#include "xg_render_primitive_utils.h"
#include "xg_world_sky.h"

#include <limits.h>
#include <string.h>

static PsxXgRenderWorldNativeSnapshot snapshot;

typedef struct XgRenderWorldSkyRange {
    uint32_t start;
    uint32_t size;
} XgRenderWorldSkyRange;

static const XgRenderWorldSkyRange code_ranges[] = {
    { UINT32_C(0x80071b50), 0x18u },
    { UINT32_C(0x800736dc), 0x110u },
    { UINT32_C(0x800737ec), 0x1ccu },
    { UINT32_C(0x8009a280), 0x80u },
    { UINT32_C(0x80048ab0), 0x10cu },
    { UINT32_C(0x8004931c), 0x160u },
    { UINT32_C(0x80049efc), 0x30u },
    { UINT32_C(0x80049f8c), 0x20u },
    { UINT32_C(0x8004a12c), 0x18u },
    { UINT32_C(0x8004a14c), 0x0cu },
    { UINT32_C(0x8004a73c), 0x78u },
    { UINT32_C(0x8004a92c), 0x68u },
};

bool xg_render_world_sky_code_write_overlaps(
        uint32_t address, uint32_t size) {
    const uint64_t begin = address & UINT32_C(0x1fffffff);
    const uint64_t end = begin + size;

    if (size == 0u) return false;
    for (uint32_t index = 0u;
         index < sizeof(code_ranges) / sizeof(code_ranges[0]); ++index) {
        const uint64_t range_begin =
            code_ranges[index].start & UINT32_C(0x1fffffff);
        if (range_begin < end && begin < range_begin + code_ranges[index].size)
            return true;
    }
    return false;
}

void xg_render_world_sky_register_code_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    if (set_range == NULL) return;
    for (uint32_t index = 0u;
         index < sizeof(code_ranges) / sizeof(code_ranges[0]); ++index)
        set_range(code_ranges[index].start & UINT32_C(0x1fffffff),
                  code_ranges[index].size);
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool compute_fog(uint32_t mode, int32_t projection_distance,
                        int16_t *depth_cue_a, int32_t *depth_cue_b) {
    const int32_t near_distance = mode == 2u ? 0xb00 : 0x800;
    const int32_t far_distance = 0xe80;
    const int32_t range = far_distance - near_distance;
    int64_t value;

    if (depth_cue_a == NULL || depth_cue_b == NULL ||
        projection_distance == 0 || range <= 99)
        return false;
    value = ((int64_t)-near_distance * far_distance / range) * 256;
    value /= projection_distance;
    if (value < INT16_MIN) value = INT16_MIN;
    if (value > INT16_MAX) value = INT16_MAX;
    *depth_cue_a = (int16_t)value;
    value = ((int64_t)far_distance * 4096 / range) * 4096;
    *depth_cue_b = (int32_t)(uint32_t)value;
    return true;
}

void xg_render_world_sky_store_matrix(
        CPUState *cpu, uint32_t address, const XgHost3dMatrix *matrix) {
    const uint32_t words[8] = {
        (uint16_t)matrix->rotation[0][0] |
            ((uint32_t)(uint16_t)matrix->rotation[0][1] << 16u),
        (uint16_t)matrix->rotation[0][2] |
            ((uint32_t)(uint16_t)matrix->rotation[1][0] << 16u),
        (uint16_t)matrix->rotation[1][1] |
            ((uint32_t)(uint16_t)matrix->rotation[1][2] << 16u),
        (uint16_t)matrix->rotation[2][0] |
            ((uint32_t)(uint16_t)matrix->rotation[2][1] << 16u),
        (uint16_t)matrix->rotation[2][2] |
            ((uint32_t)matrix->pad << 16u),
        (uint32_t)matrix->translation[0],
        (uint32_t)matrix->translation[1],
        (uint32_t)matrix->translation[2],
    };

    for (uint32_t index = 0u; index < 8u; ++index) {
        psx_store_cycle_barrier();
        cpu->write_word(address + index * 4u, words[index]);
    }
}

bool xg_render_world_sky_cutover(
        CPUState *cpu, const XgRenderWorldSkyServices *services) {
    enum {
        WORLD_SKY_VERTEX_BASE = 0x8009a280u,
        WORLD_SKY_CAMERA_MATRIX = 0x8009c808u,
        WORLD_SKY_ANGLE = 0x8009bd3au,
        WORLD_SKY_PROJECTION_DISTANCE = 0x8009bcdcu,
        WORLD_SKY_SCREEN_OFFSET_Y = 0x8009be0cu,
        WORLD_SKY_CONTEXT = 0x8009be3cu,
        WORLD_SKY_BUFFER_INDEX = 0x8009d7f0u,
        WORLD_SKY_FOG_MODE = 0x8009d7ccu,
        WORLD_SKY_ORDERING_SHIFT = 0x80050100u,
        WORLD_SKY_OT_BUCKET_COUNT = 0x1000u,
    };
    XgWorldSkySource source = {0};
    XgWorldSkyRecord records[XG_WORLD_SKY_QUAD_COUNT];
    XgHost3dMatrix camera;
    XgHost3dMatrix local = {0};
    XgHost3dMatrix composed;
    GpuDrawState draw = {0};
    uint32_t packet_tags[XG_WORLD_SKY_QUAD_COUNT] = {0};
    uint32_t ot_addresses[XG_WORLD_SKY_QUAD_COUNT] = {0};
    uint32_t angle_word;
    uint32_t context;
    uint32_t ot_base;
    uint32_t projection_distance;
    uint32_t accepted_count = 0u;
    uint32_t quad;
    uint32_t vertex;

    if (services == NULL || services->authorize_native_dispatch == NULL ||
        services->authorize_guest_word == NULL ||
        services->stage_native == NULL || services->stage_temporal == NULL ||
        services->abort_submission == NULL ||
        !services->authorize_native_dispatch())
        return false;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071b60)) ||
        !xg_render_runtime_capture_matrix(
            cpu, WORLD_SKY_CAMERA_MATRIX, &camera))
        return false;

    angle_word = cpu->read_word(UINT32_C(0x800523f0) +
        ((uint32_t)cpu->read_half(WORLD_SKY_ANGLE) & 0xfffu) * 4u);
    local.rotation[0][0] = (int16_t)(angle_word >> 16u);
    local.rotation[0][2] = (int16_t)angle_word;
    local.rotation[1][1] = 0x1000;
    local.rotation[2][0] = (int16_t)-(int32_t)(int16_t)angle_word;
    local.rotation[2][2] = (int16_t)(angle_word >> 16u);
    if (!xg_host_3d_comp_matrix(&camera, &local, &composed)) return false;

    memcpy(source.projection.rotation, composed.rotation,
           sizeof(source.projection.rotation));
    memcpy(source.projection.translation, composed.translation,
           sizeof(source.projection.translation));
    source.projection.screen_offset_x = 160 << 16;
    source.projection.screen_offset_y = (int32_t)(
        cpu->read_word(WORLD_SKY_SCREEN_OFFSET_Y) << 16u);
    projection_distance = cpu->read_word(WORLD_SKY_PROJECTION_DISTANCE);
    if (projection_distance == 0u || projection_distance > UINT16_MAX ||
        !compute_fog(cpu->read_word(WORLD_SKY_FOG_MODE),
                     (int32_t)projection_distance,
                     &source.projection.depth_cue_a,
                     &source.projection.depth_cue_b))
        return false;
    source.projection.projection_distance = (uint16_t)projection_distance;
    source.ordering_shift = cpu->read_word(WORLD_SKY_ORDERING_SHIFT);
    source.buffer_index = cpu->read_word(WORLD_SKY_BUFFER_INDEX);
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(&source.material, &draw);
    source.material.shading = XG_RENDER_IR_SHADING_GOURAUD;

    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t address = WORLD_SKY_VERTEX_BASE +
                (quad * XG_HOST_3D_VERTEX_COUNT + vertex) * 8u;
            const uint32_t xy = cpu->read_word(address);
            const uint32_t zp = cpu->read_word(address + 4u);

            source.vertices[quad][vertex] = (XgHost3dVector){
                xg_render_runtime_low_s16(xy),
                xg_render_runtime_low_s16(xy >> 16u),
                xg_render_runtime_low_s16(zp), (uint16_t)(zp >> 16u),
            };
        }
    }
    if (xg_world_sky_build(&source, records) != XG_WORLD_SKY_OK) return false;

    context = cpu->read_word(WORLD_SKY_CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !services->authorize_guest_word(context + 0x70u))
        return false;
    ot_base = cpu->read_word(context + 0x70u);
    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        if (!records[quad].accepted) continue;
        ++accepted_count;
        if (records[quad].ordering_bucket >= WORLD_SKY_OT_BUCKET_COUNT ||
            ot_base > UINT32_MAX - records[quad].ordering_bucket * 4u ||
            !services->authorize_guest_word(records[quad].packet_address) ||
            !services->authorize_guest_word(records[quad].packet_address + 32u))
            return false;
        ot_addresses[quad] = ot_base + records[quad].ordering_bucket * 4u;
        if (!services->authorize_guest_word(ot_addresses[quad])) return false;
        packet_tags[quad] = cpu->read_word(records[quad].packet_address);
        if ((packet_tags[quad] >> 24u) != 8u) return false;
    }
    if (snapshot.native_cutover_count == UINT64_MAX ||
        snapshot.native_primitive_count > UINT64_MAX - accepted_count)
        return false;
    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        if (records[quad].accepted) {
            if (!services->stage_native(
                    &records[quad].primitive, records[quad].packet_address,
                    UINT32_C(0x50000000) |
                        (records[quad].packet_address & UINT32_C(0x001ffffc)),
                    UINT32_C(0x800737ec), quad)) {
                services->abort_submission();
                return false;
            }
        } else {
            const GpuRenderTemporalCullPolicy policy = {
                .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE,
            };

            if (!services->stage_temporal(
                    &records[quad].primitive, UINT32_C(0x800737ec), quad,
                    &policy)) {
                services->abort_submission();
                return false;
            }
        }
    }
    ++snapshot.native_cutover_count;
    snapshot.native_primitive_count += accepted_count;

    psx_store_cycle_barrier();
    cpu->write_word(UINT32_C(0x1f800000),
                    (uint32_t)cpu->read_half(WORLD_SKY_ANGLE) << 16u);
    psx_store_cycle_barrier();
    cpu->write_word(UINT32_C(0x1f800004), 0u);
    xg_render_world_sky_store_matrix(cpu, UINT32_C(0x1f800008), &composed);
    xg_render_world_sky_store_matrix(cpu, UINT32_C(0x1f800028), &local);
    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        uint32_t previous_head;

        if (!records[quad].accepted) continue;
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t xy =
                (uint16_t)records[quad].vertices[vertex].x |
                ((uint32_t)(uint16_t)records[quad].vertices[vertex].y << 16u);

            psx_store_cycle_barrier();
            cpu->write_word(records[quad].packet_address + 8u + vertex * 8u,
                            xy);
        }
        previous_head = cpu->read_word(ot_addresses[quad]);
        psx_store_cycle_barrier();
        cpu->write_word(records[quad].packet_address,
            (packet_tags[quad] & UINT32_C(0xff000000)) |
            (previous_head & UINT32_C(0x00ffffff)));
        psx_store_cycle_barrier();
        cpu->write_word(ot_addresses[quad],
            (previous_head & UINT32_C(0xff000000)) |
            (records[quad].packet_address & UINT32_C(0x00ffffff)));
    }
    psx_store_cycle_barrier();
    cpu->write_word(UINT32_C(0x1f800048),
        records[XG_WORLD_SKY_QUAD_COUNT - 1u].vertices[3].z >> 2u);
    psx_store_cycle_barrier();
    cpu->write_word(UINT32_C(0x1f80004c),
        records[XG_WORLD_SKY_QUAD_COUNT - 1u].projection_flags);
    cpu->gpr[2] = records[XG_WORLD_SKY_QUAD_COUNT - 1u].vertices[3].z >> 2u;
    cpu->pc = cpu->gpr[31];
    return true;
}

void xg_render_world_sky_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = snapshot;
}

void xg_render_world_sky_reset(void) {
    snapshot = (PsxXgRenderWorldNativeSnapshot){0};
}

void xg_render_world_sky_classify_code_write(
        uint32_t address, uint32_t size,
        XgRenderMutationClassification *out_classification) {
    const bool overlaps =
        xg_render_world_sky_code_write_overlaps(address, size);

    if (out_classification == NULL) return;
    *out_classification = (XgRenderMutationClassification){
        .properties = {
            .watched_range_mutation = overlaps,
            .runtime_variant_mutation = overlaps,
            .executable_mutation = overlaps,
            .authentication_mutation = overlaps,
            .authority_loss = overlaps,
            .interpolation_reset = overlaps,
            .reset_runtime_variant = overlaps,
        },
        .code_write_mask = overlaps
            ? UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_SKY : 0u,
    };
}

void xg_render_world_sky_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_render_world_sky_reset();
}
