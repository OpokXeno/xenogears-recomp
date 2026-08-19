#ifndef XG_RENDER_AUTH_RUNTIME_H
#define XG_RENDER_AUTH_RUNTIME_H

#include "game_identity.h"
#include "field_character_shadow.h"
#include "overlay_api.h"
#include "native_render_mode_control.h"
#include "xg_host_3d.h"
#include "xg_field_character_source.h"
#include "xg_render_auth.h"
#include "xg_world_clouds_shadow.h"
#include "xg_world_decorations_shadow.h"
#include "xg_world_entity_shadows_shadow.h"
#include "xg_world_minimap_shadow.h"
#include "xg_world_terrain_water_shadow.h"

#include <stdbool.h>
#include <stdint.h>

#define PSX_XG_RENDER_SOURCE_EVENT_CAPACITY 28u
#define PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY 256u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxXgRenderAuthCandidate {
    uint32_t producer_entry;
    uint32_t range_start;
    uint32_t range_size;
    uint32_t dispatch_pc;
    PsxGameIdentity identity;
    uint64_t pair_id;
    uint32_t artifact_base;
    uint32_t artifact_size;
    uint32_t artifact_crc32;
    uint8_t runtime_variant_identity[PSX_GAME_IDENTITY_SHA256_BYTES];
    bool authority_provenance;
    bool pair_bound;
    bool runtime_variant_bound;
} PsxXgRenderAuthCandidate;

typedef struct PsxXgRenderAuthProvenance {
    bool manifest_bound;
    bool range_bound;
    bool candidate_matched;
    bool candidate_dispatched;
} PsxXgRenderAuthProvenance;

typedef enum PsxXgRenderAuthRejectionSource {
    PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE = 0,
    PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
    PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
    PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH,
    PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NATIVE_BAD_ENTRY,
    PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION,
} PsxXgRenderAuthRejectionSource;

typedef struct PsxXgRenderAuthRejectionReceipt {
    PsxXgRenderAuthRejectionSource source;
    PsxXgRenderAuthHook hook;
    uint32_t guest_pc;
    bool has_hook;
} PsxXgRenderAuthRejectionReceipt;

typedef struct PsxXgRenderAuthCompletedProofTuple {
    uint32_t producer_entry;
    uint32_t capture_site;
    uint32_t static_callee;
    uint32_t return_site;
} PsxXgRenderAuthCompletedProofTuple;

typedef struct PsxXgRenderAuthCompletedProofReceipt {
    bool available;
    bool blocked;
    uint32_t producer_record_id;
    uint32_t site_record_id;
    PsxXgRenderAuthCompletedProofTuple tuple;
    XgRenderAuthTier tier;
    GuestRenderVisualStateId state_id;
    uint64_t entry_event_sequence;
    uint64_t capture_event_sequence;
    uint64_t return_event_sequence;
    bool candidate_matched;
    bool candidate_dispatched;
    XgRenderAuthReason blocker_reason;
    PsxXgRenderAuthRejectionReceipt blocker_rejection;
} PsxXgRenderAuthCompletedProofReceipt;

typedef struct PsxXgRenderAuthInstrumentation {
    uint32_t revision;
    uint64_t cold_hook_ingress_count;
    uint64_t activation_physical_count;
    uint64_t activation_exact_count;
    uint64_t entry_physical_count;
    uint64_t entry_exact_count;
    uint64_t capture_physical_count;
    uint64_t capture_exact_count;
    uint64_t return_physical_count;
    uint64_t return_exact_count;
    uint64_t last_progress_sequence;
    uint64_t last_reset_sequence;
    uint64_t last_publish_sequence;
    uint64_t scene_boundary_count;
    uint64_t disarm_count;
    uint64_t completed_proof_publication_count;
    uint64_t native_ir_flush_attempt_count;
    uint64_t native_ir_flush_failure_count;
    uint64_t first_native_ir_flush_failure_index;
    uint32_t first_native_ir_flush_failure_reason;
    uint32_t first_native_ir_flush_failure_packet;
    uint32_t first_native_ir_flush_failure_status;
} PsxXgRenderAuthInstrumentation;

