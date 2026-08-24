#ifndef XG_FIELD_CHARACTER_SOURCE_TYPES_H
#define XG_FIELD_CHARACTER_SOURCE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

enum {
    XG_FIELD_CHARACTER_SOURCE_SCHEMA_VERSION = 2u,
    XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE = 32u,
    XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT = 4u,
    XG_FIELD_CHARACTER_SOURCE_MATRIX_ELEMENT_COUNT = 9u,
    XG_FIELD_CHARACTER_SOURCE_SNAPSHOT_MAX_SIZE = 512u,
    XG_FIELD_CHARACTER_SOURCE_MAX_AUTHENTICATED_READS = 64u,
    XG_FIELD_CHARACTER_SOURCE_ACTOR_STRIDE = 0x5cu,
    XG_FIELD_CHARACTER_SOURCE_DYNAMIC_MODEL_FT4_COUNT = 2u,
    XG_FIELD_CHARACTER_SOURCE_MATRIX_FIELD_CAMERA_800AFA64 = 1u,
    XG_FIELD_CHARACTER_SOURCE_MODEL_INITIALIZER_8007AA44 = 1u,
    XG_FIELD_CHARACTER_SOURCE_MATERIAL_INITIALIZER_8007AA44 = 1u,
    XG_FIELD_CHARACTER_SOURCE_PROJECTION_SETUP_80074108 = 1u,
    XG_FIELD_CHARACTER_SOURCE_ORDERING_800769EC = 1u,
};

typedef enum XgFieldCharacterSourcePresence {
    XG_FIELD_CHARACTER_SOURCE_HAS_IDENTITY = 1u << 0,
    XG_FIELD_CHARACTER_SOURCE_HAS_GENERATION = 1u << 1,
    XG_FIELD_CHARACTER_SOURCE_HAS_VISUAL_STATE = 1u << 2,
    XG_FIELD_CHARACTER_SOURCE_HAS_TEXTURE_MEMORY_SERIAL = 1u << 3,
    XG_FIELD_CHARACTER_SOURCE_HAS_ACTOR_VALUES = 1u << 4,
    XG_FIELD_CHARACTER_SOURCE_HAS_MODEL_VALUES = 1u << 5,
    XG_FIELD_CHARACTER_SOURCE_HAS_SOURCE_MATRIX = 1u << 6,
    XG_FIELD_CHARACTER_SOURCE_HAS_CULLING_INPUTS = 1u << 7,
    XG_FIELD_CHARACTER_SOURCE_HAS_ORDERING_INPUTS = 1u << 8,
    XG_FIELD_CHARACTER_SOURCE_HAS_UNITS = 1u << 9,
    XG_FIELD_CHARACTER_SOURCE_HAS_ABSENCE_EVIDENCE = 1u << 10,
    XG_FIELD_CHARACTER_SOURCE_HAS_PROJECTION = 1u << 11,
    XG_FIELD_CHARACTER_SOURCE_HAS_MATERIAL = 1u << 12,
    XG_FIELD_CHARACTER_SOURCE_HAS_RASTER_STATE = 1u << 13,
} XgFieldCharacterSourcePresence;

typedef enum XgFieldCharacterSourceUnresolved {
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_COLOR = 1u << 0,
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_TEXCOORD = 1u << 1,
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_TPAGE = 1u << 2,
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_CLUT = 1u << 3,
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_SEMITRANSPARENCY = 1u << 4,
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_PROJECTION_SCALARS = 1u << 5,
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_ACTOR_MODEL_TRANSFORM = 1u << 6,
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_ORDERING_DEPTH = 1u << 7,
    XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_ORDERING_BUCKET = 1u << 8,
} XgFieldCharacterSourceUnresolved;

enum {
    XG_FIELD_CHARACTER_SOURCE_MATERIAL_UNRESOLVED =
        XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_COLOR |
        XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_TEXCOORD |
        XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_TPAGE |
        XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_CLUT |
        XG_FIELD_CHARACTER_SOURCE_UNRESOLVED_SEMITRANSPARENCY,
    XG_FIELD_CHARACTER_SOURCE_REQUIRED_PRESENCE =
        XG_FIELD_CHARACTER_SOURCE_HAS_IDENTITY |
        XG_FIELD_CHARACTER_SOURCE_HAS_GENERATION |
        XG_FIELD_CHARACTER_SOURCE_HAS_VISUAL_STATE |
        XG_FIELD_CHARACTER_SOURCE_HAS_TEXTURE_MEMORY_SERIAL |
        XG_FIELD_CHARACTER_SOURCE_HAS_ACTOR_VALUES |
        XG_FIELD_CHARACTER_SOURCE_HAS_MODEL_VALUES |
        XG_FIELD_CHARACTER_SOURCE_HAS_SOURCE_MATRIX |
        XG_FIELD_CHARACTER_SOURCE_HAS_CULLING_INPUTS |
        XG_FIELD_CHARACTER_SOURCE_HAS_ORDERING_INPUTS |
        XG_FIELD_CHARACTER_SOURCE_HAS_UNITS |
        XG_FIELD_CHARACTER_SOURCE_HAS_ABSENCE_EVIDENCE |
        XG_FIELD_CHARACTER_SOURCE_HAS_PROJECTION |
        XG_FIELD_CHARACTER_SOURCE_HAS_MATERIAL |
        XG_FIELD_CHARACTER_SOURCE_HAS_RASTER_STATE,
    XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_COLOR = 1u << 0,
    XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_TEXCOORD = 1u << 1,
    XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_TPAGE = 1u << 2,
    XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_CLUT = 1u << 3,
    XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_SEMITRANSPARENCY = 1u << 4,
    XG_FIELD_CHARACTER_SOURCE_MATERIAL_REQUIRED =
        XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_COLOR |
        XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_TEXCOORD |
        XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_TPAGE |
        XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_CLUT |
        XG_FIELD_CHARACTER_SOURCE_MATERIAL_HAS_SEMITRANSPARENCY,
};

