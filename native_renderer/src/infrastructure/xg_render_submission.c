#include "xg_render_submission.h"

#include "guest_render_bridge.h"
#include "guest_render_native_stream.h"
#include "xg_field_character_adapter.h"
#include "xg_host_3d.h"
#include "xg_render_backend.h"
#include "xg_render_vram_resources.h"
#include "xg_render_instrumentation.h"
#include "xg_render_fragment_runtime.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_source_frame.h"
#include "xg_render_native_work.h"
#include "psx_cycles.h"
#include "xg_render_temporal_submission.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static XgRenderPreSceneState pre_scene;
static XgRenderStandaloneSubmissionState standalone_submission;
static uint32_t standalone_stage_failure_detail;
static XgRenderSubmissionServices submission_services;
static bool submission_services_configured;
static bool native_work_mode;
static XgRenderSubmissionObserver submission_observer;
static void *submission_observer_user_data;
static uint32_t source_frame_insertion_ordinal;

typedef struct XgRenderSourceCapture {
    XgRenderSubmissionCommand command;
    uint64_t scene_generation;
    uint64_t capture_sequence;
    XgRenderTemporalBinding temporal;
    uint32_t temporal_scope;
    bool consumed;
} XgRenderSourceCapture;

/* Retain both packet arenas, independently of native-stream visual retirement.
 * Only consumed captures may be evicted to admit a new packet address. */
static XgRenderSourceCapture source_captures[2u * XG_RENDER_IR_ITEM_CAPACITY];
static uint32_t source_capture_count;
static uint32_t source_capture_by_command[UINT32_C(0x80000)];
static uint32_t source_capture_reuse_cursor;
static uint64_t capture_sequence;
static uint64_t pre_scene_capture_sequences[XG_RENDER_IR_ITEM_CAPACITY];
static XgRenderSubmissionDiagnostics submission_diagnostics;

static void release_temporal_capture(XgRenderSourceCapture *capture) {
    const XgSemanticResourceRef ref = capture->temporal.coverage;
    if (ref.resource_id)
        (void)xg_render_resource_release((XgRenderResourceHandle){ref.resource_id, ref.generation});
    capture->temporal = (XgRenderTemporalBinding){0};
    capture->temporal_scope = 0;
}

static void clear_source_captures(void) {
    for (uint32_t i = 0; i < source_capture_count; ++i)
        release_temporal_capture(&source_captures[i]);
    source_capture_count = 0;
}

void xg_render_submission_diagnostics(XgRenderSubmissionDiagnostics *out) {
    if (out != NULL) *out = submission_diagnostics;
}

typedef struct XgRenderPreparedSourceOt {
    XgRenderSourceFrameDescription description;
    XgSemanticPassRecord pass;
    XgSemanticDrawRecord draws[XG_RENDER_SCENE_DRAW_CAPACITY];
    XgSemanticResourceRef resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    uint32_t command_ids[XG_RENDER_SCENE_DRAW_CAPACITY];
    uint32_t draw_count;
    uint32_t resource_count;
    uint32_t start_address;
    uint32_t transferred_words;
    uint32_t insertion_end;
    bool blocked;
} XgRenderPreparedSourceOt;

static XgRenderPreparedSourceOt *prepared_source_ot;
static XgRenderSourceFrameDescription world_ot_description;
static XgSemanticResourceRef world_ot_target;
static uint32_t ot_failure_detail;
static uint32_t ot_failure_index;
static uint32_t ot_failure_tpage;
static uint32_t ot_failure_clut;

static bool capture_source_command(const GpuRenderSemantic *semantic,
                                   uint32_t command_id,
                                   uint32_t source_primitive_index,
                                   XgRenderIrProvenanceKey provenance) {
    uint32_t index;
    uint64_t generation;

    if (semantic == NULL || !submission_services_configured ||
        submission_services.scene_generation == NULL ||
        capture_sequence == UINT64_MAX ||
        (generation = submission_services.scene_generation()) == 0u)
        return false;
    command_id &= UINT32_C(0x001ffffc);
    index = source_capture_by_command[command_id >> 2u];
    if (index >= source_capture_count ||
        source_captures[index].command.command_id != command_id)
        index = source_capture_count;
    if (index == source_capture_count) {
        if (source_capture_count < 2u * XG_RENDER_IR_ITEM_CAPACITY) {
            ++source_capture_count;
        } else {
            for (uint32_t probe = 0u; probe < source_capture_count; ++probe) {
                const uint32_t candidate =
                    source_capture_reuse_cursor++ % source_capture_count;
                if (source_captures[candidate].consumed ||
                    source_captures[candidate].scene_generation != generation) {
                    index = candidate;
                    break;
                }
            }
            if (index == source_capture_count) return false;
        }
    }
    release_temporal_capture(&source_captures[index]);
    source_captures[index] = (XgRenderSourceCapture){
        .command =
            {
                .semantic = *semantic,
                .provenance = provenance,
                .command_id = command_id,
                .source_primitive_index = source_primitive_index,
                .has_provenance = provenance.state_id.scene_epoch != 0u,
                .producer_captured = true,
            },
        .scene_generation = generation,
        .capture_sequence = ++capture_sequence,
    };
    source_capture_by_command[command_id >> 2u] = index;
    return true;
}

static void discard_source_visual(GpuRenderTransactionId visual) {
    for (uint32_t index = 0u; index < source_capture_count;) {
        const XgRenderIrProvenanceKey *provenance =
            &source_captures[index].command.provenance;
        if (provenance->state_id.scene_epoch == visual.scene_epoch &&
            provenance->state_id.state_sequence == visual.state_sequence) {
            release_temporal_capture(&source_captures[index]);
            source_captures[index] = source_captures[--source_capture_count];
            source_captures[source_capture_count].temporal = (XgRenderTemporalBinding){0};
            source_capture_by_command
                [source_captures[index].command.command_id >> 2u] = index;
        } else {
            ++index;
        }
    }
}

typedef struct XgRenderStandaloneSourceFragment {
    XgRenderSourceFrameDescription description;
    XgSemanticDrawRecord draws[XG_RENDER_SCENE_DRAW_CAPACITY];
    XgSemanticResourceRef resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    uint32_t draw_count;
    uint32_t resource_count;
    bool active;
} XgRenderStandaloneSourceFragment;

static XgRenderStandaloneSourceFragment standalone_source;

/* Each standalone scene gets a fresh scene_epoch, so the stream's own
 * same-epoch supersede logic never retires the previous one. Track it
 * ourselves and abandon it once its successor is active. */
static GpuRenderTransactionId last_activated_standalone_visual;

static void retire_previous_standalone_visual(GpuRenderTransactionId visual) {
    if (last_activated_standalone_visual.scene_epoch != 0u &&
        !(last_activated_standalone_visual.scene_epoch == visual.scene_epoch &&
          last_activated_standalone_visual.state_sequence ==
              visual.state_sequence))
        guest_render_native_stream_abandon_visual(
            last_activated_standalone_visual);
    last_activated_standalone_visual = visual;
}

static uint64_t interpolation_generation(void) {
    return submission_services_configured &&
            submission_services.interpolation_generation != NULL
        ? submission_services.interpolation_generation() : 0u;
}

uint64_t xg_render_submission_temporal_scene(void) {
    return interpolation_generation();
}

static bool authenticated_ir_failure(uint32_t reason, uint64_t index,
                                     uint32_t packet_address,
                                     uint32_t status) {
    xg_render_instrumentation_record_flush_failure(
        reason, index, packet_address, status);
    return false;
}

bool xg_render_submission_validate_authenticated_ir(
        const XgRenderAuthenticatedIrAccess *access) {
    XgRenderAuthenticatedIrDescription description = {0};

    xg_render_instrumentation_record_flush_attempt();
    if (access == NULL || access->describe == NULL || access->item_get == NULL)
        return authenticated_ir_failure(1u, 0u, 0u, 0u);
    if (!access->describe(&description))
        return authenticated_ir_failure(2u, 0u, 0u, 0u);
    if (description.render_mode != GUEST_RENDER_RENDER_NATIVE)
        return authenticated_ir_failure(
            3u, 0u, 0u, (uint32_t)description.render_mode);
    for (size_t index = 0u; index < description.item_count; ++index) {
        XgRenderIrNativeItem item = {0};
        GpuRenderSemantic semantic;
        uint32_t packet_address;

        if (!access->item_get(index, &item))
            return authenticated_ir_failure(4u, index, 0u, 0u);
        packet_address = item.base.ordering.packet_guest_address &
            UINT32_C(0x001ffffc);
        if (xg_render_backend_translate_primitive(&item.native, &semantic) !=
                XG_RENDER_BACKEND_OK)
            return authenticated_ir_failure(
                5u, index, packet_address, 0u);
    }
    return true;
}

static void clear_standalone(void) {
    standalone_submission = (XgRenderStandaloneSubmissionState){0};
    standalone_source.description = (XgRenderSourceFrameDescription){0};
    standalone_source.draw_count = 0u;
    standalone_source.resource_count = 0u;
    standalone_source.active = false;
}

void xg_render_submission_configure(
        const XgRenderSubmissionServices *services) {
    submission_services = services != NULL
        ? *services : (XgRenderSubmissionServices){0};
    submission_services_configured = services != NULL;
}

void xg_render_submission_set_native_work_mode(bool enabled) {
    if (native_work_mode == enabled) return;
    xg_render_submission_cancel_ordering_table();
    xg_render_temporal_submission_reset();
    for (uint32_t i = 0; i < source_capture_count; ++i)
        release_temporal_capture(&source_captures[i]);
    native_work_mode = enabled;
}

bool xg_render_submission_native_work_mode(void) { return native_work_mode; }

void xg_render_submission_set_observer(
        XgRenderSubmissionObserver observer, void *user_data) {
    submission_observer = observer;
    submission_observer_user_data = user_data;
}

static void notify_primitive_staged(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t source_primitive_index) {
    if (submission_observer != NULL)
        submission_observer(
            primitive, source_primitive_index, submission_observer_user_data);
}

