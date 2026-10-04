#include "xg_render_battling.h"
#include "xg_render_battling_draw_distance.h"

#include "cpu_state.h"
#include "gte_native_provenance.h"
#include "mod_memory.h"
#include "psx_gte_divide.h"
#include "psx_render_nclip.h"
#include "xg_field_render_services.h"
#include "xg_host_3d.h"
#include "xg_render_depth_policy.h"
#include "xg_render_model_kernel.h"
#include "xg_render_motion.h"
#include "xg_render_native_mesh.h"
#include "xg_render_native_work.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_producer_lifecycle.h"
#include "xg_render_submission.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BATTLING_OVERLAY_TAG_ADDRESS 0x8006faf0u
#define BATTLING_OVERLAY_TAG 7u
#define BATTLING_ENTITY_TAG UINT64_C(0x424c000000000000)   /* "BL" */
#define BATTLING_CAMERA_ID UINT64_C(0x424c43414d455241)    /* "BLCAMERA" */
/* World-space poses own their cameras: each differs by its world origin. */
#define BATTLING_TERRAIN_CAMERA_ID UINT64_C(0x424c54455252414e) /* "BLTERRAN" */
#define BATTLING_RING_CAMERA_ID UINT64_C(0x424c52494e474341)    /* "BLRINGCA" */
#define BATTLING_POOL_INDEX 0x800928a0u                     /* double-buffer byte */
#define ADD_PRIM_CALL 0x0c010ed2u                           /* jal 0x80043b48 */

static volatile XgRenderModelKernelStats battling_model_diagnostics;
static volatile struct {
    uint64_t terrain_calls, terrain_rejected, terrain_polygons, terrain_motion;
    uint64_t ring_calls, ring_rejected, ring_quads;
    uint64_t shadow_calls, shadow_rejected, shadow_quads;
    uint64_t tap_frames, effect_draws, effect_bound, effect_unmatched;
    uint64_t effect_invalid, rect_centered;
    uint64_t far_cells, far_triangles, far_meshes, far_mesh_failures, far_ring_segments;
    uint64_t far_clipped, near_suppressed, far_phase_only;
} battling_diagnostics;

static bool battling_resident(const CPUState *cpu) {
    return cpu && cpu->read_word &&
        cpu->read_word(BATTLING_OVERLAY_TAG_ADDRESS) == BATTLING_OVERLAY_TAG;
}

/* ---- Gear parts --------------------------------------------------------- */

/* BattlingRenderModelPacket (8008a63c) is only called by
 * BattlingRenderModelHierarchy, whose hierarchy node stays in s1 across the
 * SubmitModelPacket call: a stable identity per part across frames. */
bool xg_render_battling_model_capture(
        const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle) {
    if (!cpu || !xg_render_battling_model_authorizes_call(cpu->gpr[31] - 8u)) {
        ++battling_model_diagnostics.rejected;
        return false;
    }
    const XgRenderModelKernelCall call = {
        .entity_tag = BATTLING_ENTITY_TAG,
        .camera_id = BATTLING_CAMERA_ID,
        .source_pc = cpu->gpr[31] - 8u,
        .identity = cpu->gpr[17] & 0x1fffffffu,
        .depth_family = XG_RENDER_DEPTH_FAMILY_BATTLING_MODELS,
    };
    return xg_render_model_kernel_capture(cpu, lifecycle, &call,
                                          &battling_model_diagnostics);
}

/* ---- Effect projection tap ---------------------------------------------- */

/* Every Battling function that projects effect geometry with RTPS/RTPT, and
 * the resident ProjectRotated* helpers only its particle types call. Terrain,
 * ring, shadows and Gears have their own producers and stay out. */
static const uint32_t battling_effect_ranges[][2] = {
    {0x8007bba0u, 0x8007c100u}, /* BattlingRenderWorldEffectQuads */
    {0x8007c280u, 0x8007c880u}, /* BattlingRenderLinkedEffectRibbons */
    {0x8007caa4u, 0x8007cd14u}, /* BattlingLinkedSegmentTrailsRender */
    {0x8007d918u, 0x8007db28u}, /* BattlingDotParticlesUpdateAndRender */
    {0x8007e020u, 0x8007e24cu}, /* BattlingTileParticlesUpdateAndRender */
    {0x8007e3ccu, 0x8007e528u}, /* BattlingLatchedLineSegmentsRender */
    {0x80087ea0u, 0x8008820cu}, /* BattlingRenderQueuedEffectGeometry */
    {0x8008832cu, 0x800884e0u}, /* BattlingEffectPrimitiveUpdateAndRender */
    {0x8008c4b0u, 0x8008cf9cu}, /* ground-projected model shadows, particles 0-4 */
    {0x8008de54u, 0x8008df30u}, /* BattlingTransformParticlePrimitive */
    {0x8004a64cu, 0x8004a6dcu}, /* ProjectRotatedVertex / Triangle */
    {0x8004a73cu, 0x8004a7bcu}, /* ProjectRotatedQuadrilateral */
};
static bool battling_tap_open;

static void battling_tap_frame(void) {
    if (!battling_tap_open) {
        gte_native_projection_tap_configure(
            battling_effect_ranges,
            (uint32_t)(sizeof(battling_effect_ranges) / sizeof(battling_effect_ranges[0])));
        battling_tap_open = true;
    }
    /* Lookups accept this frame and the previous one, so effects projected
     * before the terrain walk in the same frame stay valid. */
    gte_native_projection_tap_advance();
    ++battling_diagnostics.tap_frames;
}

void xg_render_battling_leave(void) {
    if (!battling_tap_open) return;
    gte_native_projection_tap_configure(NULL, 0u);
    battling_tap_open = false;
}

/* The status HUD (portraits, names, gauges, health bars and their frame
 * lines) is screen space; it is never a projected effect. Textured parts
 * keep both buffers at 0x80095698..; the polygon and line bank is double
 * buffered 0x310 apart (0x8009a2f8.. and 0x8009a608..). */
bool xg_render_battling_hud_packet(uint32_t command_address) {
    const uint32_t address = command_address & 0x1fffffffu;
    return (address >= 0x00095680u && address < 0x00095940u) ||
           (address >= 0x0009a2f0u && address < 0x0009a930u);
}

static uint32_t packed_sxy(const GpuRenderSemanticVertex *vertex) {
    return ((uint32_t)(uint16_t)(vertex->x >> 16)) |
           ((uint32_t)(uint16_t)(vertex->y >> 16) << 16);
}

static void apply_tapped_vertex(GpuRenderSemanticVertex *target,
                                const GteNativeVertexProvenance *source,
                                int32_t offset_x, int32_t offset_y) {
    const int64_t margin = (int64_t)xg_host_3d_native_view_margin() * 65536;
    target->native_view_x = (int32_t)((int64_t)source->x_16_16 + margin + offset_x);
    target->native_view_y = source->y_16_16 + offset_y;
    target->native_view_position = 1u;
    if (offset_x || offset_y) return;
    /* The projected point itself: depth and the projective transport. */
    target->native_view_depth = source->view_z > 0 ? source->view_z * 4096 : 0;
    target->projective_view_x = source->view_x;
    target->projective_view_y = source->view_y;
    target->projective_view_z = source->view_z;
    target->projective_offset_x = source->projection_offset_x_16_16;
    target->projective_offset_y = source->projection_offset_y_16_16;
    target->projective_native_offset_x = (int32_t)margin;
    target->projective_native_offset_y = 0;
    target->projective_distance = source->projection_distance;
    target->projective_position = source->projective_valid;
}