typedef enum PsxXgRenderSourceOperation {
    PSX_XG_RENDER_SOURCE_OPERATION_READ = 0,
    PSX_XG_RENDER_SOURCE_OPERATION_WRITE = 1,
    PSX_XG_RENDER_SOURCE_OPERATION_SWC2 = 2,
    PSX_XG_RENDER_SOURCE_OPERATION_CALL = 3,
    PSX_XG_RENDER_SOURCE_OPERATION_BUCKET = 4,
} PsxXgRenderSourceOperation;

typedef enum PsxXgRenderSourceAuxiliaryRule {
    PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS = 0,
    PSX_XG_RENDER_SOURCE_AUXILIARY_NONE = 1,
    PSX_XG_RENDER_SOURCE_AUXILIARY_RESULT_REGISTER = 2,
} PsxXgRenderSourceAuxiliaryRule;

typedef enum PsxXgRenderSourceStage {
    PSX_XG_RENDER_SOURCE_STAGE_PRE = 0,
    PSX_XG_RENDER_SOURCE_STAGE_COMMIT = 1,
} PsxXgRenderSourceStage;

typedef struct PsxXgRenderSourceSiteMetadata {
    PsxXgRenderSourceOperation operation;
    PsxXgRenderSourceAuxiliaryRule auxiliary_rule;
    uint8_t width;
} PsxXgRenderSourceSiteMetadata;

typedef struct PsxXgRenderSourceEvent {
    uint64_t sequence;
    uint32_t pc;
    uint32_t auxiliary;
    PsxXgRenderSourceOperation operation;
    PsxXgRenderSourceStage stage;
    uint8_t width;
} PsxXgRenderSourceEvent;

typedef struct PsxXgRenderSourceSnapshot {
    PsxXgRenderSourceEvent events[PSX_XG_RENDER_SOURCE_EVENT_CAPACITY];
    uint64_t next_sequence;
    uint32_t count;
    uint32_t blocker;
    uint32_t context_bits;
    bool blocked;
    bool overflowed;
} PsxXgRenderSourceSnapshot;

typedef struct PsxXgRenderFt4Geometry {
    uint64_t sequence;
    uint32_t packet_guest_address;
    uint32_t semantic_template;
    int16_t x[4];
    int16_t y[4];
    XgHost3dRotAverage4Input pre_transform;
    uint16_t ordering_depth;
    int16_t depth_cue;
    uint32_t projection_flags;
    XgRenderAuthTier tier;
    XgFieldCharacterSourceSnapshot source_snapshot;
    uint32_t source_capture_result;
    bool source_captured;
    bool host_transformed;
} PsxXgRenderFt4Geometry;

enum {
    PSX_XG_RENDER_FT4_SEMANTIC_STATIC_TABLE = 0u,
    PSX_XG_RENDER_FT4_SEMANTIC_DYNAMIC_ACTOR = 1u,
};

typedef struct PsxXgRenderFt4GeometrySnapshot {
    uint64_t completed_count;
    uint64_t host_transform_count;
    uint64_t oracle_match_count;
    uint64_t oracle_mismatch_count;
    uint32_t queued_count;
    bool enabled;
    bool pending;
    bool blocked;
    bool overflowed;
} PsxXgRenderFt4GeometrySnapshot;

typedef struct PsxXgRenderZoomTemplateContractSnapshot {
    uint64_t generation;
    uint32_t producer_store_pc;
    uint32_t template_count;
    uint32_t buffer_count;
    uint8_t opcode;
    bool authenticated;
    uint64_t initializer_begin_count;
    uint64_t initializer_commit_count;
    uint64_t initializer_2e_count;
    uint64_t rgb_update_count;
    uint64_t invocation_count;
    uint64_t cutover_attempt_count;
    uint64_t native_invocation_count;
    uint64_t native_primitive_count;
    uint64_t replay_invocation_count;
    uint64_t replay_primitive_count;
    uint64_t rejection_count;
    uint32_t last_rejection_blocker;
} PsxXgRenderZoomTemplateContractSnapshot;