static bool stage_source_draw(
        const XgRenderIrNativePrimitive *primitive,
        const GpuRenderSemantic *semantic,
        uint32_t packet_address, uint32_t source_primitive_index,
        uint32_t ot_bucket,
        XgRenderIrProvenanceKey provenance) {
    XgSemanticDrawRecord draw = {0};
    XgRenderVramResourceResolvedResources resources = {0};
    XgSemanticResourceRef resource_refs[2];
    XgRenderSourceFrameDescription description = {0};
    XgRenderSourceFrameFragment fragment = {0};
    uint32_t resource_count = 0u;

    if (native_work_mode)
        return capture_source_command(semantic, packet_address + 4u,
                                      source_primitive_index, provenance);
    if (primitive == NULL || semantic == NULL ||
        ot_bucket > (uint32_t)INT32_MAX ||
        !submission_services_configured ||
        submission_services.source_frame_description == NULL)
        goto reject;
    if (!submission_services.source_frame_description(&description))
        goto reject;
    if (description.scene.module == XG_SEMANTIC_MODULE_FIELD ||
        description.scene.module == XG_SEMANTIC_MODULE_WORLD) {
        if (!capture_source_command(
                semantic, packet_address + 4u, source_primitive_index,
                provenance))
            goto reject;
        return true;
    }
    if (primitive->material.textured) {
        /* resolve_draw may succeed with only the texture OR the palette. */
        if (!xg_render_vram_resources_resolve_draw(primitive, &resources) ||
            !resources.has_texture ||
            (primitive->material.texture_depth != XG_RENDER_IR_TEXTURE_15_BIT &&
             !resources.has_clut))
            goto reject;
    }
    if (!xg_render_submission_ensure_source_frame(&description) ||
        source_frame_insertion_ordinal == UINT32_MAX)
        goto reject;
    draw.order.pass_id = 0u;
    draw.order.layer = -(int32_t)ot_bucket;
    draw.order.insertion_ordinal =
        UINT32_MAX - source_frame_insertion_ordinal;
    draw.provenance = provenance;
    draw.source_primitive_index = source_primitive_index;
    draw.has_provenance = true;
    draw.interpolable = semantic->interpolation_identity.valid;
    if (draw.interpolable)
        draw.interpolation_id =
            ((uint64_t)semantic->interpolation_identity.producer_id << 32u) |
            semantic->interpolation_identity.primitive_id;
    draw.primitive = *primitive;
    if (primitive->material.textured) {
        if (resources.has_texture) {
            resource_refs[resource_count++] = resources.texture;
            draw.texture_resource_id = resources.texture.resource_id;
            draw.texture_generation = resources.texture.generation;
        }
        if (resources.has_clut) {
            resource_refs[resource_count++] = resources.clut;
            draw.clut_resource_id = resources.clut.resource_id;
            draw.clut_generation = resources.clut.generation;
        }
    }
    fragment.draws = &draw;
    fragment.resources = resource_refs;
    fragment.draw_count = 1u;
    fragment.resource_count = resource_count;
    if (xg_render_fragment_runtime_ingress_production_fragment(
            &description, &fragment) != XG_RENDER_FRAGMENT_RUNTIME_OK)
        goto reject;
    ++source_frame_insertion_ordinal;
    return true;

reject:
    xg_render_fragment_runtime_reject_source_frame();
    return false;
}

static bool append_unique_resource(
        XgSemanticResourceRef *resources, uint32_t *resource_count,
        uint32_t capacity, const XgSemanticResourceRef *resource) {
    if (resources == NULL || resource_count == NULL || resource == NULL ||
        resource->resource_id == 0u || resource->generation == 0u ||
        resource->content_digest == 0u)
        return false;
    for (uint32_t index = 0u; index < *resource_count; ++index) {
        if (resources[index].resource_id != resource->resource_id ||
            resources[index].generation != resource->generation)
            continue;
        return resources[index].content_digest == resource->content_digest;
    }
    if (*resource_count >= capacity) return false;
    resources[(*resource_count)++] = *resource;
    return true;
}

static bool captured_geometry_matches(const GpuRenderSemantic *capture,
                                      const GpuRenderSemantic *packet) {
    if (capture->topology != packet->topology ||
        capture->triangle_count != packet->triangle_count ||
        capture->line_count != packet->line_count ||
        capture->material.textured != packet->material.textured ||
        capture->material.shading != packet->material.shading) {
        ++submission_diagnostics.layout_rejected;
        return false;
    }
    const bool lines = packet->topology == GPU_RENDER_SEMANTIC_LINES;
    const uint32_t count = lines ? packet->line_count : packet->triangle_count;
    if (count > (lines ? GPU_RENDER_SEMANTIC_LINE_CAPACITY
                       : GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY))
        return false;
    for (uint32_t item = 0u; item < count; ++item) {
        for (uint32_t vertex = 0u; vertex < (lines ? 2u : 3u); ++vertex) {
            const GpuRenderSemanticVertex *a =
                lines ? &capture->lines[item].vertices[vertex]
                      : &capture->triangles[item].vertices[vertex];
            const GpuRenderSemanticVertex *b =
                lines ? &packet->lines[item].vertices[vertex]
                      : &packet->triangles[item].vertices[vertex];
            if (a->x != b->x || a->y != b->y)
                return false;
        }
    }
    return true;
}

bool xg_render_submission_temporal_binding(
    const GpuRenderSemantic *semantic, XgRenderTemporalBinding *out) {
    if (!semantic || !out || semantic->submission_command_id > UINT32_C(0x001ffffc) ||
        (semantic->submission_command_id & 3u)) return false;
    const uint32_t id = (uint32_t)semantic->submission_command_id;
    const uint32_t index = source_capture_by_command[id >> 2];
    if (index >= source_capture_count) return false;
    const XgRenderSourceCapture *capture = &source_captures[index];
    const GpuRenderSemantic *source = &capture->command.semantic;
    if (capture->command.command_id != id || !capture->temporal.coverage.resource_id ||
        !submission_services.scene_generation || capture->scene_generation != submission_services.scene_generation() ||
        source->interpolation_identity.scene_id != semantic->interpolation_identity.scene_id ||
        source->interpolation_identity.producer_id != semantic->interpolation_identity.producer_id ||
        source->interpolation_identity.primitive_id != semantic->interpolation_identity.primitive_id ||
        !captured_geometry_matches(source, semantic)) return false;
    *out = capture->temporal;
    return true;
}

bool xg_render_submission_publish_temporal_coverage(
    uint32_t scope, const XgRenderTemporalComponent *components, uint32_t component_count,
    const XgRenderTemporalSample *samples, uint32_t sample_count,
    const XgRenderTemporalCommandBinding *bindings, uint32_t binding_count) {
    if (!native_work_mode) return true;
    if (!scope || (binding_count && !bindings) || (component_count && !components) ||
        !submission_services.scene_generation) return false;
    const uint64_t scene = submission_services.scene_generation();
    /* Preflight all command captures before publishing anything. */
    for (uint32_t i = 0; i < binding_count; ++i) {
        const uint32_t id = bindings[i].command_id & UINT32_C(0x1fffffff);
        if (id > UINT32_C(0x001ffffc) || (id & 3)) return false;
        const uint32_t index = source_capture_by_command[id >> 2];
        if (index >= source_capture_count || source_captures[index].command.command_id != id ||
            source_captures[index].scene_generation != scene || source_captures[index].consumed) return false;
        const GpuRenderInterpolationIdentity *owner = &source_captures[index].command.semantic.interpolation_identity;
        uint32_t c = 0;
        while (c < component_count && components[c].component_id != bindings[i].component_id) ++c;
        if (c == component_count || components[c].scene_id != owner->scene_id ||
            components[c].producer_id != owner->producer_id) return false;
    }
    XgSemanticResourceRef ref;
    if (!xg_render_native_work_temporal_coverage(scope, components, component_count, samples,
            sample_count, psx_get_cycle_count(), &ref)) return false;
    uint32_t retained = 0;
    for (; retained < binding_count; ++retained)
        if (xg_render_resource_acquire_snapshot((XgRenderResourceHandle){ref.resource_id, ref.generation},
                                               ref.content_digest) != XG_RENDER_RESOURCE_OK) break;
    const bool ok = retained == binding_count;
    if (ok) {
        for (uint32_t i = 0; i < source_capture_count; ++i)
            if (source_captures[i].temporal_scope == scope) release_temporal_capture(&source_captures[i]);
        for (uint32_t i = 0; i < binding_count; ++i) {
            const uint32_t id = bindings[i].command_id & UINT32_C(0x001ffffc);
            XgRenderSourceCapture *capture = &source_captures[source_capture_by_command[id >> 2]];
            release_temporal_capture(capture);
            capture->temporal = (XgRenderTemporalBinding){ref, bindings[i].component_id};
            capture->temporal_scope = scope;
        }
    } else while (retained != 0) {
        --retained;
        (void)xg_render_resource_release((XgRenderResourceHandle){ref.resource_id, ref.generation});
    }
    (void)xg_render_resource_release((XgRenderResourceHandle){ref.resource_id, ref.generation});
    return ok;
}

