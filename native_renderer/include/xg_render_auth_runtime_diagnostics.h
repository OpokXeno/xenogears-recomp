#ifndef XG_RENDER_AUTH_RUNTIME_DIAGNOSTICS_H
#define XG_RENDER_AUTH_RUNTIME_DIAGNOSTICS_H

#include "xg_render_auth_diagnostics_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void psx_xg_render_auth_ui_ot_snapshot(PsxXgRenderUiOtSnapshot *out_snapshot);
void psx_xg_render_auth_source_snapshot(
    PsxXgRenderSourceSnapshot *out_snapshot);
void psx_xg_render_auth_source_collector_snapshot(
    FieldCharacterShadowSummary *out_summary);
void psx_xg_render_auth_ft4_geometry_snapshot(
    PsxXgRenderFt4GeometrySnapshot *out_snapshot);
void psx_xg_render_auth_zoom_template_contract_snapshot(
    PsxXgRenderZoomTemplateContractSnapshot *out_snapshot);
void psx_xg_render_auth_overlay_ft4_snapshot(
    PsxXgRenderOverlayFt4Snapshot *out_snapshot);
void psx_xg_render_auth_producer_family_snapshot(
    PsxXgRenderProducerFamilySnapshot *out_snapshot);
void psx_xg_render_auth_projected_lifecycle_snapshot(
    PsxXgRenderProjectedLifecycleSnapshot *out_snapshot);
void psx_xg_render_auth_model_ft4_shadow_snapshot(
    PsxXgRenderModelFt4ShadowSnapshot *out_snapshot);
void psx_xg_render_auth_model_ft3_shadow_snapshot(
    PsxXgRenderModelFt3ShadowSnapshot *out_snapshot);
void psx_xg_render_auth_sprite_ft4_shadow_snapshot(
    PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot);
void psx_xg_render_auth_field_polyline_snapshot(
    PsxXgRenderFieldPolylineSnapshot *out_snapshot);
void psx_xg_render_auth_world_horizon_shadow_snapshot(
    PsxXgRenderWorldHorizonShadowSnapshot *out_snapshot);
void psx_xg_render_auth_world_effects_shadow_snapshot(
    PsxXgRenderWorldEffectsShadowSnapshot *out_snapshot);
void psx_xg_render_auth_world_terrain_water_shadow_snapshot(
    PsxXgRenderWorldTerrainWaterShadowSnapshot *out_snapshot);
void psx_xg_render_auth_world_entity_shadows_shadow_snapshot(
    PsxXgRenderWorldEntityShadowsShadowSnapshot *out_snapshot);
void psx_xg_render_auth_world_decorations_shadow_snapshot(
    PsxXgRenderWorldDecorationsShadowSnapshot *out_snapshot);
void psx_xg_render_auth_world_clouds_shadow_snapshot(
    PsxXgRenderWorldCloudsShadowSnapshot *out_snapshot);
void psx_xg_render_auth_world_minimap_shadow_snapshot(
    PsxXgRenderWorldMinimapShadowSnapshot *out_snapshot);
void psx_xg_render_auth_world_models_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
void psx_xg_render_auth_world_actor_sprites_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
void psx_xg_render_auth_world_sky_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
void psx_xg_render_auth_world_execution_snapshot(
    PsxXgRenderWorldExecutionSnapshot *out_snapshot);
bool psx_xg_render_auth_authenticated_producer_entry(
    uint32_t *out_producer_entry);
void psx_xg_render_auth_provenance_snapshot(
    PsxXgRenderAuthProvenance *out_provenance);
void psx_xg_render_auth_rejection_snapshot(
    PsxXgRenderAuthRejectionReceipt *out_receipt);
void psx_xg_render_auth_completed_proof_snapshot(
    PsxXgRenderAuthCompletedProofReceipt *out_receipt);
void psx_xg_render_auth_instrumentation_snapshot(
    PsxXgRenderAuthInstrumentation *out_instrumentation);
void psx_xg_render_auth_mode_snapshot(
    PsxXgRenderModeSnapshot *out_snapshot);
void psx_xg_render_auth_runtime_snapshot(
    PsxXgRenderAuthRuntimeSnapshot *out_snapshot);
const char *psx_xg_render_auth_rejection_source_name(uint32_t source);
const char *psx_xg_render_auth_hook_name(uint32_t hook);

#ifdef __cplusplus
}
#endif

#endif
