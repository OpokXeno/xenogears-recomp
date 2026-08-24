#include "xg_render_auth_runtime_test_adapter.h"

#include "gpu.h"
#include "xg_field_particles.h"
#include "xg_field_projected.h"
#include "xg_render_auth.h"
#include "xg_render_auth_runtime_control.h"
#include "xg_render_auth_runtime_diagnostics.h"
#include "xg_render_invalidation_dispatch.h"
#include "xg_render_model_repository.h"
#include "xg_render_mutation_classifier.h"
#include "xg_render_resource_watch.h"
#include "xg_render_runtime_composition.h"
#include "xg_render_runtime_host_services.h"
#include "xg_render_submission.h"
#include "xg_render_world_models_pipeline.h"
#include "xg_render_world_pending_services.h"
#include "xg_world_models_native.h"

#include <stddef.h>
#include <string.h>

enum { PRIMITIVE_SNAPSHOT_CAPACITY = 32u };

typedef struct PrimitiveSnapshot {
    XgRenderIrNativePrimitive primitive;
    uint32_t source_primitive_index;
} PrimitiveSnapshot;

static PrimitiveSnapshot primitive_snapshots[PRIMITIVE_SNAPSHOT_CAPACITY];
static uint32_t primitive_snapshot_count;
static bool ui_ot_gpu_enabled;

static void observe_staged_primitive(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t source_primitive_index, void *user_data) {
    (void)user_data;
    if (primitive_snapshot_count == PRIMITIVE_SNAPSHOT_CAPACITY) {
        memmove(primitive_snapshots, primitive_snapshots + 1u,
                (PRIMITIVE_SNAPSHOT_CAPACITY - 1u) *
                    sizeof(primitive_snapshots[0]));
        --primitive_snapshot_count;
    }
    primitive_snapshots[primitive_snapshot_count++] = (PrimitiveSnapshot){
        .primitive = *primitive,
        .source_primitive_index = source_primitive_index,
    };
}

static uint64_t test_frame_count(void) {
    return 0u;
}

static uint32_t test_read_word(uint32_t address) {
    (void)address;
    return 0u;
}

static void configure_host_services(void) {
    const XgRenderRuntimeHostServices services = {
        .frame_count = test_frame_count,
        .read_word = test_read_word,
    };

    (void)xg_render_runtime_configure_host_services(&services);
}

#ifdef XG_RENDER_RUNTIME_COMPOSITION_TESTING
void psx_xg_render_auth_runtime_test_prepare_host(void) {
    configure_host_services();
}

void psx_xg_render_auth_runtime_test_fail_registration_after(
        uint32_t successful_registrations) {
    xg_render_runtime_composition_test_fail_registration_after(
        successful_registrations);
}

void psx_xg_render_auth_runtime_test_clear_registration_failure(void) {
    xg_render_runtime_composition_test_clear_registration_failure();
}

void psx_xg_render_auth_runtime_test_clear_invalidation_configuration(void) {
    xg_render_invalidation_clear_modules();
    xg_render_mutation_classifier_clear_sources();
}
#endif

void psx_xg_render_auth_runtime_test_reset(void) {
    configure_host_services();
    primitive_snapshot_count = 0u;
    ui_ot_gpu_enabled = false;
    xg_render_submission_set_observer(observe_staged_primitive, NULL);
    psx_xg_render_auth_reset();
}

void psx_xg_render_auth_runtime_test_enable_ui_ot_gpu(bool enabled) {
    ui_ot_gpu_enabled = enabled;
}

uint64_t psx_xg_render_auth_runtime_test_interpolation_scene(void) {
    PsxXgRenderAuthRuntimeSnapshot snapshot;
    psx_xg_render_auth_runtime_snapshot(&snapshot);
    return snapshot.interpolation_scene_generation;
}

uint64_t psx_xg_render_auth_runtime_test_artifact_generation(void) {
    PsxXgRenderAuthRuntimeSnapshot snapshot;
    psx_xg_render_auth_runtime_snapshot(&snapshot);
    return snapshot.authenticated_artifact_generation;
}