bool xg_render_submission_resolve_command(
    const XgRenderSourceFrameDescription *description, uint32_t command_id,
    const GpuRenderSemantic *packet, XgRenderSubmissionCommand *out_command) {
    XgRenderSourceCapture *capture = NULL;
    GpuRenderSemantic pre_scene_semantic;
    XgRenderSubmissionCommand pre_scene_command = {0};
    const XgRenderSubmissionCommand *source = NULL;
    uint32_t pre_scene_index = UINT32_MAX;

    if (description == NULL || packet == NULL || out_command == NULL ||
        description->scene_generation == 0u ||
        packet->triangle_count > GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY ||
        packet->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY)
        return false;
    command_id &= UINT32_C(0x001ffffc);
    *out_command = (XgRenderSubmissionCommand){
        .semantic = *packet,
        .command_id = command_id,
        .source_primitive_index = command_id,
    };
    const uint32_t capture_index = source_capture_by_command[command_id >> 2u];
    if (capture_index < source_capture_count &&
        source_captures[capture_index].command.command_id == command_id &&
        source_captures[capture_index].scene_generation ==
            description->scene_generation) {
        capture = &source_captures[capture_index];
        source = &capture->command;
    }
    /* A newly staged pre-scene record supersedes a retained, consumed capture
     * at the same reusable packet address. It need not open a bridge scene. */
    for (uint32_t index = 0u; index < pre_scene.count; ++index) {
        const XgRenderPreScenePrimitive *record = &pre_scene.records[index];
        if (record->temporal_only || ((record->packet_address + 4u) &
                                      UINT32_C(0x001ffffc)) != command_id)
            continue;
        if (capture != NULL &&
            capture->capture_sequence > pre_scene_capture_sequences[index])
            break;
        pre_scene_index = index;
        if (pre_scene.blocked || xg_render_backend_translate_primitive(
                                     &record->primitive, &pre_scene_semantic) !=
                                     XG_RENDER_BACKEND_OK) {
            if (!native_work_mode) return false;
            source = NULL;
            break;
        }
        if (record->interpolation_identity_valid)
            xg_render_semantic_set_interpolation_identity(
                &pre_scene_semantic, interpolation_generation(),
                record->interpolation_producer_id,
                record->interpolation_primitive_id);
        pre_scene_command = (XgRenderSubmissionCommand){
            .semantic = pre_scene_semantic,
            .command_id = command_id,
            .source_primitive_index = record->source_primitive_index,
            .producer_captured = true,
        };
        capture = NULL;
        source = &pre_scene_command;
        break;
    }
    if (source != NULL) {
        if (!captured_geometry_matches(&source->semantic, packet)) {
            ++submission_diagnostics.geometry_rejected;
            submission_diagnostics.last_rejected_command = command_id;
            /* A pending producer/packet disagreement is not a licence to
             * silently discard native geometry. Consumed cache entries may
             * legitimately have been overwritten by an uncaptured producer. */
            if (!native_work_mode && (capture == NULL || !capture->consumed))
                return false;
        } else {
            ++submission_diagnostics.geometry_matched;
            *out_command = *source;
            /* Draw environment is consumed in OT order, not capture order. */
            out_command->semantic.material = packet->material;
            /* Lighting/fog, texture animation and blend flags may change after
             * projection. Keep verified geometry, but consume final attributes. */
            const bool lines = packet->topology == GPU_RENDER_SEMANTIC_LINES;
            const uint32_t count = lines ? packet->line_count : packet->triangle_count;
            bool updated = source->semantic.material.raw_texture != packet->material.raw_texture ||
                source->semantic.material.semi_transparent != packet->material.semi_transparent;
            for (uint32_t item = 0u; item < count; ++item) {
                for (uint32_t vertex = 0u; vertex < (lines ? 2u : 3u); ++vertex) {
                    GpuRenderSemanticVertex *a = lines
                        ? &out_command->semantic.lines[item].vertices[vertex]
                        : &out_command->semantic.triangles[item].vertices[vertex];
                    const GpuRenderSemanticVertex *b = lines
                        ? &packet->lines[item].vertices[vertex]
                        : &packet->triangles[item].vertices[vertex];
                    updated |= a->u != b->u || a->v != b->v ||
                        a->r != b->r || a->g != b->g || a->b != b->b;
                    a->u = b->u;
                    a->v = b->v;
                    a->r = b->r;
                    a->g = b->g;
                    a->b = b->b;
                }
            }
            submission_diagnostics.attributes_updated += updated;
            if (source == &pre_scene_command) {
                if (out_command->semantic.interpolation_identity.valid) {
                    xg_render_semantic_set_corner_identities(
                        &out_command->semantic,
                        out_command->semantic.interpolation_identity
                            .producer_id,
                        out_command->semantic.interpolation_identity
                            .primitive_id);
                    if (!xg_render_submission_record_interpolation_anchors(
                            &out_command->semantic))
                        return false;
                }
                if (!capture_source_command(&out_command->semantic, command_id,
                                            source->source_primitive_index,
                                            source->provenance))
                    return false;
            }
        }
    } else {
        ++submission_diagnostics.source_missing;
    }
    out_command->semantic.submission_command_id = command_id;
    if (native_work_mode) {
        /* Acceptance supplies the final raster classification as well as the
         * material. Producer geometry must not erase 2D/effect modes. */
        out_command->semantic.screen_space_2d = packet->screen_space_2d;
        out_command->semantic.native_view_effect = packet->native_view_effect;
        out_command->semantic.native_view_effect_index =
            packet->native_view_effect_index;
        if (packet->screen_space_2d != GPU_RENDER_SCREEN_SPACE_2D_NONE) {
            for (uint32_t triangle = 0u;
                 triangle < out_command->semantic.triangle_count; ++triangle) {
                for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                    if (!out_command->semantic.triangles[triangle]
                             .vertices[vertex]
                             .native_view_position)
                        continue;
                    *out_command = (XgRenderSubmissionCommand){
                        .semantic = *packet,
                        .command_id = command_id,
                        .source_primitive_index = command_id,
                    };
                    out_command->semantic.submission_command_id = command_id;
                    break;
                }
            }
        }
        /* A snapped source-space billboard still belongs to the camera, not
         * the HUD. Explicit translated positions retain its authored size and
         * snapping while making it eligible for the presentation scissor.
         * Never infer this from an unbound packet or from its screen bounds. */
        const int64_t margin =
            (int64_t)xg_host_3d_native_view_margin() * INT64_C(65536);
        GpuRenderSemantic *geometry = &out_command->semantic;
        bool camera_geometry = out_command->producer_captured && margin > 0 &&
            (description->scene.module == XG_SEMANTIC_MODULE_FIELD ||
             description->scene.module == XG_SEMANTIC_MODULE_WORLD) &&
            geometry->screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE &&
            geometry->native_view_effect == GPU_RENDER_NATIVE_VIEW_EFFECT_NONE &&
            geometry->topology == GPU_RENDER_SEMANTIC_TRIANGLES &&
            geometry->triangle_count != 0u;
        for (uint32_t ti = 0u; camera_geometry &&
             ti < geometry->triangle_count; ++ti) {
            for (uint32_t vi = 0u; vi < 3u; ++vi) {
                const GpuRenderSemanticVertex *v = &geometry->triangles[ti].vertices[vi];
                camera_geometry &= !v->native_view_position &&
                    (v->projective_position || v->temporal_depth_valid) &&
                    (int64_t)v->x + margin <= INT32_MAX;
            }
        }
        if (camera_geometry) {
            for (uint32_t ti = 0u; ti < geometry->triangle_count; ++ti) {
                for (uint32_t vi = 0u; vi < 3u; ++vi) {
                    GpuRenderSemanticVertex *v = &geometry->triangles[ti].vertices[vi];
                    v->native_view_x = (int32_t)((int64_t)v->x + margin);
                    v->native_view_y = v->y;
                    v->native_view_position = 1u;
                    v->projective_native_offset_x = (int32_t)margin;
                }
            }
        }
        /* This hook runs at GPU acceptance in work mode. No legacy OT
         * completion follows to retire captures or drain pre-scene records. */
        const uint32_t index = source_capture_by_command[command_id >> 2u];
        if (index < source_capture_count &&
            source_captures[index].command.command_id == command_id)
            source_captures[index].consumed = true;
        if (pre_scene_index < pre_scene.count)
            (void)xg_render_submission_pre_scene_discard(
                &pre_scene.records[pre_scene_index]);
    }
    return true;
}

bool xg_render_submission_materialize_semantic_draw(
    const GpuRenderSemantic *semantic, XgSemanticDrawRecord *out_draw) {
    _Static_assert((int)XG_RENDER_IR_FIXED_FRACTION_BITS ==
                       (int)GPU_RENDER_FIXED_FRACTION_BITS,
                   "semantic coordinate format");
    if (semantic == NULL || out_draw == NULL ||
        semantic->screen_space_2d > GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE ||
        semantic->native_view_effect >
            GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID ||
        (semantic->native_view_effect == GPU_RENDER_NATIVE_VIEW_EFFECT_NONE &&
         semantic->native_view_effect_index != 0u) ||
        (semantic->native_view_effect ==
             GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID &&
         semantic->native_view_effect_index >= 340u))
        return false;
    if (semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
        if (semantic->triangle_count == 0u ||
            semantic->triangle_count > XG_RENDER_IR_TRIANGLE_CAPACITY ||
            semantic->line_count != 0u)
            return false;
    } else if (semantic->topology == GPU_RENDER_SEMANTIC_LINES) {
        if (semantic->triangle_count != 0u || semantic->line_count == 0u ||
            semantic->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY ||
            semantic->material.textured || semantic->material.raw_texture ||
            semantic->screen_space_2d != GPU_RENDER_SCREEN_SPACE_2D_NONE ||
            semantic->native_view_effect != GPU_RENDER_NATIVE_VIEW_EFFECT_NONE)
            return false;
    } else {
        return false;
    }
    XgRenderIrNativePrimitive *primitive = &out_draw->primitive;
    *primitive = (XgRenderIrNativePrimitive){0};
    out_draw->topology = semantic->topology;
    out_draw->line_count = semantic->line_count;
    memset(out_draw->lines, 0, sizeof(out_draw->lines));
    memcpy(out_draw->lines, semantic->lines,
           semantic->line_count * sizeof(out_draw->lines[0]));
    out_draw->screen_space_2d = semantic->screen_space_2d;
    out_draw->native_view_effect = semantic->native_view_effect;
    out_draw->native_view_effect_index = semantic->native_view_effect_index;
#define COPY_MATERIAL(field)                                                   \
    primitive->material.field = semantic->material.field
    COPY_MATERIAL(tpage);
    COPY_MATERIAL(texture_page_x);
    COPY_MATERIAL(texture_page_y);
    COPY_MATERIAL(clut_x);
    COPY_MATERIAL(clut_y);
    COPY_MATERIAL(draw_area_left);
    COPY_MATERIAL(draw_area_top);
    COPY_MATERIAL(draw_area_right);
    COPY_MATERIAL(draw_area_bottom);
    COPY_MATERIAL(draw_offset_x);
    COPY_MATERIAL(draw_offset_y);
    COPY_MATERIAL(texture_window_mask_x);
    COPY_MATERIAL(texture_window_mask_y);
    COPY_MATERIAL(texture_window_offset_x);
    COPY_MATERIAL(texture_window_offset_y);
    COPY_MATERIAL(textured);
    COPY_MATERIAL(raw_texture);
    COPY_MATERIAL(semi_transparent);
    COPY_MATERIAL(dither);
    COPY_MATERIAL(mask_set);
    COPY_MATERIAL(mask_check);
#undef COPY_MATERIAL
    primitive->material.texture_depth =
        (XgRenderIrTextureDepth)semantic->material.texture_depth;
    primitive->material.shading = (XgRenderIrShading)semantic->material.shading;
    primitive->material.blend_mode =
        (XgRenderIrBlendMode)semantic->material.blend_mode;
    primitive->triangle_count = semantic->triangle_count;
    for (uint32_t triangle = 0u; triangle < semantic->triangle_count;
         ++triangle) {
        primitive->triangles[triangle].split_index =
            semantic->triangles[triangle].split_index;
        primitive->triangles[triangle].split_count =
            semantic->triangles[triangle].split_count;
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            XgRenderIrVertex *target =
                &primitive->triangles[triangle].vertices[vertex];
            const GpuRenderSemanticVertex *source =
                &semantic->triangles[triangle].vertices[vertex];
#define COPY_VERTEX(field) target->field = source->field
            COPY_VERTEX(x);
            COPY_VERTEX(y);
            COPY_VERTEX(u);
            COPY_VERTEX(v);
            COPY_VERTEX(r);
            COPY_VERTEX(g);
            COPY_VERTEX(b);
            COPY_VERTEX(native_view_x);
            COPY_VERTEX(native_view_y);
            COPY_VERTEX(native_view_position);
            COPY_VERTEX(projective_view_x);
            COPY_VERTEX(projective_view_y);
            COPY_VERTEX(projective_view_z);
            COPY_VERTEX(projective_offset_x);
            COPY_VERTEX(projective_offset_y);
            COPY_VERTEX(projective_native_offset_x);
            COPY_VERTEX(projective_native_offset_y);
            COPY_VERTEX(projective_distance);
            COPY_VERTEX(projective_position);
            COPY_VERTEX(temporal_depth);
            COPY_VERTEX(temporal_depth_valid);
            COPY_VERTEX(interpolation_group_id);
            COPY_VERTEX(interpolation_vertex_id);
            COPY_VERTEX(interpolation_vertex_identity_valid);
#undef COPY_VERTEX
        }
    }
    return true;
}