bool xg_render_battling_bind_projected(GpuRenderSemantic *semantic) {
    GteNativeVertexProvenance vertex;
    if (!semantic || !battling_tap_open ||
        semantic->submission_command_id > UINT32_C(0x001ffffc) ||
        xg_render_battling_hud_packet((uint32_t)semantic->submission_command_id))
        return false;
    const bool lines = semantic->topology == GPU_RENDER_SEMANTIC_LINES;
    const uint32_t count = lines ? semantic->line_count : semantic->triangle_count;
    const unsigned corners = lines ? 2u : 3u;
    if (!count) return false;
    for (uint32_t i = 0; i < count; ++i)
        for (unsigned v = 0; v < corners; ++v) {
            const GpuRenderSemanticVertex *item = lines
                ? &semantic->lines[i].vertices[v] : &semantic->triangles[i].vertices[v];
            if (item->native_view_position) return false;
        }
    ++battling_diagnostics.effect_draws;
    GpuRenderSemantic bound = *semantic;
    if (!lines && semantic->screen_space_2d != GPU_RENDER_SCREEN_SPACE_2D_NONE) {
        /* DOT/TILE/SPRT: the projected point is the rectangle's top-left
         * word; keep the authored pixel size around its native position. */
        int32_t left = INT32_MAX, top = INT32_MAX;
        for (uint32_t t = 0; t < count; ++t)
            for (unsigned v = 0; v < 3u; ++v) {
                const GpuRenderSemanticVertex *item = &semantic->triangles[t].vertices[v];
                if (item->x < left) left = item->x;
                if (item->y < top) top = item->y;
            }
        const GpuRenderSemanticVertex anchor = {.x = left, .y = top};
        const bool found = gte_native_projection_tap_lookup(packed_sxy(&anchor), &vertex);
        if (!found || !vertex.projective_valid) {
            int32_t right = INT32_MIN;
            for (uint32_t t = 0; t < count; ++t)
                for (unsigned v = 0; v < 3u; ++v)
                    if (semantic->triangles[t].vertices[v].x > right)
                        right = semantic->triangles[t].vertices[v].x;
            if (found) ++battling_diagnostics.effect_invalid;
            else ++battling_diagnostics.effect_unmatched;
            /* A display-width rectangle is a tint or fade and must cover the
             * wide view. Any other Battling rectangle is a particle or panel:
             * keep its authored size in the centred 4:3 plane, where a point
             * inside the 4:3 frame projects in the native view anyway. */
            if ((right - left) / 65536 >= 320) return false;
            semantic->screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_NONE;
            ++battling_diagnostics.rect_centered;
            return true;
        }
        for (uint32_t t = 0; t < count; ++t)
            for (unsigned v = 0; v < 3u; ++v) {
                GpuRenderSemanticVertex *item = &bound.triangles[t].vertices[v];
                apply_tapped_vertex(item, &vertex, item->x - left, item->y - top);
            }
        bound.screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_NONE;
    } else {
        for (uint32_t i = 0; i < count; ++i)
            for (unsigned v = 0; v < corners; ++v) {
                GpuRenderSemanticVertex *item = lines
                    ? &bound.lines[i].vertices[v] : &bound.triangles[i].vertices[v];
                if (!gte_native_projection_tap_lookup(packed_sxy(item), &vertex)) {
                    ++battling_diagnostics.effect_unmatched;
                    return false;
                }
                if (!vertex.projective_valid) {
                    ++battling_diagnostics.effect_invalid;
                    return false;
                }
                apply_tapped_vertex(item, &vertex, 0, 0);
            }
        if (!lines)
            xg_render_depth_policy_stamp_semantic(&bound, XG_RENDER_DEPTH_FAMILY_BATTLING_EFFECTS);
    }
    *semantic = bound;
    ++battling_diagnostics.effect_bound;
    return true;
}

/* ---- Arena heightfield -------------------------------------------------- */

/* BattlingRenderVisibleTerrainStrips (0x80072d18) walks the 128x128 arena
 * heightfield. Row r draws the visible span [start, end) the guest left in
 * scratchpad (end at 0x1f800000 + r, start at 0x1f800080 + r). A cell
 * projects V0..V2 with one RTPT and keeps (V0, V1, V2) when NCLIP > 0, then
 * (V0, V3, V1); each kept triangle takes the next 0x20-byte POLY_FT3 of the
 * double-buffered pool. Replay that walk on the host, without guest cycles,
 * so every packet the guest will emit is bound to native geometry and to one
 * rigid pose of the whole field. Colour and UV still come from the packet.
 * The walk feeds the GTE camera-relative cells (col * 256 - a1, row * 256 -
 * a2); the pose binds them about the field's centre instead, so the camera's
 * travel interpolates with the camera rather than stepping with the cells. */
#define TERRAIN_PC 0x80072d18u
#define TERRAIN_CENTRE 0x4000
#define TERRAIN_HEIGHTS 0x800928dcu
#define TERRAIN_POOLS 0x80092854u

/* The NCLIP the walk branches on. In Native render mode the runtime replaces
 * its MAC0 with the sign of the 16.16 screen accumulator (OFX + IR * H/SZ), so
 * a nearly edge-on cell whose integer area rounds to zero still draws. */
static int32_t nclip(const XgHost3dProjectedVertex *a, const XgHost3dProjectedVertex *b,
                     const XgHost3dProjectedVertex *c) {
    const int32_t area =
        (int32_t)a->x * b->y + (int32_t)b->x * c->y + (int32_t)c->x * a->y -
        (int32_t)a->x * c->y - (int32_t)b->x * a->y - (int32_t)c->x * b->y;
    const int32_t x[3] = {a->x_16_16, b->x_16_16, c->x_16_16};
    const int32_t y[3] = {a->y_16_16, b->y_16_16, c->y_16_16};
    return psx_render_nclip(area, x, y);
}

/* Perimeter ring (see BattlingRenderArenaPerimeterRing below). */
#define RING_PC 0x80082e60u
#define RING_LOWER_RETURN 0x8008315cu
#define RING_UPPER_RETURN 0x80083168u
#define RING_CENTRE 0x3f80
#define RING_CAMERA 0x80096fa8u
#define RING_POOLS 0x80092788u     /* 32 x (lower, upper) POLY_FT4 per buffer */
#define RING_TRIG 0x800523f0u      /* rsin/rcos pairs per 4096 angle units */
#define RING_RADIUS 0x3f80

static struct {
    uint64_t scene_generation;
    XgHost3dProjection projection; /* the camera it was published under */
    XgRenderMotionSource source;
    XgRenderMotionRef motion;
    int32_t offset[3];
    uint64_t drawn[4];      /* segment angles the guest linked since the far ring */
    uint32_t rgbc;          /* GTE RGBC under which the guest cues its colour */
    bool valid;
} ring_pose;

/* ---- Draw distance (Mods: xenogears.battling-draw-distance) ------------- */

/* The guest rasterizes only a fan of cells around the camera into the row
 * spans (0x80082178: 24 cells ahead, 10 behind; 0x80082300: 32 ahead), and
 * its two 0x708-triangle pools could hold no more. With an arena in the
 * GPU-DMA aperture the host adds every other cell in view: POLY_FT3 packets
 * built exactly as the walk builds its own (UV, CLUT/tpage, DPCS colour,
 * bucket max SZ >> 4), linked into the same ordering table before the walk
 * links its cells, bound to the walk's rigid pose. Aperture packets cost no
 * guest DMA time; the game's own work and timing are unchanged. */
#define TERRAIN_ROW_ENDS 0x1f800000u
#define TERRAIN_ROW_STARTS 0x1f800080u
#define TERRAIN_UV_TABLE 0x1f800120u      /* 4 orientations x 4 corner UVs */
#define TERRAIN_CLUT_TPAGE 0x1f800140u    /* 4 CLUT | tpage << 16 words */
#define TERRAIN_ROW_FIRST 0x80091834u     /* clamps of 0x80087698 */
#define TERRAIN_ROW_LAST 0x800918b4u
#define TERRAIN_PACKET_BYTES 0x20u
#define RING_PACKET_BYTES 0x28u
/* Arena: per buffer the far terrain's mesh markers (one POLY_FT3 per
 * CLUT/tpage selector); then per buffer one lower/upper POLY_FT4 pair per
 * ring angle. */
#define FAR_TERRAIN_BYTES (4u * TERRAIN_PACKET_BYTES)
#define RING_ARENA_BYTES (256u * 2u * RING_PACKET_BYTES)
#define DRAW_DISTANCE_ARENA_BYTES (2u * FAR_TERRAIN_BYTES + 2u * RING_ARENA_BYTES)

static struct {
    uint32_t arena;
} draw_distance;

void xg_render_battling_set_draw_distance(uint32_t arena, uint32_t size) {
    draw_distance.arena = 0u;
    /* Command identities cover the aperture's first 4 MiB. */
    const uint32_t physical = arena & 0x1fffffffu;
    if (!arena || (arena & 0x1fu) || physical < 0x00800000u ||
        size < DRAW_DISTANCE_ARENA_BYTES || physical + DRAW_DISTANCE_ARENA_BYTES > 0x00c00000u)
        return;
    draw_distance.arena = arena;
}