bool psx_xg_render_auth_runtime_test_artifact_active(void) {
    PsxXgRenderAuthRuntimeSnapshot snapshot;
    psx_xg_render_auth_runtime_snapshot(&snapshot);
    return snapshot.authenticated_artifact_active;
}

uint32_t psx_xg_render_auth_runtime_test_pre_scene_count(void) {
    return xg_render_submission_pre_scene_count();
}

bool psx_xg_render_auth_runtime_test_resource_write_may_overlap(
        uint32_t address, uint32_t size) {
    return xg_render_resource_watch_overlaps(address, size);
}

uint64_t psx_xg_render_auth_runtime_test_particle_generation(
        uint32_t particle_address) {
    XgRenderParticleSource source;
    return xg_field_particles_lookup(particle_address, &source)
        ? source.generation : 0u;
}

bool psx_xg_render_auth_runtime_test_model_ft4_packet_template_present(
        uint32_t packet_address) {
    return xg_render_model_repository_packet_template_present(packet_address);
}

bool psx_xg_render_auth_runtime_test_model_ft4_descriptor_template_present(
        uint32_t descriptor_address) {
    return xg_render_model_repository_descriptor_template_present(
        descriptor_address);
}

void psx_xg_render_auth_runtime_test_watch_resource(
        uint32_t address, uint32_t size) {
    xg_render_resource_watch_add(address, size);
}

static uint32_t copy_family_primitives(
        uint32_t family, XgRenderIrNativePrimitive *out_primitives,
        uint32_t capacity) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot auth_snapshot;
    uint32_t count = 0u;

    if (out_primitives == NULL || capacity == 0u) return 0u;
    for (uint32_t index = 0u; index < primitive_snapshot_count; ++index) {
        const PrimitiveSnapshot *snapshot = &primitive_snapshots[index];
        if ((snapshot->source_primitive_index & UINT32_C(0xf0000000)) != family)
            continue;
        if (count == capacity) {
            memmove(out_primitives, out_primitives + 1u,
                    (capacity - 1u) * sizeof(out_primitives[0]));
            --count;
        }
        out_primitives[count++] = snapshot->primitive;
    }
    if (count != 0u) return count;
    if (xg_render_auth_process_owner(&auth) != XG_RENDER_AUTH_OK ||
        xg_render_auth_snapshot(auth, &auth_snapshot) != XG_RENDER_AUTH_OK)
        return 0u;
    for (size_t index = 0u; index < auth_snapshot.native_item_count; ++index) {
        XgRenderIrNativeItem item;
        if (xg_render_auth_native_item_get(auth, index, &item) !=
                XG_RENDER_AUTH_OK ||
            (item.base.source_primitive_index & UINT32_C(0xf0000000)) != family)
            continue;
        if (count == capacity) {
            memmove(out_primitives, out_primitives + 1u,
                    (capacity - 1u) * sizeof(out_primitives[0]));
            --count;
        }
        out_primitives[count++] = item.native;
    }
    if (count != 0u) return count;
    for (uint32_t index = 0u;
         index < xg_render_submission_pre_scene_count(); ++index) {
        XgRenderPreScenePrimitive record;
        if (!xg_render_submission_pre_scene_item_copy(index, &record) ||
            (record.source_primitive_index & UINT32_C(0xf0000000)) != family)
            continue;
        if (count == capacity) {
            memmove(out_primitives, out_primitives + 1u,
                    (capacity - 1u) * sizeof(out_primitives[0]));
            --count;
        }
        out_primitives[count++] = record.primitive;
    }
    return count;
}

bool psx_xg_render_auth_particle_test_primitive(
        XgRenderIrNativePrimitive *out_primitive) {
    return copy_family_primitives(
        UINT32_C(0x20000000), out_primitive, 1u) == 1u;
}

bool psx_xg_render_auth_particle_test_source_present(
        uint32_t particle_address) {
    return xg_field_particles_lookup(particle_address, NULL);
}