void xg_render_submission_cancel_ordering_table(void) {
    if (prepared_source_ot == NULL) return;
    for (uint32_t index = 0u; index < prepared_source_ot->resource_count;
         ++index)
        (void)xg_render_resource_release((XgRenderResourceHandle){
            prepared_source_ot->resources[index].resource_id,
            prepared_source_ot->resources[index].generation,
        });
    free(prepared_source_ot);
    prepared_source_ot = NULL;
}

bool xg_render_submission_prepare_ordering_table(
    const XgRenderSourceFrameDescription *description, uint32_t start_address,
    uint32_t transferred_words, const XgRenderSubmissionCommand *commands,
    uint32_t command_count) {
    XgSemanticResourceRef resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgSemanticResourceRef target;
    uint32_t resource_count = 0u;
    uint32_t insertion_base = 0u;
    XgRenderFragmentRuntimeDiagnostics fragment_snapshot;

    ot_failure_detail = 1u;
    ot_failure_index = 0u;
    ot_failure_tpage = 0u;
    ot_failure_clut = 0u;

    if (description == NULL || description->scene_generation == 0u ||
        prepared_source_ot != NULL || transferred_words == 0u ||
        command_count > XG_RENDER_SCENE_DRAW_CAPACITY ||
        (command_count != 0u && commands == NULL) ||
        !submission_services_configured ||
        submission_services.source_frame_target == NULL)
        goto reject;
    prepared_source_ot = calloc(1u, sizeof(*prepared_source_ot));
    if (prepared_source_ot == NULL) goto reject;
    prepared_source_ot->description = *description;
    prepared_source_ot->start_address = start_address & UINT32_C(0x001ffffc);
    prepared_source_ot->transferred_words = transferred_words;
    xg_render_fragment_runtime_diagnostics(&fragment_snapshot);
    if (fragment_snapshot.terminal_failure) goto reject;
    if (description->scene.module == XG_SEMANTIC_MODULE_WORLD &&
        fragment_snapshot.frame_owned) {
        /* Consecutive OTs to the same target append in submission order. A page
         * change needs the framebuffer queue, not a second ambiguous pass 0. */
        if (world_ot_target.resource_id == 0u ||
            fragment_snapshot.primary_owner != XG_SEMANTIC_MODULE_WORLD ||
            world_ot_description.scene_generation !=
                description->scene_generation ||
            world_ot_description.display.width != description->display.width ||
            world_ot_description.display.height !=
                description->display.height ||
            world_ot_description.display.display_x !=
                description->display.display_x ||
            world_ot_description.display.display_y !=
                description->display.display_y ||
            world_ot_description.display.depth24 !=
                description->display.depth24 ||
            world_ot_description.display.interlaced !=
                description->display.interlaced) {
            ot_failure_detail = 2u;
            goto reject;
        }
        target = world_ot_target;
        insertion_base = source_frame_insertion_ordinal;
    } else {
        ot_failure_detail = 19u;
        if (!submission_services.source_frame_target(description, &target))
            goto reject;
    }
    if (command_count > UINT32_MAX - insertion_base) goto reject;
    prepared_source_ot->insertion_end = insertion_base + command_count;
    if (!append_unique_resource(resources, &resource_count,
                                XG_RENDER_SCENE_RESOURCE_CAPACITY, &target))
        goto reject;
    prepared_source_ot->pass = (XgSemanticPassRecord){
        .pass_id = 0u,
        .target_surface_id = target.resource_id,
        .target_generation = target.generation,
        .viewport_width = description->display.width,
        .viewport_height = description->display.height,
        .load_operation = XG_SEMANTIC_PASS_LOAD,
        .store = true,
        .presentation_output = true,
    };
    for (uint32_t index = 0u; index < command_count; ++index) {
        const XgRenderSubmissionCommand *command = &commands[index];
        XgSemanticDrawRecord *draw = &prepared_source_ot->draws[index];
        XgRenderVramResourceResolvedResources resolved = {0};

        ot_failure_detail = 300u;
        ot_failure_index = index;
        ot_failure_tpage = command->semantic.material.tpage;
        ot_failure_clut = ((uint32_t)command->semantic.material.clut_y << 16u) |
                          command->semantic.material.clut_x;
        if (command->command_id > UINT32_C(0x001ffffc) ||
            (command->command_id & 3u) != 0u)
            goto reject;
        if (submission_services.materialize_semantic_draw != NULL) {
            if (!submission_services.materialize_semantic_draw(
                    &command->semantic, draw))
                goto reject;
        } else if (!xg_render_submission_materialize_semantic_draw(
                       &command->semantic, draw)) {
            goto reject;
        }
        draw->order.insertion_ordinal = insertion_base + index;
        draw->provenance = command->provenance;
        draw->source_primitive_index = command->source_primitive_index;
        draw->has_provenance = command->has_provenance;
        draw->interpolable = command->semantic.interpolation_identity.valid;
        if (draw->interpolable)
            draw->interpolation_id =
                ((uint64_t)command->semantic.interpolation_identity.producer_id
                 << 32u) |
                command->semantic.interpolation_identity.primitive_id;
        if (draw->primitive.material.textured) {
            XgRenderIrNativePrimitive sampling = draw->primitive;

            /* SPRT endpoints are exclusive and can cross UV 255. Resolve the
             * wrapped sampling footprint without changing published vertices.
             */
            if (command->opcode >= 0x64u && command->opcode <= 0x7fu) {
                int32_t minimum[2] = {INT32_MAX, INT32_MAX};
                int32_t maximum[2] = {INT32_MIN, INT32_MIN};
                for (uint32_t triangle = 0u; triangle < sampling.triangle_count;
                     ++triangle) {
                    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                        const XgRenderIrVertex *point =
                            &sampling.triangles[triangle].vertices[vertex];
                        const int32_t uv[2] = {point->u / 65536,
                                               point->v / 65536};
                        for (uint32_t axis = 0u; axis < 2u; ++axis) {
                            if (uv[axis] < minimum[axis])
                                minimum[axis] = uv[axis];
                            if (uv[axis] > maximum[axis])
                                maximum[axis] = uv[axis];
                        }
                    }
                }
                for (uint32_t axis = 0u; axis < 2u; ++axis) {
                    if (minimum[axis] < 0) goto reject;
                    if (maximum[axis] > minimum[axis]) --maximum[axis];
                    if (minimum[axis] / 256 != maximum[axis] / 256) {
                        minimum[axis] = 0;
                        maximum[axis] = 255;
                    } else {
                        minimum[axis] &= 255;
                        maximum[axis] &= 255;
                    }
                }
                for (uint32_t triangle = 0u; triangle < sampling.triangle_count;
                     ++triangle) {
                    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                        XgRenderIrVertex *point =
                            &sampling.triangles[triangle].vertices[vertex];
                        point->u =
                            (vertex == 0u ? minimum[0] : maximum[0]) * 65536;
                        point->v =
                            (vertex == 0u ? minimum[1] : maximum[1]) * 65536;
                    }
                }
            }
            ot_failure_detail = 530u;
            if (!xg_render_vram_resources_resolve_draw(&sampling, &resolved) ||
                !resolved.has_texture ||
                (draw->primitive.material.texture_depth !=
                     XG_RENDER_IR_TEXTURE_15_BIT &&
                 !resolved.has_clut) ||
                !append_unique_resource(resources, &resource_count,
                                        XG_RENDER_SCENE_RESOURCE_CAPACITY,
                                        &resolved.texture) ||
                (resolved.has_clut &&
                 !append_unique_resource(resources, &resource_count,
                                         XG_RENDER_SCENE_RESOURCE_CAPACITY,
                                         &resolved.clut)))
                goto reject;
            draw->texture_resource_id = resolved.texture.resource_id;
            draw->texture_generation = resolved.texture.generation;
            if (resolved.has_clut) {
                draw->clut_resource_id = resolved.clut.resource_id;
                draw->clut_generation = resolved.clut.generation;
            }
        }
        prepared_source_ot->command_ids[index] = command->command_id;
    }
    prepared_source_ot->draw_count = command_count;
    ot_failure_detail = 533u;
    for (uint32_t index = 0u; index < resource_count; ++index) {
        if (xg_render_resource_acquire_snapshot(
                (XgRenderResourceHandle){resources[index].resource_id,
                                         resources[index].generation},
                resources[index].content_digest) != XG_RENDER_RESOURCE_OK)
            goto reject;
        prepared_source_ot->resources[prepared_source_ot->resource_count++] =
            resources[index];
    }
    ot_failure_detail = 0u;
    return true;
reject:
    xg_render_submission_cancel_ordering_table();
    xg_render_fragment_runtime_reject_source_frame();
    return false;
}

bool xg_render_submission_complete_ordering_table(uint32_t start_address,
                                                uint32_t transferred_words) {
    if (prepared_source_ot == NULL) return false;
    if (prepared_source_ot->blocked) goto reject;
    ot_failure_detail = 600u;
    if (prepared_source_ot->start_address !=
            (start_address & UINT32_C(0x001ffffc)) ||
        prepared_source_ot->transferred_words != transferred_words ||
        submission_services.scene_generation == NULL ||
        submission_services.scene_generation() !=
            prepared_source_ot->description.scene_generation)
        goto reject;
    const XgRenderSourceFrameFragment fragment = {
        .passes = &prepared_source_ot->pass,
        .pass_count = 1u,
        .draws = prepared_source_ot->draws,
        .draw_count = prepared_source_ot->draw_count,
        .resources = prepared_source_ot->resources,
        .resource_count = prepared_source_ot->resource_count,
    };
    if (xg_render_fragment_runtime_ingress_production_fragment(
            &prepared_source_ot->description, &fragment) !=
        XG_RENDER_FRAGMENT_RUNTIME_OK)
        goto reject;
    if (prepared_source_ot->description.scene.module ==
        XG_SEMANTIC_MODULE_WORLD) {
        world_ot_description = prepared_source_ot->description;
        world_ot_target = prepared_source_ot->resources[0];
        source_frame_insertion_ordinal = prepared_source_ot->insertion_end;
    }
    for (uint32_t index = 0u; index < prepared_source_ot->draw_count; ++index) {
        const uint32_t command_id = prepared_source_ot->command_ids[index];
        const uint32_t capture_index =
            source_capture_by_command[command_id >> 2u];
        if (capture_index < source_capture_count &&
            source_captures[capture_index].command.command_id == command_id)
            source_captures[capture_index].consumed = true;
        for (uint32_t capture = 0u; capture < pre_scene.count; ++capture) {
            const XgRenderPreScenePrimitive *record =
                &pre_scene.records[capture];
            if (!record->temporal_only &&
                ((record->packet_address + 4u) & UINT32_C(0x001ffffc)) ==
                    command_id) {
                (void)xg_render_submission_pre_scene_discard(record);
                break;
            }
        }
    }
    xg_render_submission_cancel_ordering_table();
    ot_failure_detail = 0u;
    return true;
reject:
    xg_render_submission_cancel_ordering_table();
    xg_render_fragment_runtime_reject_source_frame();
    return false;
}