typedef struct PsxXgRenderOverlayFt4Snapshot {
    uint64_t producer_entry_count;
    uint64_t producer_return_count;
    uint64_t caller_call_count;
    uint64_t caller_finish_count;
    uint64_t rectangle_helper_count;
    uint64_t static_quad_count;
    uint64_t dynamic_uv_template_count;
    uint64_t rejected_site_count;
    uint64_t direct_template_count;
    uint64_t direct_add_prim_count;
    uint64_t direct_native_count;
    uint64_t direct_stage_failure_count;
    uint64_t rectangle_template_count;
    uint64_t rectangle_add_prim_count;
    uint64_t rectangle_native_count;
    uint64_t rectangle_stage_failure_count;
    uint64_t projected_material_count;
    uint64_t projected_geometry_count;
    uint64_t projected_add_prim_count;
    uint64_t projected_native_count;
    uint64_t projected_stage_failure_count;
    uint64_t projected_missing_material_count;
    uint64_t projected_missing_outer_counts[16];
    uint32_t projected_missing_outer_returns[16];
    uint32_t projected_missing_outer_count;
    uint32_t projected_missing_outer_overflow;
    uint64_t projected_2e_material_count;
    uint64_t projected_2e_geometry_count;
    uint64_t projected_2e_add_prim_count;
    uint64_t projected_2e_native_count;
    uint64_t projected_2e_stage_failure_count;
    uint64_t field_source_template_count;
    uint64_t field_base_template_count;
    uint64_t field_offset_template_count;
    uint64_t field_material_count;
    uint64_t field_add_prim_count;
    uint64_t field_native_count;
    uint64_t field_stage_failure_count;
    uint32_t last_pc;
    uint32_t last_packet;
    uint32_t last_ot;
    uint32_t substitution_blocker;
} PsxXgRenderOverlayFt4Snapshot;

typedef struct PsxXgRenderProducerFamilySnapshot {
    uint64_t source_capture_count;
    uint64_t source_incomplete_count;
    uint64_t geometry_count;
    uint64_t candidate_count;
    uint64_t match_count;
    uint64_t mismatch_count;
    uint32_t last_ot_bucket;
    uint32_t last_runtime_result;
    uint32_t last_compare_result;
    uint32_t first_mismatch_word;
    uint32_t first_mismatch_byte;
    uint32_t blocker;
    bool enabled;
    bool blocked;
} PsxXgRenderProducerFamilySnapshot;

typedef enum PsxXgRenderCodeWriteClass {
    PSX_XG_RENDER_CODE_WRITE_DESCRIPTOR = 0,
    PSX_XG_RENDER_CODE_WRITE_DIRECT,
    PSX_XG_RENDER_CODE_WRITE_ZOOM,
    PSX_XG_RENDER_CODE_WRITE_PROJECTED,
    PSX_XG_RENDER_CODE_WRITE_WORLD_SKY,
    PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON,
    PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS,
    PSX_XG_RENDER_CODE_WRITE_MODEL_FT4,
    PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4,
    PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA,
    PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA,
    PSX_XG_RENDER_CODE_WRITE_ARTIFACT,
    PSX_XG_RENDER_CODE_WRITE_CLASS_COUNT,
} PsxXgRenderCodeWriteClass;

typedef struct PsxXgRenderProjectedLifecycleSnapshot {
    uint64_t initializer_begin_count;
    uint64_t initializer_registration_count;
    uint64_t cutover_attempt_count;
    uint64_t cutover_success_count;
    uint64_t primitive_count;
    uint64_t source_miss_count;
    uint64_t source_blocked_count;
    uint64_t pending_reset_count;
    uint64_t disable_reset_count;
    uint64_t code_write_reset_count;
    uint64_t code_write_class_counts[PSX_XG_RENDER_CODE_WRITE_CLASS_COUNT];
    uint32_t code_write_class_first_address[
        PSX_XG_RENDER_CODE_WRITE_CLASS_COUNT];
    uint32_t code_write_class_last_address[
        PSX_XG_RENDER_CODE_WRITE_CLASS_COUNT];
    uint64_t loader_reset_count;
    uint32_t first_code_write_address;
    uint32_t first_code_write_size;
    uint32_t first_code_write_mask;
    uint32_t last_code_write_address;
    uint32_t last_code_write_size;
    uint32_t last_code_write_mask;
    uint32_t last_registered_object;
    uint32_t last_source_success_object;
    uint32_t last_source_miss_object;
} PsxXgRenderProjectedLifecycleSnapshot;

