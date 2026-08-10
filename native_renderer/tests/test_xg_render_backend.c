#include "xg_render_backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

typedef enum EventKind {
    EVENT_BEGIN = 1,
    EVENT_BARRIER,
    EVENT_COMPATIBILITY,
    EVENT_DRAW,
    EVENT_COMMIT,
    EVENT_ROLLBACK,
} EventKind;

typedef enum FailureMode {
    FAIL_NONE = 0,
    FAIL_BEGIN,
    FAIL_BARRIER,
    FAIL_DRAW,
    FAIL_COMMIT,
    FAIL_ROLLBACK,
} FailureMode;

typedef struct Event {
    EventKind kind;
    uint32_t value;
} Event;

typedef struct CompatibilityContext {
    bool fail;
    uint32_t fail_ordinal;
} CompatibilityContext;

static Event events[32];
static size_t event_count;
static GpuRenderSemantic submitted_semantics[8];
static size_t submitted_semantic_count;
static FailureMode failure_mode;
static uint32_t failure_value;
static size_t begin_call_count;
static size_t barrier_call_count;
static size_t draw_call_count;
static size_t rollback_call_count;
static GpuRenderTransactionId observed_begin_id;
static GpuRenderTransactionId observed_rollback_id;
static const GpuRenderPresent *observed_present;

static void record_event(EventKind kind, uint32_t value) {
    if (event_count < sizeof(events) / sizeof(events[0])) {
        events[event_count].kind = kind;
        events[event_count].value = value;
        ++event_count;
    }
}

static void reset_facade(void) {
    memset(events, 0, sizeof(events));
    memset(submitted_semantics, 0, sizeof(submitted_semantics));
    memset(&observed_begin_id, 0, sizeof(observed_begin_id));
    memset(&observed_rollback_id, 0, sizeof(observed_rollback_id));
    event_count = 0u;
    submitted_semantic_count = 0u;
    failure_mode = FAIL_NONE;
    failure_value = 0u;
    begin_call_count = 0u;
    barrier_call_count = 0u;
    draw_call_count = 0u;
    rollback_call_count = 0u;
    observed_present = NULL;
}