void xg_render_submission_note_semantic_current(
    const GpuRenderSemantic *semantic) {
    if (prepared_source_ot == NULL || semantic == NULL ||
        semantic->submission_command_id > UINT32_C(0x001ffffc))
        return;
    for (uint32_t index = 0u; index < prepared_source_ot->draw_count; ++index) {
        if (prepared_source_ot->command_ids[index] !=
            semantic->submission_command_id)
            continue;
        XgSemanticDrawRecord *draw = &prepared_source_ot->draws[index];
        const XgRenderIrMaterialState previous = draw->primitive.material;
        bool valid = semantic->material.textured == previous.textured;

        if (previous.textured)
            valid = valid && previous.tpage == semantic->material.tpage &&
                    previous.clut_x == semantic->material.clut_x &&
                    previous.clut_y == semantic->material.clut_y &&
                    previous.texture_window_mask_x ==
                        semantic->material.texture_window_mask_x &&
                    previous.texture_window_mask_y ==
                        semantic->material.texture_window_mask_y &&
                    previous.texture_window_offset_x ==
                        semantic->material.texture_window_offset_x &&
                    previous.texture_window_offset_y ==
                        semantic->material.texture_window_offset_y;
        if (submission_services.materialize_semantic_draw != NULL)
            valid = valid && submission_services.materialize_semantic_draw(
                                 semantic, draw);
        else
            valid = valid && xg_render_submission_materialize_semantic_draw(semantic, draw);
        if (!valid) {
            prepared_source_ot->blocked = true;
            ot_failure_detail = 301u;
            ot_failure_index = index;
        }
        return;
    }
}

bool xg_render_submission_prepared_draw_copy(uint32_t index,
                                             XgSemanticDrawRecord *out_draw,
                                             uint32_t *out_command_id) {
    if (prepared_source_ot == NULL || out_draw == NULL ||
        out_command_id == NULL || index >= prepared_source_ot->draw_count)
        return false;
    *out_draw = prepared_source_ot->draws[index];
    *out_command_id = prepared_source_ot->command_ids[index];
    return true;
}

void xg_render_submission_ordering_table_status(uint32_t *out_detail,
                                                uint32_t *out_index,
                                                uint32_t *out_tpage,
                                                uint32_t *out_clut) {
    if (out_detail != NULL) *out_detail = ot_failure_detail;
    if (out_index != NULL) *out_index = ot_failure_index;
    if (out_tpage != NULL) *out_tpage = ot_failure_tpage;
    if (out_clut != NULL) *out_clut = ot_failure_clut;
}

static void reject_standalone_source(void) {
    XgRenderSourceFrameDescription description = {0};
    const XgRenderSourceFrameDescription *frame = NULL;

    if (native_work_mode) return;
    if (standalone_source.active) {
        frame = &standalone_source.description;
    } else if (submission_services_configured &&
               submission_services.source_frame_description != NULL &&
               submission_services.source_frame_description(&description)) {
        frame = &description;
    }
    (void)xg_render_fragment_runtime_ingress_production_fragment(
        frame, NULL);
}

static bool stage_standalone_source_primitive(
        const XgRenderIrNativePrimitive *primitive,
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index) {
    XgRenderSourceFrameDescription description = {0};
    XgRenderVramResourceResolvedResources resolved = {0};
    XgSemanticDrawRecord draw = {0};

    if (native_work_mode) return standalone_submission.open;
    if (primitive == NULL || semantic == NULL) {
        standalone_stage_failure_detail = 510u;
        return false;
    }
    if (semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES) {
        standalone_stage_failure_detail = 511u;
        return false;
    }
    if (semantic->triangle_count != primitive->triangle_count ||
        primitive->triangle_count == 0u) {
        standalone_stage_failure_detail = 512u;
        return false;
    }
    if (standalone_source.draw_count >= XG_RENDER_SCENE_DRAW_CAPACITY) {
        standalone_stage_failure_detail = 513u;
        return false;
    }
    if (!standalone_submission.open) {
        standalone_stage_failure_detail = 514u;
        return false;
    }
    if (!submission_services_configured) {
        standalone_stage_failure_detail = 515u;
        return false;
    }
    if (standalone_submission.producer.state_id.scene_epoch !=
            standalone_submission.visual_id.scene_epoch ||
        standalone_submission.producer.state_id.state_sequence !=
            standalone_submission.visual_id.state_sequence) {
        standalone_stage_failure_detail = 516u;
        return false;
    }
    if (!standalone_source.active) {
        if (submission_services.standalone_source_frame_description == NULL) {
            standalone_stage_failure_detail = 520u;
            return false;
        }
        if (!submission_services.standalone_source_frame_description(
                &description)) {
            standalone_stage_failure_detail = 521u;
            return false;
        }
        standalone_source.description = description;
        standalone_source.active = true;
    }

    if (standalone_source.description.scene.module == XG_SEMANTIC_MODULE_FIELD ||
        standalone_source.description.scene.module == XG_SEMANTIC_MODULE_WORLD)
        return capture_source_command(
            semantic, packet_address + 4u, source_primitive_index,
            standalone_submission.producer);

    draw.order.pass_id = 0u;
    draw.order.layer = 0;
    draw.order.insertion_ordinal = standalone_source.draw_count;
    draw.provenance = standalone_submission.producer;
    draw.source_primitive_index = source_primitive_index;
    draw.has_provenance = true;
    draw.interpolable = semantic->interpolation_identity.valid;
    if (draw.interpolable)
        draw.interpolation_id =
            ((uint64_t)semantic->interpolation_identity.producer_id << 32u) |
            semantic->interpolation_identity.primitive_id;
    draw.primitive = *primitive;

    if (primitive->material.textured) {
        if (!xg_render_vram_resources_resolve_draw(primitive, &resolved)) {
            standalone_stage_failure_detail = 530u;
            return false;
        }
        if (!resolved.has_texture) {
            standalone_stage_failure_detail = 531u;
            return false;
        }
        if (primitive->material.texture_depth != XG_RENDER_IR_TEXTURE_15_BIT &&
            !resolved.has_clut) {
            standalone_stage_failure_detail = 532u;
            return false;
        }
        if (!append_unique_resource(
                standalone_source.resources,
                &standalone_source.resource_count,
                XG_RENDER_SCENE_RESOURCE_CAPACITY, &resolved.texture)) {
            standalone_stage_failure_detail = 533u;
            return false;
        }
        if (resolved.has_clut &&
            !append_unique_resource(
                standalone_source.resources,
                &standalone_source.resource_count,
                XG_RENDER_SCENE_RESOURCE_CAPACITY, &resolved.clut)) {
            standalone_stage_failure_detail = 534u;
            return false;
        }
        draw.texture_resource_id = resolved.texture.resource_id;
        draw.texture_generation = resolved.texture.generation;
        if (resolved.has_clut) {
            draw.clut_resource_id = resolved.clut.resource_id;
            draw.clut_generation = resolved.clut.generation;
        }
    }
    standalone_source.draws[standalone_source.draw_count++] = draw;
    return true;
}