typedef struct PsxXgRenderFt4PayloadMismatch {
    uint32_t field_bits;
    uint32_t packet_address;
    uint32_t descriptor_address;
    uint32_t expected_material_word;
    uint32_t actual_material_word;
    uint16_t expected_uv[4];
    uint16_t actual_uv[4];
    uint16_t expected_tpage;
    uint16_t actual_tpage;
    uint16_t expected_clut;
    uint16_t actual_clut;
} PsxXgRenderFt4PayloadMismatch;

typedef struct PsxXgRenderModelFt4ShadowSnapshot {
    uint64_t dispatch_begin_count;
    uint64_t dispatch_caller_reject_count;
    uint64_t dispatch_mode_reject_count;
    uint64_t average_seam_count;
    uint64_t farthest_seam_count;
    uint64_t seam_without_context_count;
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t invocation_count;
    uint64_t primitive_count;
    uint64_t match_count;
    uint64_t mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t ot_mismatch_count;
    uint64_t cursor_mismatch_count;
    uint64_t counter_mismatch_count;
    uint64_t projection_matrix_mismatch_count;
    uint64_t guest_pass_observation_count;
    uint64_t guest_pass_projection_disagreement_count;
    uint64_t replay_attempt_count;
    uint64_t replay_resolved_count;
    uint64_t replay_lookup_miss_count;
    uint64_t replay_record_reject_count;
    uint64_t replay_container_reject_count;
    uint64_t replay_lifecycle_reject_count;
    uint64_t replay_translate_reject_count;
    uint64_t publish_invocation_count;
    uint64_t publish_source_count;
    uint64_t validation_rejected_source_count;
    uint64_t framing_rejected_invocation_count;
    uint64_t template_capture_count;
    uint64_t template_hit_count;
    uint64_t template_miss_count;
    uint32_t last_primitive_count;
    uint32_t first_mismatch_primitive;
    uint32_t first_mismatch_packet;
    uint32_t last_model_address;
    uint32_t last_topology_base;
    uint32_t last_material_base;
    uint32_t last_attribute_address;
    uint32_t last_material_word;
    uint32_t last_group_count;
    uint32_t last_target_count;
    uint32_t last_dispatch_caller;
    uint32_t last_dispatch_mode;
    uint32_t last_seam_pc;
    uint32_t last_projection_matrix_mismatch_mask;
    uint32_t last_expected_counter_delta;
    uint32_t last_actual_counter_delta;
    uint32_t prepare_failure_detail;
    uint32_t prepare_precondition_failure_mask;
    PsxXgRenderFt4PayloadMismatch first_payload_mismatch;
    uint32_t blocker;
    bool pending;
    bool blocked;
} PsxXgRenderModelFt4ShadowSnapshot;

typedef struct PsxXgRenderModelFt3ShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t invocation_count;
    uint64_t primitive_count;
    uint64_t match_count;
    uint64_t mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t ot_mismatch_count;
    uint64_t cursor_mismatch_count;
    uint64_t counter_mismatch_count;
    uint64_t counter_actual_greater_count;
    uint64_t counter_actual_less_count;
    uint64_t handler_projection_mismatch_count;
    uint64_t guest_pass_observation_count;
    uint64_t guest_pass_projection_disagreement_count;
    uint64_t template_capture_count;
    uint64_t template_hit_count;
    uint64_t template_miss_count;
    uint64_t raw_color_difference_count;
    uint64_t replay_attempt_count;
    uint64_t replay_resolved_count;
    uint64_t replay_lookup_miss_count;
    uint64_t replay_lookup_invalid_count;
    uint64_t replay_lookup_absent_count;
    uint64_t replay_record_reject_count;
    uint64_t replay_container_reject_count;
    uint64_t replay_lifecycle_reject_count;
    uint64_t replay_translate_reject_count;
    uint64_t publish_invocation_count;
    uint64_t publish_source_count;
    uint64_t validation_rejected_source_count;
    uint64_t framing_rejected_invocation_count;
    uint32_t last_expected_counter_delta;
    uint32_t last_actual_counter_delta;
    uint32_t last_mismatch_expected_counter_delta;
    uint32_t last_mismatch_actual_counter_delta;
    uint32_t last_nclip_positive_count;
    uint32_t last_guest_screen_accepted_count;
    uint32_t last_guest_vertical_accepted_count;
    uint32_t last_guest_horizontal_accepted_count;
    uint32_t last_projection_flag_negative_count;
    uint32_t last_handler_projection_mismatch_mask;
    uint32_t last_mismatch_target_count;
    uint32_t last_mismatch_nclip_positive_count;
    uint32_t last_mismatch_guest_screen_accepted_count;
    uint32_t last_mismatch_guest_vertical_accepted_count;
    uint32_t last_mismatch_guest_horizontal_accepted_count;
    uint32_t last_mismatch_projection_flag_negative_count;
    uint32_t last_mismatch_screen_right;
    uint32_t last_mismatch_screen_bottom;
    uint32_t first_mismatch_packet;
    uint32_t last_replay_lookup_miss_source;
    uint32_t source_count;
    uint32_t last_group_count;
    uint32_t last_target_count;
    uint32_t prepare_failure_detail;
    uint32_t prepare_precondition_failure_mask;
    PsxXgRenderFt4PayloadMismatch first_payload_mismatch;
    uint32_t blocker;
    bool pending;
    bool blocked;
} PsxXgRenderModelFt3ShadowSnapshot;

