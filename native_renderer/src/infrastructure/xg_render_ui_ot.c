#include "xg_render_ui_ot.h"

#include "gpu.h"
#include "guest_render_native_stream.h"
#include "xg_render_submission.h"
#include "xg_render_fragment_runtime.h"

#include <stddef.h>
#include <stdlib.h>

enum {
    UI_OT_MAX_NODES = 131072u,
    UI_OT_MAX_CANDIDATES = 4096u,
    UI_OT_MAX_WORDS = 131072u,
};

static bool pending;
static uint32_t pending_frame;
static uint32_t pending_start_address;
static GpuRenderTransactionId pending_visual_id;
static XgRenderSourceFrameDescription pending_description;
static GpuRenderTransactionId last_adapter_visual;
static bool prepared;
static uint32_t prepared_start_address;
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
    hash = hash_u32(hash, semantic->topology);
    hash = hash_u32(hash, semantic->screen_space_2d);
    hash = hash_u32(hash, semantic->native_view_effect);
    hash = hash_u32(hash, semantic->native_view_effect_index);
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
    hash = hash_u32(hash, semantic->line_count);
    for (uint32_t line = 0u; line < semantic->line_count; ++line) {
        for (uint32_t vertex = 0u; vertex < 2u; ++vertex) {
            const GpuRenderSemanticVertex *point =
                &semantic->lines[line].vertices[vertex];
            hash = hash_u32(hash, (uint32_t)point->x);
            hash = hash_u32(hash, (uint32_t)point->y);
            hash = hash_u32(hash, point->r);
            hash = hash_u32(hash, point->g);
            hash = hash_u32(hash, point->b);
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
        uint32_t frame, uint32_t start_address,
        GpuRenderTransactionId visual_id,
        const XgRenderSourceFrameDescription *description) {
    if (visual_id.scene_epoch == 0u || description == NULL ||
        description->scene_generation == 0u) return;
    pending = true;
    pending_frame = frame;
    pending_start_address = start_address & UINT32_C(0x001ffffc);
    pending_visual_id = visual_id;
    pending_description = *description;
}

void xg_render_ui_ot_clear_pending(void) {
    xg_render_submission_cancel_ordering_table();
    prepared = false;
    pending = false;
    pending_start_address = 0u;
    pending_visual_id = (GpuRenderTransactionId){0};
}

bool xg_render_ui_ot_pending_matches(
        uint32_t start_address, const XgRenderSourceFrameDescription *description) {
    return pending && description != NULL &&
        pending_start_address == (start_address & UINT32_C(0x001ffffc)) &&
        pending_description.scene_generation == description->scene_generation &&
        pending_description.scene.module == description->scene.module &&
        pending_description.scene.executable_identity ==
            description->scene.executable_identity &&
        pending_description.scene.primary_overlay_identity ==
            description->scene.primary_overlay_identity;
}

bool xg_render_ui_ot_prepare(uint32_t start_address,
                             GuestRenderRenderMode requested_mode,
                             uint32_t current_frame,
                             XgRenderUiOtReadWord read_word) {
    XgRenderSubmissionCommand *candidates = NULL;
    uint32_t *words = NULL;
    uint32_t *word_addresses = NULL;
    uint8_t *seen_commands = NULL;
    GpuNativeDrawEnvironment environment;
    uint32_t address = start_address & UINT32_C(0x001ffffc);
    uint32_t nodes = 0u;
    uint32_t word_count = 0u;
    uint32_t candidate_count = 0u;
    uint32_t prebound_count = 0u;
    uint32_t staged_count = 0u;
    uint64_t ot_digest = UINT64_C(1469598103934665603);
    uint64_t packet_digest = UINT64_C(1469598103934665603);
    uint64_t semantic_digest = UINT64_C(1469598103934665603);
    uint64_t environment_digest = UINT64_C(1469598103934665603);
    GpuRenderTransactionId visual_id = {0};
    bool visual_open = false;
    bool success = false;

    if (xg_render_submission_native_work_mode()) {
        xg_render_ui_ot_clear_pending();
        return true;
    }
    if (!pending || requested_mode != GUEST_RENDER_RENDER_NATIVE) return true;
    /* Bind DMA to its authenticated root. VBlank is not a lifetime limit for
     * a queued DrawOTag; scene identity and packet matching govern validity. */
    (void)current_frame;
    if (prepared || address != pending_start_address) {
        snapshot.pending = true;
        ++snapshot.blocked_count;
        snapshot.blocked = true;
        return false;
    }
    ++snapshot.prepare_count;
    snapshot.pending = true;
    snapshot.last_start_address = address;
    candidates = (XgRenderSubmissionCommand *)calloc(UI_OT_MAX_CANDIDATES,
                                                     sizeof(*candidates));
    words = malloc(UI_OT_MAX_WORDS * sizeof(*words));
    word_addresses = malloc(UI_OT_MAX_WORDS * sizeof(*word_addresses));
    seen_commands = calloc(UINT32_C(0x10000), 1u);
    if (candidates == NULL || words == NULL || word_addresses == NULL ||
        seen_commands == NULL || read_word == NULL)
        goto done;
    gpu_native_environment_get(&environment);

    /* The GP0 stream continues across DMA tags, including packets containing
     * several commands. Flatten payloads with their command IDs, never pixels.
     * This is an explicit transitional OT/packet adapter. */
    for (;;) {
        uint32_t header;
        uint32_t packet_words;
        uint32_t next;

        if (nodes++ >= UI_OT_MAX_NODES) goto done;
        header = read_word(address);
        packet_words = header >> 24u;
        next = header & UINT32_C(0x00ffffff);
        ot_digest = hash_u32(ot_digest, address);
        ot_digest = hash_u32(ot_digest, header);
        ot_digest = hash_u32(ot_digest, next);
        if (packet_words > UI_OT_MAX_WORDS - word_count) goto done;
        for (uint32_t index = 0u; index < packet_words; ++index) {
            const uint32_t word_address =
                (address + 4u + index * 4u) & UINT32_C(0x001ffffc);
            word_addresses[word_count] = word_address;
            words[word_count] = read_word(word_address);
            ot_digest = hash_u32(ot_digest, words[word_count++]);
        }
        if (next == UINT32_C(0x00ffffff)) break;
        if ((next & 3u) != 0u || next > UINT32_C(0x001ffffc)) goto done;
        address = next;
    }
    for (uint32_t offset = 0u; offset < word_count;) {
        const uint8_t opcode = (uint8_t)(words[offset] >> 24u);
        const uint32_t word_address = word_addresses[offset];
        int command_words = gpu_gp0_command_word_count(opcode);
        const bool polyline = (opcode >= 0x48u && opcode <= 0x4fu) ||
                              (opcode >= 0x58u && opcode <= 0x5fu);

        if (polyline) {
            const uint32_t stride = (opcode & 0x10u) != 0u ? 2u : 1u;
            uint32_t end = offset + (stride == 2u ? 4u : 3u);
            for (; end < word_count; end += stride)
                if ((words[end] & UINT32_C(0xf000f000)) == UINT32_C(0x50005000))
                    break;
            if (end >= word_count) goto done;
            command_words = (int)(end - offset + 1u);
        }
        if (command_words <= 0 || (uint32_t)command_words > word_count - offset)
            goto done;
        environment_digest = hash_environment(environment_digest, &environment);
        if (opcode >= 0x20u && opcode <= 0x7fu) {
            const uint32_t word_index = word_address >> 2u;
            const uint8_t bit = (uint8_t)(1u << (word_index & 7u));
            if ((seen_commands[word_index >> 3u] & bit) != 0u) goto done;
            seen_commands[word_index >> 3u] |= bit;
            packet_digest = hash_u32(packet_digest, word_address);
            packet_digest = hash_u32(packet_digest, opcode);
            for (uint32_t index = 0u; index < (uint32_t)command_words; ++index)
                packet_digest = hash_u32(packet_digest, words[offset + index]);
            const GuestRenderNativeDiagnosticSource source = {
                .valid_fields =
                    GUEST_RENDER_NATIVE_DIAGNOSTIC_SOURCE_COMMAND_ID |
                    GUEST_RENDER_NATIVE_DIAGNOSTIC_SOURCE_VISUAL_ID |
                    GUEST_RENDER_NATIVE_DIAGNOSTIC_SOURCE_ADDRESS |
                    GUEST_RENDER_NATIVE_DIAGNOSTIC_SOURCE_OPCODE |
                    GUEST_RENDER_NATIVE_DIAGNOSTIC_SOURCE_KIND,
                .source_word_address = word_address,
                .command_id = word_address,
                .visual_id = pending_visual_id,
                .source_kind =
                    GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
                .opcode = opcode,
            };
            GpuRenderSemantic semantic;
            int build;

            if (guest_render_native_stream_note_diagnostic_event(
                    GUEST_RENDER_NATIVE_DIAGNOSTIC_TARGET_OT_PAYLOAD_READ,
                    &source) != GUEST_RENDER_NATIVE_STREAM_OK)
                goto done;
            build = opcode >= 0x40u && opcode <= 0x5fu
                        ? gpu_native_line_semantic_from_gp0(
                              &words[offset], (size_t)command_words,
                              &environment, &semantic)
                        : gpu_native_semantic_from_gp0(&words[offset],
                                                       command_words,
                                                       &environment, &semantic);

            if (build != 1 || candidate_count == UI_OT_MAX_CANDIDATES)
                goto done;
            if (!xg_render_submission_resolve_command(
                    &pending_description, word_address, &semantic,
                    &candidates[candidate_count]))
                goto done;
            candidates[candidate_count].opcode = opcode;
            if (candidates[candidate_count].producer_captured)
                ++prebound_count;
            else if (guest_render_native_stream_note_diagnostic_event(
                         GUEST_RENDER_NATIVE_DIAGNOSTIC_PACKET_DERIVED,
                         &source) != GUEST_RENDER_NATIVE_STREAM_OK)
                goto done;
            semantic_digest = hash_semantic(
                semantic_digest, &candidates[candidate_count].semantic);
            ++candidate_count;
        } else if (opcode != 0x00u && opcode != 0x01u && opcode != 0x1fu &&
                   !(opcode >= 0xe1u && opcode <= 0xe6u)) {
            /* Transfers within an OT need ordered surface events. Never
             * silently snapshot draws against the pre-transfer resources. */
            goto done;
        }
        gpu_native_environment_apply(&words[offset], command_words,
                                     &environment);
        offset += (uint32_t)command_words;
    }

    if (!xg_render_submission_prepare_ordering_table(
            &pending_description, start_address, nodes + word_count, candidates,
            candidate_count))
        goto done;
    if (candidate_count != 0u) {
        visual_id = pending_visual_id;
        if (visual_id.scene_epoch == last_adapter_visual.scene_epoch &&
            visual_id.state_sequence < last_adapter_visual.state_sequence)
            visual_id.state_sequence = last_adapter_visual.state_sequence;
        if (visual_id.state_sequence == UINT64_MAX) goto done;
        ++visual_id.state_sequence;
        /* All captured geometry is now copied into this complete OT. The
         * source-capture cache, unlike this transport stream, retains commands
         * belonging to other OTs and to the alternate packet arena. */
        guest_render_native_stream_clear();
        visual_open = true;
        for (size_t index = 0u; index < candidate_count; ++index) {
            if (guest_render_native_stream_stage_exact(
                    visual_id, candidates[index].command_id,
                    &candidates[index].semantic) !=
                GUEST_RENDER_NATIVE_STREAM_OK)
                goto done;
            ++staged_count;
        }
        if (guest_render_native_stream_activate_visual(visual_id) !=
            GUEST_RENDER_NATIVE_STREAM_OK)
            goto done;
        visual_open = false;
        last_adapter_visual = visual_id;
    }
    prepared = true;
    prepared_start_address = start_address & UINT32_C(0x001ffffc);
    success = true;

done:
    if (!success) {
        xg_render_submission_cancel_ordering_table();
        xg_render_fragment_runtime_reject_source_frame();
    }
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
    snapshot.last_environment_digest =
        hash_environment(environment_digest, &environment);
    snapshot.last_vram_serial = gpu_render_vram_mutation_serial();
    if (success) {
        pending = false;
        pending_start_address = 0u;
        pending_visual_id = (GpuRenderTransactionId){0};
        snapshot.pending = false;
        snapshot.blocked = false;
    } else {
        snapshot.pending = true;
        ++snapshot.blocked_count;
        snapshot.blocked = true;
    }
    free(candidates);
    free(words);
    free(word_addresses);
    free(seen_commands);
    return success;
}

bool xg_render_ui_ot_complete(uint32_t start_address,
                              uint32_t transferred_words, bool *out_published) {
    if (out_published != NULL) *out_published = false;
    if (!prepared) return true;
    prepared = false;
    if ((start_address & UINT32_C(0x001ffffc)) != prepared_start_address) {
        xg_render_submission_cancel_ordering_table();
        return false;
    }
    const bool success = xg_render_submission_complete_ordering_table(
        start_address, transferred_words);
    if (!success) {
        ++snapshot.blocked_count;
        snapshot.blocked = true;
    } else {
        ++snapshot.completed_count;
        if (out_published != NULL) *out_published = true;
    }
    return success;
}

void xg_render_ui_ot_snapshot(PsxXgRenderUiOtSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = snapshot;
}

void xg_render_ui_ot_reset(void) {
    xg_render_ui_ot_clear_pending();
    pending_frame = 0u;
    pending_start_address = 0u;
    pending_visual_id = (GpuRenderTransactionId){0};
    last_adapter_visual = (GpuRenderTransactionId){0};
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