static bool publish_standalone_source(void) {
    XgRenderFragmentRuntimeDiagnostics fragment_snapshot = {0};
    XgSemanticPassRecord pass = {0};
    XgSemanticResourceRef target = {0};
    XgSemanticResourceRef resources[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    XgRenderSourceFrameFragment fragment = {0};
    uint32_t resource_count = 0u;
    uint32_t insertion_base;

    if (native_work_mode || !standalone_source.active) return true;
    if (standalone_source.description.scene.module == XG_SEMANTIC_MODULE_FIELD ||
        standalone_source.description.scene.module == XG_SEMANTIC_MODULE_WORLD)
        return true;
    xg_render_fragment_runtime_diagnostics(&fragment_snapshot);
    insertion_base = fragment_snapshot.frame_owned ? source_frame_insertion_ordinal : 0u;
    if (standalone_source.draw_count == 0u ||
        standalone_source.draw_count > UINT32_MAX - insertion_base)
        goto reject;

    if (!fragment_snapshot.frame_owned) {
        if (!submission_services_configured ||
            submission_services.source_frame_target == NULL ||
            !submission_services.source_frame_target(
                &standalone_source.description, &target) ||
            !append_unique_resource(
                resources, &resource_count,
                XG_RENDER_SCENE_RESOURCE_CAPACITY, &target))
            goto reject;
        pass = (XgSemanticPassRecord){
            .pass_id = 0u,
            .target_surface_id = target.resource_id,
            .target_generation = target.generation,
            .viewport_width = standalone_source.description.display.width,
            .viewport_height = standalone_source.description.display.height,
            .load_operation = standalone_source.description.scene.module ==
                    XG_SEMANTIC_MODULE_FIELD
                ? XG_SEMANTIC_PASS_LOAD : XG_SEMANTIC_PASS_CLEAR,
            .store = true,
            .presentation_output = true,
        };
        fragment.passes = &pass;
        fragment.pass_count = 1u;
    }
    for (uint32_t index = 0u;
         index < standalone_source.resource_count; ++index) {
        if (!append_unique_resource(
                resources, &resource_count,
                XG_RENDER_SCENE_RESOURCE_CAPACITY,
                &standalone_source.resources[index]))
            goto reject;
    }
    for (uint32_t index = 0u; index < standalone_source.draw_count; ++index)
        standalone_source.draws[index].order.insertion_ordinal =
            UINT32_MAX - (insertion_base + index);

    fragment.draws = standalone_source.draws;
    fragment.resources = resources;
    fragment.draw_count = standalone_source.draw_count;
    fragment.resource_count = resource_count;
    if (xg_render_fragment_runtime_ingress_production_fragment(
            &standalone_source.description, &fragment) !=
            XG_RENDER_FRAGMENT_RUNTIME_OK)
        return false;
    source_frame_insertion_ordinal =
        insertion_base + standalone_source.draw_count;
    return true;

reject:
    reject_standalone_source();
    return false;
}

bool xg_render_submission_ensure_source_frame(
        const XgRenderSourceFrameDescription *provided_description) {
    XgRenderSourceFrameDescription resolved_description = {0};
    const XgRenderSourceFrameDescription *description = provided_description;
    XgRenderFragmentRuntimeDiagnostics fragment_snapshot = {0};
    XgSemanticResourceRef target = {0};
    XgSemanticPassRecord pass = {0};
    XgRenderSourceFrameFragment fragment;

    if (description == NULL) {
        if (!submission_services_configured ||
            submission_services.source_frame_description == NULL ||
            !submission_services.source_frame_description(
                &resolved_description))
            return false;
        description = &resolved_description;
    }
    xg_render_fragment_runtime_diagnostics(&fragment_snapshot);
    if (fragment_snapshot.frame_owned)
        return !fragment_snapshot.terminal_failure &&
            fragment_snapshot.scene_generation == description->scene_generation &&
            fragment_snapshot.primary_owner == description->scene.module;
    if (submission_services.source_frame_target == NULL ||
        !submission_services.source_frame_target(description, &target))
        return false;
    pass = (XgSemanticPassRecord){
        .pass_id = 0u,
        .target_surface_id = target.resource_id,
        .target_generation = target.generation,
        .viewport_width = description->display.width,
        .viewport_height = description->display.height,
        .load_operation = description->scene.module == XG_SEMANTIC_MODULE_FIELD
            ? XG_SEMANTIC_PASS_LOAD : XG_SEMANTIC_PASS_CLEAR,
        .store = true,
        .presentation_output = true,
    };
    fragment = (XgRenderSourceFrameFragment){
        .passes = &pass,
        .resources = &target,
        .pass_count = 1u,
        .resource_count = 1u,
    };
    if (xg_render_fragment_runtime_ingress_production_fragment(
            description, &fragment) != XG_RENDER_FRAGMENT_RUNTIME_OK)
        return false;
    source_frame_insertion_ordinal = 0u;
    return true;
}

bool xg_render_submission_begin_source_frame(
        const XgRenderSourceFrameDescription *description,
        XgSemanticResourceRef *out_target) {
    XgRenderFragmentRuntimeDiagnostics fragment_snapshot = {0};
    XgSemanticPassRecord pass;
    XgRenderSourceFrameFragment fragment;

    if (description == NULL || out_target == NULL ||
        !submission_services_configured ||
        submission_services.source_frame_target == NULL)
        return false;
    xg_render_fragment_runtime_diagnostics(&fragment_snapshot);
    if (fragment_snapshot.frame_owned ||
        !submission_services.source_frame_target(description, out_target))
        return false;
    pass = (XgSemanticPassRecord){
        .pass_id = 0u,
        .target_surface_id = out_target->resource_id,
        .target_generation = out_target->generation,
        .viewport_width = description->display.width,
        .viewport_height = description->display.height,
        .load_operation = description->scene.module == XG_SEMANTIC_MODULE_FIELD
            ? XG_SEMANTIC_PASS_LOAD : XG_SEMANTIC_PASS_CLEAR,
        .store = true,
        .presentation_output = true,
    };
    fragment = (XgRenderSourceFrameFragment){
        .passes = &pass,
        .resources = out_target,
        .pass_count = 1u,
        .resource_count = 1u,
    };
    if (xg_render_fragment_runtime_ingress_production_fragment(
            description, &fragment) != XG_RENDER_FRAGMENT_RUNTIME_OK)
        return false;
    source_frame_insertion_ordinal = 0u;
    return true;
}

void xg_render_submission_pre_scene_clear(void) {
    /* count bounds every record access, so stale large primitive records are
     * unreachable after a lifecycle reset. */
    pre_scene.count = 0u;
    pre_scene.blocker = 0u;
    pre_scene.blocked = false;
}

void xg_render_submission_pre_scene_block(
        uint32_t blocker, bool preserve_existing_blocker) {
    pre_scene.blocked = true;
    if (!preserve_existing_blocker || pre_scene.blocker == 0u)
        pre_scene.blocker = blocker;
}

bool xg_render_submission_pre_scene_blocked(void) {
    return pre_scene.blocked;
}

bool xg_render_submission_pre_scene_available(uint32_t primitive_count) {
    return !pre_scene.blocked &&
        primitive_count <= XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY &&
        (native_work_mode || pre_scene.count <=
            XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY - primitive_count);
}

uint32_t xg_render_submission_pre_scene_count(void) {
    return pre_scene.count;
}

uint32_t xg_render_submission_pre_scene_blocker(void) {
    return pre_scene.blocker;
}

bool xg_render_submission_pre_scene_item_copy(
        uint32_t index, XgRenderPreScenePrimitive *out_record) {
    if (out_record == NULL || index >= pre_scene.count) return false;
    *out_record = pre_scene.records[index];
    return true;
}

bool xg_render_submission_pre_scene_discard(
        const XgRenderPreScenePrimitive *record) {
    const uint32_t packet_address = record != NULL
        ? record->packet_address & UINT32_C(0x001ffffc) : 0u;

    if (record == NULL) return false;
    for (uint32_t index = 0u; index < pre_scene.count; ++index) {
        const bool matches =
            (record->temporal_only && pre_scene.records[index].temporal_only &&
             pre_scene.records[index].interpolation_producer_id ==
                 record->interpolation_producer_id &&
             pre_scene.records[index].interpolation_primitive_id ==
                 record->interpolation_primitive_id) ||
            (!record->temporal_only &&
             !pre_scene.records[index].temporal_only &&
             (pre_scene.records[index].packet_address &
              UINT32_C(0x001ffffc)) == packet_address);

        if (!matches) continue;
        if (index + 1u < pre_scene.count)
            memmove(&pre_scene.records[index], &pre_scene.records[index + 1u],
                    (pre_scene.count - index - 1u) *
                        sizeof(pre_scene.records[0]));
        if (index + 1u < pre_scene.count)
            memmove(&pre_scene_capture_sequences[index],
                    &pre_scene_capture_sequences[index + 1u],
                    (pre_scene.count - index - 1u) * sizeof(pre_scene_capture_sequences[0]));
        --pre_scene.count;
        return true;
    }
    return false;
}

bool xg_render_submission_pre_scene_stage(
        const XgRenderPreScenePrimitive *record) {
    const uint32_t packet_address = record != NULL
        ? record->packet_address & UINT32_C(0x001ffffc) : 0u;

    if (record == NULL || pre_scene.blocked || capture_sequence == UINT64_MAX)
        return false;
    if (native_work_mode) {
        /* Work mode has no OT flush. Culled temporal records have no accepted
         * command, and packet captures belong in the reusable two-arena cache. */
        if (record->temporal_only) {
            ++submission_diagnostics.work_temporal_skipped;
            return true;
        }
        GpuRenderSemantic semantic;
        if (xg_render_backend_translate_primitive(&record->primitive, &semantic) !=
                XG_RENDER_BACKEND_OK)
            return false;
        if (record->interpolation_identity_valid) {
            xg_render_semantic_set_interpolation_identity(
                &semantic, interpolation_generation(),
                record->interpolation_producer_id, record->interpolation_primitive_id);
            xg_render_semantic_set_corner_identities(
                &semantic, record->interpolation_producer_id,
                record->interpolation_primitive_id);
        }
        if (!capture_source_command(&semantic, packet_address + 4u,
                                    record->source_primitive_index,
                                    (XgRenderIrProvenanceKey){0}))
            return false;
        ++submission_diagnostics.work_captures;
        return true;
    }
    for (uint32_t index = 0u; index < pre_scene.count; ++index) {
        if ((record->temporal_only && pre_scene.records[index].temporal_only &&
             pre_scene.records[index].interpolation_producer_id ==
                 record->interpolation_producer_id &&
             pre_scene.records[index].interpolation_primitive_id ==
                 record->interpolation_primitive_id) ||
            (!record->temporal_only &&
             !pre_scene.records[index].temporal_only &&
             (pre_scene.records[index].packet_address &
              UINT32_C(0x001ffffc)) == packet_address)) {
            pre_scene.records[index] = *record;
            pre_scene_capture_sequences[index] = ++capture_sequence;
            return true;
        }
    }
    if (pre_scene.count == XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY)
        return false;
    pre_scene_capture_sequences[pre_scene.count] = ++capture_sequence;
    pre_scene.records[pre_scene.count++] = *record;
    return true;
}

GuestRenderTransactionStatus xg_render_submission_stage_exact(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        const GpuRenderSemantic *semantic) {
    GuestRenderNativeStreamStatus status;

    if (semantic == NULL)
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (native_work_mode) {
        XgSemanticDrawRecord draw = {0};
        if (exact_command_id > UINT32_MAX || (exact_command_id & 3u) != 0u ||
            !xg_render_submission_materialize_semantic_draw(semantic, &draw))
            return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
        return capture_source_command(
                   semantic, (uint32_t)exact_command_id,
                   (uint32_t)exact_command_id, (XgRenderIrProvenanceKey){0})
            ? GUEST_RENDER_TRANSACTION_OK : GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED;
    }
    if (!guest_render_native_stream_enabled())
        return guest_render_transaction_stage_exact(
            visual_id, exact_command_id, semantic);

    status = guest_render_native_stream_stage_exact(
        visual_id, exact_command_id, semantic);
    switch (status) {
    case GUEST_RENDER_NATIVE_STREAM_OK:
        return GUEST_RENDER_TRANSACTION_OK;
    case GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT:
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    case GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED:
        return GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED;
    case GUEST_RENDER_NATIVE_STREAM_DUPLICATE_COMMAND:
        return GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET;
    case GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID:
        return GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID;
    case GUEST_RENDER_NATIVE_STREAM_DISABLED:
    case GUEST_RENDER_NATIVE_STREAM_NOT_FOUND:
    default:
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    }
}

bool xg_render_submission_record_interpolation_anchors(
        const GpuRenderSemantic *semantic) {
    GpuRenderInterpolationVertexAnchor anchors[
        GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY * 3u];
    size_t anchor_count = 0u;

    /* Native work carries vertex identities in DRAW itself, without a second
     * renderer transaction for temporal anchors. */
    if (native_work_mode) return semantic != NULL;
    for (uint32_t triangle = 0u; triangle < semantic->triangle_count;
         ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const GpuRenderSemanticVertex *candidate =
                &semantic->triangles[triangle].vertices[vertex];
            bool duplicate = false;

            if (!candidate->interpolation_vertex_identity_valid) continue;
            for (size_t prior = 0u; prior < anchor_count; ++prior) {
                duplicate |= anchors[prior].vertex.interpolation_group_id ==
                        candidate->interpolation_group_id &&
                    anchors[prior].vertex.interpolation_vertex_id ==
                        candidate->interpolation_vertex_id;
            }
            if (duplicate) continue;
            anchors[anchor_count++] = (GpuRenderInterpolationVertexAnchor){
                .scene_id = semantic->interpolation_identity.scene_id,
                .producer_id = semantic->interpolation_identity.producer_id,
                .primitive_id = semantic->interpolation_identity.primitive_id,
                .material = semantic->material,
                .vertex = *candidate,
            };
        }
    }
    return anchor_count == 0u ||
        gr_record_interpolation_anchors(anchors, anchor_count) ==
            GPU_RENDER_TRANSACTION_OK;
}

