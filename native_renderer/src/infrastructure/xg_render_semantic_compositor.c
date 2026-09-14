#include "xg_render_semantic_compositor.h"

#include "xg_render_resource_repository.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct XgRenderSemanticOwnerSlot {
    XgRenderSemanticOwnerKey key;
    XgRenderSemanticOwnerAuthority authority;
    bool active;
} XgRenderSemanticOwnerSlot;

typedef struct XgRenderSemanticBank {
    XgRenderSourceFrameDescription description;
    XgRenderSemanticOwnerSlot owners[XG_RENDER_SEMANTIC_OWNER_CAPACITY];
    XgSemanticPassRecord passes[XG_RENDER_SCENE_PASS_CAPACITY];
    XgSemanticDrawRecord draws[XG_RENDER_SCENE_DRAW_CAPACITY];
    XgSemanticResourceRef resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY];
    XgSemanticUiNodeRecord ui_nodes[XG_RENDER_SCENE_UI_NODE_CAPACITY];
    XgSemanticUiGlyphRunRecord
        ui_runs[XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY];
    XgSemanticUiGlyphPlacementRecord
        ui_placements[XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY];
    uint64_t pass_owners[XG_RENDER_SCENE_PASS_CAPACITY];
    uint8_t draw_owners[XG_RENDER_SCENE_DRAW_CAPACITY];
    bool draw_resources_follow_vram_mutations[XG_RENDER_SCENE_DRAW_CAPACITY];
    uint64_t resource_owners[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    uint64_t edge_owners[XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY];
    uint8_t ui_node_owners[XG_RENDER_SCENE_UI_NODE_CAPACITY];
    uint8_t ui_run_owners[XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY];
    uint8_t ui_placement_owners[XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY];
    uint64_t touched_owners;
    uint32_t pass_count;
    uint32_t draw_count;
    uint32_t resource_count;
    uint32_t edge_count;
    uint32_t ui_node_count;
    uint32_t ui_run_count;
    uint32_t ui_placement_count;
    bool active;
    bool blocked;
    bool field_submission_required;
} XgRenderSemanticBank;

enum {
    XG_RENDER_FIELD_COMPLETED_CAPACITY = 3u,
    XG_RENDER_FIELD_DISPLAYED_CAPACITY = 3u,
};

typedef struct XgRenderCompletedFieldSubmission {
    XgRenderSemanticBank bank;
    uint64_t sequence;
    uint32_t start_address;
    uint32_t transferred_words;
    uint16_t draw_left;
    uint16_t draw_top;
    uint16_t draw_right;
    uint16_t draw_bottom;
    bool active;
} XgRenderCompletedFieldSubmission;

typedef struct XgRenderDisplayedFieldPage {
    XgRenderSemanticBank bank;
    uint64_t sequence;
    bool active;
} XgRenderDisplayedFieldPage;

typedef enum XgRenderPreparedBankKind {
    XG_RENDER_PREPARED_BANK_NONE = 0,
    XG_RENDER_PREPARED_BANK_PENDING,
    XG_RENDER_PREPARED_BANK_COMPLETED_FIELD,
    XG_RENDER_PREPARED_BANK_DISPLAYED_FIELD,
    XG_RENDER_PREPARED_BANK_DISPLAY_ROOT,
} XgRenderPreparedBankKind;

typedef struct XgRenderSemanticMaterialized {
    XgSemanticPassRecord passes[XG_RENDER_SCENE_PASS_CAPACITY];
    XgSemanticDrawRecord draws[XG_RENDER_SCENE_DRAW_CAPACITY];
    XgSemanticResourceRef resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgSemanticSurfaceEdge edges[XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY];
    XgSemanticUiNodeRecord ui_nodes[XG_RENDER_SCENE_UI_NODE_CAPACITY];
    XgSemanticUiGlyphRunRecord
        ui_runs[XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY];
    XgSemanticUiGlyphPlacementRecord
        ui_placements[XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY];
} XgRenderSemanticMaterialized;

struct XgRenderSemanticCompositorTransaction {
    XgRenderSemanticBank pending;
    XgRenderSemanticCompositorDiagnostics diagnostics;
    uint64_t lifecycle_generation;
};

static XgRenderSemanticBank g_retained;
static XgRenderSemanticBank g_pending;
static XgRenderSemanticBank g_display_root;
static XgRenderCompletedFieldSubmission
    g_completed_field[XG_RENDER_FIELD_COMPLETED_CAPACITY];
static XgRenderDisplayedFieldPage
    g_displayed_field[XG_RENDER_FIELD_DISPLAYED_CAPACITY];
static XgRenderSemanticMaterialized g_materialized;
static XgRenderSemanticCompositorDiagnostics g_diagnostics;
static XgRenderSourceFrameDescription g_field_boundary_target_description;
static XgSemanticResourceRef g_field_boundary_target;
static XgRenderSemanticBoundaryKind g_prepared_kind;
static XgRenderPreparedBankKind g_prepared_bank_kind;
static uint32_t g_prepared_completed_field_index;
static uint32_t g_prepared_displayed_field_index;
static uint64_t g_next_completed_field_sequence = 1u;
static uint64_t g_next_displayed_field_sequence = 1u;
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static bool g_prepared;
static uint64_t g_lifecycle_generation;

static void compositor_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {}
}

