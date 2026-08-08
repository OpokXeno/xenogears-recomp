#ifndef XG_FIELD_COMPASS_H
#define XG_FIELD_COMPASS_H

#include "xg_field_character_capture.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

bool xg_field_compass_capture_material(
    CPUState *cpu, uint32_t source_address,
    XgFieldCharacterCapture *capture);

#endif
