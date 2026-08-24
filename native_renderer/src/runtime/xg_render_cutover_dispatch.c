#include "xg_render_cutover_dispatch.h"

#include "psx_xg_render_auth_hook_types.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_overlay_cutovers_generated.h"
#include "xg_render_runtime_variant_auth.h"

#include <stddef.h>

typedef struct XgRenderCutoverRoute {
    uint32_t pc;
    uint32_t instruction;
    XgRenderCutoverAction action;
} XgRenderCutoverRoute;

#define ROUTE_COUNT(routes) (sizeof(routes) / sizeof((routes)[0]))

static const XgRenderCutoverRoute f4_routes[] = {
    { UINT32_C(0x801c6f70), UINT32_C(0x27bdffb8),
      XG_CUTOVER_F4_FIXED_2A },
    { UINT32_C(0x800b393c), UINT32_C(0x3c108006),
      XG_CUTOVER_F4_BATTLE_FADER },
    { UINT32_C(0x801cf550), UINT32_C(0x0c0129cf),
      XG_CUTOVER_F4_PROJECTED_2A },
    { UINT32_C(0x80043b48), UINT32_C(0x3c0600ff),
      XG_CUTOVER_F4_OBSERVE_2A_OT },
};

static const XgRenderCutoverRoute overlay_add_prim_routes[] = {
    { UINT32_C(0x80043b48), UINT32_C(0x3c0600ff), XG_CUTOVER_ROUTE_ONLY },
};

static const XgRenderCutoverRoute resident_line_routes[] = {
    { UINT32_C(0x8007fbe0), UINT32_C(0x00a04021), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80073b64), UINT32_C(0x3c03800d), XG_CUTOVER_ROUTE_ONLY },
};

static const XgRenderCutoverRoute residual_routes[] = {
    { UINT32_C(0x80045ed0), UINT32_C(0x95020000),
      XG_CUTOVER_RESIDUAL_CLEAR_TILE },
    { UINT32_C(0x80079784), UINT32_C(0x27bdffe8),
      XG_CUTOVER_RESIDUAL_FULLSCREEN_TILE },
    { UINT32_C(0x8007da44), UINT32_C(0x27bdff88),
      XG_CUTOVER_RESIDUAL_FADE_TILES },
};

static const XgRenderCutoverRoute overlay_routes[] = {
    { UINT32_C(0x801ce23c), UINT32_C(0x0c0129cf), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x801d09b0), UINT32_C(0x0c0129cf), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x801d0bdc), UINT32_C(0x0c0129cf), XG_CUTOVER_ROUTE_ONLY },
};

static const XgRenderCutoverRoute model_pre_field_routes[] = {
    { UINT32_C(0x8002d100), UINT32_C(0x3c048006),
      XG_CUTOVER_MODEL_FT4_TEMPLATE },
    { UINT32_C(0x8002da00), UINT32_C(0x8fbf0014),
      XG_CUTOVER_MODEL_FT3_TEMPLATE },
    { UINT32_C(0x8002e1c8), UINT32_C(0x480d3800),
      XG_CUTOVER_MODEL_FT3_LINK_CAPTURE },
    { UINT32_C(0x8002e0fc), UINT32_C(0x4a280030),
      XG_CUTOVER_MODEL_FT3_LINK_FINISH },
    { UINT32_C(0x8002e404), UINT32_C(0x26520001),
      XG_CUTOVER_MODEL_FT4_GUEST_AVERAGE },
    { UINT32_C(0x8002e5f0), UINT32_C(0x02794021),
      XG_CUTOVER_MODEL_FT3_GUEST },
    { UINT32_C(0x8002e880), UINT32_C(0x15400002),
      XG_CUTOVER_MODEL_FT4_GUEST_FARTHEST },
    { UINT32_C(0x8001e298), UINT32_C(0x27bdffe0),
      XG_CUTOVER_SPRITE_WRAPPER_BEGIN },
};

static const XgRenderCutoverRoute field_sprite_routes[] = {
    { UINT32_C(0x8002675c), UINT32_C(0x27bdffb0), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800269cc), UINT32_C(0x8fa90020), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x801c9984), UINT32_C(0xa4620008), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x801c9b80), UINT32_C(0x02801021), XG_CUTOVER_ROUTE_ONLY },
};

