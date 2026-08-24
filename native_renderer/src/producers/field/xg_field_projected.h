#ifndef XG_FIELD_PROJECTED_H
#define XG_FIELD_PROJECTED_H

#include "xg_render_ir.h"
#include "gpu_render.h"
#include "xg_host_3d.h"
#include "xg_native_view.h"
#include "xg_render_field_sprite_types.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_producer_lifecycle.h"
#include "xg_render_submission.h"
#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;
enum {
    XG_RENDER_PROJECTED_MAX_STRIPS = 8u,
    XG_RENDER_PROJECTED_MAX_RECORDS = 11u,
    XG_RENDER_PROJECTED_SOURCE_CAPACITY = 16u,
};

typedef struct XgRenderProjectedSource {
    uint64_t generation;
    uint32_t object_address;
    uint16_t clut_x;
    uint16_t clut_y;
    uint8_t upper_color[3];
    uint8_t middle_top_color[3];
    uint8_t lower_color[3];
    bool valid;
} XgRenderProjectedSource;

typedef struct XgRenderProjectedInitializerPending {
    uint32_t entry_sp;
    uint32_t return_address;
    uint16_t clut_x;
    uint16_t clut_y;
    uint8_t upper_color[3];
    uint8_t middle_top_color[3];
    uint8_t lower_color[3];
    bool valid;
} XgRenderProjectedInitializerPending;

typedef struct XgRenderProjectedSourceState {
    XgRenderProjectedSource records[XG_RENDER_PROJECTED_SOURCE_CAPACITY];
    uint64_t next_generation;
    bool blocked;
} XgRenderProjectedSourceState;

typedef enum XgRenderProjectedRecordKind {
    XG_RENDER_PROJECTED_RECORD_FT4 = 0,
    XG_RENDER_PROJECTED_RECORD_F4_UPPER = 1,
    XG_RENDER_PROJECTED_RECORD_G4 = 2,
    XG_RENDER_PROJECTED_RECORD_F4_LOWER = 3,
} XgRenderProjectedRecordKind;

typedef struct XgRenderProjectedNativeRecord {
    XgRenderIrNativePrimitive primitive;
    int16_t x[4];
    int16_t y[4];
    uint8_t u[4];
    uint8_t v[4];
    uint16_t tpage;
    uint32_t packet_address;
    uint32_t packet_tag;
    XgRenderProjectedRecordKind kind;
    uint8_t payload_word_count;
} XgRenderProjectedNativeRecord;

typedef enum XgFieldProjectedOrderingDomain {
    XG_FIELD_PROJECTED_ORDERING_UNKNOWN = 0,
    XG_FIELD_PROJECTED_ORDERING_FIELD,
    XG_FIELD_PROJECTED_ORDERING_BATTLE,
} XgFieldProjectedOrderingDomain;

typedef struct XgFieldProjectedPipelineServices {
    bool (*proof_active)(void);
    bool (*stage_active)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t ot_bucket,
        uint8_t payload_word_count, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_blocker);
    bool (*stage_standalone)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id);
    bool (*pre_scene_available)(uint32_t primitive_count);
    bool (*stage_pre_scene)(const XgRenderPreScenePrimitive *record);
    bool (*stage_temporal)(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id,
        const GpuRenderTemporalCullPolicy *policy);
    bool (*begin_lifecycle)(
        uint32_t producer_pc, XgRenderProducerLifecycle *out_lifecycle);
    bool (*has_template)(uint32_t packet_address);
    uint32_t (*available_template_capacity)(void);
    bool (*capture_template)(const XgRenderFieldSpriteTemplateInput *input);
    const XgNativeView *(*native_view)(void);
    void (*reject_policy)(uint32_t blocker);
} XgFieldProjectedPipelineServices;

void xg_field_projected_reset(void);
void xg_field_projected_classify_code_write(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
void xg_field_projected_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_field_projected_reset_pending(void);
bool xg_field_projected_lookup(
    uint32_t object_address, XgRenderProjectedSource *out_source);
void xg_field_projected_observe_initializer_begin(CPUState *cpu,
                                                    GuestRenderRenderMode mode);
void xg_field_projected_observe_initializer_commit(CPUState *cpu);
void xg_field_projected_note_initializer_result(CPUState *cpu);
bool xg_field_projected_cutover(
    CPUState *cpu, uint32_t producer_pc,
    const XgFieldProjectedPipelineServices *services);
bool xg_field_projected_reject(
    uint32_t blocker, const XgFieldProjectedPipelineServices *services);
void xg_field_projected_lifecycle_reset(void);
void xg_field_projected_lifecycle_snapshot(
    PsxXgRenderProjectedLifecycleSnapshot *out_snapshot);
void xg_field_projected_note_pending_reset(void);
void xg_field_projected_note_cutover_attempt(void);
void xg_field_projected_note_disable_reset(void);
void xg_field_projected_note_loader_reset(void);
void xg_field_projected_note_code_write(
    uint32_t address, uint32_t size, uint32_t code_write_mask);
bool xg_field_projected_code_write_overlaps(uint32_t address, uint32_t size);
void xg_field_projected_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));

#endif
