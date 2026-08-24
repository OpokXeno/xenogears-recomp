#ifndef XG_FIELD_PARTICLES_H
#define XG_FIELD_PARTICLES_H

#include "xg_render_invalidation_event.h"

#include "guest_render_types.h"
#include "gpu_render.h"
#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

enum { XG_RENDER_PARTICLE_SOURCE_CAPACITY = 128u };

typedef struct XgRenderParticleSource {
    uint64_t generation;
    uint32_t particle_address;
    uint32_t producer_pc;
    int16_t x[4];
    int16_t y[4];
    uint8_t u[4];
    uint8_t v[4];
    uint16_t tpage;
    uint16_t clut_x;
    uint16_t clut_y;
    uint8_t command;
    uint8_t payload_word_count;
    bool semi_transparent;
    bool valid;
} XgRenderParticleSource;

typedef struct XgRenderParticleSourceState {
    XgRenderParticleSource records[XG_RENDER_PARTICLE_SOURCE_CAPACITY];
    uint64_t next_generation;
    bool blocked;
} XgRenderParticleSourceState;

typedef struct XgFieldParticlePipelineServices {
    bool (*stage_active)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t ot_bucket,
        uint8_t payload_word_count, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_blocker);
    bool (*stage_temporal)(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id,
        const GpuRenderTemporalCullPolicy *policy);
    void (*reject)(uint32_t blocker);
} XgFieldParticlePipelineServices;

void xg_field_particles_reset(void);
void xg_field_particles_classify_code_write(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
void xg_field_particles_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
bool xg_field_particles_lookup(
    uint32_t particle_address, XgRenderParticleSource *out_source);
void xg_field_particles_invalidate(uint32_t particle_address);
bool xg_field_particles_shared_data_write_overlaps(
    uint32_t address, uint32_t size);
void xg_field_particles_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
bool xg_field_particles_observe_initializer(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    bool artifact_candidate_authorized, uint32_t producer_pc);
bool xg_field_particles_source_matches_memory(
    CPUState *cpu, const XgRenderParticleSource *source);
bool xg_field_particles_build_matrix(
    CPUState *cpu, uint32_t particle_address, uint32_t source_matrix_address,
    uint32_t extra_scale_address, uint32_t composition_selector,
    XgHost3dMatrix *matrix);
bool xg_field_particles_cutover(
    CPUState *cpu, uint32_t producer_pc,
    const XgFieldParticlePipelineServices *services);

#endif
