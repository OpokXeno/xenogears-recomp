#ifndef XG_RENDER_RUNTIME_COMPOSITION_H
#define XG_RENDER_RUNTIME_COMPOSITION_H

#include "cpu_state.h"
#include "guest_render_types.h"
#include "gpu_render.h"
#include "xg_render_auth_types.h"
#include "xg_render_ir.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_source_types.h"
#include "xg_render_invalidation_event.h"
#include "xg_render_mutation_classifier.h"
#include "xg_render_route_descriptor.h"
#include "xg_render_resource_repository.h"
#include "xg_render_source_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XgRenderRuntimeAuthSceneState {
    GuestRenderRenderMode render_mode;
    XgRenderAuthTier pending_tier;
    uint64_t scene_generation;
    uint64_t interpolation_generation;
    uint64_t artifact_generation;
    XgRenderResourceProvenance artifact_provenance;
    uint32_t pending_producer_entry;
    bool armed;
    bool active;
    bool completed;
    bool movie_owner_active;
    bool pending_sequence;
    bool pending_capture_ready;
    bool candidate_matched;
    bool candidate_dispatched;
} XgRenderRuntimeAuthSceneState;

typedef struct XgRenderArtifactAuthority {
    uint64_t generation;
    XgRenderResourceProvenance provenance;
} XgRenderArtifactAuthority;

typedef struct PsxXgRenderTimRouteDiagnostics PsxXgRenderTimRouteDiagnostics;

typedef struct XgRenderRuntimeAuthSceneServices {
    void (*query_state)(XgRenderRuntimeAuthSceneState *out_state);
    bool (*auth_snapshot)(XgRenderAuthSnapshot *out_snapshot);
    bool (*auth_ir_item_get)(size_t index, XgRenderIrNativeItem *out_item);
    bool (*append_authenticated_ir)(
        uint32_t packet_address, uint32_t source_primitive_index,
        uint32_t ot_bucket, uint8_t payload_word_count,
        const XgRenderIrNativePrimitive *primitive,
        bool force_pending_capture, uint32_t *out_failure_detail);
    bool (*prepare_source_target)(
        const XgRenderSourceFrameDescription *description,
        XgSemanticResourceRef *out_target);
    bool (*standalone_source_identity)(
        XgSemanticSceneIdentity *out_identity,
        uint32_t *out_scene_generation);
    bool (*retained_movie_surface)(
        XgSemanticResourceRef *out_surface);
    bool (*artifact_authority_for_pc)(
        uint32_t pc, XgRenderArtifactAuthority *out_authority);
    bool (*static_artifact_authority_for_cutover)(
        uint32_t pc, uint32_t instruction_word,
        XgRenderArtifactAuthority *out_authority);
    bool (*artifact_authorizes_pc)(uint32_t pc);
    bool (*native_text_authorizes_pc)(uint32_t owner_entry);
    bool (*artifact_is_authorized)(void);
    bool (*completed_proof_matches_tier)(XgRenderAuthTier tier);
    void (*reject_auth)(uint32_t blocker);
    bool (*presentation_gate)(void);
    uint64_t (*frame_count)(void);
    uint32_t (*read_guest_word)(uint32_t address);
} XgRenderRuntimeAuthSceneServices;

typedef enum XgRenderRuntimeCompositionResult {
    XG_RENDER_RUNTIME_COMPOSITION_OBSERVED = 0,
    XG_RENDER_RUNTIME_COMPOSITION_BYPASS,
} XgRenderRuntimeCompositionResult;

bool xg_render_runtime_composition_configure(
    const XgRenderRuntimeAuthSceneServices *services);
#ifdef XG_RENDER_RUNTIME_COMPOSITION_TESTING
void xg_render_runtime_composition_test_fail_registration_after(
    uint32_t successful_registrations);
void xg_render_runtime_composition_test_clear_registration_failure(void);
#endif
void xg_render_runtime_composition_reset(void);
bool xg_render_runtime_composition_is_transition_mask(
    const GpuRenderSemantic *semantic);
void xg_render_runtime_composition_tim_route_diagnostics(
    PsxXgRenderTimRouteDiagnostics *out_diagnostics);