static void compositor_unlock(void) {
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

static bool owner_key_equal(const XgRenderSemanticOwnerKey *left,
                            const XgRenderSemanticOwnerKey *right) {
    return left->module == right->module && left->domain == right->domain &&
        left->primary_root == right->primary_root &&
        left->secondary_root == right->secondary_root;
}

static bool authority_equal(const XgRenderSemanticOwnerAuthority *left,
                            const XgRenderSemanticOwnerAuthority *right) {
    return left->scene_generation == right->scene_generation &&
        left->owner_generation == right->owner_generation &&
        left->receipt == right->receipt && left->proof == right->proof;
}

static bool owner_valid(const XgRenderSemanticOwnerKey *owner,
                        const XgRenderSemanticOwnerAuthority *authority) {
    return owner != NULL && authority != NULL &&
        owner->module <= XG_SEMANTIC_MODULE_MOVIE &&
        owner->domain < XG_RENDER_SEMANTIC_OWNER_DOMAIN_COUNT &&
        owner->primary_root != 0u && authority->scene_generation != 0u &&
        authority->owner_generation != 0u && authority->receipt != 0u;
}

static bool scene_equal(const XgSemanticSceneIdentity *left,
                        const XgSemanticSceneIdentity *right) {
    return left->disc_id == right->disc_id &&
        left->executable_identity == right->executable_identity &&
        left->primary_overlay_identity == right->primary_overlay_identity &&
        left->companion_set_identity == right->companion_set_identity &&
        left->authored_scene_id == right->authored_scene_id &&
        left->authored_submode == right->authored_submode &&
        left->module == right->module;
}

static bool display_equal(const XgSemanticDisplayState *left,
                          const XgSemanticDisplayState *right) {
    return left->width == right->width && left->height == right->height &&
        left->display_x == right->display_x &&
        left->display_y == right->display_y &&
        left->aspect_num == right->aspect_num &&
        left->aspect_den == right->aspect_den &&
        left->depth24 == right->depth24 &&
        left->interlaced == right->interlaced &&
        left->disabled == right->disabled &&
        left->native_width == right->native_width &&
        left->native_height == right->native_height &&
        left->native_offset_x == right->native_offset_x &&
        left->temporal_hz == right->temporal_hz &&
        left->dithering_disabled == right->dithering_disabled &&
        (left->render_scale ? left->render_scale : 1u) ==
            (right->render_scale ? right->render_scale : 1u);
}

static bool resource_equal(const XgSemanticResourceRef *left,
                           const XgSemanticResourceRef *right) {
    return left->resource_id == right->resource_id &&
        left->generation == right->generation &&
        left->content_digest == right->content_digest;
}

static bool resource_handle_equal(const XgSemanticResourceRef *left,
                                  const XgSemanticResourceRef *right) {
    return left->resource_id == right->resource_id &&
        left->generation == right->generation;
}

static bool pass_equal(const XgSemanticPassRecord *left,
                       const XgSemanticPassRecord *right) {
    return left->pass_id == right->pass_id &&
        left->target_surface_id == right->target_surface_id &&
        left->target_generation == right->target_generation &&
        left->dependency_mask == right->dependency_mask &&
        left->viewport_x == right->viewport_x &&
        left->viewport_y == right->viewport_y &&
        left->viewport_width == right->viewport_width &&
        left->viewport_height == right->viewport_height &&
        left->load_operation == right->load_operation &&
        left->store == right->store &&
        left->presentation_output == right->presentation_output;
}

static bool edge_equal(const XgSemanticSurfaceEdge *left,
                       const XgSemanticSurfaceEdge *right) {
    return left->source_surface_id == right->source_surface_id &&
        left->source_generation == right->source_generation &&
        left->target_surface_id == right->target_surface_id &&
        left->target_generation == right->target_generation &&
        left->kind == right->kind &&
        left->order.pass_id == right->order.pass_id &&
        left->order.layer == right->order.layer &&
        left->order.authored_depth == right->order.authored_depth &&
        left->order.insertion_ordinal == right->order.insertion_ordinal &&
        left->order.split_ordinal == right->order.split_ordinal &&
        left->source.x == right->source.x &&
        left->source.y == right->source.y &&
        left->source.width == right->source.width &&
        left->source.height == right->source.height &&
        left->destination.x == right->destination.x &&
        left->destination.y == right->destination.y &&
        left->destination.width == right->destination.width &&
        left->destination.height == right->destination.height &&
        left->sample.sampler == right->sample.sampler &&
        left->sample.wrap_u == right->sample.wrap_u &&
        left->sample.wrap_v == right->sample.wrap_v &&
        left->sample.clut_resource_id == right->sample.clut_resource_id &&
        left->sample.clut_generation == right->sample.clut_generation &&
        left->sample.clut_x == right->sample.clut_x &&
        left->sample.clut_y == right->sample.clut_y &&
        left->sample.texture_window_mask_x ==
            right->sample.texture_window_mask_x &&
        left->sample.texture_window_mask_y ==
            right->sample.texture_window_mask_y &&
        left->sample.texture_window_offset_x ==
            right->sample.texture_window_offset_x &&
        left->sample.texture_window_offset_y ==
            right->sample.texture_window_offset_y &&
        left->sample.texture_depth == right->sample.texture_depth &&
        left->sample.blend_mode == right->sample.blend_mode &&
        left->sample.palette_enabled == right->sample.palette_enabled &&
        left->sample.semi_transparent == right->sample.semi_transparent &&
        left->sample.mask_set == right->sample.mask_set &&
        left->sample.mask_check == right->sample.mask_check &&
        left->effect_phase == right->effect_phase &&
        left->effect_phase_count == right->effect_phase_count;
}

static void bank_release_resources(XgRenderSemanticBank *bank) {
    for (uint32_t index = 0u; index < bank->resource_count; ++index)
        (void)xg_render_resource_release((XgRenderResourceHandle){
            bank->resources[index].resource_id,
            bank->resources[index].generation,
        });
}

/* Record arrays and their ownership arrays are count-bounded and populated
 * together before a count grows. Only owner slots are scanned by capacity. */
static void bank_forget(XgRenderSemanticBank *bank) {
    bank->description = (XgRenderSourceFrameDescription){0};
    for (uint32_t index = 0u; index < XG_RENDER_SEMANTIC_OWNER_CAPACITY; ++index)
        bank->owners[index].active = false;
    bank->touched_owners = 0u;
    bank->pass_count = 0u;
    bank->draw_count = 0u;
    bank->resource_count = 0u;
    bank->edge_count = 0u;
    bank->ui_node_count = 0u;
    bank->ui_run_count = 0u;
    bank->ui_placement_count = 0u;
    bank->active = false;
    bank->blocked = false;
    bank->field_submission_required = false;
}

static void bank_clear(XgRenderSemanticBank *bank) {
    bank_release_resources(bank);
    bank_forget(bank);
}

static XgRenderSemanticCompositorResult bank_clone(
        XgRenderSemanticBank *destination,
        const XgRenderSemanticBank *source) {
    *destination = *source;
    destination->touched_owners = 0u;
    destination->field_submission_required = false;
    for (uint32_t index = 0u; index < destination->resource_count; ++index) {
        const XgSemanticResourceRef *resource = &destination->resources[index];
        if (xg_render_resource_acquire_snapshot((XgRenderResourceHandle){
                resource->resource_id, resource->generation},
                resource->content_digest) != XG_RENDER_RESOURCE_OK) {
            destination->resource_count = index;
            bank_clear(destination);
            return XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
        }
    }
    return XG_RENDER_SEMANTIC_COMPOSITOR_OK;
}

static XgRenderSemanticCompositorResult bank_snapshot_exact(
        XgRenderSemanticBank *destination,
        const XgRenderSemanticBank *source) {
    *destination = *source;
    for (uint32_t index = 0u; index < destination->resource_count; ++index) {
        const XgSemanticResourceRef *resource = &destination->resources[index];

        if (xg_render_resource_acquire_snapshot((XgRenderResourceHandle){
                resource->resource_id, resource->generation},
                resource->content_digest) != XG_RENDER_RESOURCE_OK) {
            destination->resource_count = index;
            bank_clear(destination);
            return XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
        }
    }
    return XG_RENDER_SEMANTIC_COMPOSITOR_OK;
}

XgRenderSemanticCompositorResult
xg_render_semantic_compositor_transaction_begin(
        XgRenderSemanticCompositorTransaction **out_transaction) {
    XgRenderSemanticCompositorTransaction *transaction;
    XgRenderSemanticCompositorResult result;

    if (out_transaction == NULL)
        return XG_RENDER_SEMANTIC_COMPOSITOR_INVALID_ARGUMENT;
    *out_transaction = NULL;
    transaction = (XgRenderSemanticCompositorTransaction *)calloc(
        1u, sizeof(*transaction));
    if (transaction == NULL)
        return XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
    compositor_lock();
    if (g_prepared) {
        compositor_unlock();
        free(transaction);
        return XG_RENDER_SEMANTIC_COMPOSITOR_BLOCKED;
    }
    result = bank_snapshot_exact(&transaction->pending, &g_pending);
    if (result == XG_RENDER_SEMANTIC_COMPOSITOR_OK) {
        transaction->diagnostics = g_diagnostics;
        transaction->lifecycle_generation = g_lifecycle_generation;
    }
    compositor_unlock();
    if (result != XG_RENDER_SEMANTIC_COMPOSITOR_OK) {
        free(transaction);
        return result;
    }
    *out_transaction = transaction;
    return XG_RENDER_SEMANTIC_COMPOSITOR_OK;
}

void xg_render_semantic_compositor_transaction_commit(
        XgRenderSemanticCompositorTransaction *transaction) {
    if (transaction == NULL) return;
    bank_clear(&transaction->pending);
    free(transaction);
}

bool xg_render_semantic_compositor_transaction_rollback(
        XgRenderSemanticCompositorTransaction *transaction) {
    bool restored;

    if (transaction == NULL) return false;
    compositor_lock();
    restored = !g_prepared &&
        transaction->lifecycle_generation == g_lifecycle_generation;
    if (restored) {
        bank_clear(&g_pending);
        g_pending = transaction->pending;
        bank_forget(&transaction->pending);
        g_diagnostics = transaction->diagnostics;
    }
    compositor_unlock();
    xg_render_semantic_compositor_transaction_commit(transaction);
    return restored;
}

static const XgRenderSemanticBank *newest_completed_field_bank(
        const XgRenderSourceFrameDescription *description) {
    const XgRenderSemanticBank *newest = NULL;
    uint64_t sequence = 0u;

    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_COMPLETED_CAPACITY; ++index) {
        const XgRenderCompletedFieldSubmission *candidate =
            &g_completed_field[index];
        if (!candidate->active || candidate->sequence <= sequence ||
            !scene_equal(&candidate->bank.description.scene,
                         &description->scene) ||
            candidate->bank.description.scene_generation !=
                description->scene_generation)
            continue;
        newest = &candidate->bank;
        sequence = candidate->sequence;
    }
    return newest;
}

static uint32_t owner_count(const XgRenderSemanticBank *bank) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < XG_RENDER_SEMANTIC_OWNER_CAPACITY; ++index)
        if (bank->owners[index].active) ++count;
    return count;
}

static int owner_find(const XgRenderSemanticBank *bank,
                      const XgRenderSemanticOwnerKey *owner) {
    for (uint32_t index = 0u; index < XG_RENDER_SEMANTIC_OWNER_CAPACITY; ++index)
        if (bank->owners[index].active &&
            owner_key_equal(&bank->owners[index].key, owner))
            return (int)index;
    return -1;
}

static int owner_allocate(XgRenderSemanticBank *bank,
                          const XgRenderSemanticOwnerKey *owner,
                          const XgRenderSemanticOwnerAuthority *authority) {
    for (uint32_t index = 0u; index < XG_RENDER_SEMANTIC_OWNER_CAPACITY; ++index) {
        if (bank->owners[index].active) continue;
        bank->owners[index] = (XgRenderSemanticOwnerSlot){
            .key = *owner,
            .authority = *authority,
            .active = true,
        };
        return (int)index;
    }
    return -1;
}

