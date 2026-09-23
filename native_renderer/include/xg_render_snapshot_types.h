#ifndef XG_RENDER_SNAPSHOT_TYPES_H
#define XG_RENDER_SNAPSHOT_TYPES_H

#include "field_character_shadow_types.h"
#include "guest_render_types.h"
#include "native_render_mode_types.h"
#include "psx_xg_render_auth_hook_types.h"
#include "xg_render_auth_candidate_types.h"
#include "xg_render_auth_types.h"
#include "xg_render_instrumentation_types.h"
#include "xg_render_source_types.h"
#include "xg_world_clouds_snapshot_types.h"
#include "xg_world_decorations_snapshot_types.h"
#include "xg_world_entity_shadows_snapshot_types.h"
#include "xg_world_minimap_snapshot_types.h"
#include "xg_world_terrain_water_snapshot_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef XgWorldCloudsShadowSnapshot PsxXgRenderWorldCloudsShadowSnapshot;
typedef XgWorldDecorationsShadowSnapshot
    PsxXgRenderWorldDecorationsShadowSnapshot;
typedef XgWorldEntityShadowsShadowSnapshot
    PsxXgRenderWorldEntityShadowsShadowSnapshot;
typedef XgWorldMinimapShadowSnapshot PsxXgRenderWorldMinimapShadowSnapshot;
typedef XgWorldTerrainWaterShadowSnapshot
    PsxXgRenderWorldTerrainWaterShadowSnapshot;

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

typedef struct PsxXgRenderPreScenePrimitiveSnapshot {
    uint64_t texture_resource_id;
    uint32_t packet_address;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint16_t tpage;
    uint16_t min_u;
    uint16_t min_v;
    uint16_t max_u;
    uint16_t max_v;
    int32_t min_x;
    int32_t min_y;
    int32_t max_x;
    int32_t max_y;
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    uint8_t texture_page_x;
    uint8_t texture_page_y;
    uint8_t texture_depth;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t blend_mode;
    uint8_t triangle_count;
    bool has_texture;
    bool has_clut;
    bool semi_transparent;
} PsxXgRenderPreScenePrimitiveSnapshot;

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

typedef struct PsxXgRenderProjectedLifecycleSnapshot {
    uint64_t initializer_begin_count;
    uint64_t initializer_registration_count;
    uint64_t cutover_attempt_count;
    uint64_t cutover_success_count;
    uint64_t cutover_rejection_count;
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
    uint32_t last_rejection_blocker;
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
    uint64_t field_builder_resource_deferral_count;
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
    uint32_t last_resident_miss_source;
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

/* One staged Field actor card piece (native_sprite_cards TCP diagnostic):
 * its two triangles' canonical positions and UVs, Q16.16. */
typedef struct PsxXgRenderSpriteCardDiagnostic {
    uint32_t sequence;
    uint32_t sprite_address;
    uint32_t descriptor_address;
    int32_t x[6], y[6], u[6], v[6];
} PsxXgRenderSpriteCardDiagnostic;

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

typedef struct PsxXgRenderModeSnapshot {
    GuestRenderModes modes;
    NativeRenderPresentationSnapshot presentation;
    GuestRenderFallbackReason fallback_reason;
    uint64_t transaction_count;
    uint64_t substitution_count;
    uint64_t fallback_count;
} PsxXgRenderModeSnapshot;

enum { PSX_XG_RENDER_RESIDENT_TEXT_ROUTE_COUNT = 14u };

typedef struct PsxXgRenderResidentTextSnapshot {
    uint64_t observation_count;
    uint64_t route_counts[PSX_XG_RENDER_RESIDENT_TEXT_ROUTE_COUNT];
    uint64_t invalidation_count;
    uint64_t font_content_digest;
    uint32_t last_pc;
    uint32_t last_owner_entry;
    uint32_t context_address;
    uint32_t active_advance_context;
    uint32_t render_context_address;
    uint32_t render_ot_address;
    uint32_t font_address;
    uint32_t last_destination_address;
    uint32_t last_row_record_address;
    uint16_t font_header[7];
    int16_t base_x;
    int16_t base_y;
    uint16_t width_units;
    uint16_t row_count;
    uint16_t line_height;
    uint16_t display_first_row;
    uint16_t last_row_advance;
    uint16_t last_glyph_id;
    uint16_t last_glyph_x;
    uint16_t last_glyph_row;
    uint16_t last_glyph_width;
    uint16_t last_lead;
    uint16_t last_trail;
    uint16_t last_stride;
    uint8_t selected_row;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t render_parity;
    uint8_t last_plane;
    bool context_valid;
    bool font_valid;
    bool render_active;
    bool frame_frozen;
} PsxXgRenderResidentTextSnapshot;

typedef struct PsxXgRenderAuthRuntimeSnapshot {
    uint64_t interpolation_scene_generation;
    uint64_t authenticated_artifact_generation;
    bool authenticated_artifact_active;
    bool active;
    bool armed;
    bool completed;
    bool artifact_candidate_valid;
    uint64_t artifact_scene_generation;
    uint32_t pre_scene_count;
    uint32_t pre_scene_blocker;
    uint64_t ui_ot_completed_count;
    uint64_t ui_ot_staged_count;
    bool ui_ot_blocked;
    uint64_t title_restage_attempts;
    uint32_t title_restage_last_result;
    uint32_t title_restage_last_detail;
    uint32_t title_restage_fail_index;
    uint32_t title_restage_tpage;
    uint32_t title_restage_clut;
    uint64_t retained_movie_resource_id;
    uint64_t retained_movie_generation;
    uint32_t retained_movie_kind;
    uint32_t retained_movie_width;
    uint32_t retained_movie_height;
    uint32_t retained_movie_retain_count;
    bool retained_movie_available;
    bool retained_movie_current;
    uint64_t invalidation_kind_counts[7];
    uint64_t invalidation_mutation_counts[12];
} PsxXgRenderAuthRuntimeSnapshot;

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

#ifdef __cplusplus
}
#endif

#endif