XgRenderRuntimeCompositionResult xg_render_runtime_composition_observe_dispatch(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word);
bool xg_render_runtime_composition_cutover_pc_relevant(uint32_t pc);
bool xg_render_runtime_composition_cutover_post_pc_relevant(uint32_t pc);
bool xg_render_runtime_composition_overlay_relevant(
    uint32_t pc, uint32_t instruction_word);
bool xg_render_runtime_composition_flush_authenticated_ir(void);
bool xg_render_runtime_composition_observe_auth_hook(
    CPUState *cpu, XgRenderAuthTier tier, uint32_t hook, uint32_t pc,
    uint32_t instruction_word, uint32_t auxiliary);
XgRenderHookRouteKind xg_render_runtime_composition_hook_route_kind(
    uint32_t hook, uint32_t pc, uint32_t instruction_word);
bool xg_render_runtime_composition_source_site_lookup(
    uint32_t pc, uint32_t instruction_word,
    PsxXgRenderSourceSiteMetadata *out_metadata);
bool xg_render_runtime_composition_source_pc_relevant(uint32_t pc);
bool xg_render_runtime_composition_source_observe(
    CPUState *cpu, XgRenderAuthTier tier, PsxXgRenderSourceStage stage,
    uint32_t pc, uint32_t instruction_word, uint32_t auxiliary);
bool xg_render_runtime_composition_resident_ft4_observe(
    CPUState *cpu, uint32_t stage, uint32_t pc, uint32_t instruction_word);
void xg_render_runtime_composition_capture_model_ft3_link(CPUState *cpu);
void xg_render_runtime_composition_capture_clear_tile(CPUState *cpu);
void xg_render_runtime_composition_capture_logo_sprite(
    uint32_t command_address, uint8_t color);
void xg_render_runtime_composition_capture_tile_write(
    CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
    uint8_t color);

bool xg_render_runtime_composition_authority_authorizes_pc(uint32_t pc);
bool xg_render_runtime_composition_pending_authorizes_pc(uint32_t pc);
void xg_render_runtime_composition_classify_code_write(
    uint32_t address, uint32_t size,
    const XgRenderMutationContext *context,
    XgRenderMutationClassification *out_classification);
bool xg_render_runtime_composition_resource_write_may_overlap(
    uint32_t address, uint32_t size);
bool xg_render_runtime_composition_resource_write_needs_invalidation(
    uint32_t address, uint32_t size);
void xg_render_runtime_composition_handle_invalidation(
    const XgRenderInvalidationEvent *event);
void xg_render_runtime_composition_invalidation_counts(
    uint64_t *out_counts, size_t capacity,
    uint64_t *out_mutation_counts, size_t mutation_capacity);
void xg_render_runtime_composition_configure_invalidation(void);
void xg_render_runtime_composition_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));

bool xg_render_runtime_composition_configure_native_view(
    bool enabled, uint16_t aspect_num, uint16_t aspect_den,
    uint16_t canonical_width, uint16_t canonical_height);
void xg_render_runtime_composition_set_terrain_temporal_coverage(bool enabled);
void xg_render_runtime_composition_set_exec_phase_exchange(
    int (*exchange)(int phase));
void xg_render_runtime_composition_before_gpu_submission(void);
void xg_render_runtime_composition_set_native_work_mode(bool enabled);
void xg_render_runtime_composition_native_work_view(XgSemanticDisplayState *display);
void xg_render_runtime_composition_prepare_gpu_source_boundary(void);
void xg_render_runtime_composition_note_gpu_semantic_current(
    const GpuRenderSemantic *semantic);
XgRenderSourceFrameResult
xg_render_runtime_composition_complete_gpu_source_frame(void);
bool xg_render_runtime_composition_ensure_source_frame(
    const XgRenderSourceFrameDescription *description);
bool xg_render_runtime_composition_begin_source_frame(
    const XgRenderSourceFrameDescription *description,
    XgSemanticResourceRef *out_target);
bool xg_render_runtime_composition_prepare_ui_ot(uint32_t start_addr);
void xg_render_runtime_composition_complete_ordering_table(
    uint32_t start_addr, uint32_t transferred_words);