bool psx_xg_render_auth_projected_test_source_present(
        uint32_t object_address) {
    return xg_field_projected_lookup(object_address, NULL);
}

uint32_t psx_xg_render_auth_zoom_test_primitives(
        XgRenderIrNativePrimitive *out_primitives, uint32_t capacity) {
    return copy_family_primitives(
        UINT32_C(0x30000000), out_primitives, capacity);
}

uint32_t psx_xg_render_auth_projected_test_primitives(
        XgRenderIrNativePrimitive *out_primitives, uint32_t capacity) {
    return copy_family_primitives(
        UINT32_C(0x40000000), out_primitives, capacity);
}

bool psx_xg_render_auth_runtime_test_materialize_world_models_original(
        CPUState *cpu) {
    XgWorldModelsNativePreparation preparation;
    XgWorldModelsNativeCommit commit;
    CPUState *owner_cpu;
    uint32_t entry_stack_pointer;

    if (cpu == NULL || cpu->write_word == NULL || cpu->write_half == NULL ||
        !xg_render_world_models_pending_metadata_copy(
            &owner_cpu, &entry_stack_pointer, &preparation, &commit) ||
        owner_cpu != cpu || entry_stack_pointer < 0x40u)
        return false;
    cpu->gpr[29] = entry_stack_pointer - 0x40u;
    cpu->write_word(cpu->gpr[29] + 0x38u, commit.continuation_pc);
    for (uint32_t index = 0u; index < 3u; ++index) {
        cpu->write_word(
            XG_WORLD_MODELS_SCALE_X_SCRATCH + index * 4u,
            commit.entry_side_effects.scratch_scale[index]);
        cpu->write_half(
            XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH + index * 2u,
            (uint16_t)commit.entry_side_effects.coarse_origin[index]);
    }
    cpu->write_word(XG_WORLD_MODELS_RESIDENT_CULL_MODE_GLOBAL,
                    commit.entry_side_effects.resident_cull_mode);
    cpu->write_word(XG_WORLD_MODELS_RESIDENT_VERTEX_TOTAL_GLOBAL,
                    commit.resident_vertex_total);
    cpu->write_word(XG_WORLD_MODELS_RESIDENT_EMITTED_COUNT_GLOBAL,
                    commit.resident_emitted_count);
    for (uint32_t index = 0u; index < preparation.transform_node_count;
         ++index) {
        XgWorldModelsNodeSideEffect effect;
        if (!xg_render_world_models_pending_node_copy(index, &effect))
            return false;
        for (uint32_t component = 0u; component < 3u; ++component)
            cpu->write_word(
                effect.guest_address + XG_WORLD_MODELS_NODE_WRITEBACK_X_OFFSET +
                    component * 4u,
                (uint32_t)effect.translation[component]);
    }
    for (uint32_t index = 0u; index < preparation.primitive_count; ++index) {
        uint32_t words[XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT];
        uint32_t address;
        uint32_t word_count;
        uint32_t write_mask;
        if (!xg_render_world_models_pending_packet_copy(
                index, &address, words, XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT,
                &word_count, &write_mask))
            return false;
        for (uint32_t word = 0u; word < word_count; ++word) {
            if ((write_mask & (UINT32_C(1) << word)) != 0u)
                cpu->write_word(address + word * 4u, words[word]);
        }
    }
    for (uint32_t index = 0u; index < XG_WORLD_MODELS_OT_BUCKET_COUNT;
         ++index) {
        uint32_t address;
        uint32_t value;
        if (xg_render_world_models_pending_ot_copy(
                index, &address, &value))
            cpu->write_word(address, value);
    }
    if (commit.resident_dispatch_globals_written) {
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL,
                        commit.resident_packet_cursor);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_GROUP_CURSOR_GLOBAL,
                        commit.resident_group_cursor);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_MODEL_0C_GLOBAL,
                        commit.resident_model_0c);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_VERTEX_BASE_GLOBAL,
                        commit.resident_vertex_base);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_OT_BASE_GLOBAL,
                        commit.resident_ot_base);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_MODEL_18_GLOBAL,
                        commit.resident_model_18);
    }
    return true;
}