typedef struct PsxXgRenderSpriteFt4ShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t resident_publish_source_count;
    uint64_t resident_replay_attempt_count;
    uint64_t resident_replay_resolved_count;
    uint64_t resident_replay_lookup_miss_count;
    uint64_t resident_replay_record_reject_count;
    uint64_t resident_replay_container_reject_count;
    uint64_t resident_replay_lifecycle_reject_count;
    uint64_t resident_replay_translate_reject_count;
    uint64_t field_builder_begin_count;
    uint64_t field_builder_native_cutover_count;
    uint64_t field_builder_native_primitive_count;
    uint64_t field_builder_template_capture_count;
    uint64_t field_builder_template_update_count;
    uint64_t field_builder_template_invalidation_count;
    uint64_t field_builder_dma_replay_primitive_count;
    uint64_t field_builder_primitive_count;
    uint64_t field_builder_match_count;
    uint64_t field_builder_mismatch_count;
    uint64_t field_builder_active_scene_count;
    uint64_t caller_count;
    uint64_t empty_caller_count;
    uint64_t projection_count;
    uint64_t match_count;
    uint64_t mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t payload_mismatch_count;
    uint32_t last_caller;
    uint32_t last_field_builder_caller;
    uint32_t field_builder_caller_candidates[4];
    uint64_t field_builder_caller_counts[4];
    uint32_t field_builder_first_mismatch_packet;
    uint32_t field_builder_first_mismatch_descriptor;
    uint32_t field_builder_first_mismatch_caller;
    uint32_t field_builder_first_mismatch_bits;
    uint32_t field_builder_expected_xy[4];
    uint32_t field_builder_actual_xy[4];
    uint16_t field_builder_expected_uv[4];
    uint16_t field_builder_actual_uv[4];
    uint16_t field_builder_expected_tpage;
    uint16_t field_builder_actual_tpage;
    uint16_t field_builder_expected_clut;
    uint16_t field_builder_actual_clut;
    uint32_t field_builder_actual_command;
    uint32_t field_builder_failure_detail;
    uint32_t field_builder_blocker;
    uint32_t field_builder_min_packet;
    uint32_t field_builder_max_packet;
    uint32_t field_builder_template_count;
    uint32_t last_sprite_address;
    uint32_t last_data_address;
    uint32_t last_descriptor_address;
    uint32_t last_primitive_count;
    uint32_t first_mismatch_packet;
    uint32_t first_mismatch_descriptor;
    PsxXgRenderFt4PayloadMismatch first_payload_mismatch;
    uint32_t blocker;
    uint32_t blocker_detail;
    bool context_active;
    bool pending;
    bool blocked;
    bool field_builder_pending;
    bool field_builder_blocked;
} PsxXgRenderSpriteFt4ShadowSnapshot;

