#ifndef XG_FIELD_RENDER_SERVICES_H
#define XG_FIELD_RENDER_SERVICES_H

#include "xg_host_3d.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

uint32_t xg_render_runtime_guest_address(uint32_t address);
int16_t xg_render_runtime_low_s16(uint32_t value);
bool xg_render_runtime_vector_address_is_valid(uint32_t address);
bool xg_render_runtime_stack_address_is_valid(uint32_t address);
bool xg_render_runtime_word_address_is_valid(uint32_t address);
void xg_render_runtime_capture_shadow_projection(
    const CPUState *cpu, XgHost3dProjection *projection);
bool xg_render_runtime_capture_matrix(
    CPUState *cpu, uint32_t address, XgHost3dMatrix *matrix);
void xg_render_runtime_store_matrix_rotation(
    CPUState *cpu, uint32_t address, const XgHost3dMatrix *matrix);

#endif
