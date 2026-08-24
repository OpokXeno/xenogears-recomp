#ifndef XG_RENDER_AUTH_RUNTIME_TEST_ADAPTER_H
#define XG_RENDER_AUTH_RUNTIME_TEST_ADAPTER_H

#include "cpu_state.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void psx_xg_render_auth_runtime_test_reset(void);
#ifdef XG_RENDER_RUNTIME_COMPOSITION_TESTING
void psx_xg_render_auth_runtime_test_prepare_host(void);
void psx_xg_render_auth_runtime_test_fail_registration_after(
    uint32_t successful_registrations);
void psx_xg_render_auth_runtime_test_clear_registration_failure(void);
void psx_xg_render_auth_runtime_test_clear_invalidation_configuration(void);
#endif
void psx_xg_render_auth_runtime_test_enable_ui_ot_gpu(bool enabled);
uint64_t psx_xg_render_auth_runtime_test_interpolation_scene(void);
uint64_t psx_xg_render_auth_runtime_test_artifact_generation(void);
bool psx_xg_render_auth_runtime_test_artifact_active(void);
uint32_t psx_xg_render_auth_runtime_test_pre_scene_count(void);
bool psx_xg_render_auth_runtime_test_resource_write_may_overlap(
    uint32_t address, uint32_t size);
uint64_t psx_xg_render_auth_runtime_test_particle_generation(
    uint32_t particle_address);
bool psx_xg_render_auth_runtime_test_model_ft4_packet_template_present(
    uint32_t packet_address);
bool psx_xg_render_auth_runtime_test_model_ft4_descriptor_template_present(
    uint32_t descriptor_address);
void psx_xg_render_auth_runtime_test_watch_resource(
    uint32_t address, uint32_t size);
bool psx_xg_render_auth_runtime_test_materialize_world_models_original(
    CPUState *cpu);
bool psx_xg_render_auth_runtime_test_materialize_world_actor_original(
    CPUState *cpu);
bool psx_xg_render_auth_particle_test_primitive(
    XgRenderIrNativePrimitive *out_primitive);
bool psx_xg_render_auth_particle_test_source_present(
    uint32_t particle_address);
bool psx_xg_render_auth_projected_test_source_present(
    uint32_t object_address);
uint32_t psx_xg_render_auth_zoom_test_primitives(
    XgRenderIrNativePrimitive *out_primitives, uint32_t capacity);
uint32_t psx_xg_render_auth_projected_test_primitives(
    XgRenderIrNativePrimitive *out_primitives, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