static void materialize_actor_scratch(
        CPUState *cpu, const XgWorldActorSpritesScratchOutput *scratch) {
    if (!scratch->written) return;
    for (uint32_t index = 0u; index < XG_HOST_3D_VERTEX_COUNT; ++index) {
        const XgHost3dVector *vertex = &scratch->vertices[index];
        const uint32_t address = scratch->address + index * 8u;
        cpu->write_half(address, (uint16_t)vertex->x);
        cpu->write_half(address + 2u, (uint16_t)vertex->y);
        cpu->write_half(address + 4u, (uint16_t)vertex->z);
        cpu->write_half(address + 6u, vertex->pad);
    }
}

bool psx_xg_render_auth_runtime_test_materialize_world_actor_original(
        CPUState *cpu) {
    XgWorldActorSpritesNativePreparation preparation;
    CPUState *owner_cpu;

    if (cpu == NULL || cpu->write_word == NULL || cpu->write_half == NULL ||
        !xg_render_world_actor_pending_metadata_copy(
            &owner_cpu, &preparation) || owner_cpu != cpu)
        return false;
    if (preparation.packet_cursor_written)
        cpu->write_word(XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR,
                        preparation.final_packet_cursor);
    for (uint32_t index = 0u; index < preparation.record_count; ++index) {
        uint32_t payload[XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT];
        uint32_t address;
        uint32_t tag;
        if (!xg_render_world_actor_pending_packet_copy(
                index, &address, &tag, payload,
                XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT))
            return false;
        cpu->write_word(address, tag);
        for (uint32_t word = 0u;
             word < XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT; ++word)
            cpu->write_word(address + 4u + word * 4u, payload[word]);
    }
    for (uint32_t index = 0u;
         index < XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT; ++index) {
        uint32_t address;
        uint32_t value;
        if (xg_render_world_actor_pending_ot_copy(index, &address, &value))
            cpu->write_word(address, value);
    }
    materialize_actor_scratch(cpu, &preparation.body_scratch);
    materialize_actor_scratch(cpu, &preparation.shadow_scratch);
    return true;
}

int gpu_gp0_command_word_count(uint8_t opcode) {
    return ui_ot_gpu_enabled && opcode == 0x20u ? 4 : 0;
}

int gpu_native_semantic_from_gp0(
        const uint32_t *words, int word_count,
        const GpuNativeDrawEnvironment *environment, GpuRenderSemantic *out) {
    if (!ui_ot_gpu_enabled || words == NULL || word_count != 4 ||
        environment == NULL || out == NULL || (words[0] >> 24u) != 0x20u)
        return 0;
    memset(out, 0, sizeof(*out));
    out->material.draw_area_left = environment->draw.left;
    out->material.draw_area_top = environment->draw.top;
    out->material.draw_area_right = environment->draw.right;
    out->material.draw_area_bottom = environment->draw.bottom;
    out->material.draw_offset_x = environment->draw.offset_x;
    out->material.draw_offset_y = environment->draw.offset_y;
    out->material.texture_depth = GPU_RENDER_TEXTURE_4_BIT;
    out->material.shading = GPU_RENDER_SHADING_FLAT;
    out->triangle_count = 1u;
    out->triangles[0].split_count = 1u;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        out->triangles[0].vertices[vertex].r = (uint8_t)words[0];
        out->triangles[0].vertices[vertex].g = (uint8_t)(words[0] >> 8u);
        out->triangles[0].vertices[vertex].b = (uint8_t)(words[0] >> 16u);
    }
    return 1;
}

int gpu_native_line_semantic_from_gp0(
        const uint32_t *words, size_t word_count,
        const GpuNativeDrawEnvironment *environment, GpuRenderSemantic *out) {
    return gpu_native_semantic_from_gp0(
        words, (int)word_count, environment, out);
}

void gpu_native_environment_apply(
        const uint32_t *words, int word_count,
        GpuNativeDrawEnvironment *environment) {
    (void)words;
    (void)word_count;
    (void)environment;
}