bool xg_render_submission_pre_scene_flush(void) {
    GpuRenderTransactionId visual_id;
    XgRenderIrProvenanceKey provenance;

    if (pre_scene.blocked || !submission_services_configured ||
        submission_services.active_auth_snapshot == NULL ||
        submission_services.active_auth_append == NULL ||
        !submission_services.active_auth_snapshot(&visual_id, &provenance))
        return false;

    for (uint32_t index = 0u; index < pre_scene.count; ++index) {
        const XgRenderPreScenePrimitive *record = &pre_scene.records[index];
        GpuRenderSemantic semantic;

        if (xg_render_backend_translate_primitive(
                &record->primitive, &semantic) != XG_RENDER_BACKEND_OK) {
            pre_scene.blocker = 7u;
            return false;
        }
        if (record->interpolation_identity_valid)
            xg_render_semantic_set_interpolation_identity(
                &semantic, interpolation_generation(),
                record->interpolation_producer_id,
                record->interpolation_primitive_id);
        if (record->interpolation_identity_valid &&
            !xg_render_submission_record_interpolation_anchors(&semantic)) {
            pre_scene.blocker = 8u;
            return false;
        }
        if (record->temporal_only) {
            if (native_work_mode) continue;
            if (!record->interpolation_identity_valid ||
                !xg_render_temporal_submission_stage(
                    &semantic, &record->temporal_cull)) {
                pre_scene.blocker = 10u;
                return false;
            }
            continue;
        }
        uint32_t append_detail = 0u;
        if (!submission_services.active_auth_append(
                record->packet_address & UINT32_C(0x001ffffc),
                record->source_primitive_index, record->ot_bucket,
                record->payload_word_count, &record->primitive,
                true,
                &append_detail)) {
            pre_scene.blocker = 11u + append_detail;
            return false;
        }
        if (xg_render_submission_stage_exact(
                visual_id,
                (record->packet_address & UINT32_C(0x001ffffc)) + 4u,
                &semantic) != GUEST_RENDER_TRANSACTION_OK) {
            pre_scene.blocker = 12u;
            return false;
        }
        if (!stage_source_draw(
                &record->primitive, &semantic,
                record->packet_address, record->source_primitive_index,
                record->ot_bucket,
                provenance)) {
            pre_scene.blocker = 13u;
            return false;
        }
    }
    xg_render_submission_pre_scene_clear();
    return true;
}

bool xg_render_submission_stage_active_primitive(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t ot_bucket,
        uint8_t payload_word_count, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_blocker) {
    GuestRenderBridgeSnapshot bridge = {0};
    GpuRenderSemantic semantic;
    GpuRenderTransactionId visual_id;
    XgRenderIrProvenanceKey provenance;

    if (failure_blocker != NULL) *failure_blocker = 0u;
    if (primitive == NULL || !submission_services_configured ||
        submission_services.active_auth_available == NULL ||
        !submission_services.active_auth_available()) {
        if (failure_blocker != NULL) *failure_blocker = 58u;
        return false;
    }
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK) {
        if (failure_blocker != NULL) *failure_blocker = 59u;
        return false;
    }
    if (bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE) {
        if (failure_blocker != NULL) *failure_blocker = 60u;
        return false;
    }
    if (submission_services.active_auth_snapshot == NULL ||
        !submission_services.active_auth_snapshot(&visual_id, &provenance)) {
        if (failure_blocker != NULL) *failure_blocker = 61u;
        return false;
    }
    if (xg_render_backend_translate_primitive(primitive, &semantic) !=
            XG_RENDER_BACKEND_OK) {
        if (failure_blocker != NULL) *failure_blocker = 62u;
        return false;
    }
    xg_render_semantic_set_interpolation_identity(
        &semantic, interpolation_generation(),
        interpolation_producer_id, interpolation_primitive_id);
    if (submission_services.active_auth_append == NULL ||
        !submission_services.active_auth_append(
            packet_address & UINT32_C(0x001ffffc), source_primitive_index,
            ot_bucket, payload_word_count, primitive, false, NULL)) {
        if (failure_blocker != NULL) *failure_blocker = 63u;
        return false;
    }
    if (xg_render_submission_stage_exact(
            visual_id, (packet_address & UINT32_C(0x001ffffc)) + 4u,
            &semantic) != GUEST_RENDER_TRANSACTION_OK) {
        if (failure_blocker != NULL) *failure_blocker = 64u;
        return false;
    }
    if (!stage_source_draw(
            primitive, &semantic, packet_address, source_primitive_index, ot_bucket,
            provenance)) {
        if (failure_blocker != NULL) *failure_blocker = 65u;
        return false;
    }
    notify_primitive_staged(primitive, source_primitive_index);
    return true;
}

XgRenderFieldCharacterStageResult
xg_render_submission_stage_field_character(
        const XgRenderIrNativePrimitive *primitive,
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t ot_bucket,
        GpuRenderTransactionId visual_id) {
    GpuRenderTransactionId authenticated_visual_id;
    XgRenderIrProvenanceKey provenance;

    if (!submission_services_configured || primitive == NULL ||
        semantic == NULL || submission_services.active_auth_append == NULL ||
        !submission_services.active_auth_append(
            packet_address, source_primitive_index, ot_bucket,
            XG_FIELD_CHARACTER_PACKET_WORD_COUNT, primitive, false, NULL))
        return XG_RENDER_FIELD_CHARACTER_STAGE_AUTH_FAILED;
    if (xg_render_submission_stage_exact(
            visual_id, packet_address + 4u, semantic) !=
        GUEST_RENDER_TRANSACTION_OK)
        return XG_RENDER_FIELD_CHARACTER_STAGE_SUBMISSION_FAILED;
    if (submission_services.active_auth_snapshot == NULL ||
        !submission_services.active_auth_snapshot(
            &authenticated_visual_id, &provenance) ||
        authenticated_visual_id.scene_epoch != visual_id.scene_epoch ||
        authenticated_visual_id.state_sequence != visual_id.state_sequence ||
        !stage_source_draw(
            primitive, semantic, packet_address, source_primitive_index, ot_bucket,
            provenance))
        return XG_RENDER_FIELD_CHARACTER_STAGE_SUBMISSION_FAILED;
    return XG_RENDER_FIELD_CHARACTER_STAGE_OK;
}

bool xg_render_submission_standalone_open(void) {
    return standalone_submission.open;
}

static bool begin_standalone_scene(void) {
    GuestRenderSceneConfig config;
    GuestRenderTransactionPendingSnapshot pending = {0};
    GuestRenderTransactionSnapshot transaction = {0};

    if (!submission_services_configured ||
        submission_services.standalone_scene_config == NULL ||
        !submission_services.standalone_scene_config(&config) ||
        guest_render_transaction_pending_snapshot(&pending) !=
            GUEST_RENDER_TRANSACTION_OK ||
        pending.binding_count != 0u ||
        guest_render_transaction_snapshot(&transaction) !=
            GUEST_RENDER_TRANSACTION_OK ||
        transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE ||
        guest_render_bridge_begin_scene(&config) != GUEST_RENDER_OK)
        return false;
    if (submission_services.presentation_gate == NULL ||
        !submission_services.presentation_gate())
        guest_render_bridge_force_original(
            GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    return true;
}

bool xg_render_submission_standalone_begin(void) {
    const GuestRenderProducerProvenance provenance = {
        GUEST_RENDER_PRODUCER_NATIVE,
        {0},
    };
    GuestRenderBridgeSnapshot bridge = {0};

    if (standalone_submission.open) return true;
    if (!begin_standalone_scene())
        return false;
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        guest_render_bridge_begin_state(&standalone_submission.visual_id) !=
            GUEST_RENDER_OK ||
        guest_render_bridge_producer_begin(
            standalone_submission.visual_id, &provenance,
            &standalone_submission.producer) != GUEST_RENDER_OK) {
        guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
        clear_standalone();
        return false;
    }
    standalone_submission.open = true;
    return true;
}

void xg_render_submission_standalone_abort(void) {
    if (standalone_submission.open) {
        discard_source_visual((GpuRenderTransactionId){
            standalone_submission.visual_id.scene_epoch,
            standalone_submission.visual_id.state_sequence,
        });
        guest_render_native_stream_abandon_visual(
            (GpuRenderTransactionId){
                standalone_submission.visual_id.scene_epoch,
                standalone_submission.visual_id.state_sequence,
            });
        guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
        guest_render_transaction_clear_pending();
        clear_standalone();
    }
    xg_render_temporal_submission_reset();
}

bool xg_render_submission_standalone_finalize(void) {
    GuestRenderProducerSlot slot = {0};
    GuestRenderCompletedState completed = {0};
    GuestRenderBridgeSnapshot completed_snapshot = {0};
    GpuRenderTransactionId activated_visual = {0};
    bool visual_activated = false;

    if (!standalone_submission.open) {
        if (native_work_mode) return true;
        if (guest_render_bridge_last_completed(
                &completed_snapshot, &completed) == GUEST_RENDER_OK &&
            completed.binding_count != 0u &&
            guest_render_native_stream_has_staged_predecessor(
                (GpuRenderTransactionId){
                    completed.id.scene_epoch, completed.id.state_sequence,
                })) {
            const GpuRenderTransactionId visual_id = {
                completed.id.scene_epoch, completed.id.state_sequence,
            };
            if (guest_render_native_stream_activate_visual(visual_id) ==
                    GUEST_RENDER_NATIVE_STREAM_OK)
                retire_previous_standalone_visual(visual_id);
        }
        return true;
    }
    if (guest_render_bridge_producer_end(
            standalone_submission.producer, &slot) != GUEST_RENDER_OK ||
        !guest_render_bridge_id_equal(
            slot.handle.state_id, standalone_submission.visual_id) ||
        guest_render_bridge_finalize_state(
            standalone_submission.visual_id, &completed) != GUEST_RENDER_OK ||
        !guest_render_bridge_id_equal(
            completed.id, standalone_submission.visual_id)) {
        reject_standalone_source();
        xg_render_submission_standalone_abort();
        return false;
    }
    if (completed.binding_count == 0u) {
        const bool source_was_active = standalone_source.active;
        if (source_was_active) reject_standalone_source();
        xg_render_submission_standalone_abort();
        return !source_was_active;
    }
    if (!native_work_mode && guest_render_native_stream_enabled()) {
        activated_visual = (GpuRenderTransactionId){
            completed.id.scene_epoch, completed.id.state_sequence,
        };
        if (guest_render_native_stream_activate_visual(activated_visual) !=
                GUEST_RENDER_NATIVE_STREAM_OK) {
            reject_standalone_source();
            xg_render_submission_standalone_abort();
            return false;
        }
        visual_activated = true;
    }
    if (!publish_standalone_source()) {
        standalone_stage_failure_detail = 600u;
        xg_render_submission_standalone_abort();
        return false;
    }
    if (visual_activated)
        retire_previous_standalone_visual(activated_visual);
    clear_standalone();
    return true;
}

