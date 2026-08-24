#include "xg_render_ui_ot.h"

#include "gpu.h"
#include "guest_render_native_stream.h"

#include <stddef.h>
#include <stdlib.h>

enum {
    UI_OT_MAX_NODES = 131072u,
    UI_OT_MAX_CANDIDATES = 4096u,
};

typedef struct XgRenderUiOtCandidate {
    uint32_t command_address;
    GpuRenderSemantic semantic;
} XgRenderUiOtCandidate;

static bool pending;
static uint32_t pending_frame;
static GpuRenderTransactionId pending_visual_id;
static PsxXgRenderUiOtSnapshot snapshot;

static uint64_t hash_u32(uint64_t hash, uint32_t value) {
    for (uint32_t byte = 0u; byte < 4u; ++byte) {
        hash ^= (uint8_t)(value >> (byte * 8u));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_material(
        uint64_t hash, const GpuRenderMaterial *material) {
#define HASH_MATERIAL(field) \
    hash = hash_u32(hash, (uint32_t)material->field)
    HASH_MATERIAL(tpage);
    HASH_MATERIAL(texture_page_x);
    HASH_MATERIAL(texture_page_y);
    HASH_MATERIAL(clut_x);
    HASH_MATERIAL(clut_y);
    HASH_MATERIAL(draw_area_left);
    HASH_MATERIAL(draw_area_top);
    HASH_MATERIAL(draw_area_right);
    HASH_MATERIAL(draw_area_bottom);
    HASH_MATERIAL(draw_offset_x);
    HASH_MATERIAL(draw_offset_y);
    HASH_MATERIAL(texture_depth);
    HASH_MATERIAL(texture_window_mask_x);
    HASH_MATERIAL(texture_window_mask_y);
    HASH_MATERIAL(texture_window_offset_x);
    HASH_MATERIAL(texture_window_offset_y);
    HASH_MATERIAL(shading);
    HASH_MATERIAL(textured);
    HASH_MATERIAL(raw_texture);
    HASH_MATERIAL(semi_transparent);
    HASH_MATERIAL(blend_mode);
    HASH_MATERIAL(dither);
    HASH_MATERIAL(mask_set);
    HASH_MATERIAL(mask_check);
#undef HASH_MATERIAL
    return hash;
}

static uint64_t hash_semantic(
        uint64_t hash, const GpuRenderSemantic *semantic) {
    hash = hash_material(hash, &semantic->material);
    hash = hash_u32(hash, semantic->triangle_count);
    for (uint32_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];

        hash = hash_u32(hash, triangle->split_index);
        hash = hash_u32(hash, triangle->split_count);
        for (uint32_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];

            hash = hash_u32(hash, (uint32_t)vertex->x);
            hash = hash_u32(hash, (uint32_t)vertex->y);
            hash = hash_u32(hash, (uint32_t)vertex->u);
            hash = hash_u32(hash, (uint32_t)vertex->v);
            hash = hash_u32(hash, vertex->r);
            hash = hash_u32(hash, vertex->g);
            hash = hash_u32(hash, vertex->b);
        }
    }
    return hash;
}

static uint64_t hash_environment(
        uint64_t hash, const GpuNativeDrawEnvironment *environment) {
    const GpuDrawState *draw = &environment->draw;

#define HASH_DRAW(field) hash = hash_u32(hash, (uint32_t)draw->field)
    HASH_DRAW(left);
    HASH_DRAW(top);
    HASH_DRAW(right);
    HASH_DRAW(bottom);
    HASH_DRAW(offset_x);
    HASH_DRAW(offset_y);
    HASH_DRAW(texture_window_mask_x);
    HASH_DRAW(texture_window_mask_y);
    HASH_DRAW(texture_window_offset_x);
    HASH_DRAW(texture_window_offset_y);
    HASH_DRAW(dither);
    HASH_DRAW(mask_set);
    HASH_DRAW(mask_check);
#undef HASH_DRAW
    return hash_u32(hash, environment->tpage);
}

void xg_render_ui_ot_note_draw_observation(
        uint32_t frame, GpuRenderTransactionId visual_id) {
    if (visual_id.scene_epoch == 0u) return;
    pending = true;
    pending_frame = frame;
    pending_visual_id = visual_id;
}

void xg_render_ui_ot_clear_pending(void) {
    pending = false;
    pending_visual_id = (GpuRenderTransactionId){0};
}