static void remove_owner_records(XgRenderSemanticBank *bank,
                                 uint32_t owner_index) {
    const uint64_t owner_bit = UINT64_C(1) << owner_index;
    uint32_t write = 0u;

    for (uint32_t read = 0u; read < bank->pass_count; ++read) {
        bank->pass_owners[read] &= ~owner_bit;
        if (bank->pass_owners[read] == 0u) continue;
        if (write != read) {
            bank->passes[write] = bank->passes[read];
            bank->pass_owners[write] = bank->pass_owners[read];
        }
        ++write;
    }
    bank->pass_count = write;

    write = 0u;
    for (uint32_t read = 0u; read < bank->draw_count; ++read) {
        if (bank->draw_owners[read] == owner_index) continue;
        if (write != read) {
            bank->draws[write] = bank->draws[read];
            bank->draw_owners[write] = bank->draw_owners[read];
            bank->draw_resources_follow_vram_mutations[write] =
                bank->draw_resources_follow_vram_mutations[read];
        }
        ++write;
    }
    bank->draw_count = write;

    write = 0u;
    for (uint32_t read = 0u; read < bank->resource_count; ++read) {
        bank->resource_owners[read] &= ~owner_bit;
        if (bank->resource_owners[read] == 0u) {
            (void)xg_render_resource_release((XgRenderResourceHandle){
                bank->resources[read].resource_id,
                bank->resources[read].generation,
            });
            continue;
        }
        if (write != read) {
            bank->resources[write] = bank->resources[read];
            bank->resource_owners[write] = bank->resource_owners[read];
        }
        ++write;
    }
    bank->resource_count = write;

    write = 0u;
    for (uint32_t read = 0u; read < bank->edge_count; ++read) {
        bank->edge_owners[read] &= ~owner_bit;
        if (bank->edge_owners[read] == 0u) continue;
        if (write != read) {
            bank->edges[write] = bank->edges[read];
            bank->edge_owners[write] = bank->edge_owners[read];
        }
        ++write;
    }
    bank->edge_count = write;

#define REMOVE_EXCLUSIVE(records, owners, count)                                \
    do {                                                                         \
        write = 0u;                                                              \
        for (uint32_t read = 0u; read < bank->count; ++read) {                  \
            if (bank->owners[read] == owner_index) continue;                    \
            if (write != read) {                                                 \
                bank->records[write] = bank->records[read];                     \
                bank->owners[write] = bank->owners[read];                       \
            }                                                                    \
            ++write;                                                             \
        }                                                                        \
        bank->count = write;                                                      \
    } while (0)

    REMOVE_EXCLUSIVE(ui_nodes, ui_node_owners, ui_node_count);
    REMOVE_EXCLUSIVE(ui_runs, ui_run_owners, ui_run_count);
    REMOVE_EXCLUSIVE(ui_placements, ui_placement_owners, ui_placement_count);
#undef REMOVE_EXCLUSIVE
}

static XgRenderSemanticCompositorResult pending_begin(
        const XgRenderSourceFrameDescription *description) {
    XgRenderSemanticCompositorResult result;
    const XgRenderSemanticBank *completed;

    if (g_pending.active) return XG_RENDER_SEMANTIC_COMPOSITOR_OK;
    completed = description->scene.module == XG_SEMANTIC_MODULE_FIELD
        ? newest_completed_field_bank(description) : NULL;
    if (completed != NULL) {
        result = bank_clone(&g_pending, completed);
        if (result != XG_RENDER_SEMANTIC_COMPOSITOR_OK) return result;
    } else if (g_retained.active &&
        scene_equal(&g_retained.description.scene, &description->scene) &&
        g_retained.description.scene_generation ==
            description->scene_generation) {
        result = bank_clone(&g_pending, &g_retained);
        if (result != XG_RENDER_SEMANTIC_COMPOSITOR_OK) return result;
    } else {
        bank_forget(&g_pending);
    }
    g_pending.description = *description;
    g_pending.active = true;
    return XG_RENDER_SEMANTIC_COMPOSITOR_OK;
}

static XgRenderSemanticCompositorResult pending_description_merge(
        const XgRenderSourceFrameDescription *description) {
    if (description == NULL || description->scene_generation == 0u)
        return XG_RENDER_SEMANTIC_COMPOSITOR_INVALID_ARGUMENT;
    if (g_pending.active &&
        (!scene_equal(&g_pending.description.scene, &description->scene) ||
         g_pending.description.scene_generation != description->scene_generation))
        return XG_RENDER_SEMANTIC_COMPOSITOR_SCENE_MISMATCH;
    if (!g_pending.active) return pending_begin(description);
    g_pending.description.display = description->display;
    if (description->source_interval_vblanks >
            g_pending.description.source_interval_vblanks)
        g_pending.description.source_interval_vblanks =
            description->source_interval_vblanks;
    g_pending.description.discontinuity |= description->discontinuity;
    g_pending.description.temporally_eligible &=
        description->temporally_eligible;
    return XG_RENDER_SEMANTIC_COMPOSITOR_OK;
}

static XgRenderSemanticCompositorResult append_fragment(
        XgRenderSemanticBank *bank, uint32_t owner_index,
        const XgRenderSourceFrameFragment *fragment) {
    const uint64_t owner_bit = UINT64_C(1) << owner_index;

    if (fragment == NULL ||
        (fragment->pass_count != 0u && fragment->passes == NULL) ||
        (fragment->draw_count != 0u && fragment->draws == NULL) ||
        (fragment->resource_count != 0u && fragment->resources == NULL) ||
        (fragment->surface_edge_count != 0u && fragment->surface_edges == NULL) ||
        (fragment->ui_node_count != 0u && fragment->ui_nodes == NULL) ||
        (fragment->ui_glyph_run_count != 0u &&
         fragment->ui_glyph_runs == NULL) ||
        (fragment->ui_glyph_placement_count != 0u &&
         fragment->ui_glyph_placements == NULL))
        return XG_RENDER_SEMANTIC_COMPOSITOR_INVALID_ARGUMENT;
    if (fragment->pass_count > XG_RENDER_SCENE_PASS_CAPACITY - bank->pass_count ||
        fragment->draw_count > XG_RENDER_SCENE_DRAW_CAPACITY - bank->draw_count ||
        fragment->resource_count >
            XG_RENDER_SCENE_RESOURCE_CAPACITY - bank->resource_count ||
        fragment->surface_edge_count >
            XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY - bank->edge_count ||
        fragment->ui_node_count >
            XG_RENDER_SCENE_UI_NODE_CAPACITY - bank->ui_node_count ||
        fragment->ui_glyph_run_count >
            XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY - bank->ui_run_count ||
        fragment->ui_glyph_placement_count >
            XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY -
                bank->ui_placement_count)
        return XG_RENDER_SEMANTIC_COMPOSITOR_CAPACITY_EXCEEDED;

    for (uint32_t index = 0u; index < fragment->pass_count; ++index) {
        bool shared = false;
        for (uint32_t prior = 0u; prior < bank->pass_count; ++prior) {
            if (!pass_equal(&bank->passes[prior], &fragment->passes[index]))
                continue;
            bank->pass_owners[prior] |= owner_bit;
            shared = true;
            break;
        }
        if (shared) continue;
        bank->passes[bank->pass_count] = fragment->passes[index];
        bank->pass_owners[bank->pass_count++] = owner_bit;
    }
    for (uint32_t index = 0u; index < fragment->draw_count; ++index) {
        bank->draws[bank->draw_count] = fragment->draws[index];
        bank->draw_owners[bank->draw_count] = (uint8_t)owner_index;
        bank->draw_resources_follow_vram_mutations[bank->draw_count++] =
            fragment->resources_follow_vram_mutations;
    }
    for (uint32_t index = 0u; index < fragment->resource_count; ++index) {
        const XgSemanticResourceRef *resource = &fragment->resources[index];
        bool shared = false;
        if (resource->resource_id == 0u || resource->generation == 0u ||
            resource->content_digest == 0u)
            return XG_RENDER_SEMANTIC_COMPOSITOR_INVALID_ARGUMENT;
        for (uint32_t prior = 0u; prior < bank->resource_count; ++prior) {
            if (!resource_handle_equal(&bank->resources[prior], resource))
                continue;
            if (!resource_equal(&bank->resources[prior], resource))
                return XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
            bank->resource_owners[prior] |= owner_bit;
            shared = true;
            break;
        }
        if (shared) continue;
        if (xg_render_resource_acquire_snapshot((XgRenderResourceHandle){
                resource->resource_id, resource->generation},
                resource->content_digest) != XG_RENDER_RESOURCE_OK)
            return XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
        bank->resources[bank->resource_count] = *resource;
        bank->resource_owners[bank->resource_count++] = owner_bit;
    }
    for (uint32_t index = 0u; index < fragment->surface_edge_count; ++index) {
        bool shared = false;
        for (uint32_t prior = 0u; prior < bank->edge_count; ++prior) {
            if (!edge_equal(&bank->edges[prior], &fragment->surface_edges[index]))
                continue;
            bank->edge_owners[prior] |= owner_bit;
            shared = true;
            break;
        }
        if (shared) continue;
        bank->edges[bank->edge_count] = fragment->surface_edges[index];
        bank->edge_owners[bank->edge_count++] = owner_bit;
    }
#define APPEND_EXCLUSIVE(source, count, records, owners, bank_count)             \
    do {                                                                         \
        for (uint32_t index = 0u; index < fragment->count; ++index) {            \
            bank->records[bank->bank_count] = fragment->source[index];           \
            bank->owners[bank->bank_count++] = (uint8_t)owner_index;             \
        }                                                                        \
    } while (0)
    APPEND_EXCLUSIVE(ui_nodes, ui_node_count, ui_nodes, ui_node_owners,
                     ui_node_count);
    APPEND_EXCLUSIVE(ui_glyph_runs, ui_glyph_run_count, ui_runs,
                     ui_run_owners, ui_run_count);
    APPEND_EXCLUSIVE(ui_glyph_placements, ui_glyph_placement_count,
                     ui_placements, ui_placement_owners, ui_placement_count);
#undef APPEND_EXCLUSIVE
    return XG_RENDER_SEMANTIC_COMPOSITOR_OK;
}

