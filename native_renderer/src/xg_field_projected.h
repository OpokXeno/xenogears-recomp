#ifndef XG_FIELD_PROJECTED_H
#define XG_FIELD_PROJECTED_H

#include "xg_render_ir.h"

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

void xg_field_projected_reset(void);
void xg_field_projected_reset_pending(void);
XgRenderProjectedSourceState *xg_field_projected_state(void);
XgRenderProjectedSource *xg_field_projected_find(uint32_t object_address);
void xg_field_projected_observe_initializer_begin(CPUState *cpu,
                                                    GuestRenderRenderMode mode);
void xg_field_projected_observe_initializer_commit(CPUState *cpu);
XgRenderProjectedInitializerPending *xg_field_projected_initializer_pending(void);

#endif