bool xg_render_ui_ot_prepare(
        uint32_t start_address, GuestRenderRenderMode requested_mode,
        uint32_t current_frame, XgRenderUiOtReadWord read_word) {
    XgRenderUiOtCandidate *candidates;
    GpuNativeDrawEnvironment environment;
    uint32_t address = start_address & UINT32_C(0x001ffffc);
    uint32_t nodes = 0u;
    size_t candidate_count = 0u;
    uint32_t prebound_count = 0u;
    uint32_t staged_count = 0u;
    uint64_t ot_digest = UINT64_C(1469598103934665603);
    uint64_t packet_digest = UINT64_C(1469598103934665603);
    uint64_t semantic_digest = UINT64_C(1469598103934665603);
    uint64_t environment_digest = UINT64_C(1469598103934665603);
    GpuRenderTransactionId visual_id = {0};
    bool visual_open = false;
    bool success = false;

    if (!pending || requested_mode != GUEST_RENDER_RENDER_NATIVE) return true;
    if (current_frame != pending_frame) {
        snapshot.pending = true;
        ++snapshot.blocked_count;
        snapshot.blocked = true;
        return false;
    }
    ++snapshot.prepare_count;
    snapshot.pending = true;
    snapshot.last_start_address = address;
    candidates = (XgRenderUiOtCandidate *)calloc(
        UI_OT_MAX_CANDIDATES, sizeof(*candidates));
    if (candidates == NULL || read_word == NULL) {
        snapshot.pending = true;
        ++snapshot.blocked_count;
        snapshot.blocked = true;
        free(candidates);
        return false;
    }
    gpu_native_environment_get(&environment);

    for (;;) {
        uint32_t header;
        uint32_t packet_words;
        uint32_t next;
        uint32_t word_address;
        uint32_t word_offset = 0u;

        if (nodes++ >= UI_OT_MAX_NODES) goto done;
        header = read_word(address);
        packet_words = header >> 24u;
        next = header & UINT32_C(0x00ffffff);
        ot_digest = hash_u32(ot_digest, address);
        ot_digest = hash_u32(ot_digest, header);
        ot_digest = hash_u32(ot_digest, next);
        word_address = (address + 4u) & UINT32_C(0x001ffffc);
        while (word_offset < packet_words) {
            uint32_t words[GPU_GP0_RING_MAX_WORDS] = {0};
            uint32_t available = packet_words - word_offset;
            int command_words;
            uint8_t opcode;

            if (available > GPU_GP0_RING_MAX_WORDS)
                available = GPU_GP0_RING_MAX_WORDS;
            for (uint32_t index = 0u; index < available; ++index)
                words[index] = read_word(
                    (word_address + index * 4u) & UINT32_C(0x001ffffc));
            opcode = (uint8_t)(words[0] >> 24u);
            command_words = gpu_gp0_command_word_count(opcode);
            if (command_words <= 0 ||
                (uint32_t)command_words > packet_words - word_offset)
                goto done;
            if (command_words <= GPU_GP0_RING_MAX_WORDS &&
                (uint32_t)command_words > available) {
                for (uint32_t index = available;
                     index < (uint32_t)command_words; ++index)
                    words[index] = read_word(
                        (word_address + index * 4u) & UINT32_C(0x001ffffc));
            }
            environment_digest = hash_environment(
                environment_digest, &environment);
            for (uint32_t index = 0u; index < (uint32_t)command_words; ++index)
                ot_digest = hash_u32(ot_digest, words[index]);
            if (opcode >= 0x20u && opcode <= 0x7fu) {
                packet_digest = hash_u32(packet_digest, word_address);
                packet_digest = hash_u32(packet_digest, opcode);
                for (uint32_t index = 0u;
                     index < (uint32_t)command_words; ++index)
                    packet_digest = hash_u32(packet_digest, words[index]);
            }
            if (opcode >= 0x20u && opcode <= 0x7fu) {
                GpuRenderSemantic semantic;
                const int build = gpu_native_semantic_from_gp0(
                    words, command_words, &environment, &semantic);

                if (build != 1 || candidate_count == UI_OT_MAX_CANDIDATES)
                    goto done;
                candidates[candidate_count].command_address = word_address;
                candidates[candidate_count].semantic = semantic;
                semantic_digest = hash_semantic(semantic_digest, &semantic);
                ++candidate_count;
            }
            gpu_native_environment_apply(words, command_words, &environment);
            word_offset += (uint32_t)command_words;
            word_address = (word_address + (uint32_t)command_words * 4u) &
                UINT32_C(0x001ffffc);
        }
        if (next == UINT32_C(0x00ffffff)) break;
        if ((next & 3u) != 0u || next > UINT32_C(0x001ffffc)) goto done;
        address = next;
    }

    if (candidate_count != 0u) {
        visual_id = pending_visual_id;
        visual_open = true;
        for (size_t index = 0u; index < candidate_count; ++index) {
            if (guest_render_native_stream_stage_exact(
                    visual_id, candidates[index].command_address,
                    &candidates[index].semantic) !=
                        GUEST_RENDER_NATIVE_STREAM_OK)
                goto done;
            ++staged_count;
        }
        if (guest_render_native_stream_activate_visual(visual_id) !=
                GUEST_RENDER_NATIVE_STREAM_OK)
            goto done;
        visual_open = false;
    }
    success = true;

done:
    if (!success && visual_open)
        guest_render_native_stream_abandon_visual(visual_id);
    snapshot.node_count += nodes;
    snapshot.candidate_count += candidate_count;
    snapshot.prebound_count += prebound_count;
    snapshot.staged_count += staged_count;
    snapshot.last_node_count = nodes;
    snapshot.last_candidate_count = (uint32_t)candidate_count;
    snapshot.last_prebound_count = prebound_count;
    snapshot.last_staged_count = staged_count;
    snapshot.last_ot_digest = ot_digest;
    snapshot.last_packet_digest = packet_digest;
    snapshot.last_semantic_digest = semantic_digest;
    snapshot.last_environment_digest = hash_environment(
        environment_digest, &environment);
    snapshot.last_vram_serial = gpu_render_vram_mutation_serial();
    if (success) {
        pending = false;
        pending_visual_id = (GpuRenderTransactionId){0};
        snapshot.pending = false;
        snapshot.blocked = false;
        ++snapshot.completed_count;
    } else {
        snapshot.pending = true;
        ++snapshot.blocked_count;
        snapshot.blocked = true;
    }
    free(candidates);
    return success;
}

void xg_render_ui_ot_snapshot(PsxXgRenderUiOtSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = snapshot;
}

void xg_render_ui_ot_reset(void) {
    pending = false;
    pending_frame = 0u;
    pending_visual_id = (GpuRenderTransactionId){0};
    snapshot = (PsxXgRenderUiOtSnapshot){0};
}

void xg_render_ui_ot_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
        event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY)
        xg_render_ui_ot_clear_pending();
    else if (event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_render_ui_ot_reset();
}