static XgRenderSemanticCompositorResult update_owner(
        const XgRenderSemanticOwnerKey *owner,
        const XgRenderSemanticOwnerAuthority *authority,
        const XgRenderSourceFrameDescription *description,
        const XgRenderSourceFrameFragment *fragment, bool remove) {
    XgRenderSemanticCompositorResult result;
    int owner_index;
    uint64_t owner_bit;

    ++g_diagnostics.update_attempts;
    if (g_prepared || !owner_valid(owner, authority) || description == NULL ||
        description->scene.module != owner->module ||
        description->scene_generation != authority->scene_generation) {
        result = XG_RENDER_SEMANTIC_COMPOSITOR_INVALID_ARGUMENT;
        goto reject;
    }
    result = pending_description_merge(description);
    if (result != XG_RENDER_SEMANTIC_COMPOSITOR_OK) goto reject;
    owner_index = owner_find(&g_pending, owner);
    if (owner_index < 0) {
        owner_index = owner_allocate(&g_pending, owner, authority);
        if (owner_index < 0) {
            result = XG_RENDER_SEMANTIC_COMPOSITOR_CAPACITY_EXCEEDED;
            goto reject;
        }
    }
    owner_bit = UINT64_C(1) << (uint32_t)owner_index;
    if ((g_pending.touched_owners & owner_bit) == 0u) {
        remove_owner_records(&g_pending, (uint32_t)owner_index);
        g_pending.owners[owner_index].authority = *authority;
        g_pending.touched_owners |= owner_bit;
    } else if (!authority_equal(&g_pending.owners[owner_index].authority,
                                authority)) {
        result = XG_RENDER_SEMANTIC_COMPOSITOR_AUTHORITY_REJECTED;
        goto reject;
    }
    if (remove) {
        g_pending.owners[owner_index].active = false;
        ++g_diagnostics.removals;
    } else {
        result = append_fragment(&g_pending, (uint32_t)owner_index, fragment);
        if (result != XG_RENDER_SEMANTIC_COMPOSITOR_OK) goto reject;
        if (description->scene.module == XG_SEMANTIC_MODULE_FIELD &&
            owner->domain != XG_RENDER_SEMANTIC_OWNER_MOVIE &&
            (fragment->draw_count != 0u ||
             fragment->surface_edge_count != 0u ||
             fragment->ui_node_count != 0u ||
             fragment->ui_glyph_run_count != 0u))
            g_pending.field_submission_required = true;
    }
    ++g_diagnostics.accepted_updates;
    g_diagnostics.last_result = XG_RENDER_SEMANTIC_COMPOSITOR_OK;
    return XG_RENDER_SEMANTIC_COMPOSITOR_OK;

reject:
    g_pending.blocked = true;
    g_diagnostics.last_result = result;
    return result;
}

XgRenderSemanticCompositorResult xg_render_semantic_compositor_upsert(
        const XgRenderSemanticOwnerKey *owner,
        const XgRenderSemanticOwnerAuthority *authority,
        const XgRenderSourceFrameDescription *description,
        const XgRenderSourceFrameFragment *fragment) {
    XgRenderSemanticCompositorResult result;
    compositor_lock();
    result = update_owner(owner, authority, description, fragment, false);
    compositor_unlock();
    return result;
}

XgRenderSemanticCompositorResult
xg_render_semantic_compositor_rebind_mutated_resource(
        const XgSemanticResourceRef *previous,
        const XgSemanticResourceRef *replacement) {
    XgRenderSemanticCompositorResult result =
        XG_RENDER_SEMANTIC_COMPOSITOR_OK;
    XgRenderSemanticBank *bank;
    uint64_t affected_owners = 0u;
    int replacement_index = -1;
    bool previous_present = false;
    uint64_t rebound_draws = 0u;

    if (previous == NULL || replacement == NULL ||
        previous->resource_id == 0u || previous->generation == 0u ||
        previous->content_digest == 0u || replacement->resource_id == 0u ||
        replacement->generation == 0u || replacement->content_digest == 0u)
        return XG_RENDER_SEMANTIC_COMPOSITOR_INVALID_ARGUMENT;

    compositor_lock();
    ++g_diagnostics.mutation_rebind_attempts;
    g_diagnostics.mutation_previous_resource_id = previous->resource_id;
    g_diagnostics.mutation_previous_generation = previous->generation;
    g_diagnostics.mutation_replacement_resource_id = replacement->resource_id;
    g_diagnostics.mutation_replacement_generation = replacement->generation;
    bank = g_pending.active ? &g_pending :
        g_retained.active ? &g_retained : NULL;
    if (bank == NULL) goto missed;
    if (g_prepared || bank->blocked) {
        result = XG_RENDER_SEMANTIC_COMPOSITOR_BLOCKED;
        goto reject;
    }
    for (uint32_t index = 0u; index < bank->draw_count; ++index) {
        const XgSemanticDrawRecord *draw = &bank->draws[index];

        if (!bank->draw_resources_follow_vram_mutations[index]) continue;
        if ((draw->texture_resource_id == previous->resource_id &&
             draw->texture_generation == previous->generation) ||
            (draw->clut_resource_id == previous->resource_id &&
             draw->clut_generation == previous->generation))
            affected_owners |= UINT64_C(1) << bank->draw_owners[index];
    }
    if (affected_owners == 0u) goto missed;
    for (uint32_t index = 0u; index < bank->resource_count; ++index) {
        const XgSemanticResourceRef *resource = &bank->resources[index];

        if (resource_handle_equal(resource, previous)) {
            if (!resource_equal(resource, previous)) {
                result = XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
                goto reject;
            }
            previous_present = true;
        }
        if (resource_handle_equal(resource, replacement)) {
            if (!resource_equal(resource, replacement)) {
                result = XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
                goto reject;
            }
            replacement_index = (int)index;
        }
    }
    if (!previous_present) {
        result = XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
        goto reject;
    }
    if (replacement_index < 0) {
        if (bank->resource_count >= XG_RENDER_SCENE_RESOURCE_CAPACITY) {
            result = XG_RENDER_SEMANTIC_COMPOSITOR_CAPACITY_EXCEEDED;
            goto reject;
        }
        if (xg_render_resource_acquire_current((XgRenderResourceHandle){
                replacement->resource_id, replacement->generation},
                replacement->content_digest) != XG_RENDER_RESOURCE_OK) {
            result = XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
            goto reject;
        }
        replacement_index = (int)bank->resource_count;
        bank->resources[bank->resource_count] = *replacement;
        bank->resource_owners[bank->resource_count++] = affected_owners;
    } else {
        bank->resource_owners[replacement_index] |= affected_owners;
    }
    for (uint32_t index = 0u; index < bank->draw_count; ++index) {
        XgSemanticDrawRecord *draw = &bank->draws[index];

        if (!bank->draw_resources_follow_vram_mutations[index]) continue;
        if (draw->texture_resource_id == previous->resource_id &&
            draw->texture_generation == previous->generation) {
            draw->texture_resource_id = replacement->resource_id;
            draw->texture_generation = replacement->generation;
            ++rebound_draws;
        }
        if (draw->clut_resource_id == previous->resource_id &&
            draw->clut_generation == previous->generation) {
            draw->clut_resource_id = replacement->resource_id;
            draw->clut_generation = replacement->generation;
            ++rebound_draws;
        }
    }
    ++g_diagnostics.mutation_rebind_matches;
    g_diagnostics.mutation_rebound_draws += rebound_draws;
    goto finished;

missed:
    ++g_diagnostics.mutation_rebind_misses;
    goto finished;
reject:
    bank->blocked = true;
    g_diagnostics.last_result = result;
finished:
    compositor_unlock();
    return result;
}

XgRenderSemanticCompositorResult xg_render_semantic_compositor_remove(
        const XgRenderSemanticOwnerKey *owner,
        const XgRenderSemanticOwnerAuthority *authority,
        const XgRenderSourceFrameDescription *description) {
    XgRenderSemanticCompositorResult result;
    compositor_lock();
    result = update_owner(owner, authority, description, NULL, true);
    compositor_unlock();
    return result;
}

void xg_render_semantic_compositor_complete_field_submission(
        uint32_t start_address, uint32_t transferred_words,
        uint16_t draw_left, uint16_t draw_top,
        uint16_t draw_right, uint16_t draw_bottom) {
    uint32_t selected = XG_RENDER_FIELD_COMPLETED_CAPACITY;
    uint64_t oldest_sequence = UINT64_MAX;

    compositor_lock();
    if (transferred_words == 0u || draw_right < draw_left ||
        draw_bottom < draw_top || draw_right >= 1024u || draw_bottom >= 512u ||
        !g_pending.active || g_pending.description.scene.module !=
            XG_SEMANTIC_MODULE_FIELD ||
        !g_pending.field_submission_required) {
        compositor_unlock();
        return;
    }
    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_COMPLETED_CAPACITY; ++index) {
        XgRenderCompletedFieldSubmission *candidate =
            &g_completed_field[index];
        if (candidate->active && candidate->draw_left == draw_left &&
            candidate->draw_top == draw_top &&
            candidate->draw_right == draw_right &&
            candidate->draw_bottom == draw_bottom) {
            selected = index;
            break;
        }
        if (!candidate->active) {
            selected = index;
            break;
        }
        if (candidate->sequence < oldest_sequence) {
            oldest_sequence = candidate->sequence;
            selected = index;
        }
    }
    if (selected < XG_RENDER_FIELD_COMPLETED_CAPACITY) {
        XgRenderCompletedFieldSubmission *candidate =
            &g_completed_field[selected];
        ++g_lifecycle_generation;
        if (candidate->active) {
            bank_clear(&candidate->bank);
            ++g_diagnostics.superseded_field_submissions;
        }
        candidate->bank = g_pending;
        candidate->bank.field_submission_required = false;
        bank_forget(&g_pending);
        candidate->sequence = g_next_completed_field_sequence++;
        if (g_next_completed_field_sequence == 0u)
            g_next_completed_field_sequence = 1u;
        candidate->start_address = start_address;
        candidate->transferred_words = transferred_words;
        candidate->draw_left = draw_left;
        candidate->draw_top = draw_top;
        candidate->draw_right = draw_right;
        candidate->draw_bottom = draw_bottom;
        candidate->active = true;
        ++g_diagnostics.completed_field_submissions;
    }
    compositor_unlock();
}