static bool stage_standalone_semantic(
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index, bool record_anchors) {
    GuestRenderStatus bind_status;
    GuestRenderTransactionStatus stage_status;

    standalone_stage_failure_detail = 0u;
    if (semantic == NULL) {
        standalone_stage_failure_detail = 1u;
        return false;
    }
    if (!xg_render_submission_standalone_begin()) {
        standalone_stage_failure_detail = 2u;
        return false;
    }
    bind_status = guest_render_bridge_bind_packet(
        standalone_submission.producer,
        packet_address & UINT32_C(0x001ffffc), source_primitive_index);
    if (bind_status != GUEST_RENDER_OK) {
        standalone_stage_failure_detail = 100u + (uint32_t)bind_status;
        xg_render_submission_standalone_abort();
        return false;
    }
    stage_status = xg_render_submission_stage_exact(
        (GpuRenderTransactionId){
            standalone_submission.visual_id.scene_epoch,
            standalone_submission.visual_id.state_sequence,
        },
        (packet_address & UINT32_C(0x001ffffc)) + 4u, semantic);
    if (stage_status != GUEST_RENDER_TRANSACTION_OK) {
        standalone_stage_failure_detail = 200u + (uint32_t)stage_status;
        xg_render_submission_standalone_abort();
        return false;
    }
    if (record_anchors && semantic->interpolation_identity.valid &&
        !xg_render_submission_record_interpolation_anchors(semantic)) {
        standalone_stage_failure_detail = 400u;
        xg_render_submission_standalone_abort();
        return false;
    }
    if (native_work_mode &&
        !capture_source_command(semantic, packet_address + 4u,
                                source_primitive_index, standalone_submission.producer)) {
        standalone_stage_failure_detail = 501u;
        xg_render_submission_standalone_abort();
        return false;
    }
    return true;
}

uint32_t xg_render_submission_standalone_failure_detail(void) {
    return standalone_stage_failure_detail;
}

bool xg_render_submission_stage_standalone_semantic_identified(
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id) {
    GpuRenderSemantic identified;

    if (semantic == NULL) return false;
    identified = *semantic;
    xg_render_semantic_set_interpolation_identity(
        &identified, interpolation_generation(),
        interpolation_producer_id, interpolation_primitive_id);
    if (!stage_standalone_semantic(
            &identified, packet_address, source_primitive_index, true))
        return false;
    if (native_work_mode) return true;
    XgRenderSourceFrameDescription description;
    if (submission_services.standalone_source_frame_description != NULL &&
        submission_services.standalone_source_frame_description(&description) &&
        (description.scene.module == XG_SEMANTIC_MODULE_FIELD ||
         description.scene.module == XG_SEMANTIC_MODULE_WORLD) &&
        capture_source_command(
            &identified, packet_address + 4u, source_primitive_index,
            standalone_submission.producer))
        return true;
    /* Semantic-only producers currently carry line topology. Native must never
     * publish a partial frame that silently drops those records. */
    standalone_stage_failure_detail = 500u;
    reject_standalone_source();
    xg_render_submission_standalone_abort();
    return false;
}

static bool stage_standalone_primitive_identified(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, bool record_anchors) {
    GpuRenderSemantic semantic;
    XgRenderBackendStatus translate_status;

    if (primitive == NULL) {
        standalone_stage_failure_detail = 1u;
        return false;
    }
    translate_status = xg_render_backend_translate_primitive(
        primitive, &semantic);
    if (translate_status != XG_RENDER_BACKEND_OK) {
        standalone_stage_failure_detail = 300u + (uint32_t)translate_status;
        return false;
    }
    xg_render_semantic_set_corner_identities(
        &semantic, interpolation_producer_id, interpolation_primitive_id);
    xg_render_semantic_set_interpolation_identity(
        &semantic, interpolation_generation(), interpolation_producer_id,
        interpolation_primitive_id);
    if (!stage_standalone_semantic(
            &semantic, packet_address, source_primitive_index, record_anchors))
        return false;
    if (!stage_standalone_source_primitive(
            primitive, &semantic, packet_address, source_primitive_index)) {
        if (standalone_stage_failure_detail == 0u)
            standalone_stage_failure_detail = 501u;
        reject_standalone_source();
        xg_render_submission_standalone_abort();
        return false;
    }
    notify_primitive_staged(primitive, source_primitive_index);
    return true;
}

bool xg_render_submission_stage_standalone_primitive_identified(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id) {
    return stage_standalone_primitive_identified(
        primitive, packet_address, source_primitive_index,
        interpolation_producer_id, interpolation_primitive_id, true);
}

bool xg_render_submission_stage_standalone_primitive_deferred_anchors(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id) {
    return stage_standalone_primitive_identified(
        primitive, packet_address, source_primitive_index,
        interpolation_producer_id, interpolation_primitive_id, false);
}

bool xg_render_submission_stage_standalone_primitive_with_detail(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_detail) {
    const bool staged =
        xg_render_submission_stage_standalone_primitive_identified(
            primitive, packet_address, source_primitive_index,
            interpolation_producer_id, interpolation_primitive_id);

    if (failure_detail != NULL)
        *failure_detail = xg_render_submission_standalone_failure_detail();
    return staged;
}

bool xg_render_submission_stage_temporal_primitive_identified(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id,
        const GpuRenderTemporalCullPolicy *policy) {
    GpuRenderSemantic semantic;

    /* Culled/virtual-only primitives are not GPU-accepted DRAW operations. */
    if (native_work_mode) return primitive != NULL && policy != NULL;
    if (primitive == NULL || policy == NULL ||
        xg_render_backend_translate_primitive(primitive, &semantic) !=
            XG_RENDER_BACKEND_OK)
        return false;
    xg_render_semantic_set_interpolation_identity(
        &semantic, interpolation_generation(),
        interpolation_producer_id, interpolation_primitive_id);
    xg_render_semantic_set_corner_identities(
        &semantic, interpolation_producer_id, interpolation_primitive_id);
    return xg_render_temporal_submission_stage(&semantic, policy);
}

bool xg_render_submission_cover_temporal_current(
        const GpuRenderSemantic *semantic) {
    if (native_work_mode) return semantic != NULL;
    return xg_render_temporal_submission_cover_current(semantic);
}

bool xg_render_submission_finalize_temporal(void) {
    if (native_work_mode) return true;
    return xg_render_temporal_submission_flush();
}

bool xg_render_submission_complete_source_frame(void) {
    XgRenderSourceFrameSnapshot snapshot = {0};

    xg_render_source_frame_snapshot(&snapshot);
    return !snapshot.active || snapshot.complete ||
        xg_render_source_frame_complete() == XG_RENDER_SOURCE_FRAME_OK;
}

void xg_render_submission_reset(void) {
    xg_render_submission_cancel_ordering_table();
    submission_diagnostics = (XgRenderSubmissionDiagnostics){0};
    clear_source_captures();
    source_capture_reuse_cursor = 0u;
    capture_sequence = 0u;
    world_ot_target = (XgSemanticResourceRef){0};
    xg_render_submission_pre_scene_clear();
    clear_standalone();
    standalone_stage_failure_detail = 0u;
    xg_render_temporal_submission_reset();
    last_activated_standalone_visual = (GpuRenderTransactionId){0};
    source_frame_insertion_ordinal = 0u;
}

void xg_render_submission_reset_transaction(void) {
    GuestRenderTransactionSnapshot snapshot = {0};

    if (guest_render_native_stream_enabled()) {
        GpuRenderTransactionId visual_id;

        if (submission_services_configured &&
            submission_services.active_auth_available != NULL &&
            submission_services.active_auth_available() &&
            submission_services.active_auth_snapshot != NULL &&
            submission_services.active_auth_snapshot(&visual_id, NULL))
            guest_render_native_stream_abandon_visual(visual_id);
        return;
    }
    if (guest_render_transaction_snapshot(&snapshot) ==
            GUEST_RENDER_TRANSACTION_OK &&
        snapshot.phase == GUEST_RENDER_TRANSACTION_ACTIVE)
        (void)guest_render_transaction_abort_before_observation(
            GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT);
    guest_render_transaction_invalidate_deferred();
    guest_render_transaction_clear_pending();
}

void xg_render_submission_reject_producer(void) {
    guest_render_transaction_invalidate_deferred();
    guest_render_transaction_clear_pending();
}

void xg_render_submission_disarm(void) {
    xg_render_submission_cancel_ordering_table();
    clear_source_captures();
    xg_render_submission_reset_transaction();
    xg_render_submission_pre_scene_clear();
    xg_render_submission_standalone_abort();
    /* Authority loss still drops every reusable capture. Native work has no
     * mutable legacy SourceFrame to wipe on this potentially per-write path. */
    if (!native_work_mode) xg_render_source_frame_reset();
}

void xg_render_submission_prepare_authenticated_scene(void) {
    if (xg_render_submission_standalone_open() &&
        !xg_render_submission_standalone_finalize())
        xg_render_fragment_runtime_reject_source_frame();
    /* A rejected cutover cannot be flushed into the new authenticated
     * producer, while valid pre-scene records remain eligible. */
    if (xg_render_submission_pre_scene_blocked())
        xg_render_submission_pre_scene_clear();
    xg_render_submission_reset_transaction();
    /* Packet addresses are reusable guest command sources. Preserve stream
     * allocation, but start each authenticated scene with no occupied IDs. */
    guest_render_native_stream_clear();
}

void xg_render_submission_scene_boundary(void) {
    xg_render_submission_cancel_ordering_table();
    clear_source_captures();
    guest_render_native_stream_clear();
    xg_render_submission_reset_transaction();
    xg_render_submission_pre_scene_clear();
    xg_render_submission_standalone_abort();
    xg_render_source_frame_reset();
    last_activated_standalone_visual = (GpuRenderTransactionId){0};
}

void xg_render_submission_source_reset(void) {
    xg_render_submission_cancel_ordering_table();
    clear_source_captures();
    xg_render_submission_pre_scene_clear();
    xg_render_temporal_submission_reset();
}

void xg_render_submission_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.watched_range_mutation ||
            event->mutation.artifact_mutation)
            xg_render_submission_standalone_abort();
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        xg_render_submission_scene_boundary();
    } else if (event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST) {
        xg_render_submission_disarm();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_submission_reset_transaction();
        xg_render_submission_reset();
    }
}
