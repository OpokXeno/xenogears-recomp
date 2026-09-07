#include "xg_render_motion.h"
#include "psx_gte_divide.h"
#include "xg_render_scene_snapshot.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define MOTION_INSTANCE_CAPACITY 512u
#define MOTION_COMMAND_CAPACITY 16384u
#define MOTION_TOMBSTONE UINT32_MAX
#define MOTION_WATCH_CAPACITY (XG_RENDER_MOTION_NODE_CAPACITY * 3u + 4u)

typedef struct MotionInstance {
    XgRenderMotionRef ref;
    uint64_t entity_id;
    uint64_t epoch;
    uint64_t scene;
    uint64_t used;
    uint64_t geometry_epoch;
    uint32_t watch_count;
    uint32_t watch_address[MOTION_WATCH_CAPACITY];
    uint32_t watch_size[MOTION_WATCH_CAPACITY];
} MotionInstance;

typedef struct MotionCommand {
    XgRenderMotionDrawBinding binding;
    uint64_t continuity_generation;
    uint32_t command_id;
    uint32_t producer_id;
    uint32_t primitive_id;
} MotionCommand;

/* Only the guest owner touches these caches; consumers use immutable resources. */
static MotionInstance instances[MOTION_INSTANCE_CAPACITY];
static MotionCommand commands[MOTION_COMMAND_CAPACITY];
static uint64_t command_pages[512][MOTION_COMMAND_CAPACITY / 64u];
static bool commands_dirty;
static uint64_t serial;
static uint64_t geometry_serial;
static XgRenderMotionDiagnostics motion_diagnostics;
static uint32_t watched_pages[0x200000u / 4096u];

void xg_render_motion_note(XgRenderMotionEvent event, uint32_t pc) {
    if ((unsigned)event >= XG_MOTION_EVENT_COUNT)
        return;
    if (motion_diagnostics.events[event] != UINT64_MAX)
        ++motion_diagnostics.events[event];
    motion_diagnostics.last_pc[event] = pc;
}

void xg_render_motion_diagnostics(XgRenderMotionDiagnostics *out) {
    if (out)
        *out = motion_diagnostics;
}

static void watch_pages(uint32_t address, uint32_t size, bool add) {
    if (!size)
        return;
    for (uint32_t page = address / 4096; page <= (address + size - 1) / 4096; ++page) {
        if (add)
            ++watched_pages[page];
        else
            --watched_pages[page];
    }
}

static void clear_command(MotionCommand *command) {
    commands_dirty = true;
    if(command->command_id&&command->command_id!=MOTION_TOMBSTONE) {
        const size_t index=(size_t)(command-commands);
        command_pages[command->command_id/4096u][index/64u]&=~(UINT64_C(1)<<(index%64u));
    }
    if (command->binding.motion.handle.resource_id) {
        (void)xg_render_resource_release(command->binding.motion.handle);
        --motion_diagnostics.active_commands;
    }
    *command = (MotionCommand){.command_id = MOTION_TOMBSTONE};
}

static bool ref_equal(XgRenderMotionRef a, XgRenderMotionRef b) {
    return a.handle.resource_id == b.handle.resource_id &&
           a.handle.generation == b.handle.generation && a.digest == b.digest;
}

static bool normalize(double q[4]) {
    double norm = 0.0;
    for (unsigned i = 0; i < 4; ++i)
        norm += q[i] * q[i];
    if (!isfinite(norm) || norm < 1e-20)
        return false;
    norm = sqrt(norm);
    for (unsigned i = 0; i < 4; ++i)
        q[i] /= norm;
    return true;
}

int32_t xg_render_motion_s16_translation(int32_t stored) {
    const uint32_t low = (uint32_t)stored & UINT32_C(0xffff);
    return low < 0x8000u ? (int32_t)low : (int32_t)low - 65536;
}

bool xg_render_motion_decompose(const XgHost3dMatrix *matrix, XgRenderMotionTrs *out) {
    XgRenderMotionTrs trs = {0};
    double r[3][3];
    if (matrix == NULL || out == NULL)
        return false;
    for (unsigned c = 0; c < 3; ++c) {
        double n = 0.0;
        for (unsigned row = 0; row < 3; ++row) {
            r[row][c] = matrix->rotation[row][c] / 4096.0;
            n += r[row][c] * r[row][c];
        }
        trs.scale[c] = sqrt(n);
        if (trs.scale[c] < 1.0 / 4096.0)
            return false;
        for (unsigned row = 0; row < 3; ++row)
            r[row][c] /= trs.scale[c];
        trs.translation[c] = matrix->translation[c];
    }
    /* Allow Q12 rounding, not an affine shear masquerading as rotation. */
    for (unsigned a = 0; a < 3; ++a)
        for (unsigned b = a + 1; b < 3; ++b) {
            double dot = 0.0;
            for (unsigned row = 0; row < 3; ++row)
                dot += r[row][a] * r[row][b];
            if (fabs(dot) > 0.002)
                return false;
        }
    const double det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                       r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                       r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0.998 || det > 1.002)
        return false;
    const double trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0) {
        const double s = sqrt(trace + 1.0) * 2.0;
        trs.rotation[3] = s * 0.25;
        trs.rotation[0] = (r[2][1] - r[1][2]) / s;
        trs.rotation[1] = (r[0][2] - r[2][0]) / s;
        trs.rotation[2] = (r[1][0] - r[0][1]) / s;
    } else {
        unsigned i = r[1][1] > r[0][0] ? 1u : 0u;
        if (r[2][2] > r[i][i])
            i = 2;
        const unsigned j = (i + 1) % 3, k = (i + 2) % 3;
        const double s = sqrt(1.0 + r[i][i] - r[j][j] - r[k][k]) * 2.0;
        trs.rotation[i] = s * 0.25;
        trs.rotation[j] = (r[j][i] + r[i][j]) / s;
        trs.rotation[k] = (r[k][i] + r[i][k]) / s;
        trs.rotation[3] = (r[k][j] - r[j][k]) / s;
    }
    if (!normalize(trs.rotation))
        return false;
    *out = trs;
    return true;
}