static uint32_t completed_field_count(void) {
    uint32_t count = 0u;
    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_COMPLETED_CAPACITY; ++index)
        if (g_completed_field[index].active) ++count;
    return count;
}

static bool candidate_scene_matches(
        const XgRenderCompletedFieldSubmission *candidate,
        const XgRenderSourceFrameDescription *description) {
    return candidate->active && description != NULL &&
        scene_equal(&candidate->bank.description.scene, &description->scene) &&
        candidate->bank.description.scene_generation ==
            description->scene_generation;
}

static int displayed_field_for_display(
        const XgRenderSourceFrameDescription *description) {
    int selected = -1;
    uint64_t sequence = 0u;

    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_DISPLAYED_CAPACITY; ++index) {
        const XgRenderDisplayedFieldPage *page = &g_displayed_field[index];
        if (!page->active || page->sequence <= sequence ||
            !scene_equal(&page->bank.description.scene, &description->scene) ||
            page->bank.description.scene_generation !=
                description->scene_generation ||
            !display_equal(&page->bank.description.display,
                           &description->display))
            continue;
        selected = (int)index;
        sequence = page->sequence;
    }
    return selected;
}

static void displayed_field_store(XgRenderSemanticBank *bank) {
    uint32_t selected = XG_RENDER_FIELD_DISPLAYED_CAPACITY;
    uint64_t oldest_sequence = UINT64_MAX;

    if (bank == NULL || !bank->active ||
        bank->description.scene.module != XG_SEMANTIC_MODULE_FIELD)
        return;
    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_DISPLAYED_CAPACITY; ++index) {
        XgRenderDisplayedFieldPage *page = &g_displayed_field[index];
        if (page->active &&
            scene_equal(&page->bank.description.scene,
                        &bank->description.scene) &&
            page->bank.description.scene_generation ==
                bank->description.scene_generation &&
            display_equal(&page->bank.description.display,
                          &bank->description.display)) {
            selected = index;
            break;
        }
        if (!page->active) {
            selected = index;
            break;
        }
        if (page->sequence < oldest_sequence) {
            oldest_sequence = page->sequence;
            selected = index;
        }
    }
    if (selected >= XG_RENDER_FIELD_DISPLAYED_CAPACITY) return;
    bank_clear(&g_displayed_field[selected].bank);
    g_displayed_field[selected].bank = *bank;
    bank_forget(bank);
    g_displayed_field[selected].sequence =
        g_next_displayed_field_sequence++;
    if (g_next_displayed_field_sequence == 0u)
        g_next_displayed_field_sequence = 1u;
    g_displayed_field[selected].active = true;
}

static uint64_t field_candidate_display_overlap(
        const XgRenderCompletedFieldSubmission *candidate,
        const XgSemanticDisplayState *display) {
    const uint32_t display_right = (uint32_t)display->display_x + display->width;
    const uint32_t display_bottom = (uint32_t)display->display_y + display->height;
    const uint32_t draw_right = (uint32_t)candidate->draw_right + 1u;
    const uint32_t draw_bottom = (uint32_t)candidate->draw_bottom + 1u;
    const uint32_t left = candidate->draw_left > display->display_x
        ? candidate->draw_left : display->display_x;
    const uint32_t top = candidate->draw_top > display->display_y
        ? candidate->draw_top : display->display_y;
    const uint32_t right = draw_right < display_right
        ? draw_right : display_right;
    const uint32_t bottom = draw_bottom < display_bottom
        ? draw_bottom : display_bottom;

    if (right <= left || bottom <= top) return 0u;
    return (uint64_t)(right - left) * (bottom - top);
}

static uint64_t bank_display_overlap(
        const XgRenderSemanticBank *bank,
        const XgSemanticDisplayState *display) {
    const uint32_t display_right = (uint32_t)display->display_x + display->width;
    const uint32_t display_bottom = (uint32_t)display->display_y + display->height;
    uint64_t overlap = 0u;

    for (uint32_t index = 0u; index < bank->draw_count; ++index) {
        const XgRenderIrMaterialState *material =
            &bank->draws[index].primitive.material;
        const uint32_t draw_right = (uint32_t)material->draw_area_right + 1u;
        const uint32_t draw_bottom = (uint32_t)material->draw_area_bottom + 1u;
        const uint32_t left = material->draw_area_left > display->display_x
            ? material->draw_area_left : display->display_x;
        const uint32_t top = material->draw_area_top > display->display_y
            ? material->draw_area_top : display->display_y;
        const uint32_t right = draw_right < display_right
            ? draw_right : display_right;
        const uint32_t bottom = draw_bottom < display_bottom
            ? draw_bottom : display_bottom;
        if (right > left && bottom > top)
            overlap += (uint64_t)(right - left) * (bottom - top);
    }
    return overlap;
}

static int field_candidate_for_display(
        const XgRenderSourceFrameDescription *description) {
    int selected = -1;
    uint64_t selected_overlap = 0u;
    uint64_t selected_sequence = 0u;

    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_COMPLETED_CAPACITY; ++index) {
        const XgRenderCompletedFieldSubmission *candidate =
            &g_completed_field[index];
        uint64_t overlap;
        if (!candidate_scene_matches(candidate, description)) continue;
        overlap = field_candidate_display_overlap(
            candidate, &description->display);
        if (overlap == 0u || overlap < selected_overlap ||
            (overlap == selected_overlap &&
             candidate->sequence <= selected_sequence))
            continue;
        selected = (int)index;
        selected_overlap = overlap;
        selected_sequence = candidate->sequence;
    }
    return selected;
}

static bool resource_targets_display(
        const XgSemanticResourceRef *resource,
        const XgRenderSourceFrameDescription *description) {
    XgRenderResourceView view;
    const XgRenderResourceDescriptor *descriptor;

    if (xg_render_resource_view((XgRenderResourceHandle){
            resource->resource_id, resource->generation}, &view) !=
            XG_RENDER_RESOURCE_OK || !view.current ||
        view.content_digest != resource->content_digest ||
        view.kind != XG_RENDER_RESOURCE_GENERATED_SURFACE)
        return false;
    descriptor = &view.descriptor;
    return (descriptor->flags &
                XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u &&
        descriptor->vram_x == description->display.display_x &&
        descriptor->vram_y == description->display.display_y &&
        descriptor->vram_width == description->display.width &&
        descriptor->vram_height == description->display.height &&
        descriptor->width == description->display.width &&
        descriptor->height == description->display.height;
}

static bool display_target_in_bank(
        const XgRenderSemanticBank *bank,
        const XgRenderSourceFrameDescription *description,
        XgSemanticResourceRef *out_target) {
    if (!bank->active ||
        !scene_equal(&bank->description.scene, &description->scene) ||
        bank->description.scene_generation != description->scene_generation)
        return false;
    for (uint32_t index = 0u; index < bank->resource_count; ++index) {
        if (!resource_targets_display(&bank->resources[index], description))
            continue;
        *out_target = bank->resources[index];
        return true;
    }
    return false;
}

static bool bank_resources_available(const XgRenderSemanticBank *bank) {
    for (uint32_t index = 0u; index < bank->resource_count; ++index) {
        const XgSemanticResourceRef *resource = &bank->resources[index];
        XgRenderResourceView view;
        if (xg_render_resource_view((XgRenderResourceHandle){
                resource->resource_id, resource->generation}, &view) !=
                XG_RENDER_RESOURCE_OK ||
            (!view.current && view.retain_count == 0u) ||
            view.content_digest != resource->content_digest)
            return false;
    }
    return true;
}

static bool field_display_target(
        const XgRenderSemanticBank *selected,
        const XgRenderSourceFrameDescription *description,
        XgSemanticResourceRef *out_target) {
    if (g_field_boundary_target.resource_id != 0u &&
        scene_equal(&g_field_boundary_target_description.scene,
                    &description->scene) &&
        g_field_boundary_target_description.scene_generation ==
            description->scene_generation &&
        display_equal(&g_field_boundary_target_description.display,
                      &description->display) &&
        resource_targets_display(&g_field_boundary_target, description)) {
        *out_target = g_field_boundary_target;
        return true;
    }
    if (display_target_in_bank(selected, description, out_target)) return true;
    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_COMPLETED_CAPACITY; ++index)
        if (display_target_in_bank(
                &g_completed_field[index].bank, description, out_target))
            return true;
    return display_target_in_bank(&g_pending, description, out_target) ||
        display_target_in_bank(&g_retained, description, out_target);
}