GpuRenderTransactionStatus gr_transaction_begin(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial) {
    observed_begin_id = transaction_id;
    ++begin_call_count;
    record_event(EVENT_BEGIN, (uint32_t)vram_mutation_serial);
    return failure_mode == FAIL_BEGIN ? GPU_RENDER_TRANSACTION_BACKEND_ERROR :
                                       GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_ordering_barrier(
        GpuRenderTransactionId transaction_id) {
    (void)transaction_id;
    ++barrier_call_count;
    record_event(EVENT_BARRIER, (uint32_t)barrier_call_count);
    if (failure_mode == FAIL_BARRIER && failure_value == barrier_call_count)
        return GPU_RENDER_TRANSACTION_CONTEXT_LOST;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_draw_semantic(
        GpuRenderTransactionId transaction_id,
        const GpuRenderSemantic *semantic) {
    (void)transaction_id;
    ++draw_call_count;
    record_event(EVENT_DRAW,
                 (uint32_t)(semantic->triangles[0].vertices[0].x /
                            INT32_C(65536)));
    if (submitted_semantic_count <
        sizeof(submitted_semantics) / sizeof(submitted_semantics[0])) {
        submitted_semantics[submitted_semantic_count] = *semantic;
        ++submitted_semantic_count;
    }
    if (failure_mode == FAIL_DRAW && failure_value == draw_call_count)
        return GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_commit_validate(
        GpuRenderTransactionId transaction_id,
        uint64_t current_vram_mutation_serial,
        const GpuRenderPresent *present) {
    (void)transaction_id;
    observed_present = present;
    record_event(EVENT_COMMIT, (uint32_t)current_vram_mutation_serial);
    return failure_mode == FAIL_COMMIT ? GPU_RENDER_TRANSACTION_VALIDATION_FAILED :
                                        GPU_RENDER_TRANSACTION_READY;
}

GpuRenderTransactionStatus gr_rollback(
        GpuRenderTransactionId transaction_id) {
    observed_rollback_id = transaction_id;
    ++rollback_call_count;
    record_event(EVENT_ROLLBACK, (uint32_t)transaction_id.state_sequence);
    return failure_mode == FAIL_ROLLBACK ? GPU_RENDER_TRANSACTION_CONTEXT_LOST :
                                          GPU_RENDER_TRANSACTION_OK;
}

static bool compatibility_submit(const XgRenderIrCompatibilityItem *item,
                                 void *user_data) {
    CompatibilityContext *context = (CompatibilityContext *)user_data;
    const uint32_t ordinal = item->base.ordering.final_ordinal;

    record_event(EVENT_COMPATIBILITY, ordinal);
    return !context || !context->fail || context->fail_ordinal != ordinal;
}

static GuestRenderVisualStateId fixture_state(void) {
    const GuestRenderVisualStateId state_id = { 31u, 9u };

    return state_id;
}

static XgRenderIrItemBase make_base(size_t index,
                                    size_t item_count,
                                    bool native_item) {
    XgRenderIrItemBase base;
    const uint32_t address = UINT32_C(0x100) + (uint32_t)index * 0x20u;

    memset(&base, 0, sizeof(base));
    base.ordering.ot_bucket = (uint32_t)(100u - index);
    base.ordering.final_ordinal = (uint32_t)index;
    base.ordering.packet_guest_address = address;
    base.ordering.predecessor_guest_address =
        index == 0u ? XG_RENDER_IR_NO_PACKET_ADDRESS : address - 0x20u;
    base.ordering.successor_guest_address =
        index + 1u == item_count ? XG_RENDER_IR_NO_PACKET_ADDRESS :
                                  address + 0x20u;
    base.ordering.tag_link_guest_address =
        index + 1u == item_count ? XG_RENDER_IR_TAG_LINK_END :
                                  address + 0x20u;
    base.ordering.tag_payload_word_count = native_item ? 9u : 0u;
    if (native_item) {
        base.has_provenance = true;
        base.provenance_key.state_id = fixture_state();
        base.provenance_key.slot_index = index + 1u;
        base.source_primitive_index = (uint32_t)(20u + index);
    }
    return base;
}

static XgRenderIrMaterialState make_material(void) {
    XgRenderIrMaterialState material;

    memset(&material, 0, sizeof(material));
    material.tpage = 0x102u;
    material.texture_page_x = 2u;
    material.clut_x = 32u;
    material.clut_y = 7u;
    material.draw_area_right = 319u;
    material.draw_area_bottom = 239u;
    material.draw_offset_x = -7;
    material.draw_offset_y = 11;
    material.texture_depth = XG_RENDER_IR_TEXTURE_15_BIT;
    material.texture_window_mask_x = 1u;
    material.texture_window_mask_y = 2u;
    material.texture_window_offset_x = 3u;
    material.texture_window_offset_y = 4u;
    material.shading = XG_RENDER_IR_SHADING_GOURAUD;
    material.textured = true;
    material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
    material.dither = true;
    return material;
}

static XgRenderIrNativeItem make_native(size_t index,
                                       size_t item_count,
                                       uint8_t triangle_count,
                                       int32_t first_x) {
    XgRenderIrNativeItem item;
    size_t triangle_index;
    size_t vertex_index;

    memset(&item, 0, sizeof(item));
    item.base = make_base(index, item_count, true);
    item.native.material = make_material();
    item.native.triangle_count = triangle_count;
    for (triangle_index = 0u; triangle_index < triangle_count; ++triangle_index) {
        XgRenderIrTriangle *triangle = &item.native.triangles[triangle_index];

        triangle->split_index = (uint8_t)triangle_index;
        triangle->split_count = triangle_count;
        for (vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            XgRenderIrVertex *vertex = &triangle->vertices[vertex_index];
            const int32_t whole = first_x + (int32_t)(triangle_index * 10u) +
                                  (int32_t)vertex_index;

            vertex->x = whole * INT32_C(65536);
            vertex->y = (whole + 1) * INT32_C(65536) + INT32_C(32768);
            vertex->u = (int32_t)vertex_index * INT32_C(65536);
            vertex->v = (int32_t)(triangle_index + vertex_index) *
                        INT32_C(65536);
            vertex->r = (uint8_t)(20u + vertex_index);
            vertex->g = (uint8_t)(40u + triangle_index);
            vertex->b = 60u;
        }
    }
    return item;
}

static int build_interleaved_ir(XgRenderIr **out_ir,
                                uint64_t vram_serial,
                                bool invalid_last_material) {
    XgRenderIr *ir = NULL;
    XgRenderIrCompatibilityItem first_compatibility;
    XgRenderIrCompatibilityItem second_compatibility;
    XgRenderIrNativeItem first_native;
    XgRenderIrNativeItem second_native;

    memset(&first_compatibility, 0, sizeof(first_compatibility));
    memset(&second_compatibility, 0, sizeof(second_compatibility));
    first_compatibility.base = make_base(0u, 4u, false);
    first_native = make_native(1u, 4u, 1u, 10);
    second_compatibility.base = make_base(2u, 4u, false);
    second_native = make_native(3u, 4u, 2u, 30);
    if (invalid_last_material) {
        second_native.native.material.draw_area_left = 200u;
        second_native.native.material.draw_area_right = 100u;
    }

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_begin(ir, fixture_state(), vram_serial) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_append_compatibility(ir, &first_compatibility) ==
          XG_RENDER_IR_OK);
    CHECK(xg_render_ir_append_native(ir, &first_native) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_append_compatibility(ir, &second_compatibility) ==
          XG_RENDER_IR_OK);
    CHECK(xg_render_ir_append_native(ir, &second_native) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_finalize(ir, fixture_state(), vram_serial) ==
          XG_RENDER_IR_OK);
    *out_ir = ir;
    return 0;
}

static int build_compatibility_only_ir(XgRenderIr **out_ir,
                                       uint64_t vram_serial) {
    XgRenderIr *ir = NULL;
    XgRenderIrCompatibilityItem item;

    memset(&item, 0, sizeof(item));
    item.base = make_base(0u, 1u, false);
    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_begin(ir, fixture_state(), vram_serial) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_append_compatibility(ir, &item) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_finalize(ir, fixture_state(), vram_serial) ==
          XG_RENDER_IR_OK);
    *out_ir = ir;
    return 0;
}

static GpuRenderPresent make_present(void) {
    GpuRenderPresent present;

    memset(&present, 0, sizeof(present));
    present.path = GPU_RENDER_PRESENT_HIRES;
    present.display_width = 640;
    present.display_height = 480;
    present.surface_width = 640u;
    present.surface_height = 480u;
    present.scale = 2u;
    return present;
}

static XgRenderBackendSubmitInfo make_submit_info(
        uint64_t current_serial,
        const GpuRenderPresent *present,
        XgRenderBackendCompatibilityCallback callback,
        void *callback_context) {
    XgRenderBackendSubmitInfo info;

    memset(&info, 0, sizeof(info));
    info.current_vram_mutation_serial = current_serial;
    info.present = present;
    info.compatibility_callback = callback;
    info.compatibility_user_data = callback_context;
    return info;
}

static int test_public_primitive_translation_uses_submit_validation(void) {
    XgRenderIrNativeItem item = make_native(0u, 1u, 2u, 12);
    GpuRenderSemantic semantic;

    memset(&semantic, 0xff, sizeof(semantic));
    CHECK(xg_render_backend_translate_primitive(&item.native, &semantic) ==
          XG_RENDER_BACKEND_OK);
    CHECK(semantic.triangle_count == 2u);
    CHECK(semantic.triangles[0].vertices[0].x == 12 * INT32_C(65536));
    CHECK(semantic.material.texture_depth == GPU_RENDER_TEXTURE_15_BIT);
    CHECK(semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE);
    item.native.material.tpage = UINT16_C(0x009b);
    item.native.material.texture_page_x = 11u;
    item.native.material.texture_page_y = 1u;
    item.native.material.clut_x = 0u;
    item.native.material.clut_y = UINT16_C(0x00e3);
    item.native.material.texture_depth = XG_RENDER_IR_TEXTURE_8_BIT;
    CHECK(xg_render_backend_translate_primitive(&item.native, &semantic) ==
          XG_RENDER_BACKEND_OK);
    CHECK(semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE);
    for (uint8_t triangle = 0u; triangle < item.native.triangle_count;
         ++triangle) {
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            item.native.triangles[triangle].vertices[vertex].native_view_position = true;
        }
    }
    CHECK(xg_render_backend_translate_primitive(&item.native, &semantic) ==
          XG_RENDER_BACKEND_OK);
    CHECK(semantic.screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE);
    for (uint8_t triangle = 0u; triangle < item.native.triangle_count;
         ++triangle) {
        for (uint8_t vertex = 0u; vertex < 3u; ++vertex) {
            XgRenderIrVertex *position =
                &item.native.triangles[triangle].vertices[vertex];
            position->native_view_position = true;
            position->native_view_x = position->x + 53 * INT32_C(65536);
            position->native_view_y = position->y;
        }
    }
    CHECK(xg_render_backend_translate_primitive(&item.native, &semantic) ==
          XG_RENDER_BACKEND_OK);
    CHECK(semantic.triangles[0].vertices[0].native_view_position == 1u);
    CHECK(semantic.triangles[0].vertices[0].native_view_x ==
          65 * INT32_C(65536));

    item.native.material.draw_area_left = 320u;
    CHECK(xg_render_backend_translate_primitive(&item.native, &semantic) ==
          XG_RENDER_BACKEND_INVALID_MATERIAL);
    item = make_native(0u, 1u, 1u, 12);
    item.native.material.texture_depth = (XgRenderIrTextureDepth)99;
    CHECK(xg_render_backend_translate_primitive(&item.native, &semantic) ==
          XG_RENDER_BACKEND_UNSUPPORTED_MATERIAL);
    CHECK(xg_render_backend_translate_primitive(NULL, &semantic) ==
          XG_RENDER_BACKEND_INVALID_ARGUMENT);
    CHECK(xg_render_backend_translate_primitive(&item.native, NULL) ==
          XG_RENDER_BACKEND_INVALID_ARGUMENT);
    return 0;
}