static bool trs_valid(const XgRenderMotionTrs *t) {
    double norm = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(t->translation[i]) || !isfinite(t->scale[i]))
            return false;
    for (unsigned i = 0; i < 4; ++i)
        norm += t->rotation[i] * t->rotation[i];
    return isfinite(norm) && fabs(norm - 1.0) < 1e-6;
}

bool xg_render_motion_camera_from_view(const XgHost3dMatrix *view, XgRenderMotionTrs *out) {
    XgRenderMotionTrs camera;
    if (out == NULL || !xg_render_motion_decompose(view, &camera))
        return false;
    const double scale = (camera.scale[0] + camera.scale[1] + camera.scale[2]) / 3.0;
    for (unsigned i = 0; i < 3; ++i)
        if (fabs(camera.scale[i] - scale) > 0.002 * scale)
            return false;
    for (unsigned i = 0; i < 3; ++i) {
        double t = 0;
        for (unsigned j = 0; j < 3; ++j)
            t -= (view->rotation[j][i] / 4096.0) * view->translation[j] / (scale * scale);
        camera.translation[i] = t;
        camera.rotation[i] = -camera.rotation[i];
        camera.scale[i] = 1.0 / scale;
    }
    *out = camera;
    return true;
}

static bool pose_valid(const XgRenderMotionPose *p) {
    if (p == NULL || p->version != XG_RENDER_MOTION_VERSION || p->node_count == 0 ||
        p->node_count > XG_RENDER_MOTION_NODE_CAPACITY || !p->presentation_epoch ||
        !p->scene_generation || !p->source_update || !p->entity_id || !p->geometry_id ||
        !p->geometry_generation || !p->camera_id || !p->continuity_generation ||
        p->discontinuity > 1 || p->world_wrapped > 1 || !trs_valid(&p->camera) ||
        (unsigned)p->translation_stage > XG_RENDER_MOTION_TRANSLATION_WORLD ||
        !isfinite(p->projection_distance) || p->projection_distance <= 0 ||
        p->projection_distance > 65535 ||
        !isfinite(p->geometry_scale) || p->geometry_scale <= 0)
        return false;
    if ((p->translation_stage == XG_RENDER_MOTION_TRANSLATION_WORLD) != (p->world_wrapped != 0))
        return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!isfinite(p->screen_offset[i]) || p->screen_offset[i] * 65536.0 < INT32_MIN ||
            p->screen_offset[i] * 65536.0 > INT32_MAX)
            return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(p->camera_origin[i]) || !isfinite(p->wrap_span[i]) || p->wrap_span[i] < 0 ||
            p->camera.scale[i] < 1e-8)
            return false;
    for (uint32_t i = 0; i < p->node_count; ++i) {
        const XgRenderMotionNode *n = &p->nodes[i];
        if (!n->id || n->parent < -1 || n->parent >= (int32_t)i || n->translation_s16 > 1 ||
            (unsigned)n->policy > XG_RENDER_MOTION_CANCEL_PARENT_SCALE || !trs_valid(&n->local) ||
            n->source_matrix_valid > 1)
            return false;
        if (n->translation_s16)
            for (unsigned axis = 0; axis < 3; ++axis)
                if (n->local.translation[axis] < -32768 || n->local.translation[axis] > 32767)
                    return false;
        if (p->translation_stage == XG_RENDER_MOTION_TRANSLATION_WORLD &&
            (n->parent != (int32_t)i - 1 || n->policy != XG_RENDER_MOTION_LOCAL_TRS))
            return false;
        if (p->translation_stage == XG_RENDER_MOTION_TRANSLATION_FIELD && !n->translation_s16)
            return false;
        if (p->translation_stage == XG_RENDER_MOTION_TRANSLATION_GEAR &&
            ((i && n->parent < 0) || !n->translation_s16))
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (n->id == p->nodes[j].id)
                return false;
        if (n->policy == XG_RENDER_MOTION_CANCEL_PARENT_SCALE && n->parent >= 0)
            for (unsigned axis = 0; axis < 3; ++axis)
                if (fabs(p->nodes[n->parent].local.scale[axis]) < 1e-8)
                    return false;
    }
    return true;
}

bool xg_render_motion_view(XgRenderMotionRef ref, const XgRenderMotionPose **out) {
    XgRenderResourceView view;
    if (out == NULL || !ref.handle.resource_id || !ref.handle.generation || !ref.digest ||
        xg_render_resource_view(ref.handle, &view) != XG_RENDER_RESOURCE_OK ||
        view.kind != XG_RENDER_RESOURCE_MODEL || view.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        view.provenance.synthetic || view.content_digest != ref.digest || !view.bytes ||
        view.byte_count != sizeof(XgRenderMotionPose))
        return false;
    const XgRenderMotionPose *p = view.bytes;
    if (!pose_valid(p))
        return false;
    *out = p;
    return true;
}

