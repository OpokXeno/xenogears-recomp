#ifndef XG_RENDER_TEMPORAL_SUBMISSION_H
#define XG_RENDER_TEMPORAL_SUBMISSION_H

#include "gpu_render.h"

#include <stdbool.h>

/* Accumulates prior-frame candidates and suppresses identities seen now. */
void xg_render_temporal_submission_reset(void);
bool xg_render_temporal_submission_cover_current(
    const GpuRenderSemantic *semantic);
bool xg_render_temporal_submission_stage(
    const GpuRenderSemantic *semantic,
    const GpuRenderTemporalCullPolicy *policy);
bool xg_render_temporal_submission_flush(void);

#endif