bool xg_render_semantic_compositor_field_boundary_target_required(
        const XgRenderSourceFrameDescription *current_description) {
    XgSemanticResourceRef target;
    const XgRenderSemanticBank *selected = NULL;
    int candidate;
    bool required = false;

    compositor_lock();
    if (current_description == NULL ||
        current_description->scene.module != XG_SEMANTIC_MODULE_FIELD)
        goto finished;
    candidate = field_candidate_for_display(current_description);
    if (candidate >= 0) {
        selected = &g_completed_field[candidate].bank;
    } else if (displayed_field_for_display(current_description) >= 0) {
        goto finished;
    } else if (!g_pending.active && g_retained.active &&
               scene_equal(&g_retained.description.scene,
                           &current_description->scene) &&
               g_retained.description.scene_generation ==
                   current_description->scene_generation &&
               !display_equal(&g_retained.description.display,
                              &current_description->display) &&
               bank_resources_available(&g_retained) &&
               bank_display_overlap(
                   &g_retained, &current_description->display) != 0u) {
        selected = &g_retained;
    }
    required = selected != NULL &&
        !field_display_target(selected, current_description, &target);
finished:
    compositor_unlock();
    return required;
}

bool xg_render_semantic_compositor_set_field_boundary_target(
        const XgRenderSourceFrameDescription *current_description,
        const XgSemanticResourceRef *target) {
    bool accepted = false;

    compositor_lock();
    if (current_description != NULL && target != NULL &&
        current_description->scene.module == XG_SEMANTIC_MODULE_FIELD &&
        resource_targets_display(target, current_description)) {
        g_field_boundary_target_description = *current_description;
        g_field_boundary_target = *target;
        accepted = true;
    }
    compositor_unlock();
    return accepted;
}

static bool bank_retarget_display(
        XgRenderSemanticBank *bank,
        const XgRenderSourceFrameDescription *description,
        const XgSemanticResourceRef *target) {
    uint64_t old_id = 0u;
    uint64_t old_generation = 0u;
    uint32_t old_resource = UINT32_MAX;
    uint32_t presentation_passes = 0u;

    for (uint32_t index = 0u; index < bank->pass_count; ++index) {
        XgSemanticPassRecord *pass = &bank->passes[index];
        if (!pass->presentation_output) continue;
        if (presentation_passes++ == 0u) {
            old_id = pass->target_surface_id;
            old_generation = pass->target_generation;
        } else if (pass->target_surface_id != old_id ||
                   pass->target_generation != old_generation) {
            return false;
        }
    }
    if (presentation_passes != 1u) return false;
    for (uint32_t index = 0u; index < bank->resource_count; ++index)
        if (bank->resources[index].resource_id == old_id &&
            bank->resources[index].generation == old_generation) {
            old_resource = index;
            break;
        }
    if (old_resource == UINT32_MAX) return false;
    for (uint32_t index = 0u; index < bank->draw_count; ++index)
        if ((bank->draws[index].texture_resource_id == old_id &&
             bank->draws[index].texture_generation == old_generation) ||
            (bank->draws[index].clut_resource_id == old_id &&
             bank->draws[index].clut_generation == old_generation))
            return false;
    for (uint32_t index = 0u; index < bank->edge_count; ++index)
        if (bank->edges[index].source_surface_id == old_id &&
            bank->edges[index].source_generation == old_generation)
            return false;

    if (old_id != target->resource_id || old_generation != target->generation) {
        if (xg_render_resource_acquire_current((XgRenderResourceHandle){
                target->resource_id, target->generation},
                target->content_digest) != XG_RENDER_RESOURCE_OK)
            return false;
        (void)xg_render_resource_release((XgRenderResourceHandle){
            old_id, old_generation});
        bank->resources[old_resource] = *target;
    }
    for (uint32_t index = 0u; index < bank->pass_count; ++index) {
        XgSemanticPassRecord *pass = &bank->passes[index];
        if (pass->target_surface_id != old_id ||
            pass->target_generation != old_generation)
            continue;
        pass->target_surface_id = target->resource_id;
        pass->target_generation = target->generation;
        if (pass->presentation_output) {
            pass->viewport_x = 0u;
            pass->viewport_y = 0u;
            pass->viewport_width = description->display.width;
            pass->viewport_height = description->display.height;
        }
    }
    for (uint32_t index = 0u; index < bank->edge_count; ++index) {
        XgSemanticSurfaceEdge *edge = &bank->edges[index];
        if (edge->target_surface_id == old_id &&
            edge->target_generation == old_generation) {
            edge->target_surface_id = target->resource_id;
            edge->target_generation = target->generation;
        }
    }
    bank->description = *description;
    return true;
}

static int first_owner(uint64_t owners) {
    for (uint32_t index = 0u; index < XG_RENDER_SEMANTIC_OWNER_CAPACITY; ++index)
        if ((owners & (UINT64_C(1) << index)) != 0u) return (int)index;
    return -1;
}

static int owner_pass_by_id(const XgRenderSemanticBank *bank,
                            uint32_t owner_index, uint32_t pass_id) {
    const uint64_t owner_bit = UINT64_C(1) << owner_index;
    uint32_t local_index = 0u;
    int ordinal_match = -1;
    for (uint32_t index = 0u; index < bank->pass_count; ++index) {
        if ((bank->pass_owners[index] & owner_bit) == 0u) continue;
        if (bank->passes[index].pass_id == pass_id) return (int)index;
        if (local_index == pass_id) ordinal_match = (int)index;
        ++local_index;
    }
    return ordinal_match;
}

static int owner_pass_by_ordinal(const XgRenderSemanticBank *bank,
                                 uint32_t owner_index, uint32_t ordinal) {
    const uint64_t owner_bit = UINT64_C(1) << owner_index;
    uint32_t local_index = 0u;
    for (uint32_t index = 0u; index < bank->pass_count; ++index) {
        if ((bank->pass_owners[index] & owner_bit) == 0u) continue;
        if (local_index++ == ordinal) return (int)index;
    }
    return -1;
}

static int pass_by_target(const XgRenderSemanticBank *bank,
                          uint64_t target_surface_id,
                          uint64_t target_generation) {
    int match = -1;

    for (uint32_t index = 0u; index < bank->pass_count; ++index) {
        if (bank->passes[index].target_surface_id != target_surface_id ||
            bank->passes[index].target_generation != target_generation)
            continue;
        if (match >= 0) return -1;
        match = (int)index;
    }
    return match;
}