static void release_instance(MotionInstance *instance) {
    if (instance->ref.handle.resource_id) {
        (void)xg_render_resource_retire_current(instance->ref.handle);
        (void)xg_render_resource_release(instance->ref.handle);
        --motion_diagnostics.active_instances;
    }
    for (uint32_t i = 0; i < instance->watch_count; ++i)
        watch_pages(instance->watch_address[i], instance->watch_size[i], false);
    *instance = (MotionInstance){0};
}

bool xg_render_motion_publish(const XgRenderMotionSource *source, const XgRenderMotionPose *input,
                              XgRenderMotionRef *out) {
    XgRenderMotionPose pose;
    XgRenderResourceCapabilityMetadata authority;
    xg_render_motion_note(XG_MOTION_PUBLISH_ATTEMPT, input ? (uint32_t)input->entity_id : 0);
    if (source == NULL || input == NULL || out == NULL || source->provenance.synthetic ||
        xg_render_resource_capability_validate(&source->provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
                                               source->scene_generation,
                                               &authority) != XG_RENDER_RESOURCE_CAPABILITY_OK)
        return false;
    /* Zero padding and unused nodes so the immutable payload has a stable digest. */
    memset(&pose, 0, sizeof(pose));
    pose.version = XG_RENDER_MOTION_VERSION;
    pose.node_count = input->node_count;
    pose.presentation_epoch = source->presentation_epoch;
    pose.scene_generation = source->scene_generation;
    pose.continuity_generation = source->continuity_generation;
    pose.source_update = source->source_update;
    pose.entity_id = input->entity_id;
    pose.geometry_id = input->geometry_id;
    pose.geometry_generation = input->geometry_generation;
    pose.camera_id = input->camera_id;
    pose.discontinuity = source->discontinuity || input->discontinuity;
    pose.world_wrapped = input->world_wrapped;
    pose.translation_stage = input->translation_stage;
    pose.camera = input->camera;
    memcpy(pose.screen_offset, input->screen_offset, sizeof(pose.screen_offset));
    pose.projection_distance = input->projection_distance;
    memcpy(pose.camera_origin, input->camera_origin, sizeof(pose.camera_origin));
    memcpy(pose.wrap_span, input->wrap_span, sizeof(pose.wrap_span));
    pose.geometry_scale = input->geometry_scale;
    if (pose.node_count > XG_RENDER_MOTION_NODE_CAPACITY)
        return false;
    for (uint32_t i = 0; i < pose.node_count; ++i) {
        pose.nodes[i].id = input->nodes[i].id;
        pose.nodes[i].parent = input->nodes[i].parent;
        pose.nodes[i].policy = input->nodes[i].policy;
        pose.nodes[i].translation_s16 = input->nodes[i].translation_s16;
        pose.nodes[i].local = input->nodes[i].local;
        pose.nodes[i].source_matrix_valid = input->nodes[i].source_matrix_valid;
        if (pose.nodes[i].source_matrix_valid) {
            memcpy(pose.nodes[i].source_model_to_view.rotation,
                   input->nodes[i].source_model_to_view.rotation,
                   sizeof(pose.nodes[i].source_model_to_view.rotation));
            memcpy(pose.nodes[i].source_model_to_view.translation,
                   input->nodes[i].source_model_to_view.translation,
                   sizeof(pose.nodes[i].source_model_to_view.translation));
        }
    }
    if (!pose_valid(&pose))
        return false;
    MotionInstance *slot = NULL, *oldest = &instances[0];
    for (unsigned i = 0; i < MOTION_INSTANCE_CAPACITY; ++i) {
        MotionInstance *s = &instances[i];
        if (s->entity_id == pose.entity_id && s->epoch == pose.presentation_epoch &&
            s->scene == pose.scene_generation) {
            slot = s;
            break;
        }
        if (s->used < oldest->used)
            oldest = s;
    }
    if (slot == NULL)
        slot = oldest;
    const bool same_instance = slot->entity_id == pose.entity_id &&
                               slot->epoch == pose.presentation_epoch &&
                               slot->scene == pose.scene_generation;
    uint64_t geometry_epoch = same_instance ? slot->geometry_epoch : ++geometry_serial;
    if (!geometry_epoch)
        return false;
    const uint64_t geometry_key[2] = {pose.geometry_generation, geometry_epoch};
    pose.geometry_generation = xg_render_resource_digest(geometry_key, sizeof(geometry_key));
    const XgRenderMotionPose *prior;
    if (slot->entity_id == pose.entity_id && xg_render_motion_view(slot->ref, &prior) &&
        prior->presentation_epoch == pose.presentation_epoch &&
        prior->scene_generation == pose.scene_generation &&
        prior->source_update == pose.source_update) {
        if (slot->ref.digest != xg_render_resource_digest(&pose, sizeof(pose)))
            return false; /* A second, conflicting pose is not another source tick. */
        *out = slot->ref;
        slot->used = ++serial;
        return true;
    }
    const uint64_t identity_words[4] = {
        UINT64_C(0x58474d4f54494f4e),
        pose.presentation_epoch,
        pose.scene_generation,
        pose.entity_id,
    };
    XgRenderResourceImport import = {
        .kind = XG_RENDER_RESOURCE_MODEL,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = pose.scene_generation,
        .provenance = source->provenance,
        .content_digest = xg_render_resource_digest(&pose, sizeof(pose)),
        .bytes = &pose,
        .byte_count = sizeof(pose),
    };
    memcpy(import.identity.bytes, identity_words, sizeof(identity_words));
    /* identity_id is a prefix reader, not a hash function. Keep the full
     * lifecycle tuple for collision checks, with its domain hash as the ID. */
    const uint64_t identity_hash =
        xg_render_resource_digest(identity_words, sizeof(identity_words));
    for (unsigned i = 0; i < 8; ++i)
        import.identity.bytes[i] = (uint8_t)(identity_hash >> (i * 8));
    import.resource_id = xg_render_resource_identity_id(&import.identity);
    XgRenderMotionRef ref = {.digest = import.content_digest};
    const XgRenderResourceResult imported = xg_render_resource_import_native(&import, &ref.handle);
    if (imported != XG_RENDER_RESOURCE_OK) {
        xg_render_motion_note(XG_MOTION_PUBLISH_IMPORT_REJECT, (uint32_t)imported);
        return false;
    }
    if (xg_render_resource_acquire_snapshot(ref.handle, ref.digest) != XG_RENDER_RESOURCE_OK) {
        (void)xg_render_resource_retire_current(ref.handle);
        return false;
    }
    MotionInstance next = {
        .ref = ref,
        .entity_id = pose.entity_id,
        .epoch = pose.presentation_epoch,
        .scene = pose.scene_generation,
        .used = ++serial,
        .geometry_epoch = geometry_epoch,
    };
    if (same_instance) {
        next.watch_count = slot->watch_count;
        memcpy(next.watch_address, slot->watch_address, sizeof(next.watch_address));
        memcpy(next.watch_size, slot->watch_size, sizeof(next.watch_size));
    }
    release_instance(slot);
    *slot = next;
    ++motion_diagnostics.active_instances;
    for (uint32_t i = 0; i < slot->watch_count; ++i)
        watch_pages(slot->watch_address[i], slot->watch_size[i], true);
    *out = ref;
    xg_render_motion_note(XG_MOTION_PUBLISHED, (uint32_t)input->entity_id);
    return true;
}