static int test_interleaved_order_and_translation(void) {
    const uint64_t serial = 77u;
    const GpuRenderPresent present = make_present();
    CompatibilityContext callback_context = { false, 0u };
    XgRenderBackendSubmitInfo submit_info;
    XgRenderBackendResult result;
    XgRenderIr *ir = NULL;
    static const Event expected[] = {
        { EVENT_BEGIN, 77u },
        { EVENT_BARRIER, 1u },
        { EVENT_COMPATIBILITY, 0u },
        { EVENT_BARRIER, 2u },
        { EVENT_DRAW, 10u },
        { EVENT_BARRIER, 3u },
        { EVENT_COMPATIBILITY, 2u },
        { EVENT_BARRIER, 4u },
        { EVENT_DRAW, 30u },
        { EVENT_BARRIER, 5u },
        { EVENT_COMMIT, 77u },
    };

    CHECK(build_interleaved_ir(&ir, serial, false) == 0);
    submit_info = make_submit_info(serial, &present, compatibility_submit,
                                   &callback_context);
    reset_facade();
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_OK);
    CHECK(result.fallback_reason == XG_RENDER_BACKEND_FALLBACK_NONE);
    CHECK(result.transaction_status == GPU_RENDER_TRANSACTION_READY);
    CHECK(result.items_preflighted == 4u);
    CHECK(result.compatibility_items_submitted == 2u);
    CHECK(result.native_items_submitted == 2u);
    CHECK(result.semantic_primitives_submitted == 2u);
    CHECK(result.failed_final_ordinal == XG_RENDER_BACKEND_NO_FINAL_ORDINAL);
    CHECK(result.transaction_began);
    CHECK(!result.rollback_attempted);
    CHECK(event_count == sizeof(expected) / sizeof(expected[0]));
    CHECK(memcmp(events, expected, sizeof(expected)) == 0);
    CHECK(observed_begin_id.scene_epoch == fixture_state().scene_epoch);
    CHECK(observed_begin_id.state_sequence == fixture_state().state_sequence);
    CHECK(observed_present == &present);
    CHECK(submitted_semantic_count == 2u);
    CHECK(submitted_semantics[0].triangle_count == 1u);
    CHECK(submitted_semantics[1].triangle_count == 2u);
    CHECK(submitted_semantics[0].triangles[0].vertices[0].x ==
          10 * INT32_C(65536));
    CHECK(submitted_semantics[0].triangles[0].vertices[0].y ==
          11 * INT32_C(65536) + INT32_C(32768));
    CHECK(submitted_semantics[1].triangles[1].vertices[0].x ==
          40 * INT32_C(65536));
    CHECK(submitted_semantics[0].material.tpage == 0x102u);
    CHECK(submitted_semantics[0].material.texture_depth ==
          GPU_RENDER_TEXTURE_15_BIT);
    CHECK(submitted_semantics[0].material.dither == 1u);
    return 0;
}