typedef struct XgFieldCharacterSourceVisualState {
    uint64_t scene_epoch;
    uint64_t state_sequence;
} XgFieldCharacterSourceVisualState;

typedef struct XgFieldCharacterSourceIdentity {
    uint8_t game_sha256[XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE];
    uint8_t manifest_sha256[XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE];
    uint32_t producer_entry;
    uint32_t producer_record_id;
    uint32_t actor_index;
    uint32_t actor_count;
    uint32_t model_initializer;
} XgFieldCharacterSourceIdentity;

typedef struct XgFieldCharacterSourceGeneration {
    uint64_t source_generation;
    uint64_t scene_generation;
    XgFieldCharacterSourceVisualState visual_state;
    uint64_t vram_mutation_serial;
} XgFieldCharacterSourceGeneration;

typedef struct XgFieldCharacterSourceVector {
    int16_t x;
    int16_t y;
    int16_t z;
    uint16_t pad;
} XgFieldCharacterSourceVector;

typedef struct XgFieldCharacterSourceActorValues {
    uint32_t active_gate_word;
    uint32_t state_flags_0;
    uint32_t state_flags_4;
    uint32_t state_flags_14;
    int16_t orientation[3];
    int32_t world_offset[3];
    int16_t shadow_scale[3];
    uint8_t scale_shift;
    uint8_t scale_reduced;
} XgFieldCharacterSourceActorValues;

typedef struct XgFieldCharacterSourceMatrix {
    uint32_t source_identity;
    int16_t rotation[XG_FIELD_CHARACTER_SOURCE_MATRIX_ELEMENT_COUNT];
    int32_t translation[3];
    int16_t source_pad;
} XgFieldCharacterSourceMatrix;

typedef struct XgFieldCharacterSourceCullingInputs {
    uint32_t active_mask;
    uint32_t active_value;
    uint32_t state_flags_4_reject_mask;
    uint32_t state_flags_0_reject_mask;
    uint32_t state_flags_14_reject_mask;
    uint8_t producer_disabled;
    uint8_t actor_active;
    uint8_t state_visible;
} XgFieldCharacterSourceCullingInputs;

typedef struct XgFieldCharacterSourceMaterialInputs {
    uint32_t source_identity;
    uint32_t present_mask;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t semi_transparent;
    uint8_t u[XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT];
    uint8_t v[XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT];
    uint16_t tpage;
    uint16_t clut_x;
    uint16_t clut_y;
} XgFieldCharacterSourceMaterialInputs;

typedef struct XgFieldCharacterSourceProjection {
    uint32_t source_identity;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    uint16_t projection_distance;
    int16_t average_z_scale4;
    uint8_t screen_offset_fraction_bits;
    uint8_t depth_cue_output_unused;
} XgFieldCharacterSourceProjection;

typedef struct XgFieldCharacterSourceRasterState {
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    uint8_t dither;
    uint8_t mask_set;
    uint8_t mask_check;
} XgFieldCharacterSourceRasterState;

typedef struct XgFieldCharacterSourceOrderingInputs {
    uint32_t source_identity;
    uint32_t ft4_index;
    uint32_t ordering_depth;
    uint32_t ordering_bucket;
    uint8_t ordering_shift;
    uint8_t depth_present;
    uint8_t bucket_present;
    uint8_t ft4_index_from_actor_state;
    uint8_t ot_base_from_producer_argument;
} XgFieldCharacterSourceOrderingInputs;

typedef struct XgFieldCharacterSourceUnits {
    uint8_t matrix_fraction_bits;
    uint8_t scale_input_fraction_bits;
    uint8_t scale_product_fraction_bits;
    uint8_t model_vertex_unit_shift;
    int16_t model_axis_seed;
    int16_t scale_numerator;
} XgFieldCharacterSourceUnits;

typedef struct XgFieldCharacterSourceAbsenceEvidence {
    uint32_t unresolved_mask;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    uint32_t packet_arena_read_count;
    uint32_t ot_payload_read_count;
    uint32_t vram_read_count;
    uint32_t post_gte_read_count;
    uint32_t general_guest_read_count;
    uint8_t rigid_quad_has_no_pose;
    uint8_t lighting_not_used;
    uint8_t depth_cue_output_not_consumed;
    uint8_t packet_is_sink_only;
} XgFieldCharacterSourceAbsenceEvidence;

typedef struct XgFieldCharacterSourceSnapshot {
    uint32_t schema_version;
    uint32_t presence_mask;
    XgFieldCharacterSourceIdentity identity;
    XgFieldCharacterSourceGeneration generation;
    XgFieldCharacterSourceActorValues actor;
    XgFieldCharacterSourceVector model_vertices[
        XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT];
    XgFieldCharacterSourceMatrix source_matrix;
    XgFieldCharacterSourceCullingInputs culling;
    XgFieldCharacterSourceMaterialInputs material;
    XgFieldCharacterSourceProjection projection;
    XgFieldCharacterSourceRasterState raster;
    XgFieldCharacterSourceOrderingInputs ordering;
    XgFieldCharacterSourceUnits units;
    XgFieldCharacterSourceAbsenceEvidence absence;
    uint8_t value_digest[XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE];
    uint8_t authenticated;
    uint8_t sealed;
} XgFieldCharacterSourceSnapshot;

#endif