typedef struct PsxXgRenderFieldPolylineSnapshot {
    uint64_t begin_count;
    uint64_t invocation_count;
    uint64_t primitive_count;
    uint64_t match_count;
    uint64_t mismatch_count;
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint32_t first_mismatch_packet;
    uint32_t blocker;
    bool pending;
    bool blocked;
} PsxXgRenderFieldPolylineSnapshot;

typedef struct PsxXgRenderWorldHorizonShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t begin_count;
    uint64_t completion_count;
    uint64_t accepted_invocation_count;
    uint64_t primitive_count;
    uint64_t match_count;
    uint64_t mismatch_count;
    uint64_t source_capture_failure_count;
    uint64_t source_read_count;
    uint64_t source_read_bytes;
    uint64_t payload_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t ot_mismatch_count;
    uint64_t texture_window_mismatch_count;
    uint32_t last_ot_bucket;
    uint32_t first_mismatch_packet;
    uint64_t first_geometry_mismatch_invocation;
    uint32_t first_geometry_mismatch_quad;
    uint32_t first_geometry_mismatch_vertex;
    uint32_t first_geometry_expected_xy;
    uint32_t first_geometry_actual_xy;
    PsxXgRenderFt4PayloadMismatch first_payload_mismatch;
    uint32_t blocker;
    bool pending;
    bool blocked;
} PsxXgRenderWorldHorizonShadowSnapshot;

typedef struct PsxXgRenderWorldEffectsShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t begin_count;
    uint64_t completion_count;
    uint64_t active_source_count;
    uint64_t primitive_count;
    uint64_t candidate_count;
    uint64_t match_count;
    uint64_t mismatch_count;
    uint64_t invocation_match_count;
    uint64_t invocation_mismatch_count;
    uint64_t source_capture_failure_count;
    uint64_t source_read_count;
    uint64_t source_read_bytes;
    uint64_t count_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t ot_mismatch_count;
    uint32_t last_primitive_count;
    uint32_t last_candidate_count;
    uint32_t first_mismatch_packet;
    uint32_t first_mismatch_source;
    PsxXgRenderFt4PayloadMismatch first_payload_mismatch;
    uint32_t blocker;
    bool pending;
    bool blocked;
} PsxXgRenderWorldEffectsShadowSnapshot;

typedef XgWorldTerrainWaterShadowSnapshot
    PsxXgRenderWorldTerrainWaterShadowSnapshot;
typedef XgWorldEntityShadowsShadowSnapshot
    PsxXgRenderWorldEntityShadowsShadowSnapshot;
typedef XgWorldDecorationsShadowSnapshot
    PsxXgRenderWorldDecorationsShadowSnapshot;
typedef XgWorldCloudsShadowSnapshot PsxXgRenderWorldCloudsShadowSnapshot;
typedef XgWorldMinimapShadowSnapshot PsxXgRenderWorldMinimapShadowSnapshot;

typedef enum PsxXgRenderWorldFamily {
    PSX_XG_RENDER_WORLD_TERRAIN_WATER = 0,
    PSX_XG_RENDER_WORLD_MODELS,
    PSX_XG_RENDER_WORLD_ACTOR_SPRITES,
    PSX_XG_RENDER_WORLD_ENTITY_SHADOWS,
    PSX_XG_RENDER_WORLD_CLOUDS,
    PSX_XG_RENDER_WORLD_EFFECTS,
    PSX_XG_RENDER_WORLD_DECORATIONS,
    PSX_XG_RENDER_WORLD_HORIZON,
    PSX_XG_RENDER_WORLD_SKY,
    PSX_XG_RENDER_WORLD_MINIMAP,
    PSX_XG_RENDER_WORLD_FAMILY_COUNT,
} PsxXgRenderWorldFamily;

typedef struct PsxXgRenderWorldExecutionSnapshot {
    uint64_t full_dispatcher_count;
    uint64_t family_entry_count[PSX_XG_RENDER_WORLD_FAMILY_COUNT];
    uint32_t observed_family_mask;
    uint32_t instruction_mismatch_mask;
    bool overflowed;
} PsxXgRenderWorldExecutionSnapshot;