static int test_preflight_rejects_before_begin(void) {
    const uint64_t serial = 78u;
    const GpuRenderPresent present = make_present();
    CompatibilityContext callback_context = { false, 0u };
    XgRenderBackendSubmitInfo submit_info;
    XgRenderBackendResult result;
    XgRenderIr *ir = NULL;

    CHECK(build_interleaved_ir(&ir, serial, false) == 0);
    submit_info = make_submit_info(serial, &present, NULL, NULL);
    reset_facade();
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_MISSING_COMPATIBILITY_CALLBACK);
    CHECK(begin_call_count == 0u && draw_call_count == 0u && event_count == 0u);

    CHECK(build_interleaved_ir(&ir, serial, true) == 0);
    submit_info = make_submit_info(serial, &present, compatibility_submit,
                                   &callback_context);
    reset_facade();
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_INVALID_MATERIAL);
    CHECK(result.fallback_reason == XG_RENDER_BACKEND_FALLBACK_MATERIAL);
    CHECK(result.failed_final_ordinal == 3u);
    CHECK(result.items_preflighted == 3u);
    CHECK(begin_call_count == 0u && draw_call_count == 0u && event_count == 0u);

    CHECK(build_compatibility_only_ir(&ir, serial) == 0);
    reset_facade();
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_MISSING_NATIVE_ITEMS);
    CHECK(result.fallback_reason ==
          XG_RENDER_BACKEND_FALLBACK_MISSING_NATIVE_ITEMS);
    CHECK(begin_call_count == 0u && draw_call_count == 0u && event_count == 0u);
    return 0;
}

