#ifndef XG_RENDER_DEPTH_POLICY_H
#define XG_RENDER_DEPTH_POLICY_H

#include "gpu_render.h"
#include "xg_render_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every Native producer family, classified once for the optional host depth
 * test (gl_renderer_set_native_depth_test). The PS1 has no depth buffer: the
 * game sorts into ordering-table buckets and relies on that order for
 * shadows, decals, water and effects drawn coplanar or slightly above. The
 * depth plane therefore only arbitrates between certified opaque 3D surfaces.
 *
 *   TEST_WRITE  certified opaque 3D geometry with a real view Z: tests and
 *               writes depth (semi-transparent draws of it never write).
 *   TEST        tests only, never occludes by depth. Kept for classification;
 *               every 3D family currently writes (sprites, shadows, props and
 *               effects included), relying on per-family test bias and on
 *               blended texels never writing.
 *   NONE        pure OT order: 2D, UI, fonts, screen-space quads, prerendered
 *               backgrounds (fake depth), sky/clouds/horizon, HUD.
 *
 * The raster also applies the draw's FINAL material and geometry: a
 * semi-transparent draw never writes, and a triangle without view depth on
 * all three vertices, a screen-space 2D draw or a line behaves as NONE. The
 * stamp is host presentation state only; guest packets, OT and counters are
 * unchanged. The table below is the single classification. */
typedef enum XgRenderDepthFamily {
    XG_RENDER_DEPTH_FAMILY_WORLD_MODELS = 0,
    XG_RENDER_DEPTH_FAMILY_FIELD_MODELS,          /* FT3/FT4 model packets */
    XG_RENDER_DEPTH_FAMILY_BATTLE_GEOMETRY,
    XG_RENDER_DEPTH_FAMILY_WORLD_TERRAIN_WATER,
    XG_RENDER_DEPTH_FAMILY_WORLD_DECORATIONS,
    XG_RENDER_DEPTH_FAMILY_WORLD_ENTITY_SHADOWS,
    XG_RENDER_DEPTH_FAMILY_WORLD_EFFECTS,
    XG_RENDER_DEPTH_FAMILY_FIELD_CHARACTER_SHADOW,
    XG_RENDER_DEPTH_FAMILY_FIELD_PARTICLES,
    XG_RENDER_DEPTH_FAMILY_BATTLE_FX,
    XG_RENDER_DEPTH_FAMILY_WORLD_ACTOR_SPRITES,   /* world billboards (body) */
    XG_RENDER_DEPTH_FAMILY_WORLD_ACTOR_SHADOW,    /* world billboard shadows */
    XG_RENDER_DEPTH_FAMILY_SPRITES,               /* field/battle FT4 billboards */
    XG_RENDER_DEPTH_FAMILY_FIELD_RESIDUAL,        /* resident gouraud quads */
    XG_RENDER_DEPTH_FAMILY_FIELD_F4,              /* resident POLY_F4 sources */
    XG_RENDER_DEPTH_FAMILY_FIELD_OVERLAY_PROJECTED, /* overlay FT4, projected families */
    XG_RENDER_DEPTH_FAMILY_WORLD_CLOUDS,
    XG_RENDER_DEPTH_FAMILY_WORLD_HORIZON,
    XG_RENDER_DEPTH_FAMILY_WORLD_SKY,
    XG_RENDER_DEPTH_FAMILY_WORLD_MINIMAP,
    XG_RENDER_DEPTH_FAMILY_FIELD_PROJECTED,       /* prerendered panoramas */
    XG_RENDER_DEPTH_FAMILY_FIELD_ZOOM,
    XG_RENDER_DEPTH_FAMILY_FIELD_COMPASS,
    XG_RENDER_DEPTH_FAMILY_FIELD_LINES,
    XG_RENDER_DEPTH_FAMILY_RESIDENT_UI,           /* menus, text, UI OT */
    XG_RENDER_DEPTH_FAMILY_COUNT
} XgRenderDepthFamily;

/* bias: test bias toward the camera as a shift (the tested key D gains
 * D >> bias, i.e. z is taken ~2^-bias nearer; 0 = none). It lets a surface
 * resting on a certified one (a shadow or billboard on the ground, a prop on
 * terrain) pass where the two meet, without changing any written key. */
typedef struct XgRenderDepthPolicyEntry {
    XgRenderDepthFamily family;
    GpuRenderDepthPolicy policy;
    uint8_t bias;
    const char *name;
    const char *reason;
} XgRenderDepthPolicyEntry;

/* The single classification of every Native producer family. Order matches
 * XgRenderDepthFamily; the policy meanings are documented above. A family
 * whose draw has no view depth on all vertices (2D quads of mixed producers
 * such as residual/F4) behaves as NONE for that draw. */