bool xg_render_motion_binding_valid(const XgRenderMotionDrawBinding *b) {
    const XgRenderMotionPose *pose;
    if (b == NULL)
        return false;
    if (!b->motion.handle.resource_id)
        return !b->motion.handle.generation && !b->motion.digest && !b->triangle_count &&
               !b->motion_part_index;
    return b->triangle_count > 0 && b->triangle_count <= 2 &&
           xg_render_motion_view(b->motion, &pose) && b->motion_part_index < pose->node_count &&
           pose->nodes[b->motion_part_index].source_matrix_valid &&
           (pose->translation_stage != XG_RENDER_MOTION_TRANSLATION_WORLD ||
            b->motion_part_index + 1 == pose->node_count);
}

bool xg_render_motion_register_command(uint32_t command_id,
                                       const XgRenderMotionDrawBinding *binding,
                                       uint32_t producer_id, uint32_t primitive_id) {
    command_id &= UINT32_C(0x1fffffff);
    if (!command_id || command_id > 0x1ffffcu || (command_id & 3u))
        return false;
    const unsigned start = ((command_id >> 2) * 2654435761u) % MOTION_COMMAND_CAPACITY;
    MotionCommand *empty = NULL, *slot = NULL;
    for (unsigned i = 0; i < MOTION_COMMAND_CAPACITY; ++i) {
        MotionCommand *c = &commands[(start + i) % MOTION_COMMAND_CAPACITY];
        if (c->command_id == command_id) {
            slot = c;
            break;
        }
        if ((!c->command_id || c->command_id == MOTION_TOMBSTONE) && empty == NULL)
            empty = c;
        if (!c->command_id)
            break;
    }
    if (binding == NULL && slot == NULL)
        return true;
    if (binding != NULL &&
        (!binding->motion.handle.resource_id || !xg_render_motion_binding_valid(binding)))
        return false;
    if (slot == NULL)
        slot = empty;
    if (slot == NULL)
        return binding == NULL;
    if (binding != NULL &&
        xg_render_resource_acquire_snapshot(binding->motion.handle, binding->motion.digest) !=
            XG_RENDER_RESOURCE_OK)
        return false;
    clear_command(slot);
    if (binding != NULL) {
        const XgRenderMotionPose *pose;
        if (!xg_render_motion_view(binding->motion, &pose)) {
            (void)xg_render_resource_release(binding->motion.handle);
            return false;
        }
        *slot = (MotionCommand){.binding = *binding,
                                .continuity_generation = pose->continuity_generation,
                                .command_id = command_id,
                                .producer_id = producer_id,
                                .primitive_id = primitive_id};
        ++motion_diagnostics.active_commands;
        const size_t index=(size_t)(slot-commands);
        command_pages[command_id/4096u][index/64u]|=UINT64_C(1)<<(index%64u);
    }
    return true;
}

void xg_render_motion_forget_range(uint32_t address, uint32_t size) {
    if (!size || !motion_diagnostics.active_commands) return;
    const uint64_t begin = address & UINT32_C(0x1fffffff), end = begin + size;
    if(begin>=0x200000u)return;
    const unsigned first=(unsigned)begin/4096u;
    const unsigned last=(unsigned)((end>0x200000u?0x200000u:end)-1u)/4096u;
    if(last-first<128u) {
        /* Union exact page membership, then visit slots in the original table
         * order. Full address predicates and reference retirement are unchanged. */
        for(unsigned word=0;word<MOTION_COMMAND_CAPACITY/64u;++word) {
            uint64_t candidates=0;
            for(unsigned page=first;page<=last;++page)candidates|=command_pages[page][word];
            while(candidates) {
                unsigned bit=0;
                while(!(candidates&(UINT64_C(1)<<bit)))++bit;
                candidates&=candidates-1u;
                MotionCommand *c=&commands[word*64u+bit];
                if(c->command_id>=begin&&c->command_id<end)clear_command(c);
            }
        }
        return;
    }
    for (unsigned i = 0; i < MOTION_COMMAND_CAPACITY; ++i) {
        MotionCommand *c = &commands[i];
        if (c->command_id && c->command_id != MOTION_TOMBSTONE && c->command_id >= begin &&
            c->command_id < end) {
            clear_command(c);
        }
    }
}

