#ifndef XG_FIELD_PARTICLES_H
#define XG_FIELD_PARTICLES_H

#include "guest_render_bridge.h"
#include "xg_host_3d.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

enum { XG_RENDER_PARTICLE_SOURCE_CAPACITY = 128u };

typedef struct XgRenderParticleSource {
    uint64_t generation;
    uint32_t particle_address;
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

void xg_field_particles_reset(void);
XgRenderParticleSourceState *xg_field_particles_state(void);
XgRenderParticleSource *xg_field_particles_find(uint32_t particle_address);
void xg_field_particles_invalidate(uint32_t particle_address);
bool xg_field_particles_observe_initializer(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    bool artifact_candidate_authorized);
bool xg_field_particles_source_matches_memory(
    CPUState *cpu, const XgRenderParticleSource *source);
bool xg_field_particles_build_matrix(
    CPUState *cpu, uint32_t particle_address, uint32_t source_matrix_address,
    uint32_t extra_scale_address, uint32_t composition_selector,
    XgHost3dMatrix *matrix);

#endif