static const XgRenderDepthPolicyEntry xg_render_depth_policy_table[XG_RENDER_DEPTH_FAMILY_COUNT] = {
    {XG_RENDER_DEPTH_FAMILY_WORLD_MODELS, GPU_RENDER_DEPTH_TEST_WRITE, 0u, "world_models",
     "opaque 3D meshes with unfloored view Z (native transform / motion poses)"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_MODELS, GPU_RENDER_DEPTH_TEST_WRITE, 0u, "field_models",
     "FT3/FT4 model packets projected by RTPS with unfloored view Z"},
    {XG_RENDER_DEPTH_FAMILY_BATTLE_GEOMETRY, GPU_RENDER_DEPTH_TEST_WRITE, 0u, "battle_geometry",
     "resident battle model dispatcher (RTPS per shared vertex)"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_TERRAIN_WATER, GPU_RENDER_DEPTH_TEST_WRITE, 0u, "world_terrain_water",
     "ground occludes models behind hills; decals on it test with a bias"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_DECORATIONS, GPU_RENDER_DEPTH_TEST_WRITE, 6u, "world_decorations",
     "props resting on terrain; test with a bias so their base does not sink"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_ENTITY_SHADOWS, GPU_RENDER_DEPTH_TEST_WRITE, 5u, "world_entity_shadows",
     "ground decal; biased so it does not sink (blended texels never write)"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_EFFECTS, GPU_RENDER_DEPTH_TEST_WRITE, 6u, "world_effects",
     "projective particles; opaque texels write, blended ones never do"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_CHARACTER_SHADOW, GPU_RENDER_DEPTH_TEST_WRITE, 5u, "field_character_shadow",
     "ground decal; biased so it does not sink (blended texels never write)"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_PARTICLES, GPU_RENDER_DEPTH_TEST_WRITE, 6u, "field_particles",
     "projective particles; opaque texels write, blended ones never do"},
    {XG_RENDER_DEPTH_FAMILY_BATTLE_FX, GPU_RENDER_DEPTH_TEST_WRITE, 5u, "battle_fx",
     "ripple on the battle floor; biased, blended texels never write"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_ACTOR_SPRITES, GPU_RENDER_DEPTH_TEST_WRITE, 5u, "world_actor_sprites",
     "billboard card projected per corner; biased so feet stay on the ground"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_ACTOR_SHADOW, GPU_RENDER_DEPTH_TEST_WRITE, 5u, "world_actor_shadow",
     "billboard shadow on the terrain; biased so it does not sink"},
    {XG_RENDER_DEPTH_FAMILY_SPRITES, GPU_RENDER_DEPTH_TEST_WRITE, 5u, "sprites",
     "field/battle character cards projected per corner; biased feet"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_RESIDUAL, GPU_RENDER_DEPTH_TEST_WRITE, 6u, "field_residual",
     "projected gouraud quads; its 2D bars have no depth"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_F4, GPU_RENDER_DEPTH_TEST_WRITE, 6u, "field_f4",
     "projected POLY_F4; faders and fixed quads have no depth"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_OVERLAY_PROJECTED, GPU_RENDER_DEPTH_TEST_WRITE, 6u, "field_overlay_projected",
     "projected overlay FT4 families"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_CLOUDS, GPU_RENDER_DEPTH_NONE, 0u, "world_clouds",
     "additive sky layer composed by OT"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_HORIZON, GPU_RENDER_DEPTH_NONE, 0u, "world_horizon",
     "reprojected wide-view backdrop"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_SKY, GPU_RENDER_DEPTH_NONE, 0u, "world_sky",
     "backdrop gradient"},
    {XG_RENDER_DEPTH_FAMILY_WORLD_MINIMAP, GPU_RENDER_DEPTH_NONE, 0u, "world_minimap",
     "HUD"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_PROJECTED, GPU_RENDER_DEPTH_NONE, 0u, "field_projected",
     "prerendered backgrounds: depth is authored by OT, not geometry"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_ZOOM, GPU_RENDER_DEPTH_NONE, 0u, "field_zoom",
     "screen-space zoom overlay"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_COMPASS, GPU_RENDER_DEPTH_NONE, 0u, "field_compass",
     "HUD"},
    {XG_RENDER_DEPTH_FAMILY_FIELD_LINES, GPU_RENDER_DEPTH_NONE, 0u, "field_lines",
     "line primitives"},
    {XG_RENDER_DEPTH_FAMILY_RESIDENT_UI, GPU_RENDER_DEPTH_NONE, 0u, "resident_ui",
     "menus, fonts and UI"},
};

/* The classification row of one family (NULL for an invalid family). */
static inline const XgRenderDepthPolicyEntry *xg_render_depth_policy_entry(XgRenderDepthFamily family) {
    if ((unsigned)family >= XG_RENDER_DEPTH_FAMILY_COUNT) return NULL;
    return &xg_render_depth_policy_table[family];
}

static inline GpuRenderDepthPolicy xg_render_depth_policy(XgRenderDepthFamily family) {
    const XgRenderDepthPolicyEntry *entry = xg_render_depth_policy_entry(family);
    return entry ? entry->policy : GPU_RENDER_DEPTH_NONE;
}

/* Stamp a producer's draw. Both forms are no-ops for NULL. */
static inline uint8_t xg_render_depth_policy_bias(XgRenderDepthFamily family) {
    const XgRenderDepthPolicyEntry *entry = xg_render_depth_policy_entry(family);
    return entry ? entry->bias : 0u;
}

static inline void xg_render_depth_policy_stamp_semantic(GpuRenderSemantic *semantic,
                                                         XgRenderDepthFamily family) {
    if (!semantic) return;
    semantic->depth_policy = (uint8_t)xg_render_depth_policy(family);
    semantic->depth_bias = xg_render_depth_policy_bias(family);
}

static inline void xg_render_depth_policy_stamp_primitive(XgRenderIrNativePrimitive *primitive,
                                                          XgRenderDepthFamily family) {
    if (!primitive) return;
    primitive->depth_policy = (uint8_t)xg_render_depth_policy(family);
    primitive->depth_bias = xg_render_depth_policy_bias(family);
}

#ifdef __cplusplus
}
#endif

#endif