void xg_render_motion_reset(void) {
    /* Code writes can invalidate repeatedly before any motion is registered.
     * An already-zero table needs neither a full scan nor another bulk clear. */
    if (!commands_dirty && !motion_diagnostics.active_instances) {
        serial = 0;
        return;
    }
    xg_render_motion_forget_range(0, 0x200000u);
    memset(commands, 0, sizeof(commands));
    memset(command_pages,0,sizeof(command_pages));
    commands_dirty = false;
    for (unsigned i = 0; i < MOTION_INSTANCE_CAPACITY; ++i)
        release_instance(&instances[i]);
    serial = 0;
}

bool xg_render_motion_watch(XgRenderMotionRef ref, uint32_t address, uint32_t size) {
    address &= 0x1fffffffu;
    if (!size || address >= 0x200000u || size > 0x200000u - address)
        return false;
    for (unsigned i = 0; i < MOTION_INSTANCE_CAPACITY; ++i) {
        MotionInstance *s = &instances[i];
        if (!ref_equal(ref, s->ref))
            continue;
        for (unsigned j = 0; j < s->watch_count; ++j) {
            if (s->watch_address[j] == address && s->watch_size[j] == size)
                return true;
        }
        if (s->watch_count == MOTION_WATCH_CAPACITY)
            return false;
        s->watch_address[s->watch_count] = address;
        s->watch_size[s->watch_count++] = size;
        watch_pages(address, size, true);
        return true;
    }
    return false;
}

void xg_render_motion_invalidate_range(uint32_t address, uint32_t size) {
    const uint64_t begin = address & 0x1fffffffu, end = begin + size;
    xg_render_motion_note(XG_MOTION_INVALIDATE_CALL, address);
    if (!size || !motion_diagnostics.active_instances || begin >= 0x200000u) {
        xg_render_motion_note(XG_MOTION_INVALIDATE_SKIPPED, address);
        return;
    }
    const uint32_t last = (uint32_t)((end > 0x200000u ? 0x200000u : end) - 1) / 4096;
    bool watched = false;
    for (uint32_t page = (uint32_t)begin / 4096; page <= last; ++page)
        watched |= watched_pages[page] != 0;
    if (!watched) {
        xg_render_motion_note(XG_MOTION_INVALIDATE_SKIPPED, address);
        return;
    }
    for (unsigned i = 0; i < MOTION_INSTANCE_CAPACITY; ++i) {
        MotionInstance *s = &instances[i];
        bool overlap = false;
        for (unsigned j = 0; j < s->watch_count; ++j)
            overlap |= s->watch_size[j] &&
                       begin < (uint64_t)s->watch_address[j] + s->watch_size[j] &&
                       s->watch_address[j] < end;
        if (!overlap)
            continue;
        for (unsigned j = 0; j < MOTION_COMMAND_CAPACITY; ++j) {
            MotionCommand *c = &commands[j];
            if (c->command_id &&
                c->binding.motion.handle.resource_id == s->ref.handle.resource_id) {
                clear_command(c);
            }
        }
        release_instance(s);
    }
}

void xg_render_motion_forget_entity(uint64_t entity_id) {
    for (unsigned i = 0; i < MOTION_INSTANCE_CAPACITY; ++i) {
        MotionInstance *s = &instances[i];
        if (!entity_id || s->entity_id != entity_id)
            continue;
        for (unsigned j = 0; j < MOTION_COMMAND_CAPACITY; ++j) {
            MotionCommand *c = &commands[j];
            if (c->binding.motion.handle.resource_id == s->ref.handle.resource_id) {
                clear_command(c);
            }
        }
        release_instance(s);
    }
}

void xg_render_motion_prune_authority(void) {
    for (unsigned i = 0; i < MOTION_INSTANCE_CAPACITY; ++i) {
        MotionInstance *s = &instances[i];
        XgRenderResourceView view;
        XgRenderResourceCapabilityMetadata metadata;
        if (s->ref.handle.resource_id &&
            (xg_render_resource_view(s->ref.handle, &view) != XG_RENDER_RESOURCE_OK ||
             xg_render_resource_capability_validate(&view.provenance,
                                                    XG_RENDER_RESOURCE_OWNER_SCENE, s->scene,
                                                    &metadata) != XG_RENDER_RESOURCE_CAPABILITY_OK))
            xg_render_motion_forget_entity(s->entity_id);
    }
}

