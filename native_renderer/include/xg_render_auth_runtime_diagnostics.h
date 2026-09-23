#ifndef XG_RENDER_AUTH_RUNTIME_DIAGNOSTICS_H
#define XG_RENDER_AUTH_RUNTIME_DIAGNOSTICS_H

#include "xg_render_auth_diagnostics_types.h"
#include "xg_render_semantic_presentation.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PsxXgRenderMovieOwnerStartBlocker {
    PSX_XG_RENDER_MOVIE_OWNER_START_OK = 0,
    PSX_XG_RENDER_MOVIE_OWNER_START_ARTIFACT,
    PSX_XG_RENDER_MOVIE_OWNER_START_CPU,
    PSX_XG_RENDER_MOVIE_OWNER_START_STACK,
    PSX_XG_RENDER_MOVIE_OWNER_START_CALLBACK_ADDRESS,
    PSX_XG_RENDER_MOVIE_OWNER_START_CALLBACK_TARGET,
    PSX_XG_RENDER_MOVIE_OWNER_START_GPU,
} PsxXgRenderMovieOwnerStartBlocker;

typedef enum PsxXgRenderTimRouteBlocker {
    PSX_XG_RENDER_TIM_ROUTE_OK = 0,
    PSX_XG_RENDER_TIM_ROUTE_AUTHORITY,
    PSX_XG_RENDER_TIM_ROUTE_TRANSACTION,
    PSX_XG_RENDER_TIM_ROUTE_RESOURCE,
} PsxXgRenderTimRouteBlocker;

typedef struct PsxXgRenderTimRouteDiagnostics {
    uint64_t attempts[4];
    uint64_t authorized[4];
    uint64_t successes[4];
    uint32_t last_result[4];
    uint32_t last_blocker[4];
} PsxXgRenderTimRouteDiagnostics;

typedef struct PsxXgRenderMovieOwnerDiagnostics {
    uint64_t start_attempts;
    uint64_t start_successes;
    uint64_t frame_complete_attempts;
    uint64_t frame_complete_successes;
    uint64_t vram_frame_events;
    uint64_t publication_successes;
    uint64_t publication_failures;
    uint32_t owner_kind;
    uint32_t expected_callback;
    uint32_t observed_callback;
    uint32_t frame_number;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t frame_callback;
    uint32_t event_artifact_base;
    uint32_t event_artifact_size;
    uint32_t publication_blocker;
    uint32_t publication_blocker_detail;
    uint32_t event_publication_count;
    uint32_t event_movie_edge_count;
    uint64_t event_coverage_bytes;
    uint64_t mdec_surface_successes;
    uint64_t mdec_surface_failures;
    uint32_t mdec_surface_x;
    uint32_t mdec_surface_y;
    uint32_t mdec_surface_width;
    uint32_t mdec_surface_height;
    uint32_t scanout_x;
    uint32_t scanout_y;
    uint32_t scanout_width;
    uint32_t scanout_height;
    uint32_t source_active_after_publication;
    uint32_t source_passes_after_publication;
    uint32_t source_active_before_boundary;
    uint32_t source_passes_before_boundary;
    uint64_t publication_boundaries;
    uint64_t publication_boundaries_active;
    uint32_t publication_boundary_completion_result;
    uint32_t publication_boundary_publish_result;
    PsxXgRenderMovieOwnerStartBlocker start_blocker;
} PsxXgRenderMovieOwnerDiagnostics;

void psx_xg_render_auth_ui_ot_snapshot(PsxXgRenderUiOtSnapshot *out_snapshot);
void psx_xg_render_auth_source_snapshot(
    PsxXgRenderSourceSnapshot *out_snapshot);
void psx_xg_render_auth_source_collector_snapshot(
    FieldCharacterShadowSummary *out_summary);
void psx_xg_render_auth_ft4_geometry_snapshot(
    PsxXgRenderFt4GeometrySnapshot *out_snapshot);
void psx_xg_render_auth_zoom_template_contract_snapshot(
    PsxXgRenderZoomTemplateContractSnapshot *out_snapshot);
size_t psx_xg_render_auth_pre_scene_snapshot(
    PsxXgRenderPreScenePrimitiveSnapshot *out_snapshots, size_t capacity);
size_t psx_xg_render_auth_field_fragment_snapshot(
    PsxXgRenderPreScenePrimitiveSnapshot *out_snapshots, size_t capacity);
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
/* Last staged Field actor card pieces, oldest first. Returns the count. */
size_t psx_xg_render_auth_sprite_cards(
    PsxXgRenderSpriteCardDiagnostic *out, size_t capacity);
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
void psx_xg_render_auth_resident_text_snapshot(
    PsxXgRenderResidentTextSnapshot *out_snapshot);
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
void psx_xg_render_auth_static_artifact_diagnostics(
    uint64_t *out_attempts, uint64_t *out_successes,
    uint32_t *out_last_pc, uint32_t *out_last_blocker);
void psx_xg_render_auth_tim_route_diagnostics(
    PsxXgRenderTimRouteDiagnostics *out_diagnostics);
void psx_xg_render_auth_presentation_snapshot(
    XgRenderPresentationDiagnostics *out_snapshot);
void psx_xg_render_auth_movie_owner_diagnostics(
    PsxXgRenderMovieOwnerDiagnostics *out_diagnostics);
const char *psx_xg_render_auth_rejection_source_name(uint32_t source);
const char *psx_xg_render_auth_hook_name(uint32_t hook);

#ifdef __cplusplus
}
#endif

#endif