static const XgRenderCutoverRoute model_post_field_routes[] = {
    { UINT32_C(0x8001e3d8), UINT32_C(0x27bdff68),
      XG_CUTOVER_SPRITE_BEGIN },
    { UINT32_C(0x8001e874), UINT32_C(0x92680006),
      XG_CUTOVER_SPRITE_GEOMETRY },
    { UINT32_C(0x8001e8d8), UINT32_C(0x8e82003c),
      XG_CUTOVER_SPRITE_MATERIAL },
    { UINT32_C(0x8001e2c0), UINT32_C(0x8e02003c),
      XG_CUTOVER_SPRITE_WRAPPER_END },
    { UINT32_C(0x8001e988), UINT32_C(0x8fbf0094),
      XG_CUTOVER_SPRITE_END },
    { UINT32_C(0x8002c700), UINT32_C(0x27bdffd0),
      XG_CUTOVER_MODEL_BEGIN },
    { UINT32_C(0x8002e268), UINT32_C(0x34190008),
      XG_CUTOVER_MODEL_FT4_SEAM },
    { UINT32_C(0x8002e688), UINT32_C(0x34190008),
      XG_CUTOVER_MODEL_FT4_SEAM },
    { UINT32_C(0x8002e484), UINT32_C(0x34190008),
      XG_CUTOVER_MODEL_FT3_SEAM },
    { UINT32_C(0x8002c86c), UINT32_C(0x86230002),
      XG_CUTOVER_MODEL_FINISH },
    { UINT32_C(0x800257dc), UINT32_C(0x8fbf0044),
      XG_CUTOVER_MODEL_END },
};

static const XgRenderCutoverRoute projected_routes[] = {
    { UINT32_C(0x8002709c), UINT32_C(0x27bdff60),
      XG_CUTOVER_PROJECTED_INITIALIZER_BEGIN },
    { UINT32_C(0x80027390), UINT32_C(0x8fbf009c),
      XG_CUTOVER_PROJECTED_INITIALIZER_COMMIT },
    { UINT32_C(0x800273c4), UINT32_C(0x27bdff80),
      XG_CUTOVER_PROJECTED_NATIVE },
};

static const XgRenderCutoverRoute world_routes[] = {
    { UINT32_C(0x8002c8cc), UINT32_C(0x27bdffd0), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8002cb4c), UINT32_C(0x03e00008), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8003f968), UINT32_C(0x1080000a), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8003f994), UINT32_C(0x03e00008), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8002caa4), UINT32_C(0x3c028006), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8004a1ac), UINT32_C(0xe8b60000), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8004a1e8), UINT32_C(0xe9360000), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8004a274), UINT32_C(0xe8d60000), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8004a2b8), UINT32_C(0xe9560000), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80085cdc), UINT32_C(0x3c02800a), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80085f38), UINT32_C(0x8fbf0020), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8001e2b4), UINT32_C(0x02002021), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8001e2e0), UINT32_C(0x8fbf0018), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800983a0), UINT32_C(0x27bdff90), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800747dc), UINT32_C(0x3c02800a), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8008615c), UINT32_C(0x27bdffd8), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800848f4), UINT32_C(0x24020800), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80084cd0), UINT32_C(0x8fbf0038), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80086798), UINT32_C(0x3c02800a), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800876dc), UINT32_C(0x8fbf004c), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8007412c), UINT32_C(0x266400b8), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80073b04), UINT32_C(0x27bdffc0), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80089c78), UINT32_C(0x27bdffb0), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800737ec), UINT32_C(0x27bdffb8), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8009932c), UINT32_C(0x27bdffc8), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800996d4), UINT32_C(0x8fbf0034), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80074e24), UINT32_C(0x8fbf0064), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80074c84), UINT32_C(0x8ecc0000), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80099bfc), UINT32_C(0x27bdfff8), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80099e78), UINT32_C(0x8fb10008), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800863bc), UINT32_C(0x8fbf0024), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80086e3c), UINT32_C(0x8e2202d4), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x800740b8), UINT32_C(0x27bdffc8), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80074564), UINT32_C(0x8fbf0030), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x8008a294), UINT32_C(0x8fbf004c), XG_CUTOVER_ROUTE_ONLY },
    { UINT32_C(0x80073e0c), UINT32_C(0x8fbf003c), XG_CUTOVER_ROUTE_ONLY },
};