bool xg_render_motion_bind_command(uint32_t command_id, XgRenderNativeOperation *op) {
    xg_render_motion_note(XG_MOTION_BIND_ATTEMPT, command_id);
    if (op == NULL || op->kind != XG_RENDER_NATIVE_OPERATION_DRAW)
        return false;
    op->motion = (XgRenderMotionDrawBinding){0};
    command_id &= UINT32_C(0x1fffffff);
    const unsigned start = ((command_id >> 2) * 2654435761u) % MOTION_COMMAND_CAPACITY;
    for (unsigned i = 0; i < MOTION_COMMAND_CAPACITY; ++i) {
        const MotionCommand *c = &commands[(start + i) % MOTION_COMMAND_CAPACITY];
        if (!c->command_id)
            break;
        if (!command_id || c->command_id != command_id)
            continue;
        if (!op->semantic.interpolation_identity.valid ||
            op->semantic.interpolation_identity.scene_id != c->continuity_generation ||
            op->semantic.interpolation_identity.producer_id != c->producer_id ||
            op->semantic.interpolation_identity.primitive_id != c->primitive_id ||
            op->semantic.triangle_count != c->binding.triangle_count ||
            op->semantic.topology != GPU_RENDER_SEMANTIC_TRIANGLES) {
            xg_render_motion_note(XG_MOTION_BIND_IDENTITY_REJECT, command_id);
            return false;
        }
        if (!xg_render_motion_binding_valid(&c->binding)) {
            xg_render_motion_note(XG_MOTION_BIND_RESOURCE_REJECT, command_id);
            return false;
        }
        op->motion = c->binding;
        xg_render_motion_note(XG_MOTION_BOUND, command_id);
        return true;
    }
    xg_render_motion_note(XG_MOTION_BIND_MISSING, command_id);
    return false;
}

static void interpolate_trs(const XgRenderMotionTrs *a, const XgRenderMotionTrs *b, double alpha,
                             XgRenderMotionTrs *out) {
    /* Do not renormalize a stationary quaternion into a different matrix. */
    if (alpha == 0 || alpha == 1 || !memcmp(a, b, sizeof(*a))) {
        *out = alpha == 0 ? *a : *b;
        return;
    }
    if (!memcmp(a->rotation, b->rotation, sizeof(a->rotation))) {
        memcpy(out->rotation, b->rotation, sizeof(out->rotation));
    } else {
        double dot = 0.0;
        for (unsigned i = 0; i < 4; ++i)
            dot += a->rotation[i] * b->rotation[i];
        const double sign = dot < 0 ? -1.0 : 1.0;
        dot = fmin(fabs(dot), 1.0);
        double wa = 1.0 - alpha, wb = alpha;
        if (dot < 0.9995) {
            const double theta = acos(dot), denom = sin(theta);
            wa = sin((1.0 - alpha) * theta) / denom;
            wb = sin(alpha * theta) / denom;
        }
        for (unsigned i = 0; i < 4; ++i)
            out->rotation[i] = wa * a->rotation[i] + wb * sign * b->rotation[i];
        (void)normalize(out->rotation);
    }
    for (unsigned i = 0; i < 3; ++i) {
        out->translation[i] = a->translation[i] + alpha * (b->translation[i] - a->translation[i]);
        out->scale[i] = a->scale[i] + alpha * (b->scale[i] - a->scale[i]);
    }
}

static void matrix_trs(const XgRenderMotionTrs *t, double m[3][4]) {
    const double x = t->rotation[0], y = t->rotation[1], z = t->rotation[2], w = t->rotation[3];
    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);
    m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c)
            m[r][c] *= t->scale[c];
        m[r][3] = t->translation[r];
    }
}

static void compose(const double a[3][4], const double b[3][4], double out[3][4]) {
    double result[3][4];
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 4; ++c) {
            result[r][c] = c == 3 ? a[r][3] : 0;
            for (unsigned k = 0; k < 3; ++k)
                result[r][c] += a[r][k] * b[k][c];
        }
    memcpy(out, result, sizeof(result));
}

static bool compatible(const XgRenderMotionPose *a, const XgRenderMotionPose *b) {
    if (a->presentation_epoch != b->presentation_epoch ||
        a->scene_generation != b->scene_generation ||
        a->continuity_generation != b->continuity_generation || a->entity_id != b->entity_id ||
        a->geometry_id != b->geometry_id || a->geometry_generation != b->geometry_generation ||
        a->camera_id != b->camera_id || a->source_update >= b->source_update || a->discontinuity ||
        b->discontinuity || a->translation_stage != b->translation_stage ||
        a->node_count != b->node_count || a->world_wrapped != b->world_wrapped ||
        a->geometry_scale != b->geometry_scale ||
        memcmp(a->wrap_span, b->wrap_span, sizeof(a->wrap_span)))
        return false;
    for (uint32_t i = 0; i < b->node_count; ++i) {
        if (a->nodes[i].id != b->nodes[i].id || a->nodes[i].parent != b->nodes[i].parent ||
            a->nodes[i].policy != b->nodes[i].policy ||
            a->nodes[i].translation_s16 != b->nodes[i].translation_s16 ||
            a->nodes[i].source_matrix_valid != b->nodes[i].source_matrix_valid)
            return false;
        if (b->nodes[i].translation_s16)
            for (unsigned axis = 0; axis < 3; ++axis)
                if (fabs(b->nodes[i].local.translation[axis] -
                         a->nodes[i].local.translation[axis]) > 32768)
                    return false;
    }
    return true;
}

static double periodic_delta(double a, double b, double span) {
    double delta = b - a;
    if (span > 0)
        delta -= floor(delta / span + 0.5) * span;
    return delta;
}