static XgRenderSemanticCompositorResult materialize_bank(
        XgRenderSemanticBank *bank) {
    XgRenderSourceFrameFragment fragment;

    if (!bank->active || bank->blocked)
        return XG_RENDER_SEMANTIC_COMPOSITOR_BLOCKED;
    /* Every emitted slot is assigned below before append_fragment copies the
     * live prefixes. Clearing the full scratch capacity is unnecessary. */
    g_diagnostics.mutation_materialized_draws = 0u;
    for (uint32_t index = 0u; index < bank->resource_count; ++index) {
        const XgSemanticResourceRef *resource = &bank->resources[index];
        XgRenderResourceView view;
        if (xg_render_resource_view((XgRenderResourceHandle){
                resource->resource_id, resource->generation}, &view) !=
                XG_RENDER_RESOURCE_OK ||
            (!view.current && view.retain_count == 0u) ||
            view.content_digest != resource->content_digest)
            return XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED;
        g_materialized.resources[index] = *resource;
    }
    for (uint32_t index = 0u; index < bank->pass_count; ++index) {
        XgSemanticPassRecord pass = bank->passes[index];
        const int owner_index = first_owner(bank->pass_owners[index]);
        uint32_t dependencies = 0u;
        if (owner_index < 0) return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
        for (uint32_t bit = 0u; bit < XG_RENDER_SCENE_PASS_CAPACITY; ++bit) {
            int dependency;
            if ((pass.dependency_mask & (UINT32_C(1) << bit)) == 0u) continue;
            dependency = owner_pass_by_ordinal(bank, (uint32_t)owner_index, bit);
            if (dependency < 0 || (uint32_t)dependency >= index)
                return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
            dependencies |= UINT32_C(1) << (uint32_t)dependency;
        }
        pass.pass_id = index;
        pass.dependency_mask = dependencies;
        g_materialized.passes[index] = pass;
    }
    for (uint32_t index = 0u; index < bank->draw_count; ++index) {
        XgSemanticDrawRecord draw = bank->draws[index];
        const int pass = owner_pass_by_id(
            bank, bank->draw_owners[index], draw.order.pass_id);
        if (pass < 0) return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
        draw.order.pass_id = (uint32_t)pass;
        g_materialized.draws[index] = draw;
        if ((draw.texture_resource_id ==
                 g_diagnostics.mutation_replacement_resource_id &&
             draw.texture_generation ==
                 g_diagnostics.mutation_replacement_generation) ||
            (draw.clut_resource_id ==
                 g_diagnostics.mutation_replacement_resource_id &&
             draw.clut_generation ==
                 g_diagnostics.mutation_replacement_generation))
            ++g_diagnostics.mutation_materialized_draws;
    }
    for (uint32_t index = 0u; index < bank->edge_count; ++index) {
        XgSemanticSurfaceEdge edge = bank->edges[index];
        const int owner = first_owner(bank->edge_owners[index]);
        int pass = owner < 0 ? -1 : owner_pass_by_id(
            bank, (uint32_t)owner, edge.order.pass_id);
        if (pass < 0)
            pass = pass_by_target(bank, edge.target_surface_id,
                                  edge.target_generation);
        if (pass < 0)
            return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
        edge.order.pass_id = (uint32_t)pass;
        g_materialized.edges[index] = edge;
    }
    for (uint32_t index = 0u; index < bank->ui_node_count; ++index) {
        XgSemanticUiNodeRecord node = bank->ui_nodes[index];
        const int pass = owner_pass_by_id(
            bank, bank->ui_node_owners[index], node.order.pass_id);
        if (pass < 0) return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
        node.order.pass_id = (uint32_t)pass;
        g_materialized.ui_nodes[index] = node;
    }
    for (uint32_t index = 0u; index < bank->ui_run_count; ++index) {
        XgSemanticUiGlyphRunRecord run = bank->ui_runs[index];
        const uint32_t owner_index = bank->ui_run_owners[index];
        const int pass = owner_pass_by_id(bank, owner_index, run.order.pass_id);
        uint32_t placement_count = 0u;
        if (pass < 0) return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
        run.order.pass_id = (uint32_t)pass;
        run.placement_offset = 0u;
        for (uint32_t prior = 0u; prior < index; ++prior)
            for (uint32_t placement = 0u;
                 placement < bank->ui_placement_count; ++placement)
                if (bank->ui_placement_owners[placement] ==
                        bank->ui_run_owners[prior] &&
                    bank->ui_placements[placement].glyph_run_id ==
                        bank->ui_runs[prior].glyph_run_id)
                    ++run.placement_offset;
        for (uint32_t placement = 0u;
             placement < bank->ui_placement_count; ++placement)
            if (bank->ui_placement_owners[placement] == owner_index &&
                bank->ui_placements[placement].glyph_run_id == run.glyph_run_id)
                ++placement_count;
        if (placement_count != run.placement_count)
            return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
        g_materialized.ui_runs[index] = run;
    }
    {
        uint32_t destination = 0u;
        for (uint32_t run = 0u; run < bank->ui_run_count; ++run)
            for (uint32_t placement = 0u;
                 placement < bank->ui_placement_count; ++placement)
                if (bank->ui_placement_owners[placement] ==
                        bank->ui_run_owners[run] &&
                    bank->ui_placements[placement].glyph_run_id ==
                        bank->ui_runs[run].glyph_run_id) {
                    if (destination >= bank->ui_placement_count)
                        return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
                    g_materialized.ui_placements[destination++] =
                        bank->ui_placements[placement];
                }
        if (destination != bank->ui_placement_count)
            return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
    }
    fragment = (XgRenderSourceFrameFragment){
        .passes = g_materialized.passes,
        .draws = g_materialized.draws,
        .resources = g_materialized.resources,
        .surface_edges = g_materialized.edges,
        .ui_nodes = g_materialized.ui_nodes,
        .ui_glyph_runs = g_materialized.ui_runs,
        .ui_glyph_placements = g_materialized.ui_placements,
        .pass_count = bank->pass_count,
        .draw_count = bank->draw_count,
        .resource_count = bank->resource_count,
        .surface_edge_count = bank->edge_count,
        .ui_node_count = bank->ui_node_count,
        .ui_glyph_run_count = bank->ui_run_count,
        .ui_glyph_placement_count = bank->ui_placement_count,
    };
    if (xg_render_source_frame_append_fragment(&bank->description, &fragment) !=
            XG_RENDER_SOURCE_FRAME_OK ||
        xg_render_source_frame_complete() != XG_RENDER_SOURCE_FRAME_OK)
        return XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
    return XG_RENDER_SEMANTIC_COMPOSITOR_OK;
}

XgRenderSourceFrameResult xg_render_semantic_compositor_prepare_boundary(
        const XgRenderSourceFrameDescription *current_description,
        XgRenderSemanticBoundaryKind *out_kind) {
    XgRenderSemanticCompositorResult result;
    XgRenderSemanticBank *selected_bank = NULL;
    XgRenderSemanticBoundaryKind kind = XG_RENDER_SEMANTIC_BOUNDARY_HOLD;
    XgRenderSourceFrameResult frame_result = XG_RENDER_SOURCE_FRAME_EMPTY;
    XgSemanticResourceRef field_target = {0};
    int field_candidate = -1;
    int displayed_field = -1;
    bool field_retarget_required = false;

    compositor_lock();
    if (g_prepared) {
        kind = g_prepared_kind;
        frame_result = kind == XG_RENDER_SEMANTIC_BOUNDARY_HOLD
            ? XG_RENDER_SOURCE_FRAME_EMPTY :
            kind == XG_RENDER_SEMANTIC_BOUNDARY_REJECTED
                ? XG_RENDER_SOURCE_FRAME_REJECTED
                : XG_RENDER_SOURCE_FRAME_OK;
        goto finished;
    }
    ++g_lifecycle_generation;
    g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_NONE;
    g_prepared_completed_field_index = 0u;
    g_prepared_displayed_field_index = 0u;

    if ((current_description != NULL &&
         current_description->scene.module == XG_SEMANTIC_MODULE_FIELD) ||
        (g_pending.active && g_pending.description.scene.module ==
             XG_SEMANTIC_MODULE_FIELD)) {
        const bool current_field = current_description != NULL &&
            current_description->scene.module == XG_SEMANTIC_MODULE_FIELD;
        const bool pending_scene_matches = current_field && g_pending.active &&
            scene_equal(&g_pending.description.scene,
                        &current_description->scene) &&
            g_pending.description.scene_generation ==
                current_description->scene_generation;

        if (!current_field) goto held;
        if (pending_scene_matches && !g_pending.field_submission_required &&
            g_pending.touched_owners != 0u) {
            selected_bank = &g_pending;
            g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_PENDING;
            kind = XG_RENDER_SEMANTIC_BOUNDARY_ENDPOINT;
        } else {
            field_candidate = field_candidate_for_display(current_description);
            if (field_candidate >= 0) {
                selected_bank = &g_completed_field[field_candidate].bank;
                field_retarget_required = true;
                g_prepared_bank_kind =
                    XG_RENDER_PREPARED_BANK_COMPLETED_FIELD;
                g_prepared_completed_field_index =
                    (uint32_t)field_candidate;
                kind = XG_RENDER_SEMANTIC_BOUNDARY_ENDPOINT;
            } else if ((displayed_field = displayed_field_for_display(
                            current_description)) >= 0) {
                selected_bank = &g_displayed_field[displayed_field].bank;
                g_prepared_bank_kind =
                    XG_RENDER_PREPARED_BANK_DISPLAYED_FIELD;
                g_prepared_displayed_field_index =
                    (uint32_t)displayed_field;
                kind = XG_RENDER_SEMANTIC_BOUNDARY_ENDPOINT;
            } else if ((pending_scene_matches &&
                        g_pending.field_submission_required) ||
                       newest_completed_field_bank(current_description) != NULL) {
                goto held;
            } else if (g_retained.active &&
                       scene_equal(&g_retained.description.scene,
                                   &current_description->scene) &&
                       g_retained.description.scene_generation ==
                           current_description->scene_generation &&
                       !display_equal(&g_retained.description.display,
                                      &current_description->display)) {
                if (!bank_resources_available(&g_retained) ||
                    bank_display_overlap(
                        &g_retained, &current_description->display) == 0u)
                    goto held;
                result = bank_clone(&g_display_root, &g_retained);
                if (result != XG_RENDER_SEMANTIC_COMPOSITOR_OK) goto rejected;
                selected_bank = &g_display_root;
                field_retarget_required = true;
                g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_DISPLAY_ROOT;
                kind = XG_RENDER_SEMANTIC_BOUNDARY_DISPLAY_ROOT;
            } else {
                goto held;
            }
        }
        if (field_retarget_required && !field_display_target(
                selected_bank, current_description, &field_target)) {
            if (g_prepared_bank_kind == XG_RENDER_PREPARED_BANK_DISPLAY_ROOT)
                bank_clear(&g_display_root);
            g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_NONE;
            goto held;
        }
        if (field_retarget_required && !bank_retarget_display(
                selected_bank, current_description, &field_target)) {
            result = XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED;
            goto rejected;
        }
    } else if (g_pending.active) {
        selected_bank = &g_pending;
        g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_PENDING;
        kind = XG_RENDER_SEMANTIC_BOUNDARY_ENDPOINT;
    } else if (g_retained.active && current_description != NULL &&
               scene_equal(&g_retained.description.scene,
                           &current_description->scene) &&
               g_retained.description.scene_generation ==
                   current_description->scene_generation &&
               !display_equal(&g_retained.description.display,
                              &current_description->display)) {
        result = bank_clone(&g_display_root, &g_retained);
        if (result != XG_RENDER_SEMANTIC_COMPOSITOR_OK) goto rejected;
        g_display_root.description = *current_description;
        g_display_root.active = true;
        selected_bank = &g_display_root;
        g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_DISPLAY_ROOT;
        kind = XG_RENDER_SEMANTIC_BOUNDARY_DISPLAY_ROOT;
    } else {
        goto held;
    }

    if (selected_bank->blocked) {
        result = XG_RENDER_SEMANTIC_COMPOSITOR_BLOCKED;
        goto rejected;
    }
    result = materialize_bank(selected_bank);
    if (result != XG_RENDER_SEMANTIC_COMPOSITOR_OK) goto rejected;
    if (kind == XG_RENDER_SEMANTIC_BOUNDARY_DISPLAY_ROOT)
        ++g_diagnostics.display_root_boundaries;
    else
        ++g_diagnostics.endpoint_boundaries;
    if (g_prepared_bank_kind == XG_RENDER_PREPARED_BANK_COMPLETED_FIELD)
        ++g_diagnostics.display_matched_field_submissions;
    g_diagnostics.last_boundary = kind;
    g_diagnostics.last_result = XG_RENDER_SEMANTIC_COMPOSITOR_OK;
    g_prepared_kind = kind;
    g_prepared = true;
    frame_result = XG_RENDER_SOURCE_FRAME_OK;
    goto finished;

held:
    kind = XG_RENDER_SEMANTIC_BOUNDARY_HOLD;
    ++g_diagnostics.hold_boundaries;
    g_diagnostics.last_boundary = kind;
    g_diagnostics.last_result = XG_RENDER_SEMANTIC_COMPOSITOR_OK;
    g_prepared_kind = kind;
    g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_NONE;
    g_prepared = true;
    goto finished;

rejected:
    kind = XG_RENDER_SEMANTIC_BOUNDARY_REJECTED;
    ++g_diagnostics.rejected_boundaries;
    g_diagnostics.last_boundary = kind;
    g_diagnostics.last_result = result;
    g_prepared_kind = kind;
    g_prepared = true;
    frame_result = XG_RENDER_SOURCE_FRAME_REJECTED;
finished:
    g_diagnostics.pending = g_pending.active;
    g_diagnostics.completed_field_candidate_count = completed_field_count();
    g_diagnostics.prepared = g_prepared;
    if (out_kind != NULL) *out_kind = kind;
    compositor_unlock();
    return frame_result;
}