void xg_render_runtime_composition_ui_ot_snapshot(
    PsxXgRenderUiOtSnapshot *out_snapshot);
void xg_render_runtime_composition_source_snapshot(
    PsxXgRenderSourceSnapshot *out_snapshot);
void xg_render_runtime_composition_source_collector_snapshot(
    FieldCharacterShadowSummary *out_summary);
void xg_render_runtime_composition_source_reset(void);
void xg_render_runtime_composition_ft4_geometry_enable(bool enabled);
bool xg_render_runtime_composition_ft4_geometry_pop(
    PsxXgRenderFt4Geometry *out_geometry);
void xg_render_runtime_composition_ft4_geometry_snapshot(
    PsxXgRenderFt4GeometrySnapshot *out_snapshot);
void xg_render_runtime_composition_zoom_template_contract_snapshot(
    PsxXgRenderZoomTemplateContractSnapshot *out_snapshot);
size_t xg_render_runtime_composition_pre_scene_snapshot(
    PsxXgRenderPreScenePrimitiveSnapshot *out_snapshots, size_t capacity);
size_t xg_render_runtime_composition_field_fragment_snapshot(
    PsxXgRenderPreScenePrimitiveSnapshot *out_snapshots, size_t capacity);
void xg_render_runtime_composition_overlay_ft4_snapshot(
    PsxXgRenderOverlayFt4Snapshot *out_snapshot);
void xg_render_runtime_composition_disable(void);
void xg_render_runtime_composition_prepare_authenticated_scene(void);
bool xg_render_runtime_composition_flush_pre_scene(void);
void xg_render_runtime_composition_pre_scene_status(
    uint32_t *out_count, uint32_t *out_blocker);
void xg_render_runtime_composition_title_restage_status(
    uint64_t *out_attempts, uint32_t *out_last_result,
    uint32_t *out_last_detail, uint32_t *out_fail_index,
    uint32_t *out_tpage, uint32_t *out_clut);
bool xg_render_runtime_composition_producer_family_enabled(void);
void xg_render_runtime_composition_enable_producer_family(bool enabled);
void xg_render_runtime_composition_producer_family_snapshot(
    PsxXgRenderProducerFamilySnapshot *out_snapshot);
void xg_render_runtime_composition_projected_lifecycle_snapshot(
    PsxXgRenderProjectedLifecycleSnapshot *out_snapshot);
void xg_render_runtime_composition_model_ft4_shadow_snapshot(
    PsxXgRenderModelFt4ShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_model_ft3_shadow_snapshot(
    PsxXgRenderModelFt3ShadowSnapshot *out_snapshot);
size_t xg_render_runtime_composition_sprite_cards(
    PsxXgRenderSpriteCardDiagnostic *out, size_t capacity);
void xg_render_runtime_composition_sprite_ft4_shadow_snapshot(
    PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_field_polyline_snapshot(
    PsxXgRenderFieldPolylineSnapshot *out_snapshot);
void xg_render_runtime_composition_world_horizon_shadow_snapshot(
    PsxXgRenderWorldHorizonShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_world_effects_shadow_snapshot(
    PsxXgRenderWorldEffectsShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_world_terrain_water_shadow_snapshot(
    PsxXgRenderWorldTerrainWaterShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_world_entity_shadows_shadow_snapshot(
    PsxXgRenderWorldEntityShadowsShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_world_decorations_shadow_snapshot(
    PsxXgRenderWorldDecorationsShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_world_clouds_shadow_snapshot(
    PsxXgRenderWorldCloudsShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_world_minimap_shadow_snapshot(
    PsxXgRenderWorldMinimapShadowSnapshot *out_snapshot);
void xg_render_runtime_composition_world_models_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
void xg_render_runtime_composition_world_actor_sprites_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
void xg_render_runtime_composition_world_sky_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
void xg_render_runtime_composition_world_execution_snapshot(
    PsxXgRenderWorldExecutionSnapshot *out_snapshot);
void xg_render_runtime_composition_resident_text_snapshot(
    PsxXgRenderResidentTextSnapshot *out_snapshot);
void xg_render_runtime_composition_scene_boundary(bool generation_advanced);

#endif