static bool evaluate_transform(const XgRenderMotionPose *a, const XgRenderMotionPose *b,
                               double alpha, double wrap_shift[3], bool choose_wrap,
                               XgRenderMotionTransform *out) {
    XgRenderMotionTrs locals[XG_RENDER_MOTION_NODE_CAPACITY], camera;
    double world[XG_RENDER_MOTION_NODE_CAPACITY][3][4];
    memset(out, 0, sizeof(*out));
    interpolate_trs(&a->camera, &b->camera, alpha, &camera);
    double camera_world[3][4];
    matrix_trs(&camera, camera_world);
    for (unsigned r = 0; r < 3; ++r) {
        out->camera[r][3] = 0;
        for (unsigned c = 0; c < 3; ++c) {
            out->camera[r][c] = camera_world[c][r] / (camera.scale[r] * camera.scale[r]);
            out->camera[r][3] -= out->camera[r][c] * camera.translation[c];
        }
    }
    for (unsigned i = 0; i < 2; ++i)
        out->screen_offset[i] =
            a->screen_offset[i] + alpha * (b->screen_offset[i] - a->screen_offset[i]);
    out->projection_distance =
        a->projection_distance + alpha * (b->projection_distance - a->projection_distance);
    const bool gear = b->translation_stage == XG_RENDER_MOTION_TRANSLATION_GEAR;
    for (uint32_t i = 0; i < b->node_count; ++i) {
        const XgRenderMotionNode *n = &b->nodes[i];
        interpolate_trs(&a->nodes[i].local, &n->local, alpha, &locals[i]);
        if (b->world_wrapped && n->parent == -1)
            for (unsigned axis = 0; axis < 3; ++axis)
                locals[i].translation[axis] =
                    a->nodes[i].local.translation[axis] +
                    alpha * periodic_delta(a->nodes[i].local.translation[axis],
                                           n->local.translation[axis], b->wrap_span[axis]);
        matrix_trs(&locals[i], world[i]);
        if (n->parent >= 0) {
            if (n->policy == XG_RENDER_MOTION_CANCEL_PARENT_SCALE) {
                for (unsigned r = 0; r < 3; ++r)
                    if (fabs(locals[n->parent].scale[r]) < 1e-8)
                        return false;
                for (unsigned r = 0; r < 3; ++r)
                    for (unsigned c = 0; c < 3; ++c)
                        world[i][r][c] /= locals[n->parent].scale[r];
            }
            if (!gear || n->parent != 0)
                compose(world[n->parent], world[i], world[i]);
        }
    }
    for (uint32_t i = 0; i < b->node_count; ++i) {
        double object[3][4];
        memcpy(object, world[i], sizeof(object));
        if (gear && i)
            compose(world[0], world[i], object);
        for (unsigned r = 0; r < 3; ++r) {
            if (b->world_wrapped) {
                const double origin = a->camera_origin[r] +
                                      alpha * periodic_delta(a->camera_origin[r],
                                                             b->camera_origin[r], b->wrap_span[r]);
                object[r][3] -= origin;
                /* One lifted World branch for the entire A/B curve. Choosing
                 * a new wrap at each alpha creates a mid-phase map seam. */
                if (choose_wrap && i + 1 == b->node_count && b->wrap_span[r] > 0) {
                    if (object[r][3] < -16384)
                        wrap_shift[r] = b->wrap_span[r];
                    else if (object[r][3] > 16384)
                        wrap_shift[r] = -b->wrap_span[r];
                }
                object[r][3] += wrap_shift[r];
            }
            for (unsigned c = 0; c < 3; ++c)
                object[r][c] *= b->geometry_scale;
        }
        compose(out->camera, object, out->model_to_view[i]);
    }
    return true;
}

bool xg_render_motion_evaluate(XgRenderMotionRef previous, XgRenderMotionRef current, double alpha,
                               XgRenderMotionEvaluation *out) {
    const XgRenderMotionPose *a, *b;
    if (out == NULL || !isfinite(alpha) || alpha < 0 || alpha > 1 ||
        !xg_render_motion_view(current, &b))
        return false;
    const bool interpolate = xg_render_motion_view(previous, &a) && compatible(a, b);
    if (!interpolate) {
        a = b;
        alpha = 1;
    }
    memset(out, 0, sizeof(*out));
    out->current = current;
    out->node_count = b->node_count;
    out->interpolated = interpolate;
    out->alpha = alpha;
    double wrap_shift[3] = {0};
    if (!evaluate_transform(a, b, 1, wrap_shift, true, &out->endpoints[1]) ||
        !evaluate_transform(a, b, 0, wrap_shift, false, &out->endpoints[0]) ||
        !evaluate_transform(a, b, alpha, wrap_shift, false, &out->phase))
        return false;
    for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
        const XgRenderMotionPose *pose = endpoint ? b : a;
        for (uint32_t i = 0; i < pose->node_count; ++i) {
            const XgHost3dMatrix *matrix = &pose->nodes[i].source_model_to_view;
            XgHost3dProjection *projection = &out->source_projection[endpoint][i];
            memcpy(projection->rotation, matrix->rotation, sizeof(projection->rotation));
            memcpy(projection->translation, matrix->translation, sizeof(projection->translation));
            projection->screen_offset_x = (int32_t)(pose->screen_offset[0] * 65536.0);
            projection->screen_offset_y = (int32_t)(pose->screen_offset[1] * 65536.0);
            projection->projection_distance = (uint16_t)pose->projection_distance;
        }
    }
    /* Anchor the shared matrix curve, not individual polygons. In particular,
     * translation-only motion starts at the exact current matrix plus DELTAS;
     * adding an absolute reconstruction residual here would move a static pose. */
    for (uint32_t i = 0; i < b->node_count; ++i)
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned c = 0; c < 4; ++c) {
                const double ca = c == 3 ? a->nodes[i].source_model_to_view.translation[r]
                    : a->nodes[i].source_model_to_view.rotation[r][c] / 4096.0;
                const double cb = c == 3 ? b->nodes[i].source_model_to_view.translation[r]
                    : b->nodes[i].source_model_to_view.rotation[r][c] / 4096.0;
                const double fa = out->endpoints[0].model_to_view[i][r][c];
                const double fb = out->endpoints[1].model_to_view[i][r][c];
                const double f = out->phase.model_to_view[i][r][c];
                out->phase.model_to_view[i][r][c] = alpha == 0 ? ca : alpha == 1 ? cb :
                    cb + (1 - alpha) * (ca - cb) + ((f - fb) - (1 - alpha) * (fa - fb));
                out->endpoints[0].model_to_view[i][r][c] = ca;
                out->endpoints[1].model_to_view[i][r][c] = cb;
            }
    return true;
}