static const uint32_t post_cutover_pcs[] = {
    UINT32_C(0x8004a1ac),
    UINT32_C(0x8004a1e8),
    UINT32_C(0x8004a274),
    UINT32_C(0x8004a2b8),
};

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static const XgRenderCutoverRoute *route_lookup(
        const XgRenderCutoverRoute *routes, size_t count,
        uint32_t pc, uint32_t instruction_word) {
    for (size_t index = 0u; index < count; ++index) {
        if (physical_address_equals(pc, routes[index].pc) &&
            instruction_word == routes[index].instruction)
            return &routes[index];
    }
    return NULL;
}

typedef struct XgRenderCutoverModuleRoute {
    XgRenderCutoverModule module;
    const XgRenderCutoverRoute *routes;
    size_t route_count;
    bool generated_overlay_routes;
    bool runtime_variant_routes;
    bool runtime_source_routes;
} XgRenderCutoverModuleRoute;

static const XgRenderCutoverModuleRoute modules[] = {
    { XG_RENDER_CUTOVER_MODULE_PREAMBLE, NULL, 0u, false, false, false },
    { XG_RENDER_CUTOVER_MODULE_F4, f4_routes, ROUTE_COUNT(f4_routes),
      false, false, false },
    { XG_RENDER_CUTOVER_MODULE_OVERLAY_ADD_PRIM, overlay_add_prim_routes,
      ROUTE_COUNT(overlay_add_prim_routes), false, false, false },
    { XG_RENDER_CUTOVER_MODULE_RESIDENT_LINE, resident_line_routes,
      ROUTE_COUNT(resident_line_routes), false, false, false },
    { XG_RENDER_CUTOVER_MODULE_RESIDUAL, residual_routes,
      ROUTE_COUNT(residual_routes),
      false, false, false },
    { XG_RENDER_CUTOVER_MODULE_OVERLAY, overlay_routes,
      ROUTE_COUNT(overlay_routes),
      true, false, false },
    { XG_RENDER_CUTOVER_MODULE_WORLD, world_routes, ROUTE_COUNT(world_routes),
      true, false, false },
    { XG_RENDER_CUTOVER_MODULE_MODEL_PRE_FIELD, model_pre_field_routes,
      ROUTE_COUNT(model_pre_field_routes), false, false, false },
    { XG_RENDER_CUTOVER_MODULE_FIELD_SPRITE, field_sprite_routes,
      ROUTE_COUNT(field_sprite_routes), true, false, false },
    { XG_RENDER_CUTOVER_MODULE_OVERLAY_FIELD_MATERIAL,
      NULL, 0u, true, false, false },
    { XG_RENDER_CUTOVER_MODULE_MODEL_POST_FIELD, model_post_field_routes,
      ROUTE_COUNT(model_post_field_routes), false, false, false },
    { XG_RENDER_CUTOVER_MODULE_PROJECTED, projected_routes,
      ROUTE_COUNT(projected_routes),
      false, false, false },
    { XG_RENDER_CUTOVER_MODULE_VARIANT, NULL, 0u, false, true, false },
    { XG_RENDER_CUTOVER_MODULE_FIELD_CHARACTER,
      NULL, 0u, false, false, true },
};

static bool module_route_lookup(
        const XgRenderCutoverModuleRoute *module,
        uint32_t pc, uint32_t instruction_word,
        XgRenderCutoverRouteDescriptor *out_descriptor) {
    PsxXgRenderSourceSiteMetadata source_metadata;
    XgRenderRuntimeVariantCutover cutover;
    const XgRenderCutoverRoute *route;

    if (module == NULL || out_descriptor == NULL) return false;
    *out_descriptor = (XgRenderCutoverRouteDescriptor){
        .module = module->module,
        .pc = pc,
        .instruction_word = instruction_word,
    };
    if (module->module == XG_RENDER_CUTOVER_MODULE_PREAMBLE) return true;
    route = route_lookup(module->routes, module->route_count,
                         pc, instruction_word);
    if (route != NULL) {
        out_descriptor->action = route->action;
        return true;
    }
    if (module->generated_overlay_routes &&
        xg_render_overlay_cutover_relevant(pc, instruction_word))
        return true;
    if (module->runtime_variant_routes &&
        xg_render_runtime_variant_native_cutover_contract_lookup(
            pc, instruction_word, &cutover)) {
        out_descriptor->action = cutover.handler;
        out_descriptor->continuation = cutover.continuation;
        return true;
    }
    return module->runtime_source_routes &&
        xg_render_runtime_variant_source_site_lookup(
            pc, instruction_word, &source_metadata);
}