typedef enum PsxXgRenderWorldNativeFailureStage {
    PSX_XG_RENDER_WORLD_NATIVE_FAILURE_NONE = 0,
    PSX_XG_RENDER_WORLD_NATIVE_FAILURE_PREPARE,
    PSX_XG_RENDER_WORLD_NATIVE_FAILURE_COMMIT_PRECONDITION,
    PSX_XG_RENDER_WORLD_NATIVE_FAILURE_ANCHOR_COLLECTION,
    PSX_XG_RENDER_WORLD_NATIVE_FAILURE_FINALIZE,
    PSX_XG_RENDER_WORLD_NATIVE_FAILURE_ANCHOR_RECORD,
} PsxXgRenderWorldNativeFailureStage;

typedef struct PsxXgRenderWorldNativeSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t native_failure_count;
    uint32_t first_failure_stage;
    uint32_t first_failure_detail;
    uint32_t first_anchor_count;
    uint32_t last_failure_stage;
    uint32_t last_failure_detail;
    uint32_t last_anchor_count;
    uint64_t packet_copy_begin_count;
    uint64_t packet_copy_finish_count;
    uint64_t packet_copy_template_count;
    uint32_t packet_copy_failure_detail;
    uint32_t packet_copy_last_destination;
    uint32_t packet_copy_last_source;
    uint32_t packet_copy_last_size;
    uint32_t packet_copy_range_count;
    uint32_t first_missing_model_address;
    uint32_t first_missing_packet_base;
    uint32_t first_missing_packet_address;
    uint32_t first_missing_copy_range_kind;
    uint32_t first_missing_copy_range_index;
} PsxXgRenderWorldNativeSnapshot;

typedef bool (*PsxXgRenderPresentationGate)(
    GuestRenderRenderMode requested_mode,
    NativeRenderPresentationSnapshot *out_snapshot,
    void *user_data);
typedef int (*PsxXgRenderExecPhaseExchange)(int phase);

extern bool g_psx_xg_render_auth_cold_enabled;

typedef struct PsxXgRenderModeSnapshot {
    GuestRenderModes modes;
    NativeRenderPresentationSnapshot presentation;
    GuestRenderFallbackReason fallback_reason;
    uint64_t transaction_count;
    uint64_t substitution_count;
    uint64_t fallback_count;
} PsxXgRenderModeSnapshot;

typedef struct PsxXgRenderUiOtSnapshot {
    uint64_t prepare_count;
    uint64_t completed_count;
    uint64_t node_count;
    uint64_t candidate_count;
    uint64_t prebound_count;
    uint64_t staged_count;
    uint64_t blocked_count;
    uint32_t last_start_address;
    uint32_t last_node_count;
    uint32_t last_candidate_count;
    uint32_t last_prebound_count;
    uint32_t last_staged_count;
    uint64_t last_ot_digest;
    uint64_t last_packet_digest;
    uint64_t last_semantic_digest;
    uint64_t last_environment_digest;
    uint64_t last_vram_serial;
    bool pending;
    bool blocked;
} PsxXgRenderUiOtSnapshot;

bool psx_xg_render_auth_configure(
    GuestRenderTimingMode requested_timing_mode,
    GuestRenderRenderMode requested_render_mode,
    PsxXgRenderPresentationGate presentation_gate,
    void *presentation_user_data);
bool psx_xg_render_auth_configure_native_view(
    bool enabled, uint16_t aspect_num, uint16_t aspect_den,
    uint16_t canonical_width, uint16_t canonical_height);
void psx_xg_render_auth_set_terrain_temporal_coverage(bool enabled);
void psx_xg_render_auth_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
void psx_xg_render_auth_set_exec_phase_exchange(
    PsxXgRenderExecPhaseExchange exchange);
void psx_xg_render_auth_cold_enable(bool enabled);
void psx_xg_render_auth_scene_boundary(void);
void psx_xg_render_auth_before_gpu_submission(void);
/* Called by DMA2 immediately before an OT walk. When an authenticated field
 * DrawOTag site was just observed, stages exact packet-level Native semantics
 * for the unbound UI packets in that OT walk. */
bool psx_xg_render_auth_prepare_ui_ot(uint32_t start_addr);
void psx_xg_render_auth_ui_ot_snapshot(PsxXgRenderUiOtSnapshot *out_snapshot);
bool psx_xg_render_auth_cold_hook_relevant(uint32_t hook, uint32_t pc,
                                           uint32_t instruction_word);