XgRenderMotionProjectResult xg_render_motion_project(const XgRenderMotionEvaluation *e,
                                                      const XgRenderMotionDrawBinding *b,
                                                      double screen_delta[2][3][3],
                                                      double native_delta[2][3][2]) {
    if (e == NULL || b == NULL || screen_delta == NULL || native_delta == NULL ||
        !ref_equal(e->current, b->motion) ||
        b->motion_part_index >= e->node_count || b->triangle_count == 0 || b->triangle_count > 2)
        return XG_RENDER_MOTION_INVALID;
    if (!e->interpolated)
        return XG_RENDER_MOTION_ENDPOINT;
    double result[2][3][3] = {0};
    double native_result[2][3][2] = {0};
    for (unsigned t = 0; t < b->triangle_count; ++t)
        for (unsigned v = 0; v < 3; ++v) {
            const XgHost3dVector *p = &b->local[t][v];
            double projected[3][2];
            for (unsigned sample = 0; sample < 3; ++sample) {
                const XgRenderMotionTransform *transform = sample < 2 ? &e->endpoints[sample] : &e->phase;
                const double (*m)[4] = transform->model_to_view[b->motion_part_index];
                double view[3];
                for (unsigned r = 0; r < 3; ++r) {
                    view[r] = m[r][0] * p->x + m[r][1] * p->y + m[r][2] * p->z + m[r][3];
                    if (!isfinite(view[r]))
                        return XG_RENDER_MOTION_INVALID;
                }
                if (sample == 2 && view[2] <= 0)
                    return XG_RENDER_MOTION_CLIP_REQUIRED;
                /* Continuous extension of IR/SZ/divide limits. Exact endpoint
                 * UNR and integer screen rounding are restored below, not
                 * confused with an unrestricted, high-precision H/Z camera. */
                const double q = view[2] <= 0 ? 131071.0 / 65536.0 :
                    fmin(transform->projection_distance / fmin(view[2], 65535.0), 131071.0 / 65536.0);
                for (unsigned axis = 0; axis < 2; ++axis)
                    projected[sample][axis] = transform->screen_offset[axis] +
                        fmax(-32768.0, fmin(view[axis], 32767.0)) * q;
                if (sample == 2)
                    result[t][v][2] = view[2];
            }
            double anchors[2][2], native_anchors[2][2];
            for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
                const XgHost3dProjection *source = &e->source_projection[endpoint][b->motion_part_index];
                int64_t mac[3];
                /* RTPS SF12: floor before wrapping MAC32, then saturate IR/SZ.
                 * All sums fit exactly in double (<2^44); division is by 2^12.
                 * Do not read the guest-owner's global native-view configuration. */
                for (unsigned r = 0; r < 3; ++r) {
                    const int64_t raw = (int64_t)source->translation[r] * 4096 +
                        (int64_t)source->rotation[r][0] * p->x +
                        (int64_t)source->rotation[r][1] * p->y +
                        (int64_t)source->rotation[r][2] * p->z;
                    const uint32_t low = (uint32_t)(int64_t)floor(raw / 4096.0);
                    mac[r] = low < UINT32_C(0x80000000) ? (int64_t)low :
                        (int64_t)low - INT64_C(4294967296);
                }
                const uint16_t depth = mac[2] < 0 ? 0 : mac[2] > 65535 ? 65535 : (uint16_t)mac[2];
                const int32_t q = psx_gte_divide(source->projection_distance, depth, NULL);
                for (unsigned axis = 0; axis < 2; ++axis) {
                    const int64_t ir = mac[axis] < -32768 ? -32768 : mac[axis] > 32767 ? 32767 : mac[axis];
                    const int64_t xy = (axis ? source->screen_offset_y : source->screen_offset_x) + ir * q;
                    const uint32_t low = (uint32_t)xy;
                    anchors[endpoint][axis] = fmax(-1024.0, fmin(1023.0, floor(xy / 65536.0)));
                    native_anchors[endpoint][axis] = (low < UINT32_C(0x80000000) ? (int64_t)low :
                        (int64_t)low - INT64_C(4294967296)) / 65536.0;
                }
            }
            for (unsigned axis = 0; axis < 2; ++axis) {
                const double a = anchors[0][axis], c = anchors[1][axis];
                const double na = native_anchors[0][axis], nc = native_anchors[1][axis];
                const double bend = (projected[2][axis] - projected[1][axis]) -
                    (1 - e->alpha) * (projected[0][axis] - projected[1][axis]);
                /* GTE(p) is a function of shared source geometry, never of a
                 * triangle/corner ID. Hidden vertex aliases get the same anchor.
                 * Difference form makes STATIC exactly zero at every alpha. */
                result[t][v][axis] = (1 - e->alpha) * (a - c) + bend;
                native_result[t][v][axis] = (1 - e->alpha) * (na - nc) + bend;
            }
        }
    memcpy(screen_delta, result, sizeof(result));
    memcpy(native_delta, native_result, sizeof(native_result));
    return XG_RENDER_MOTION_PROJECTED;
}