static int test_stale_serial_rejects_before_begin(void) {
    const uint64_t serial = 79u;
    const GpuRenderPresent present = make_present();
    CompatibilityContext callback_context = { false, 0u };
    XgRenderBackendSubmitInfo submit_info;
    XgRenderBackendResult result;
    XgRenderIr *ir = NULL;

    CHECK(build_interleaved_ir(&ir, serial, false) == 0);
    submit_info = make_submit_info(serial + 1u, &present,
                                   compatibility_submit, &callback_context);
    reset_facade();
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_STALE_VRAM_SERIAL);
    CHECK(result.fallback_reason ==
          XG_RENDER_BACKEND_FALLBACK_STALE_VRAM_SERIAL);
    CHECK(begin_call_count == 0u && draw_call_count == 0u);
    CHECK(rollback_call_count == 0u && event_count == 0u);
    return 0;
}

static int test_post_begin_failures_rollback(void) {
    const uint64_t serial = 80u;
    const GpuRenderPresent present = make_present();
    CompatibilityContext callback_context = { false, 0u };
    XgRenderBackendSubmitInfo submit_info;
    XgRenderBackendResult result;
    XgRenderIr *ir = NULL;

    CHECK(build_interleaved_ir(&ir, serial, false) == 0);
    submit_info = make_submit_info(serial, &present, compatibility_submit,
                                   &callback_context);

    reset_facade();
    failure_mode = FAIL_BEGIN;
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_TRANSACTION_BEGIN_FAILED);
    CHECK(result.transaction_status == GPU_RENDER_TRANSACTION_BACKEND_ERROR);
    CHECK(!result.transaction_began && !result.rollback_attempted);
    CHECK(rollback_call_count == 0u);

    reset_facade();
    failure_mode = FAIL_BARRIER;
    failure_value = 3u;
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_ORDERING_BARRIER_FAILED);
    CHECK(result.transaction_status == GPU_RENDER_TRANSACTION_CONTEXT_LOST);
    CHECK(result.failed_final_ordinal == 2u);
    CHECK(result.transaction_began && result.rollback_attempted);
    CHECK(rollback_call_count == 1u);
    CHECK(events[event_count - 1u].kind == EVENT_ROLLBACK);
    CHECK(observed_rollback_id.scene_epoch == fixture_state().scene_epoch);
    CHECK(observed_rollback_id.state_sequence == fixture_state().state_sequence);

    reset_facade();
    callback_context.fail = true;
    callback_context.fail_ordinal = 2u;
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_COMPATIBILITY_FAILED);
    CHECK(result.failed_final_ordinal == 2u);
    CHECK(rollback_call_count == 1u && result.rollback_attempted);
    callback_context.fail = false;

    reset_facade();
    failure_mode = FAIL_DRAW;
    failure_value = 2u;
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_NATIVE_DRAW_FAILED);
    CHECK(result.failed_final_ordinal == 3u);
    CHECK(result.semantic_primitives_submitted == 1u);
    CHECK(result.native_items_submitted == 1u);
    CHECK(rollback_call_count == 1u && result.rollback_attempted);

    reset_facade();
    failure_mode = FAIL_BARRIER;
    failure_value = 5u;
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_ORDERING_BARRIER_FAILED);
    CHECK(result.failed_final_ordinal == XG_RENDER_BACKEND_NO_FINAL_ORDINAL);
    CHECK(rollback_call_count == 1u && result.rollback_attempted);

    reset_facade();
    failure_mode = FAIL_COMMIT;
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_COMMIT_FAILED);
    CHECK(result.transaction_status ==
          GPU_RENDER_TRANSACTION_VALIDATION_FAILED);
    CHECK(result.failed_final_ordinal == XG_RENDER_BACKEND_NO_FINAL_ORDINAL);
    CHECK(result.semantic_primitives_submitted == 2u);
    CHECK(rollback_call_count == 1u && result.rollback_attempted);
    CHECK(events[event_count - 1u].kind == EVENT_ROLLBACK);

    reset_facade();
    failure_mode = FAIL_ROLLBACK;
    callback_context.fail = true;
    callback_context.fail_ordinal = 0u;
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_ROLLBACK_FAILED);
    CHECK(result.transaction_status == GPU_RENDER_TRANSACTION_CONTEXT_LOST);
    CHECK(result.rollback_attempted && !result.rollback_succeeded);
    CHECK(rollback_call_count == 1u);
    callback_context.fail = false;
    return 0;
}