void psx_xg_render_auth_cold_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word);
void psx_xg_render_auth_warm_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                   uint32_t instruction_word,
                                   uint32_t delay_slot_word);
bool psx_xg_render_auth_source_site_lookup(
    uint32_t pc, uint32_t instruction_word,
    PsxXgRenderSourceSiteMetadata *out_metadata);
bool psx_xg_render_auth_cold_source_pc_relevant(uint32_t pc);

enum {
    PSX_XG_RENDER_COLD_ENTRY = 1u << 0,
    PSX_XG_RENDER_COLD_CAPTURE = 1u << 1,
    PSX_XG_RENDER_COLD_SOURCE = 1u << 2,
    PSX_XG_RENDER_COLD_NATIVE_PRE = 1u << 3,
    PSX_XG_RENDER_COLD_NATIVE_POST = 1u << 4,
    PSX_XG_RENDER_COLD_OVERLAY = 1u << 5,
};

uint32_t psx_xg_render_auth_cold_instruction_flags(
    uint32_t pc, uint32_t instruction_word);
bool psx_xg_render_auth_cold_source_observe(
    PsxXgRenderSourceStage stage, uint32_t pc, uint32_t instruction_word,
    uint32_t auxiliary);
bool psx_xg_render_auth_cold_source_observe_cpu(
    CPUState *cpu, PsxXgRenderSourceStage stage, uint32_t pc,
    uint32_t instruction_word, uint32_t auxiliary);
void psx_xg_render_auth_source_snapshot(
    PsxXgRenderSourceSnapshot *out_snapshot);
void psx_xg_render_auth_source_collector_snapshot(
    FieldCharacterShadowSummary *out_summary);
void psx_xg_render_auth_source_reset(void);
void psx_xg_render_auth_ft4_geometry_enable(bool enabled);
bool psx_xg_render_auth_resident_ft4_observe(
    CPUState *cpu, uint32_t stage, uint32_t pc, uint32_t instruction_word);
bool psx_xg_render_auth_native_cutover_pc_relevant(uint32_t pc);
bool psx_xg_render_auth_overlay_cutover_relevant(
    uint32_t pc, uint32_t instruction_word);
bool psx_xg_render_auth_native_cutover_post_pc_relevant(uint32_t pc);
bool psx_xg_render_auth_native_ft4_bypass(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word);
bool psx_xg_render_auth_ft4_geometry_pop(
    PsxXgRenderFt4Geometry *out_geometry);
void psx_xg_render_auth_ft4_geometry_snapshot(
    PsxXgRenderFt4GeometrySnapshot *out_snapshot);
void psx_xg_render_auth_zoom_template_contract_snapshot(
    PsxXgRenderZoomTemplateContractSnapshot *out_snapshot);
void psx_xg_render_auth_overlay_ft4_snapshot(
    PsxXgRenderOverlayFt4Snapshot *out_snapshot);
void psx_xg_render_auth_producer_family_enable(bool enabled);
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
void psx_xg_render_auth_capture_model_ft3_link(CPUState *cpu);
void psx_xg_render_auth_capture_tile_write(
    CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
    uint8_t color);
void psx_xg_render_auth_note_code_write(uint64_t previous_generation,
                                         uint64_t current_generation,
                                         uint32_t guest_pc,
                                         uint32_t write_size);
void psx_xg_render_auth_loader_mismatch(uint32_t pc);
void psx_xg_render_auth_native_bad_entry(uint32_t owner, uint32_t pc);
void psx_xg_render_auth_note_artifact_candidate(
    const PsxXgRenderAuthCandidate *candidate);
bool psx_xg_render_auth_authenticated_producer_entry(
    uint32_t *out_producer_entry);
void psx_xg_render_auth_note_candidate_dispatch(
    const PsxXgRenderAuthCandidate *candidate);
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
const char *psx_xg_render_auth_rejection_source_name(uint32_t source);
const char *psx_xg_render_auth_hook_name(uint32_t hook);

#ifdef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
void psx_xg_render_auth_runtime_test_reset(void);
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
#endif

#ifdef __cplusplus
}
#endif

#endif
