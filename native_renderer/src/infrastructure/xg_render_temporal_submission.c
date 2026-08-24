#include "xg_render_temporal_submission.h"

#include "xg_render_ir.h"

#include <stddef.h>
#include <stdint.h>

enum {
    CURRENT_CAPACITY = XG_RENDER_IR_ITEM_CAPACITY,
    CANDIDATE_CAPACITY = XG_RENDER_IR_ITEM_CAPACITY,
    COVERAGE_HASH_CAPACITY = XG_RENDER_IR_ITEM_CAPACITY * 2u,
};

_Static_assert((COVERAGE_HASH_CAPACITY & (COVERAGE_HASH_CAPACITY - 1u)) == 0u,
               "temporal coverage hash capacity must be a power of two");

typedef struct XgRenderTemporalCandidate {
    GpuRenderSemantic semantic;
    GpuRenderTemporalCullPolicy policy;
} XgRenderTemporalCandidate;

typedef struct XgRenderTemporalSubmission {
    XgRenderTemporalCandidate candidates[CANDIDATE_CAPACITY];
    GpuRenderInterpolationIdentity current_identities[CURRENT_CAPACITY];
    int32_t coverage_hash[COVERAGE_HASH_CAPACITY];
    uint16_t coverage_hash_touched[CURRENT_CAPACITY];
    uint32_t candidate_count;
    uint32_t current_identity_count;
    uint32_t coverage_hash_touched_count;
    bool blocked;
} XgRenderTemporalSubmission;

static XgRenderTemporalSubmission submission;

static size_t identity_hash(const GpuRenderInterpolationIdentity *identity) {
    uint64_t value = identity->scene_id;

    value ^= (uint64_t)identity->producer_id << 32u;
    value ^= identity->primitive_id;
    value ^= value >> 33u;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33u;
    return (size_t)value & (COVERAGE_HASH_CAPACITY - 1u);
}

static bool identity_equal(
        const GpuRenderInterpolationIdentity *left,
        const GpuRenderInterpolationIdentity *right) {
    return left->valid && right->valid && left->scene_id == right->scene_id &&
        left->producer_id == right->producer_id &&
        left->primitive_id == right->primitive_id;
}

static bool current_contains(const GpuRenderInterpolationIdentity *identity) {
    size_t slot;

    if (identity == NULL || !identity->valid) return false;
    slot = identity_hash(identity);
    for (uint32_t probe = 0u; probe < COVERAGE_HASH_CAPACITY; ++probe) {
        const int32_t entry = submission.coverage_hash[slot];

        if (entry == 0) return false;
        if (identity_equal(
                identity, &submission.current_identities[(size_t)entry - 1u]))
            return true;
        slot = (slot + 1u) & (COVERAGE_HASH_CAPACITY - 1u);
    }
    return false;
}

void xg_render_temporal_submission_reset(void) {
    for (uint32_t index = 0u;
         index < submission.coverage_hash_touched_count; ++index)
        submission.coverage_hash[submission.coverage_hash_touched[index]] = 0;
    submission.candidate_count = 0u;
    submission.current_identity_count = 0u;
    submission.coverage_hash_touched_count = 0u;
    submission.blocked = false;
}

bool xg_render_temporal_submission_cover_current(
        const GpuRenderSemantic *semantic) {
    const GpuRenderInterpolationIdentity *identity;
    size_t slot;

    if (semantic == NULL) {
        submission.blocked = true;
        return false;
    }
    identity = &semantic->interpolation_identity;
    if (!identity->valid) return true;
    slot = identity_hash(identity);
    for (uint32_t probe = 0u; probe < COVERAGE_HASH_CAPACITY; ++probe) {
        const int32_t entry = submission.coverage_hash[slot];

        if (entry == 0) {
            uint32_t index;

            if (submission.current_identity_count == CURRENT_CAPACITY ||
                submission.coverage_hash_touched_count == CURRENT_CAPACITY) {
                submission.blocked = true;
                return false;
            }
            index = submission.current_identity_count++;
            submission.current_identities[index] = *identity;
            submission.coverage_hash[slot] = (int32_t)index + 1;
            submission.coverage_hash_touched[
                submission.coverage_hash_touched_count++] = (uint16_t)slot;
            return true;
        }
        if (identity_equal(
                identity, &submission.current_identities[(size_t)entry - 1u]))
            return true;
        slot = (slot + 1u) & (COVERAGE_HASH_CAPACITY - 1u);
    }
    submission.blocked = true;
    return false;
}

bool xg_render_temporal_submission_stage(
        const GpuRenderSemantic *semantic,
        const GpuRenderTemporalCullPolicy *policy) {
    uint32_t index;

    if (semantic == NULL || policy == NULL || submission.blocked) return false;
    if (submission.candidate_count == CANDIDATE_CAPACITY) {
        submission.blocked = true;
        return false;
    }
    index = submission.candidate_count++;
    submission.candidates[index] = (XgRenderTemporalCandidate){
        *semantic, *policy,
    };
    return true;
}

bool xg_render_temporal_submission_flush(void) {
    if (submission.blocked) return false;
    for (uint32_t index = 0u; index < submission.candidate_count; ++index) {
        const XgRenderTemporalCandidate *candidate =
            &submission.candidates[index];

        if (current_contains(&candidate->semantic.interpolation_identity))
            continue;
        if (gr_draw_semantic_temporal_candidate(
                &candidate->semantic, &candidate->policy) !=
                    GPU_RENDER_TRANSACTION_OK)
            return false;
    }
    xg_render_temporal_submission_reset();
    return true;
}