static int test_argument_and_finalization_failures(void) {
    const GpuRenderPresent present = make_present();
    XgRenderBackendSubmitInfo submit_info = make_submit_info(
        1u, &present, compatibility_submit, NULL);
    XgRenderBackendResult result;
    XgRenderIr *ir = NULL;

    reset_facade();
    result = xg_render_backend_submit(NULL, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_INVALID_ARGUMENT);
    CHECK(result.fallback_reason == XG_RENDER_BACKEND_FALLBACK_INVALID_REQUEST);
    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    result = xg_render_backend_submit(ir, &submit_info);
    CHECK(result.status == XG_RENDER_BACKEND_IR_NOT_FINALIZED);
    CHECK(begin_call_count == 0u && rollback_call_count == 0u);
    return 0;
}

int main(void) {
    int result;

    result = test_public_primitive_translation_uses_submit_validation();
    if (result != 0) return result;
    result = test_interleaved_order_and_translation();
    if (result != 0) return result;
    result = test_preflight_rejects_before_begin();
    if (result != 0) return result;
    result = test_stale_serial_rejects_before_begin();
    if (result != 0) return result;
    result = test_post_begin_failures_rollback();
    if (result != 0) return result;
    result = test_argument_and_finalization_failures();
    if (result != 0) return result;
    return 0;
}