void xg_render_semantic_compositor_finish_boundary(bool published) {
    XgRenderSemanticBank *prepared_bank = NULL;

    compositor_lock();
    if (!g_prepared) {
        compositor_unlock();
        return;
    }
    ++g_lifecycle_generation;
    if (g_prepared_bank_kind == XG_RENDER_PREPARED_BANK_PENDING) {
        prepared_bank = &g_pending;
    } else if (g_prepared_bank_kind ==
            XG_RENDER_PREPARED_BANK_COMPLETED_FIELD &&
        g_prepared_completed_field_index <
            XG_RENDER_FIELD_COMPLETED_CAPACITY) {
        prepared_bank =
            &g_completed_field[g_prepared_completed_field_index].bank;
    } else if (g_prepared_bank_kind ==
                   XG_RENDER_PREPARED_BANK_DISPLAYED_FIELD &&
               g_prepared_displayed_field_index <
                   XG_RENDER_FIELD_DISPLAYED_CAPACITY) {
        prepared_bank =
            &g_displayed_field[g_prepared_displayed_field_index].bank;
    } else if (g_prepared_bank_kind ==
               XG_RENDER_PREPARED_BANK_DISPLAY_ROOT) {
        prepared_bank = &g_display_root;
    }
    if (prepared_bank != NULL &&
        (g_prepared_kind == XG_RENDER_SEMANTIC_BOUNDARY_ENDPOINT ||
         g_prepared_kind == XG_RENDER_SEMANTIC_BOUNDARY_DISPLAY_ROOT)) {
        if (published) {
            if (g_prepared_bank_kind ==
                    XG_RENDER_PREPARED_BANK_DISPLAYED_FIELD) {
                bank_clear(&g_display_root);
                g_display_root = *prepared_bank;
                bank_forget(prepared_bank);
                g_displayed_field[g_prepared_displayed_field_index].active =
                    false;
                displayed_field_store(&g_retained);
                g_retained = g_display_root;
                bank_forget(&g_display_root);
            } else {
                if (g_retained.active &&
                    g_retained.description.scene.module ==
                        XG_SEMANTIC_MODULE_FIELD &&
                    prepared_bank->description.scene.module ==
                        XG_SEMANTIC_MODULE_FIELD)
                    displayed_field_store(&g_retained);
                else
                    bank_clear(&g_retained);
                g_retained = *prepared_bank;
                bank_forget(prepared_bank);
            }
            if (g_prepared_bank_kind ==
                    XG_RENDER_PREPARED_BANK_COMPLETED_FIELD)
                g_completed_field[g_prepared_completed_field_index].active = false;
            ++g_diagnostics.committed_boundaries;
        } else {
            bank_clear(prepared_bank);
            if (g_prepared_bank_kind ==
                    XG_RENDER_PREPARED_BANK_COMPLETED_FIELD)
                g_completed_field[g_prepared_completed_field_index].active = false;
            else if (g_prepared_bank_kind ==
                         XG_RENDER_PREPARED_BANK_DISPLAYED_FIELD)
                g_displayed_field[g_prepared_displayed_field_index].active =
                    false;
            ++g_diagnostics.discarded_boundaries;
        }
    } else if (g_prepared_kind == XG_RENDER_SEMANTIC_BOUNDARY_REJECTED &&
               prepared_bank != NULL) {
        bank_clear(prepared_bank);
        if (g_prepared_bank_kind ==
                XG_RENDER_PREPARED_BANK_COMPLETED_FIELD)
            g_completed_field[g_prepared_completed_field_index].active = false;
        else if (g_prepared_bank_kind ==
                     XG_RENDER_PREPARED_BANK_DISPLAYED_FIELD)
            g_displayed_field[g_prepared_displayed_field_index].active = false;
        ++g_diagnostics.discarded_boundaries;
    }
    g_prepared = false;
    g_prepared_kind = XG_RENDER_SEMANTIC_BOUNDARY_HOLD;
    g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_NONE;
    memset(&g_field_boundary_target_description, 0,
           sizeof(g_field_boundary_target_description));
    memset(&g_field_boundary_target, 0, sizeof(g_field_boundary_target));
    g_diagnostics.pending = g_pending.active;
    g_diagnostics.completed_field_candidate_count = completed_field_count();
    g_diagnostics.prepared = false;
    compositor_unlock();
}

void xg_render_semantic_compositor_invalidate_building(void) {
    compositor_lock();
    ++g_lifecycle_generation;
    if (!g_prepared) {
        bank_clear(&g_pending);
        bank_clear(&g_display_root);
        memset(&g_field_boundary_target_description, 0,
               sizeof(g_field_boundary_target_description));
        memset(&g_field_boundary_target, 0,
               sizeof(g_field_boundary_target));
    }
    ++g_diagnostics.building_invalidations;
    g_diagnostics.pending = g_pending.active;
    compositor_unlock();
}

void xg_render_semantic_compositor_reset(void) {
    compositor_lock();
    ++g_lifecycle_generation;
    ++g_diagnostics.semantic_resets;
    bank_clear(&g_pending);
    bank_clear(&g_retained);
    bank_clear(&g_display_root);
    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_COMPLETED_CAPACITY; ++index) {
        bank_clear(&g_completed_field[index].bank);
        g_completed_field[index].active = false;
    }
    for (uint32_t index = 0u;
         index < XG_RENDER_FIELD_DISPLAYED_CAPACITY; ++index) {
        bank_clear(&g_displayed_field[index].bank);
        g_displayed_field[index].active = false;
    }
    g_prepared = false;
    g_prepared_kind = XG_RENDER_SEMANTIC_BOUNDARY_HOLD;
    g_prepared_bank_kind = XG_RENDER_PREPARED_BANK_NONE;
    memset(&g_field_boundary_target_description, 0,
           sizeof(g_field_boundary_target_description));
    memset(&g_field_boundary_target, 0, sizeof(g_field_boundary_target));
    g_next_completed_field_sequence = 1u;
    g_next_displayed_field_sequence = 1u;
    g_diagnostics.pending = false;
    g_diagnostics.completed_field_candidate_count = 0u;
    g_diagnostics.prepared = false;
    g_diagnostics.retained = false;
    compositor_unlock();
}

void xg_render_semantic_compositor_diagnostics(
        XgRenderSemanticCompositorDiagnostics *out_diagnostics) {
    if (out_diagnostics == NULL) return;
    compositor_lock();
    g_diagnostics.retained_owner_count = owner_count(&g_retained);
    g_diagnostics.retained_pass_count = g_retained.pass_count;
    g_diagnostics.retained_draw_count = g_retained.draw_count;
    g_diagnostics.retained_resource_count = g_retained.resource_count;
    g_diagnostics.retained_edge_count = g_retained.edge_count;
    g_diagnostics.retained_ui_node_count = g_retained.ui_node_count;
    g_diagnostics.pending = g_pending.active;
    g_diagnostics.completed_field_candidate_count = completed_field_count();
    g_diagnostics.prepared = g_prepared;
    g_diagnostics.retained = g_retained.active;
    *out_diagnostics = g_diagnostics;
    compositor_unlock();
}