static uint8_t depth_cue_channel(uint8_t color, int32_t far_color, int16_t ir0) {
    int32_t step = far_color - (int32_t)color * 16;
    step = step < -32768 ? -32768 : step > 32767 ? 32767 : step;
    const int64_t value = (int64_t)color * 16 * 4096 + (int64_t)ir0 * step;
    const int32_t mac = value >= 0 ? (int32_t)(value / 4096)
                                   : (int32_t)(-(((-value) + 4095) / 4096));
    const int32_t c = mac >= 0 ? mac / 16 : -(((-mac) + 15) / 16);
    return (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
}

/* DPCS of the GTE's RGBC towards the far colour by the IR0 RTPT leaves. */
static uint32_t depth_cued_color(const CPUState *cpu, const XgHost3dProjection *projection,
                                 uint32_t rgbc, uint16_t sz) {
    int overflow = 0;
    const int32_t scale = psx_gte_divide(projection->projection_distance, sz, &overflow);
    const int64_t mac0 = (int64_t)projection->depth_cue_a * scale + projection->depth_cue_b;
    const int64_t shifted = mac0 >= 0 ? mac0 >> 12 : -((-mac0 + 4095) >> 12);
    const int32_t wrapped = (int32_t)(uint32_t)(uint64_t)shifted;
    const int16_t ir0 = (int16_t)(wrapped < 0 ? 0 : wrapped > 0x1000 ? 0x1000 : wrapped);
    return (rgbc & 0xff000000u) |
        depth_cue_channel((uint8_t)rgbc, (int32_t)cpu->gte_ctrl[21], ir0) |
        (uint32_t)depth_cue_channel((uint8_t)(rgbc >> 8), (int32_t)cpu->gte_ctrl[22], ir0) << 8 |
        (uint32_t)depth_cue_channel((uint8_t)(rgbc >> 16), (int32_t)cpu->gte_ctrl[23], ir0) << 16;
}

typedef struct {
    XgHost3dVector local, world;
    XgHost3dProjectedVertex screen;
    uint32_t frame;
    bool valid;
} FarGridVertex;
static FarGridVertex *far_grid;
static uint32_t far_frame;

static const FarGridVertex *far_vertex(const CPUState *cpu, const XgHost3dProjection *projection,
                                       uint32_t heights, uint32_t row, uint32_t col,
                                       int32_t camera_x, int32_t camera_z) {
    FarGridVertex *v = &far_grid[row * 128u + col];
    if (v->frame == far_frame) return v->valid ? v : NULL;
    v->frame = far_frame;
    v->valid = false;
    const int32_t x = (int32_t)col * 256 - camera_x, z = (int32_t)row * 256 - camera_z;
    /* The walk's int16 locals wrap; such a vertex is not this grid point. */
    if (x < -32768 || x > 32767 || z < -32768 || z > 32767) return NULL;
    const int16_t y = (int16_t)cpu->read_word(heights + (row * 128u + col) * 4u);
    uint32_t flags;
    v->local = (XgHost3dVector){(int16_t)x, y, (int16_t)z, 0};
    v->world = (XgHost3dVector){(int16_t)((int32_t)col * 256 - TERRAIN_CENTRE), y,
                                (int16_t)((int32_t)row * 256 - TERRAIN_CENTRE), 0};
    if (!xg_host_3d_rtps(projection, &v->local, &v->screen, &flags)) return NULL;
    v->valid = true;
    return v;
}

/* The previous guest frame's camera. Phases interpolate between that view
 * and the current one, so what either view sees stays drawn: a side leaving
 * the view does not vanish before the camera has turned away from it. */
typedef struct {
    XgHost3dProjection projection;
    int32_t camera_x, camera_z;     /* terrain walk offsets (a1, a2) */
    int32_t ring_x, ring_z;         /* ring offsets (camera - RING_CENTRE) */
    bool valid;
} FarView;
static FarView far_previous;

/* Screen position of a camera-relative point under a projection, continuous;
 * false when it is not in front of the projection plane. */
static bool far_screen_point(const XgHost3dProjection *p, double x, double y, double z,
                             double out[2]) {
    double view[3];
    for (unsigned r = 0; r < 3; ++r)
        view[r] = (p->rotation[r][0] * x + p->rotation[r][1] * y + p->rotation[r][2] * z) /
                  4096.0 + p->translation[r];
    if (view[2] * 2.0 <= p->projection_distance) return false;
    out[0] = p->screen_offset_x / 65536.0 + p->projection_distance * view[0] / view[2];
    out[1] = p->screen_offset_y / 65536.0 + p->projection_distance * view[1] / view[2];
    return true;
}

/* Interpolated phases see from cameras between the previous and current
 * ones: near the camera, beside the screen, places neither endpoint saw.
 * Every visibility test therefore keeps half a screen more on each side
 * and above/below than the widened screen; phases clip and cull the rest. */
#define FAR_PHASE_GUARD_X 160
#define FAR_PHASE_GUARD_Y 120

/* Bounding box of points against the (widened) screen. */
static bool far_box_on_screen(const double (*points)[2], unsigned count, int32_t margin) {
    double left = points[0][0], right = left, top = points[0][1], bottom = top;
    for (unsigned k = 1; k < count; ++k) {
        left = fmin(left, points[k][0]); right = fmax(right, points[k][0]);
        top = fmin(top, points[k][1]); bottom = fmax(bottom, points[k][1]);
    }
    return right >= -margin && left < 320 + margin && bottom >= -FAR_PHASE_GUARD_Y &&
           top < 256 + FAR_PHASE_GUARD_Y;
}

/* Conservative view test of a cell before projecting its corners. */
static bool far_cell_in_view(const XgHost3dProjection *p, int32_t x, int32_t y, int32_t z,
                             double half_width, double half_height) {
    double view[3];
    for (unsigned r = 0; r < 3; ++r)
        view[r] = (p->rotation[r][0] * (double)x + p->rotation[r][1] * (double)y +
                   p->rotation[r][2] * (double)z) / 4096.0 + p->translation[r];
    const double radius = 768.0, far_z = view[2] + radius;
    if (far_z <= 0.0) return false;
    return fabs(view[0]) - radius <= half_width * far_z &&
           fabs(view[1]) - radius <= half_height * far_z;
}

/* The far terrain is Native meshes, not a packet per cell. The geometry
 * holds the whole heightfield grid (pose LOCAL points about the centre) and
 * both faces of every cell with their packet UVs; it is rebuilt only when
 * the heights or the UV table change. Each frame, one instance per
 * CLUT/tpage selector lists the faces the walk would keep (visible, in
 * front, NCLIP > 0) under the frame's GTE state, which expands to exactly
 * the triangles it would build. One marker POLY_FT3 per instance in the
 * arena, linked at its farthest face's bucket, places the mesh in the
 * frame's ordering table; its acceptance appends the MESH operation. */
#define FAR_MATERIALS 4u
#define FAR_POINTS (128u * 128u)
#define FAR_FACES (127u * 127u * 2u)
static struct {
    XgSemanticResourceRef ref, retired;
    uint32_t heights;
    uint32_t samples[FAR_POINTS];
    uint16_t uv[16];
    bool valid;
} far_geometry;
static struct {
    uint16_t *faces;
    uint32_t count;
    XgRenderNativeMeshExtra *extras;
    uint32_t extra_count;
    uint16_t depth;
} far_instances[FAR_MATERIALS];

/* Near-plane clipping. The GTE does not clip: a face with a corner at or
 * behind SZ = H/2 projects through the divide's saturation (and SXY's
 * +-1024 clamp), so the walk's own packet is distorted and the far terrain
 * used to drop such faces whole, leaving holes beside a camera close to the
 * ground. Both are replaced by the face clipped in view space just in front
 * of the saturation, as extra triangles of its material's mesh; the walk's
 * packets for those faces are suppressed at GPU acceptance. */
#define FAR_EXTRA_CAPACITY 16384u
static struct {
    uint32_t count;
    uint32_t ids[1024];
} near_suppressed[2];

static bool face_near(const XgHost3dProjection *projection, const XgHost3dProjectedVertex *a,
                      const XgHost3dProjectedVertex *b, const XgHost3dProjectedVertex *c) {
    const uint32_t h = projection->projection_distance;
    return (uint32_t)a->z * 2u <= h || (uint32_t)b->z * 2u <= h || (uint32_t)c->z * 2u <= h;
}

bool xg_render_battling_near_suppressed(uint64_t command_id) {
    if (!draw_distance.arena) return false;
    for (unsigned b = 0; b < 2u; ++b)
        for (uint32_t i = 0; i < near_suppressed[b].count; ++i)
            if (near_suppressed[b].ids[i] == command_id) return true;
    return false;
}
static struct {
    XgSemanticResourceRef instance, geometry;
    XgRenderMotionRef motion;
    uint64_t continuity;
    uint32_t command_id;
} far_markers[2][FAR_MATERIALS];

static const uint8_t far_corners[2][3] = {{0, 1, 2}, {0, 3, 1}};
/* Corner UVs per face (V0,V1,V2)/(V0,V3,V1). */
static const uint8_t far_uv_corner[2][3] = {{0, 3, 1}, {0, 2, 3}};

static uint32_t far_face_index(uint32_t row, uint32_t col, unsigned t) {
    return (row * 127u + col) * 2u + t;
}

/* Current geometry for these heights and UV table, rebuilt on change. */
static bool far_geometry_current(const CPUState *cpu, uint32_t heights, const uint16_t uv[16],
                                 uint64_t owner_generation) {
    static XgRenderNativeMeshPoint points[FAR_POINTS];
    static XgRenderNativeMeshFace faces[FAR_FACES];
    bool changed = !far_geometry.valid || far_geometry.heights != heights ||
                   memcmp(far_geometry.uv, uv, sizeof(far_geometry.uv));
    for (uint32_t i = 0; i < FAR_POINTS; ++i) {
        const uint32_t sample = cpu->read_word(heights + i * 4u);
        changed |= sample != far_geometry.samples[i];
        far_geometry.samples[i] = sample;
    }
    if (!changed) return true;
    far_geometry.valid = false;
    for (uint32_t row = 0; row < 128u; ++row)
        for (uint32_t col = 0; col < 128u; ++col) {
            const uint32_t i = row * 128u + col;
            points[i] = (XgRenderNativeMeshPoint){
                {(int16_t)((int32_t)col * 256 - TERRAIN_CENTRE),
                 (int16_t)far_geometry.samples[i],
                 (int16_t)((int32_t)row * 256 - TERRAIN_CENTRE), 0},
                (row << 9) | col};
        }
    for (uint32_t row = 0; row < 127u; ++row)
        for (uint32_t col = 0; col < 127u; ++col) {
            /* V0 (col,row), V1 (col+1,row+1), V2 (col+1,row), V3 (col,row+1). */
            const uint16_t grid[4] = {
                (uint16_t)(row * 128u + col), (uint16_t)((row + 1u) * 128u + col + 1u),
                (uint16_t)(row * 128u + col + 1u), (uint16_t)((row + 1u) * 128u + col)};
            const uint32_t attributes = far_geometry.samples[row * 128u + col] >> 16;
            const uint32_t atlas = attributes & 0xf0f0u;
            const uint16_t *corner_uv = &uv[(attributes & 3u) * 4u];
            for (unsigned t = 0; t < 2u; ++t) {
                XgRenderNativeMeshFace *face = &faces[far_face_index(row, col, t)];
                for (unsigned k = 0; k < 3u; ++k) {
                    const uint32_t texel = atlas | corner_uv[far_uv_corner[t][k]];
                    face->points[k] = grid[far_corners[t][k]];
                    face->u[k] = (uint8_t)texel;
                    face->v[k] = (uint8_t)(texel >> 8);
                }
            }
        }
    XgSemanticResourceRef ref;
    if (!xg_render_native_mesh_geometry_create(owner_generation, points, FAR_POINTS,
                                               faces, FAR_FACES, &ref))
        return false;
    /* An instance of the previous geometry may still await acceptance in
     * the other buffer; its geometry is released one rebuild later. */
    if (far_geometry.retired.resource_id)
        (void)xg_render_resource_release((XgRenderResourceHandle){
            far_geometry.retired.resource_id, far_geometry.retired.generation});
    far_geometry.retired = far_geometry.ref;
    far_geometry.ref = ref;
    far_geometry.heights = heights;
    memcpy(far_geometry.uv, uv, sizeof(far_geometry.uv));
    far_geometry.valid = true;
    return true;
}

static bool far_instance_add(unsigned material, uint32_t face, uint16_t depth) {
    if (!far_instances[material].faces &&
        !(far_instances[material].faces = malloc(FAR_FACES * sizeof(uint16_t))))
        return false;
    far_instances[material].faces[far_instances[material].count++] = (uint16_t)face;
    if (depth > far_instances[material].depth) far_instances[material].depth = depth;
    return true;
}

/* One face clipped at the near plane into its material's mesh extras: the
 * corners in front of the plane plus the edge crossings, as a fan, each kept
 * when it is front-facing and on either view's screen like any far face. */
static bool far_extra_add(unsigned material, const XgRenderNativeMeshExtra *extra, uint16_t depth) {
    if (!far_instances[material].extras &&
        !(far_instances[material].extras = malloc(FAR_EXTRA_CAPACITY * sizeof(XgRenderNativeMeshExtra))))
        return false;
    if (far_instances[material].extra_count == FAR_EXTRA_CAPACITY) return false;
    far_instances[material].extras[far_instances[material].extra_count++] = *extra;
    if (depth > far_instances[material].depth) far_instances[material].depth = depth;
    return true;
}

/* A face the endpoint does not draw that the previous camera saw (in front
 * of it, facing it, on its screen): kept for the phases between the two
 * views, which move it and clip it at their own near planes. */
static void far_phase_only_face(const FarGridVertex *const corners[3], const uint8_t u[3],
                                const uint8_t v[3], unsigned material, int32_t margin) {
    if (!far_previous.valid) return;
    double previous[3][2];
    for (unsigned k = 0; k < 3u; ++k) {
        const XgHost3dVector *w = &corners[k]->world;
        if (!far_screen_point(&far_previous.projection,
                (double)w->x + TERRAIN_CENTRE - far_previous.camera_x, w->y,
                (double)w->z + TERRAIN_CENTRE - far_previous.camera_z, previous[k])) return;
    }
    const double area = previous[0][0] * previous[1][1] + previous[1][0] * previous[2][1] +
        previous[2][0] * previous[0][1] - previous[0][0] * previous[2][1] -
        previous[1][0] * previous[0][1] - previous[2][0] * previous[1][1];
    if (!(area > 0.0) || !far_box_on_screen(previous, 3u, margin)) return;
    XgRenderNativeMeshExtra extra = {.sz = corners[2]->screen.z, .flags = XG_RENDER_NATIVE_MESH_PHASE_ONLY};
    for (unsigned k = 0; k < 3u; ++k) {
        extra.local[k] = corners[k]->world;
        extra.vertex_ids[k] = ((uint32_t)(corners[k]->world.z + TERRAIN_CENTRE) / 256u) << 9 |
                              ((uint32_t)(corners[k]->world.x + TERRAIN_CENTRE) / 256u);
        extra.u[k] = u[k]; extra.v[k] = v[k];
    }
    if (far_extra_add(material, &extra, 0u)) ++battling_diagnostics.far_phase_only;
}

static uint32_t far_clip_face(const XgHost3dProjection *projection, const FarGridVertex *const corners[3],
                              const uint8_t u[3], const uint8_t v[3], uint16_t color_sz,
                              unsigned material, uint32_t face, int32_t camera_x, int32_t camera_z,
                              int32_t margin) {
    uint32_t added = 0u;
    typedef struct { double world[3], u, v, z; } ClipVertex;
    const double near_z = projection->projection_distance * 0.5 + 8.0;
    ClipVertex in[3], out[4];
    unsigned count = 0;
    for (unsigned k = 0; k < 3u; ++k) {
        const XgHost3dVector *l = &corners[k]->local, *w = &corners[k]->world;
        in[k] = (ClipVertex){{w->x, w->y, w->z}, u[k], v[k],
            (projection->rotation[2][0] * (double)l->x + projection->rotation[2][1] * (double)l->y +
             projection->rotation[2][2] * (double)l->z) / 4096.0 + projection->translation[2]};
    }
    for (unsigned k = 0; k < 3u; ++k) {
        const ClipVertex *a = &in[k], *b = &in[(k + 1u) % 3u];
        if (a->z >= near_z) out[count++] = *a;
        if ((a->z >= near_z) != (b->z >= near_z)) {
            const double t = (near_z - a->z) / (b->z - a->z);
            ClipVertex *x = &out[count++];
            for (unsigned r = 0; r < 3u; ++r) x->world[r] = a->world[r] + t * (b->world[r] - a->world[r]);
            x->u = a->u + t * (b->u - a->u);
            x->v = a->v + t * (b->v - a->v);
            x->z = near_z;
        }
    }
    if (count < 3u) return 0u;
    const int32_t offset_x = TERRAIN_CENTRE - camera_x, offset_z = TERRAIN_CENTRE - camera_z;
    for (unsigned first = 1u; first + 1u < count; ++first) {
        const unsigned fan[3] = {0u, first, first + 1u};
        XgRenderNativeMeshExtra extra = {.sz = color_sz};
        XgHost3dProjectedVertex screen[3];
        int32_t left = INT32_MAX, right = INT32_MIN, top = INT32_MAX, bottom = INT32_MIN;
        uint16_t depth = 0u;
        bool valid = true;
        for (unsigned k = 0; k < 3u && valid; ++k) {
            const ClipVertex *c = &out[fan[k]];
            const int32_t wx = (int32_t)lround(c->world[0]), wy = (int32_t)lround(c->world[1]);
            const int32_t wz = (int32_t)lround(c->world[2]);
            const int32_t lx = wx + offset_x, lz = wz + offset_z;
            valid = wx >= -32768 && wx <= 32767 && wy >= -32768 && wy <= 32767 &&
                wz >= -32768 && wz <= 32767 && lx >= -32768 && lx <= 32767 && lz >= -32768 && lz <= 32767;
            if (!valid) break;
            extra.local[k] = (XgHost3dVector){(int16_t)wx, (int16_t)wy, (int16_t)wz, 0};
            /* Clip vertices get their own identity; corners keep the grid's. */
            extra.vertex_ids[k] = UINT32_C(0x80000000) | (face << 2) | fan[k];
            extra.u[k] = (uint8_t)lround(fmin(255.0, fmax(0.0, c->u)));
            extra.v[k] = (uint8_t)lround(fmin(255.0, fmax(0.0, c->v)));
            const XgHost3dVector local = {(int16_t)lx, (int16_t)wy, (int16_t)lz, 0};
            uint32_t flags;
            valid = xg_host_3d_rtps(projection, &local, &screen[k], &flags);
            if (!valid) break;
            if (screen[k].z > depth) depth = screen[k].z;
            if (screen[k].x < left) left = screen[k].x;
            if (screen[k].x > right) right = screen[k].x;
            if (screen[k].y < top) top = screen[k].y;
            if (screen[k].y > bottom) bottom = screen[k].y;
        }
        if (!valid || nclip(&screen[0], &screen[1], &screen[2]) <= 0) continue;
        bool on_screen = right >= -margin && left < 320 + margin && bottom >= -FAR_PHASE_GUARD_Y &&
            top < 256 + FAR_PHASE_GUARD_Y;
        if (!on_screen && far_previous.valid) {
            double previous[3][2];
            bool front = true;
            for (unsigned k = 0; k < 3u && front; ++k)
                front = far_screen_point(&far_previous.projection,
                    (double)extra.local[k].x + TERRAIN_CENTRE - far_previous.camera_x, extra.local[k].y,
                    (double)extra.local[k].z + TERRAIN_CENTRE - far_previous.camera_z, previous[k]);
            on_screen = front && far_box_on_screen(previous, 3u, margin);
        }
        if (!on_screen || !far_extra_add(material, &extra, depth)) continue;
        ++battling_diagnostics.far_clipped;
        ++added;
    }
    return added;
}

static void battling_far_terrain(const CPUState *cpu, const XgHost3dProjection *projection,
                                 uint32_t heights, uint32_t ot, int32_t camera_x,
                                 int32_t camera_z, const XgRenderMotionRef *motion,
                                 uint64_t continuity, uint64_t scene_generation) {
    if (!draw_distance.arena || !cpu->write_word || !projection->projection_distance)
        return;
    if (!far_grid && !(far_grid = calloc(128u * 128u, sizeof(*far_grid)))) return;
    if (++far_frame == 0u) far_frame = 1u;
    const uint32_t buffer = cpu->read_byte(BATTLING_POOL_INDEX) & 1u;
    const uint32_t base = draw_distance.arena + buffer * FAR_TERRAIN_BYTES;
    const uint64_t owner = scene_generation ? scene_generation : 1u;
    uint16_t uv[16];
    uint32_t clut_tpage[4];
    for (unsigned i = 0; i < 16u; ++i) uv[i] = cpu->read_half(TERRAIN_UV_TABLE + i * 2u);
    for (unsigned i = 0; i < 4u; ++i)
        clut_tpage[i] = cpu->read_word(TERRAIN_CLUT_TPAGE + i * 4u);
    for (unsigned g = 0; g < FAR_MATERIALS; ++g)
        far_instances[g].count = far_instances[g].extra_count = far_instances[g].depth = 0u;
    const bool geometry = far_geometry_current(cpu, heights, uv, owner);
    const double h = projection->projection_distance;
    const int32_t margin = xg_host_3d_native_view_margin() + FAR_PHASE_GUARD_X;
    const double half_width = (160.0 + margin + 16.0) / h,
                 half_height = (136.0 + FAR_PHASE_GUARD_Y) / h;
    for (uint32_t row = 0; row < 127u && geometry; ++row) {
        const uint32_t first = cpu->read_byte(TERRAIN_ROW_FIRST + row);
        uint32_t last = cpu->read_byte(TERRAIN_ROW_LAST + row);
        if (last > 126u) last = 126u;
        const uint32_t end = cpu->read_byte(TERRAIN_ROW_ENDS + row);
        const uint32_t start = cpu->read_byte(TERRAIN_ROW_STARTS + row);
        const bool spanned = end && start != 0xffu && start < end;
        for (uint32_t col = first; col <= last; ++col) {
            /* The walk draws its spans' cells; here only their near faces. */
            const bool walk_cell = spanned && col >= start && col < end;
            const uint32_t word = far_geometry.samples[row * 128u + col];
            if (!far_cell_in_view(projection, (int32_t)col * 256 + 128 - camera_x,
                                  (int16_t)word, (int32_t)row * 256 + 128 - camera_z,
                                  half_width, half_height) &&
                (!far_previous.valid ||
                 !far_cell_in_view(&far_previous.projection,
                                   (int32_t)col * 256 + 128 - far_previous.camera_x,
                                   (int16_t)word,
                                   (int32_t)row * 256 + 128 - far_previous.camera_z,
                                   half_width, half_height)))
                continue;
            ++battling_diagnostics.far_cells;
            /* V0 (col,row), V1 (col+1,row+1), V2 (col+1,row), V3 (col,row+1). */
            const FarGridVertex *v[4] = {
                far_vertex(cpu, projection, heights, row, col, camera_x, camera_z),
                far_vertex(cpu, projection, heights, row + 1u, col + 1u, camera_x, camera_z),
                far_vertex(cpu, projection, heights, row, col + 1u, camera_x, camera_z),
                far_vertex(cpu, projection, heights, row + 1u, col, camera_x, camera_z),
            };
            if (!v[0] || !v[1] || !v[2] || !v[3]) continue;
            const unsigned material = ((word >> 16) & 0xcu) >> 2;
            const uint32_t attributes = word >> 16;
            for (unsigned t = 0; t < 2u; ++t) {
                const uint8_t *c = far_corners[t];
                if (face_near(projection, &v[c[0]]->screen, &v[c[1]]->screen, &v[c[2]]->screen)) {
                    uint8_t u[3], tv[3];
                    for (unsigned k = 0; k < 3u; ++k) {
                        const uint32_t texel = (attributes & 0xf0f0u) |
                            uv[(attributes & 3u) * 4u + far_uv_corner[t][k]];
                        u[k] = (uint8_t)texel; tv[k] = (uint8_t)(texel >> 8);
                    }
                    const FarGridVertex *corners[3] = {v[c[0]], v[c[1]], v[c[2]]};
                    if (!far_clip_face(projection, corners, u, tv, v[c[2]]->screen.z, material,
                                       far_face_index(row, col, t), camera_x, camera_z, margin))
                        far_phase_only_face(corners, u, tv, material, margin);
                    continue;
                }
                const bool front = nclip(&v[c[0]]->screen, &v[c[1]]->screen, &v[c[2]]->screen) > 0;
                if (walk_cell && front) continue; /* the walk draws it */
                int32_t left = INT32_MAX, right = INT32_MIN, top = INT32_MAX, bottom = INT32_MIN;
                uint16_t depth = 0u;
                for (unsigned k = 0; k < 3u; ++k) {
                    const XgHost3dProjectedVertex *p = &v[c[k]]->screen;
                    if (p->z > depth) depth = p->z;
                    if (p->x < left) left = p->x;
                    if (p->x > right) right = p->x;
                    if (p->y < top) top = p->y;
                    if (p->y > bottom) bottom = p->y;
                }
                bool on_screen = right >= -margin && left < 320 + margin &&
                    bottom >= -FAR_PHASE_GUARD_Y && top < 256 + FAR_PHASE_GUARD_Y;
                if (!on_screen && far_previous.valid) {
                    double previous[3][2];
                    bool front = true;
                    for (unsigned k = 0; k < 3u && front; ++k) {
                        const XgHost3dVector *w = &v[c[k]]->world;
                        front = far_screen_point(&far_previous.projection,
                            (double)w->x + TERRAIN_CENTRE - far_previous.camera_x, w->y,
                            (double)w->z + TERRAIN_CENTRE - far_previous.camera_z,
                            previous[k]);
                    }
                    on_screen = front && far_box_on_screen(previous, 3u, margin);
                }
                if (walk_cell || !on_screen || right - left > 1023 || bottom - top > 511 || !front) {
                    /* Not drawn at this endpoint: perhaps between the views. */
                    uint8_t u[3], tv[3];
                    for (unsigned k = 0; k < 3u; ++k) {
                        const uint32_t texel = (attributes & 0xf0f0u) |
                            uv[(attributes & 3u) * 4u + far_uv_corner[t][k]];
                        u[k] = (uint8_t)texel; tv[k] = (uint8_t)(texel >> 8);
                    }
                    const FarGridVertex *corners[3] = {v[c[0]], v[c[1]], v[c[2]]};
                    far_phase_only_face(corners, u, tv, material, margin);
                    continue;
                }
                if (far_instance_add(material, far_face_index(row, col, t), depth))
                    ++battling_diagnostics.far_triangles;
            }
        }
    }
    for (unsigned g = 0; g < FAR_MATERIALS; ++g) {
        __typeof__(far_markers[0][0]) *marker = &far_markers[buffer][g];
        /* The previous use of this buffer's marker is long consumed; the
         * commit that took the instance holds its own retain. */
        if (marker->instance.resource_id)
            (void)xg_render_resource_release((XgRenderResourceHandle){
                marker->instance.resource_id, marker->instance.generation});
        *marker = (__typeof__(*marker)){0};
        if (!geometry || !(far_instances[g].count + far_instances[g].extra_count)) continue;
        XgRenderNativeMeshInstance header = {
            .face_count = far_instances[g].count, .extra_count = far_instances[g].extra_count,
            .geometry = far_geometry.ref,
            .projection = *projection,
            .local_offset = {TERRAIN_CENTRE - camera_x, 0, TERRAIN_CENTRE - camera_z},
            .rgbc = cpu->gte_data[6],
            .far_color = {(int32_t)cpu->gte_ctrl[21], (int32_t)cpu->gte_ctrl[22],
                          (int32_t)cpu->gte_ctrl[23]},
        };
        XgSemanticResourceRef instance;
        if (!xg_render_native_mesh_instance_create(owner, &header, far_instances[g].faces,
                                                   far_instances[g].extras, &instance)) {
            ++battling_diagnostics.far_mesh_failures;
            continue;
        }
        /* A one-pixel POLY_FT3 under this mesh's CLUT/tpage: it carries the
         * draw environment and OT position; acceptance turns it into the
         * MESH operation, so it never draws pixels of its own. */
        const uint32_t packet = base + g * TERRAIN_PACKET_BYTES;
        const uint32_t entry = ot + (uint32_t)(far_instances[g].depth >> 4) * 4u;
        const uint32_t words[8] = {
            cpu->read_word(entry) | 0x07000000u, (cpu->gte_data[6] & 0xff000000u) | 0x808080u,
            0u, clut_tpage[g] & 0xffff0000u, 1u, clut_tpage[g] << 16, 1u << 16, 0u,
        };
        uint8_t *host = psx_mod_gpu_dma_host(packet, sizeof(words));
        if (!host) {
            (void)xg_render_resource_release((XgRenderResourceHandle){instance.resource_id,
                                                                     instance.generation});
            continue;
        }
        memcpy(host, words, sizeof(words));
        cpu->write_word(entry, packet & 0x00ffffffu);
        *marker = (__typeof__(*marker)){
            .instance = instance, .geometry = far_geometry.ref,
            .motion = motion ? *motion : (XgRenderMotionRef){0},
            .continuity = continuity, .command_id = (packet & 0x1fffffffu) + 4u};
        ++battling_diagnostics.far_meshes;
    }
}

bool xg_render_battling_accept_mesh_marker(const GpuRenderSemantic *packet,
                                           uint64_t guest_cycle) {
    if (!packet || !draw_distance.arena) return false;
    const uint64_t id = packet->submission_command_id;
    for (unsigned b = 0; b < 2u; ++b)
        for (unsigned g = 0; g < FAR_MATERIALS; ++g) {
            const __typeof__(far_markers[0][0]) *marker = &far_markers[b][g];
            if (!marker->command_id || marker->command_id != id) continue;
            GpuRenderSemantic material = *packet;
            xg_render_depth_policy_stamp_semantic(&material,
                                                  XG_RENDER_DEPTH_FAMILY_BATTLING_ARENA);
            XgRenderMotionDrawBinding binding = {0};
            if (marker->motion.handle.resource_id) {
                binding.motion = marker->motion;
                xg_render_semantic_set_interpolation_identity(
                    &material, marker->continuity, TERRAIN_PC, UINT32_C(0x80000000) | g);
            }
            if (!xg_render_native_work_mesh(&material, &binding, marker->instance,
                                            marker->geometry, guest_cycle))
                ++battling_diagnostics.far_mesh_failures;
            /* A marker never draws itself, accepted or not. */
            return true;
        }
    return false;
}

static void battling_far_ring(const CPUState *cpu, const XgHost3dProjection *projection,
                              uint32_t ot);

bool xg_render_battling_terrain_capture(
        const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle) {
    XgHost3dProjection projection = {0};
    XgRenderMotionSource motion_source = {0};
    XgRenderMotionRef motion = {0};
    ++battling_diagnostics.terrain_calls;
    /* Battling's resident overlay tag and the walk's own RTPT words. */
    if (!battling_resident(cpu) || !cpu->read_byte || !lifecycle ||
        !lifecycle->guest_data_range_is_valid ||
        !xg_render_submission_native_work_mode() ||
        cpu->read_word(TERRAIN_PC) != 0xafb0fffcu ||
        cpu->read_word(0x80072e68u) != 0x4a280030u ||
        cpu->read_word(0x80072f38u) != 0x4a280030u)
        goto reject;
    battling_tap_frame();
    const uint32_t heights = cpu->read_word(TERRAIN_HEIGHTS);
    uint32_t packet = cpu->read_word(TERRAIN_POOLS + cpu->read_byte(BATTLING_POOL_INDEX) * 4u);
    if (!lifecycle->guest_data_range_is_valid(heights, 0x10000u, 4u, false))
        goto reject;
    xg_render_runtime_capture_shadow_projection(cpu, &projection);
    const uint32_t identity = TERRAIN_PC;
    const int32_t camera_x = (int32_t)cpu->gpr[5], camera_z = (int32_t)cpu->gpr[6];
    const XgRenderRigidPoseRequest request = {
        .entity_tag = BATTLING_ENTITY_TAG, .camera_id = BATTLING_TERRAIN_CAMERA_ID,
        .source_pc = TERRAIN_PC, .identity = identity,
        .key = {TERRAIN_PC, heights, 128u, 128u},
        .watch = {{heights, 0x8000u}, {heights + 0x8000u, 0x8000u}},
        .world_space = true,
        .local_offset = {camera_x - TERRAIN_CENTRE, 0, camera_z - TERRAIN_CENTRE},
    };
    if (xg_render_rigid_pose_publish(&request, &projection, &motion_source, &motion))
        ++battling_diagnostics.terrain_motion;
    const bool posed = motion.handle.resource_id != 0u;
    const uint32_t pool_buffer = cpu->read_byte(BATTLING_POOL_INDEX) & 1u;
    near_suppressed[pool_buffer].count = 0u;
    for (uint32_t row = 0; row < 127u; ++row) {
        const uint32_t end = cpu->read_byte(0x1f800000u + row);
        const uint32_t start = cpu->read_byte(0x1f800080u + row);
        if (!end || start == 0xffu || start >= end) continue;
        const int16_t z = (int16_t)(row * 256u - (uint32_t)camera_z);
        int32_t x = (int32_t)(start * 256u) - camera_x;
        for (uint32_t col = start; col < end; ++col, x += 256) {
            const uint32_t sample = heights + (row * 128u + col) * 4u;
            /* V0 (col,row), V1 (col+1,row+1), V2 (col+1,row), V3 (col,row+1). */
            const XgHost3dVector local[4] = {
                {(int16_t)x, (int16_t)cpu->read_word(sample), z, 0},
                {(int16_t)(x + 256), (int16_t)cpu->read_word(sample + 0x204u),
                 (int16_t)(z + 256), 0},
                {(int16_t)(x + 256), (int16_t)cpu->read_word(sample + 4u), z, 0},
                {(int16_t)x, (int16_t)cpu->read_word(sample + 0x200u),
                 (int16_t)(z + 256), 0},
            };
            /* The same corners about the centre; |cell - centre| <= 0x4000. */
            const int16_t wx = (int16_t)(col * 256u - TERRAIN_CENTRE);
            const int16_t wz = (int16_t)(row * 256u - TERRAIN_CENTRE);
            const XgHost3dVector world[4] = {
                {wx, local[0].y, wz, 0}, {(int16_t)(wx + 256), local[1].y, (int16_t)(wz + 256), 0},
                {(int16_t)(wx + 256), local[2].y, wz, 0}, {wx, local[3].y, (int16_t)(wz + 256), 0},
            };
            /* A cell the guest's int16 locals wrapped is not this world cell. */
            const bool cell_posed = posed &&
                (int32_t)local[0].x == (int32_t)col * 256 - camera_x &&
                (int32_t)local[0].z == (int32_t)row * 256 - camera_z &&
                (int32_t)local[1].x == (int32_t)col * 256 + 256 - camera_x &&
                (int32_t)local[1].z == (int32_t)row * 256 + 256 - camera_z;
            const uint32_t grid[4] = {
                (row << 9) | col, ((row + 1u) << 9) | (col + 1u),
                (row << 9) | (col + 1u), ((row + 1u) << 9) | col,
            };
            XgHost3dProjectedVertex screen[4];
            uint32_t flags;
            for (unsigned v = 0; v < 4u; ++v)
                if (!xg_host_3d_rtps(&projection, &local[v], &screen[v], &flags))
                    goto reject;
            static const uint8_t corners[2][3] = {{0, 1, 2}, {0, 3, 1}};
            for (unsigned t = 0; t < 2u; ++t) {
                const uint8_t *c = corners[t];
                if (nclip(&screen[c[0]], &screen[c[1]], &screen[c[2]]) <= 0)
                    continue;
                if (!lifecycle->guest_data_range_is_valid(packet, 0x20u, 4u, false))
                    goto reject;
                const uint32_t command_id = (packet & 0x1fffffffu) + 4u;
                const uint32_t primitive_id = (row << 10) | (col << 1) | t;
                GpuRenderSemantic semantic = {0};
                if (cell_posed)
                    xg_render_semantic_set_interpolation_identity(
                        &semantic, motion_source.continuity_generation, identity,
                        primitive_id);
                semantic.material.textured = 1;
                semantic.material.shading = GPU_RENDER_SHADING_FLAT;
                semantic.triangle_count = 1;
                semantic.triangles[0].split_index = 0;
                semantic.triangles[0].split_count = 1;
                XgRenderMotionDrawBinding binding = {.motion = motion, .triangle_count = 1,
                                                     .native_exact = 1u};
                for (unsigned v = 0; v < 3u; ++v) {
                    GpuRenderSemanticVertex *target = &semantic.triangles[0].vertices[v];
                    xg_render_projected_vertex_semantic(target, &screen[c[v]]);
                    if (cell_posed) {
                        target->interpolation_group_id = identity;
                        target->interpolation_vertex_id = grid[c[v]];
                        target->interpolation_vertex_identity_valid = 1u;
                    }
                    binding.local[0][v] = world[c[v]];
                    binding.vertex_ids[0][v] = grid[c[v]];
                }
                xg_render_depth_policy_stamp_semantic(&semantic,
                                                      XG_RENDER_DEPTH_FAMILY_BATTLING_ARENA);
                if (xg_render_submission_stage_exact((GpuRenderTransactionId){0},
                        command_id, &semantic) != GUEST_RENDER_TRANSACTION_OK)
                    goto reject;
                (void)xg_render_motion_register_command(command_id,
                                                        cell_posed ? &binding : NULL,
                                                        cell_posed ? identity : 0u,
                                                        cell_posed ? primitive_id : 0u);
                /* The draw distance replaces a near face with its clipped
                 * triangles: its own packet draws nothing. */
                if (draw_distance.arena && face_near(&projection, &screen[c[0]], &screen[c[1]], &screen[c[2]]) &&
                    near_suppressed[pool_buffer].count < 1024u) {
                    near_suppressed[pool_buffer].ids[near_suppressed[pool_buffer].count++] = command_id;
                    ++battling_diagnostics.near_suppressed;
                }
                ++battling_diagnostics.terrain_polygons;
                packet += 0x20u;
            }
        }
    }
    battling_far_terrain(cpu, &projection, heights, cpu->gpr[4], camera_x, camera_z,
                         posed ? &motion : NULL, motion_source.continuity_generation,
                         motion_source.scene_generation);
    battling_far_ring(cpu, &projection, cpu->gpr[4]);
    far_previous = (FarView){
        .projection = projection, .camera_x = camera_x, .camera_z = camera_z,
        .ring_x = (int16_t)cpu->read_half(RING_CAMERA) - RING_CENTRE,
        .ring_z = (int16_t)cpu->read_half(RING_CAMERA + 8u) - RING_CENTRE,
        .valid = true,
    };
    return true;
reject:
    ++battling_diagnostics.terrain_rejected;
    return false;
}

/* ---- Perimeter ring ----------------------------------------------------- */

/* BattlingRenderArenaPerimeterRing (0x80082e60) draws 32 wall segments
 * around the arena. Each keeps A,B at height 0, C,D at -0x290 and E,F at
 * -0x520 in its frame at sp+0x20..0x4c (A,C,E at the previous angle, B,D,F at
 * the new one, in s3) and links two POLY_FT4 with AddPrim: the lower band
 * (A,B,C,D) returning to 0x8008315c, the upper (C,D,E,F) to 0x80083168.
 * AddPrim is a leaf, so at its entry the ring's frame is intact and the GTE
 * still holds the camera. The ring turns with the camera in 16-unit steps,
 * so vertices are named by absolute angle. One pose serves all 64 quads.
 * Its XZ are 0x3f80 * cos/sin(angle) - (camera - 0x3f80), camera at
 * 0x80096fa8/0x80096fb0 (low halves): the pose binds them about the arena
 * centre (0x3f80, 0x3f80), where each angle is one fixed point. */

/* The ring's pose under the current camera; publishes it on first use. The
 * terrain walk runs under the same camera matrix, so the draw-distance pass
 * can publish it when the guest linked no ring quad this frame. One pose per
 * camera, not per VBlank: a 30 Hz frame whose ring and terrain passes
 * straddle a VBlank still binds all its ring geometry to a single pose. */
static bool ring_pose_current(const CPUState *cpu, const XgHost3dProjection *projection) {
    XgRenderMotionSource current = {0};
    if (!psx_xg_render_motion_source(RING_PC, &current)) return false;
    const int32_t offset_x = (int16_t)cpu->read_half(RING_CAMERA) - RING_CENTRE;
    const int32_t offset_z = (int16_t)cpu->read_half(RING_CAMERA + 8u) - RING_CENTRE;
    if (!ring_pose.valid || ring_pose.scene_generation != current.scene_generation ||
        ring_pose.offset[0] != offset_x || ring_pose.offset[2] != offset_z ||
        memcmp(&ring_pose.projection, projection, sizeof(*projection))) {
        ring_pose.offset[0] = offset_x;
        ring_pose.offset[1] = 0;
        ring_pose.offset[2] = offset_z;
        ring_pose.projection = *projection;
        const XgRenderRigidPoseRequest request = {
            .entity_tag = BATTLING_ENTITY_TAG, .camera_id = BATTLING_RING_CAMERA_ID,
            .source_pc = RING_PC, .identity = RING_PC,
            .key = {RING_PC, 32u, 2u, 0u},
            .watch = {{RING_PC, 0x368u}, {RING_CAMERA, 0x0cu}},
            .world_space = true,
            .local_offset = {ring_pose.offset[0], 0, ring_pose.offset[2]},
        };
        ring_pose.valid = xg_render_rigid_pose_publish(&request,
                                                       projection, &ring_pose.source,
                                                       &ring_pose.motion);
        ring_pose.scene_generation = current.scene_generation;
    }
    return ring_pose.valid && ring_pose.motion.handle.resource_id;
}

static bool battling_ring_capture(const CPUState *cpu,
                                  const XgRenderProducerLifecycleServices *lifecycle) {
    XgHost3dProjection projection = {0};
    const uint32_t ra = cpu->gpr[31];
    if (ra != RING_LOWER_RETURN && ra != RING_UPPER_RETURN) return false;
    ++battling_diagnostics.ring_calls;
    const uint32_t sp = cpu->gpr[29], packet = cpu->gpr[5];
    if (!cpu->read_half || !lifecycle || !lifecycle->guest_data_range_is_valid ||
        cpu->read_word(RING_PC) != 0x27bdff78u ||
        cpu->read_word(RING_LOWER_RETURN - 8u) != ADD_PRIM_CALL ||
        cpu->read_word(RING_UPPER_RETURN - 8u) != ADD_PRIM_CALL ||
        !lifecycle->guest_data_range_is_valid(sp + 0x20u, 0x30u, 4u, false) ||
        !lifecycle->guest_data_range_is_valid(packet, 0x28u, 4u, false))
        goto reject;
    xg_render_runtime_capture_shadow_projection(cpu, &projection);
    bool posed = ring_pose_current(cpu, &projection);
    ring_pose.rgbc = cpu->gte_data[6];
    XgHost3dVector local[6], world[6];
    for (unsigned v = 0; v < 6u; ++v) {
        local[v] = (XgHost3dVector){(int16_t)cpu->read_half(sp + 0x20u + v * 8u),
                                    (int16_t)cpu->read_half(sp + 0x22u + v * 8u),
                                    (int16_t)cpu->read_half(sp + 0x24u + v * 8u), 0};
        const int32_t x = local[v].x + ring_pose.offset[0];
        const int32_t z = local[v].z + ring_pose.offset[2];
        posed &= x >= -RING_CENTRE && x <= RING_CENTRE && z >= -RING_CENTRE && z <= RING_CENTRE;
        world[v] = (XgHost3dVector){(int16_t)x, local[v].y, (int16_t)z, 0};
    }
    const uint32_t angle = (cpu->gpr[19] >> 4) & 0xffu;
    const uint32_t previous = (angle - 1u) & 0xffu;
    const uint32_t band = ra == RING_LOWER_RETURN ? 0u : 1u;
    ring_pose.drawn[angle / 64u] |= UINT64_C(1) << (angle % 64u);
    /* Packet corner order v0..v3: (A,B,C,D) or (C,D,E,F). */
    const unsigned base = band * 2u;
    uint32_t ids[4];
    for (unsigned v = 0; v < 4u; ++v)
        ids[v] = (((base + v) / 2u) << 8) | ((base + v) & 1u ? angle : previous);
    if (!xg_render_rigid_stage_ft4(&projection, packet, &local[base], &world[base],
                                   ids, RING_PC,
                                   (band << 8) | angle,
                                   posed ? &ring_pose.motion : NULL,
                                   ring_pose.source.continuity_generation,
                                   XG_RENDER_DEPTH_FAMILY_BATTLING_ARENA))
        goto reject;
    ++battling_diagnostics.ring_quads;
    return true;
reject:
    ++battling_diagnostics.ring_rejected;
    return false;
}

/* ---- Actor ground shadows ----------------------------------------------- */

/* BattlingRenderActorGroundShadow (0x80087b74) projects one ground quad per
 * actor, frame sp+0x10..0x2f, through its own terrain-aligned matrix
 * composed with the camera, then links it with AddPrim (return 0x80087e1c).
 * At AddPrim entry the GTE still holds that matrix; the actor is the packet's
 * owner (packet = actor + 0x1604 + buffer * 0x28). */
#define SHADOW_PC 0x80087b74u
#define SHADOW_RETURN 0x80087e1cu

static bool battling_shadow_capture(const CPUState *cpu,
                                    const XgRenderProducerLifecycleServices *lifecycle) {
    XgHost3dProjection projection = {0};
    XgRenderMotionSource motion_source = {0};
    XgRenderMotionRef motion = {0};
    if (cpu->gpr[31] != SHADOW_RETURN) return false;
    ++battling_diagnostics.shadow_calls;
    const uint32_t sp = cpu->gpr[29], packet = cpu->gpr[5];
    if (!cpu->read_half || !cpu->read_byte || !lifecycle ||
        !lifecycle->guest_data_range_is_valid ||
        cpu->read_word(SHADOW_PC) != 0x27bdff60u ||
        cpu->read_word(SHADOW_RETURN - 8u) != ADD_PRIM_CALL ||
        !lifecycle->guest_data_range_is_valid(sp + 0x10u, 0x20u, 4u, false) ||
        !lifecycle->guest_data_range_is_valid(packet, 0x28u, 4u, false))
        goto reject;
    const uint32_t actor = (packet - 0x1604u -
                            cpu->read_byte(BATTLING_POOL_INDEX) * 0x28u) & 0x1fffffffu;
    xg_render_runtime_capture_shadow_projection(cpu, &projection);
    XgHost3dVector local[4];
    for (unsigned v = 0; v < 4u; ++v)
        local[v] = (XgHost3dVector){(int16_t)cpu->read_half(sp + 0x10u + v * 8u),
                                    (int16_t)cpu->read_half(sp + 0x12u + v * 8u),
                                    (int16_t)cpu->read_half(sp + 0x14u + v * 8u), 0};
    const XgRenderRigidPoseRequest request = {
        .entity_tag = BATTLING_ENTITY_TAG, .camera_id = BATTLING_CAMERA_ID,
        .source_pc = SHADOW_PC, .identity = actor,
        .key = {SHADOW_PC, actor, 4u, 0u},
        .watch = {{SHADOW_PC, 0x2c4u}, {actor + 0x92cu, 0x1cu}},
    };
    const bool posed = xg_render_rigid_pose_publish(&request, &projection,
                                                    &motion_source, &motion) &&
                       motion.handle.resource_id;
    static const uint32_t ids[4] = {0u, 1u, 2u, 3u};
    if (!xg_render_rigid_stage_ft4(&projection, packet, local, NULL, ids, actor, SHADOW_PC,
                                   posed ? &motion : NULL,
                                   motion_source.continuity_generation,
                                   XG_RENDER_DEPTH_FAMILY_BATTLING_SHADOWS))
        goto reject;
    ++battling_diagnostics.shadow_quads;
    return true;
reject:
    ++battling_diagnostics.shadow_rejected;
    return false;
}

/* Draw distance: the ring segments the guest leaves out, all 256 angles
 * around the arena (it links 32 around the view direction, and only those
 * whose max SZ < 0x1c00). Its walk, under the terrain's camera matrix:
 * XZ = 0x3f80 * rsin/rcos(angle) - (camera - 0x3f80) in int16, DPCS of
 * its RGBC by C's IR0 with code 0x2c, bucket max(SZ A, B, C, F) >> 4, the
 * lower quad linked before the upper one; UV/CLUT/tpage are the pool's. */
static void battling_far_ring(const CPUState *cpu, const XgHost3dProjection *projection,
                              uint32_t ot) {
    if (!draw_distance.arena || !cpu->write_word || !projection->projection_distance)
        return;
    /* The guest's segments since the last far ring: this frame's. */
    uint64_t guest_drawn[4];
    memcpy(guest_drawn, ring_pose.drawn, sizeof(guest_drawn));
    memset(ring_pose.drawn, 0, sizeof(ring_pose.drawn));
    const uint32_t buffer = cpu->read_byte(BATTLING_POOL_INDEX) & 1u;
    const uint32_t base = draw_distance.arena + 2u * FAR_TERRAIN_BYTES +
                          buffer * RING_ARENA_BYTES;
    const bool posed = ring_pose_current(cpu, projection);
    const uint32_t pool = cpu->read_word(RING_POOLS + buffer * 4u);
    if (!ring_pose.rgbc || !pool) return;
    uint32_t uv[2][4];
    for (unsigned b = 0; b < 2u; ++b)
        for (unsigned k = 0; k < 4u; ++k)
            uv[b][k] = cpu->read_word(pool + b * RING_PACKET_BYTES + 12u + k * 8u);
    const int32_t camera_x = (int16_t)cpu->read_half(RING_CAMERA) - RING_CENTRE;
    const int32_t camera_z = (int16_t)cpu->read_half(RING_CAMERA + 8u) - RING_CENTRE;
    const int32_t margin = xg_host_3d_native_view_margin();
    static const int16_t heights[3] = {0, -0x290, -0x520};
    for (uint32_t angle = 0; angle < 256u; ++angle) {
        const uint32_t lower = base + angle * 2u * RING_PACKET_BYTES;
        const uint32_t upper = lower + RING_PACKET_BYTES;
        bool drawn = false;
        if (!(guest_drawn[angle / 64u] & (UINT64_C(1) << (angle % 64u)))) {
            const uint32_t previous = (angle - 1u) & 0xffu;
            XgHost3dVector local[6], world[6];
            XgHost3dProjectedVertex screen[6];
            bool valid = true;
            for (unsigned v = 0; v < 6u && valid; ++v) {
                const uint32_t a = ((v & 1u) ? angle : previous) * 16u;
                const int32_t wx = (int16_t)(((int32_t)(int16_t)cpu->read_half(
                    RING_TRIG + (a & 0xfffu) * 4u) * RING_RADIUS) >> 12);
                const int32_t wz = (int16_t)(((int32_t)(int16_t)cpu->read_half(
                    RING_TRIG + (a & 0xfffu) * 4u + 2u) * RING_RADIUS) >> 12);
                world[v] = (XgHost3dVector){(int16_t)wx, heights[v / 2u], (int16_t)wz, 0};
                local[v] = (XgHost3dVector){(int16_t)(wx - camera_x), heights[v / 2u],
                                            (int16_t)(wz - camera_z), 0};
                uint32_t flags;
                valid = xg_host_3d_rtps(projection, &local[v], &screen[v], &flags) &&
                        (uint32_t)screen[v].z * 2u > projection->projection_distance;
            }
            int32_t left = INT32_MAX, right = INT32_MIN, top = INT32_MAX, bottom = INT32_MIN;
            for (unsigned v = 0; v < 6u && valid; ++v) {
                if (screen[v].x < left) left = screen[v].x;
                if (screen[v].x > right) right = screen[v].x;
                if (screen[v].y < top) top = screen[v].y;
                if (screen[v].y > bottom) bottom = screen[v].y;
            }
            bool on_screen = right >= -margin && left < 320 + margin && bottom >= 0 &&
                top < 256;
            if (valid && !on_screen && far_previous.valid) {
                double previous[6][2];
                bool front = true;
                for (unsigned v = 0; v < 6u && front; ++v)
                    front = far_screen_point(&far_previous.projection,
                        (double)world[v].x - far_previous.ring_x, world[v].y,
                        (double)world[v].z - far_previous.ring_z, previous[v]);
                on_screen = front && far_box_on_screen(previous, 6u, margin);
            }
            if (valid && on_screen && right - left <= 1023 && bottom - top <= 511) {
                uint16_t depth = screen[0].z;
                if (screen[1].z > depth) depth = screen[1].z;
                if (screen[2].z > depth) depth = screen[2].z;
                if (screen[5].z > depth) depth = screen[5].z;
                const uint32_t color = (depth_cued_color(cpu, projection, ring_pose.rgbc,
                                                         screen[2].z) & 0x00ffffffu) |
                                       0x2c000000u;
                uint32_t xy[6];
                for (unsigned v = 0; v < 6u; ++v)
                    xy[v] = (uint16_t)screen[v].x | (uint32_t)(uint16_t)screen[v].y << 16;
                const uint32_t packets[2] = {lower, upper};
                bool staged = true;
                for (unsigned band = 0; band < 2u && staged; ++band) {
                    const unsigned first = band * 2u;
                    uint32_t ids[4];
                    for (unsigned v = 0; v < 4u; ++v)
                        ids[v] = (((first + v) / 2u) << 8) |
                                 ((first + v) & 1u ? angle : previous);
                    staged = xg_render_rigid_stage_ft4(
                        projection, packets[band], &local[first], &world[first], ids,
                        RING_PC, (band << 8) | angle, posed ? &ring_pose.motion : NULL,
                        ring_pose.source.continuity_generation,
                        XG_RENDER_DEPTH_FAMILY_BATTLING_ARENA);
                }
                if (staged) {
                    const uint32_t entry = ot + (uint32_t)(depth >> 4) * 4u;
                    for (unsigned band = 0; band < 2u; ++band) {
                        const unsigned first = band * 2u;
                        const uint32_t packet = packets[band];
                        uint32_t words[10] = {cpu->read_word(entry) | 0x09000000u, color};
                        for (unsigned v = 0; v < 4u; ++v) {
                            words[2u + v * 2u] = xy[first + v];
                            words[3u + v * 2u] = uv[band][v];
                        }
                        uint8_t *host = psx_mod_gpu_dma_host(packet, sizeof(words));
                        if (!host) break;
                        memcpy(host, words, sizeof(words));
                        cpu->write_word(entry, packet & 0x00ffffffu);
                    }
                    ++battling_diagnostics.far_ring_segments;
                    drawn = true;
                }
            }
        }
        if (!drawn) {
            (void)xg_render_motion_register_command((lower & 0x1fffffffu) + 4u, NULL, 0u, 0u);
            (void)xg_render_motion_register_command((upper & 0x1fffffffu) + 4u, NULL, 0u, 0u);
        }
    }
}

bool xg_render_battling_add_prim_capture(
        const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle) {
    if (!battling_resident(cpu) || !xg_render_submission_native_work_mode())
        return false;
    return battling_ring_capture(cpu, lifecycle) ||
           battling_shadow_capture(cpu, lifecycle);
}