XgRenderCutoverDispatchResult xg_render_cutover_dispatch(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        const XgRenderCutoverDispatchContext *context) {
    XgRenderCutoverRouteDescriptor route;
    XgRenderCutoverDispatchResult result;

    if (context == NULL || context->observe_route == NULL)
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;

    for (size_t index = 0u; index < ROUTE_COUNT(modules); ++index) {
        if (!module_route_lookup(
                &modules[index], pc, instruction_word, &route))
            continue;
        result = context->observe_route(cpu, &route);
        if (result != XG_RENDER_CUTOVER_DISPATCH_CONTINUE) return result;
    }
    return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
}

bool xg_render_cutover_dispatch_pc_relevant(uint32_t pc) {
    if (xg_render_overlay_cutover_pc_relevant(pc) ||
        xg_render_runtime_variant_native_dispatch_pc_relevant(pc))
        return true;
    for (size_t module_index = 0u;
         module_index < ROUTE_COUNT(modules); ++module_index)
        for (size_t route_index = 0u;
             route_index < modules[module_index].route_count; ++route_index)
            if (physical_address_equals(
                    pc, modules[module_index].routes[route_index].pc))
                return true;
    return false;
}

bool xg_render_cutover_dispatch_post_pc_relevant(uint32_t pc) {
    for (size_t index = 0u; index < ROUTE_COUNT(post_cutover_pcs); ++index)
        if (physical_address_equals(pc, post_cutover_pcs[index])) return true;
    return false;
}

bool xg_render_cutover_dispatch_hook_route_lookup(
        uint32_t hook_type, uint32_t pc, uint32_t instruction_word,
        XgRenderHookRouteDescriptor *out_descriptor) {
    XgRenderHookRouteKind kind = XG_RENDER_HOOK_ROUTE_NONE;

    if (hook_type == PSX_XG_RENDER_AUTH_HOOK_PRODUCER_ENTRY &&
        physical_address_equals(
            pc, xg_render_manifest_validation.producer_entry) &&
        instruction_word == UINT32_C(0x27bdff18)) {
        kind = XG_RENDER_HOOK_ROUTE_CANONICAL_ENTRY;
    } else if (hook_type == PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION &&
               physical_address_equals(
                   pc, xg_render_manifest_validation.caller_site) &&
               instruction_word == UINT32_C(0x0c012d53)) {
        kind = XG_RENDER_HOOK_ROUTE_CANONICAL_CAPTURE;
    } else if (hook_type == PSX_XG_RENDER_AUTH_HOOK_CONTINUATION &&
               physical_address_equals(
                   pc, xg_render_manifest_validation.return_site)) {
        kind = XG_RENDER_HOOK_ROUTE_CANONICAL_RETURN;
    } else if (hook_type ==
                   PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION &&
               (pc == UINT32_C(0x800758c8) ||
                pc == UINT32_C(0x800759cc)) &&
               instruction_word == UINT32_C(0x0c0112f4)) {
        kind = XG_RENDER_HOOK_ROUTE_UI_DRAW_OT;
    }
    if (kind == XG_RENDER_HOOK_ROUTE_NONE || out_descriptor == NULL)
        return false;
    *out_descriptor = (XgRenderHookRouteDescriptor){
        .kind = kind,
        .hook_type = hook_type,
        .pc = pc,
        .instruction_word = instruction_word,
    };
    return true;
}

uint32_t xg_render_cutover_dispatch_exact_route_count(void) {
    uint32_t count = 0u;

    for (size_t index = 0u; index < ROUTE_COUNT(modules); ++index)
        count += (uint32_t)modules[index].route_count;
    return count;
}

bool xg_render_cutover_dispatch_exact_route_at(
        uint32_t index, uint32_t *out_pc, uint32_t *out_instruction_word) {
    for (size_t module_index = 0u;
         module_index < ROUTE_COUNT(modules); ++module_index) {
        const XgRenderCutoverModuleRoute *module = &modules[module_index];

        if (index < module->route_count) {
            if (out_pc != NULL) *out_pc = module->routes[index].pc;
            if (out_instruction_word != NULL)
                *out_instruction_word = module->routes[index].instruction;
            return out_pc != NULL && out_instruction_word != NULL;
        }
        index -= (uint32_t)module->route_count;
    }
    return false;
}
