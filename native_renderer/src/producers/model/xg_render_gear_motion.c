#include "xg_render_gear_motion.h"
#include "cpu_state.h"
#include "xg_field_render_services.h"
#include "xg_host_3d.h"

#include <string.h>

#define GEAR_SLOTS 32u
#define JOINT_SIZE 0x7cu

typedef struct GearConsumedPose {
    XgRenderMotionSource source;
    XgRenderMotionPose pose;
    XgHost3dMatrix local[XG_RENDER_MOTION_NODE_CAPACITY];
    XgHost3dMatrix accumulated[XG_RENDER_MOTION_NODE_CAPACITY];
    int16_t euler[XG_RENDER_MOTION_NODE_CAPACITY][3];
    int16_t scale[XG_RENDER_MOTION_NODE_CAPACITY][3];
    uint32_t parent[XG_RENDER_MOTION_NODE_CAPACITY];
    uint16_t geometry[XG_RENDER_MOTION_NODE_CAPACITY];
    uint8_t selector[XG_RENDER_MOTION_NODE_CAPACITY];
    uint32_t skeleton;
    uint32_t entry_sp;
    uint32_t return_pc;
    int32_t overall_scale;
    uint64_t revision;
    bool scaled;
    bool rebuilding;
    bool valid;
    bool published;
} GearConsumedPose;

typedef struct GearRenderScope {
    XgRenderMotionRef pose;
    GearConsumedPose *consumed;
    uint32_t models[XG_RENDER_MOTION_NODE_CAPACITY];
    uint32_t packets[XG_RENDER_MOTION_NODE_CAPACITY];
    uint32_t packet_sizes[XG_RENDER_MOTION_NODE_CAPACITY];
    uint32_t entry_sp;
    uint32_t return_pc;
    uint32_t render_pc;
    XgHost3dMatrix camera;
    bool valid;
    bool pending;
} GearRenderScope;

/* Guest-owner state only. Active counts nonzero slot revisions, including pending
 * rebuilds; published flags preserve retirement responsibility across rebuilds. */
static GearConsumedPose slots[GEAR_SLOTS];
static GearRenderScope render;
static uint32_t active_slots;
static uint32_t pending_rebuilds;
static uint64_t revision;
static XgRenderGearMotionDiagnostics diagnostics;

static bool same_address(uint32_t a, uint32_t b) { return (a & 0x1fffffffu) == (b & 0x1fffffffu); }

static int32_t q12(int64_t value) {
    return (int32_t)(value >= 0 ? value / 4096 : -((-value + 4095) / 4096));
}

static bool matrix_equal(const XgHost3dMatrix *a, const XgHost3dMatrix *b) {
    return memcmp(a->rotation, b->rotation, sizeof(a->rotation)) == 0 &&
           memcmp(a->translation, b->translation, sizeof(a->translation)) == 0;
}

static bool reject(XgRenderGearMotionReject reason) {
    ++diagnostics.rejected[reason];
    return false;
}

static bool source_matches(const XgRenderMotionSource *a, const XgRenderMotionSource *b) {
    XgRenderResourceCapabilityMetadata left, right;
    if (a->presentation_epoch != b->presentation_epoch ||
        a->scene_generation != b->scene_generation ||
        a->continuity_generation != b->continuity_generation ||
        xg_render_resource_capability_validate(&a->provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
                                               a->scene_generation,
                                               &left) != XG_RENDER_RESOURCE_CAPABILITY_OK ||
        xg_render_resource_capability_validate(&b->provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
                                               b->scene_generation,
                                               &right) != XG_RENDER_RESOURCE_CAPABILITY_OK)
        return false;
    /* Producer receipts may differ across entry points of the same artifact. */
    const XgRenderArtifactIdentity *la = left.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE
                                             ? &left.source.origin_artifact
                                             : &left.artifact;
    const XgRenderArtifactIdentity *ra = right.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE
                                             ? &right.source.origin_artifact
                                             : &right.artifact;
    return same_address(la->base, ra->base) && la->size == ra->size &&
           memcmp(la->sha256, ra->sha256, sizeof(la->sha256)) == 0;
}

/* slus_006.64 8003f738: Rx*Ry*Rz; 8004a92c: Ry*Rx*Rz.
 * The latter reads abs(angle) and negates sine for negative signed angles.
 * Keep the original Q12 products/floors for the rebuild equality check. */
static bool rotation_matrix(CPUState *cpu, const XgRenderGearMotionServices *services,
                            const int16_t angles[3], uint8_t selector, XgHost3dMatrix *out) {
    int32_t s[3], c[3];
    for (unsigned i = 0; i < 3; ++i) {
        const int32_t a = angles[i];
        const uint32_t index = (uint32_t)(selector && a < 0 ? -a : a) & 0xfffu;
        const uint32_t address = 0x800523f0u + index * 4;
        if (!services->range(address, 4, 4, false))
            return false;
        const uint32_t word = cpu->read_word(address);
        s[i] = (int16_t)word;
        c[i] = (int16_t)(word >> 16);
        if (selector && a < 0)
            s[i] = -s[i];
    }
    const int32_t sx = s[0], sy = s[1], sz = s[2], cx = c[0], cy = c[1], cz = c[2];
    *out = (XgHost3dMatrix){0};
    int16_t (*m)[3] = out->rotation;
    if (!selector) {
        const int32_t zsy = q12((int64_t)cz * -sy), ssy = q12((int64_t)sz * -sy);
        m[0][0] = (int16_t)q12((int64_t)cz * cy);
        m[0][1] = (int16_t)q12(-(int64_t)sz * cy);
        m[0][2] = (int16_t)sy;
        m[1][0] = (int16_t)(q12((int64_t)sz * cx) - q12((int64_t)zsy * sx));
        m[1][1] = (int16_t)(q12((int64_t)cz * cx) + q12((int64_t)ssy * sx));
        m[1][2] = (int16_t)q12(-(int64_t)cy * sx);
        m[2][0] = (int16_t)(q12((int64_t)zsy * cx) + q12((int64_t)sz * sx));
        m[2][1] = (int16_t)(q12((int64_t)cz * sx) - q12((int64_t)ssy * cx));
        m[2][2] = (int16_t)q12((int64_t)cy * cx);
    } else {
        const int32_t ysx = q12((int64_t)sy * sx), csx = q12((int64_t)cy * sx);
        m[0][0] = (int16_t)(q12((int64_t)cy * cz) + q12((int64_t)ysx * sz));
        m[0][1] = (int16_t)(q12((int64_t)ysx * cz) - q12((int64_t)cy * sz));
        m[0][2] = (int16_t)q12((int64_t)sy * cx);
        m[1][0] = (int16_t)q12((int64_t)sz * cx);
        m[1][1] = (int16_t)q12((int64_t)cz * cx);
        m[1][2] = (int16_t)-sx;
        m[2][0] = (int16_t)(q12((int64_t)csx * sz) - q12((int64_t)sy * cz));
        m[2][1] = (int16_t)(q12((int64_t)sy * sz) + q12((int64_t)csx * cz));
        m[2][2] = (int16_t)q12((int64_t)cy * cx);
    }
    return true;
}

static GearConsumedPose *find_slot(uint32_t skeleton) {
    for (unsigned i = 0; i < GEAR_SLOTS; ++i)
        if (slots[i].revision && same_address(slots[i].skeleton, skeleton))
            return &slots[i];
    return NULL;
}

static void cancel_render(void) {
    if (!render.pending)
        return;
    render.valid = false;
    render.pending = false;
    if (render.pose.handle.resource_id && render.consumed && render.consumed->published) {
        xg_render_motion_forget_entity(UINT64_C(0x4745415200000000) |
                                       (render.consumed->skeleton & 0x1fffffffu));
        render.consumed->published = false;
    }
    for (unsigned i = 0; i < XG_RENDER_MOTION_NODE_CAPACITY; ++i)
        if (render.packet_sizes[i])
            xg_render_motion_forget_range(render.packets[i], render.packet_sizes[i]);
    render.consumed = NULL;
    render.pose = (XgRenderMotionRef){0};
}

static void clear_slot(GearConsumedPose *slot) {
    if (!slot->revision)
        return;
    if (render.consumed == slot)
        cancel_render();
    if (slot->published)
        xg_render_motion_forget_entity(UINT64_C(0x4745415200000000) |
                                       (slot->skeleton & 0x1fffffffu));
    if (slot->rebuilding)
        --pending_rebuilds;
    --active_slots;
    /* Payload is initialized on reuse, not on every overlapping World write. */
    slot->skeleton = 0;
    slot->revision = 0;
    slot->rebuilding = false;
    slot->valid = false;
    slot->published = false;
}

static void rebuild_begin(CPUState *cpu, uint32_t pc, const XgRenderGearMotionServices *services,
                          const XgRenderMotionSource *source) {
    const uint32_t skeleton = cpu->gpr[4];
    GearConsumedPose *slot = find_slot(skeleton);
    ++diagnostics.rebuild_entries;
    diagnostics.last_skeleton = skeleton;
    if (!slot) {
        slot = &slots[0];
        for (unsigned i = 1; i < GEAR_SLOTS; ++i)
            if (slots[i].revision < slot->revision)
                slot = &slots[i];
        clear_slot(slot);
    }
    if (render.consumed == slot)
        cancel_render();
    if (slot->rebuilding)
        --pending_rebuilds;
    slot->rebuilding = false;
    slot->valid = false;
    if (!services->range(skeleton, JOINT_SIZE, 4, false) || revision == UINT64_MAX) {
        reject(XG_RENDER_GEAR_MOTION_RANGE);
        goto fail;
    }
    const uint32_t count = cpu->read_half(skeleton + 0xa);
    if (count < 2 || count > XG_RENDER_MOTION_NODE_CAPACITY ||
        !services->range(skeleton, count * JOINT_SIZE, 4, false)) {
        reject(XG_RENDER_GEAR_MOTION_HIERARCHY);
        goto fail;
    }
    if (!slot->revision)
        ++active_slots;
    /* A normal same-skeleton rebuild must preserve the prior published instance
     * for interpolation; failed captures and eviction retire it via clear_slot. */
    const bool published = slot->published;
    *slot = (GearConsumedPose){.published = published};
    slot->skeleton = skeleton;
    slot->revision = ++revision;
    slot->source = *source;
    slot->source.source_update = slot->revision;
    slot->entry_sp = cpu->gpr[29];
    slot->return_pc = cpu->gpr[31];
    slot->scaled = pc == 0x801dc848u;
    slot->overall_scale = (int32_t)cpu->gpr[5];
    diagnostics.last_overall_scale = slot->overall_scale;
    if (slot->overall_scale <= 0 || slot->overall_scale > 32767) {
        reject(XG_RENDER_GEAR_MOTION_ROTATION);
        goto fail;
    }
    slot->pose.node_count = count;
    slot->pose.translation_stage = XG_RENDER_MOTION_TRANSLATION_GEAR;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t joint = skeleton + i * JOINT_SIZE;
        const uint32_t parent = cpu->read_word(joint);
        uint32_t parent_index = 0;
        if (parent) {
            const uint32_t delta = (parent & 0x1fffffffu) - (skeleton & 0x1fffffffu);
            if (!i || delta % JOINT_SIZE || !(parent_index = delta / JOINT_SIZE) ||
                parent_index >= i) {
                reject(XG_RENDER_GEAR_MOTION_HIERARCHY);
                goto fail;
            }
        }
        slot->parent[i] = parent;
        slot->geometry[i] = cpu->read_half(joint + 8);
        slot->selector[i] = cpu->read_byte(joint + 6);
        if ((i == 0 && slot->geometry[i] != 0xffff) || (i && cpu->read_half(joint + 0xa) != i)) {
            reject(XG_RENDER_GEAR_MOTION_HIERARCHY);
            goto fail;
        }
        XgRenderMotionNode *node = &slot->pose.nodes[i];
        node->id = joint & 0x1fffffffu;
        node->parent = i ? (int32_t)parent_index : -1;
        node->policy = slot->scaled && parent ? XG_RENDER_MOTION_CANCEL_PARENT_SCALE
                                              : XG_RENDER_MOTION_LOCAL_TRS;
        for (unsigned axis = 0; axis < 3; ++axis) {
            slot->euler[i][axis] = (int16_t)cpu->read_half(joint + 0x54 + axis * 2);
            slot->scale[i][axis] = (int16_t)cpu->read_half(joint + 0x4c + axis * 2);
        }
        XgHost3dMatrix rotation;
        if (!rotation_matrix(cpu, services, slot->euler[i], slot->selector[i], &rotation) ||
            !xg_render_motion_decompose(&rotation, &node->local)) {
            reject(XG_RENDER_GEAR_MOTION_ROTATION);
            goto fail;
        }
        slot->local[i] = rotation;
        XgHost3dMatrix scale = {0}, scaled;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const int32_t factor = i ? (slot->scaled ? slot->scale[i][axis] : 4096)
                                     : q12((int64_t)slot->overall_scale * slot->scale[0][axis]);
            if (factor < -32768 || factor > 32767) {
                reject(XG_RENDER_GEAR_MOTION_ROTATION);
                goto fail;
            }
            scale.rotation[axis][axis] = (int16_t)factor;
            node->local.scale[axis] = factor / 4096.0;
            node->local.translation[axis] = (int32_t)cpu->read_word(joint + 0x5c + axis * 4);
            slot->local[i].translation[axis] = (int32_t)node->local.translation[axis];
        }
        /* MulMatrix0 only writes rotation; comp_matrix uses zero translation here. */
        if (!xg_host_3d_comp_matrix(&rotation, &scale, &scaled)) {
            reject(XG_RENDER_GEAR_MOTION_ROTATION);
            goto fail;
        }
        memcpy(slot->local[i].rotation, scaled.rotation, sizeof(scaled.rotation));
        if (slot->scaled && parent) {
            scale = (XgHost3dMatrix){0};
            for (unsigned axis = 0; axis < 3; ++axis) {
                const int32_t factor = slot->scale[parent_index][axis];
                if (!factor || 0x1000000 / factor < -32768 || 0x1000000 / factor > 32767) {
                    reject(XG_RENDER_GEAR_MOTION_ROTATION);
                    goto fail;
                }
                scale.rotation[axis][axis] = (int16_t)(0x1000000 / factor);
            }
            rotation = slot->local[i];
            memset(rotation.translation, 0, sizeof(rotation.translation));
            if (!xg_host_3d_comp_matrix(&scale, &rotation, &scaled)) {
                reject(XG_RENDER_GEAR_MOTION_REBUILD);
                goto fail;
            }
            memcpy(slot->local[i].rotation, scaled.rotation, sizeof(scaled.rotation));
        }
        if (!i) {
            slot->accumulated[i] = rotation;
            memcpy(slot->accumulated[i].translation, slot->local[i].translation,
                   sizeof(slot->local[i].translation));
        } else if (parent) {
            if (!xg_host_3d_comp_matrix(&slot->accumulated[parent_index], &slot->local[i],
                                         &slot->accumulated[i])) {
                reject(XG_RENDER_GEAR_MOTION_REBUILD);
                goto fail;
            }
        } else
            slot->accumulated[i] = slot->local[i];
    }
    /* Preserve raw matrices above: root t is assigned directly and null-parent
     * bones copy local -> accumulated. Rendering then consumes root and each
     * bone-relative accumulated t through separate CompMatrix RHS stages. */
    XgHost3dMatrix relative[XG_RENDER_MOTION_NODE_CAPACITY];
    for (uint32_t i = 0; i < count; ++i) {
        XgRenderMotionNode *node = &slot->pose.nodes[i];
        XgHost3dMatrix effective = slot->local[i];
        node->translation_s16 = 1;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const int32_t value = xg_render_motion_s16_translation(effective.translation[axis]);
            if (value != effective.translation[axis])
                xg_render_motion_note(XG_MOTION_TRANSLATION_CANONICALIZED, node->id);
            node->local.translation[axis] = value;
            effective.translation[axis] = value;
        }
        if (i && slot->parent[i]) {
            if (!xg_host_3d_comp_matrix(&relative[node->parent], &effective, &relative[i])) {
                reject(XG_RENDER_GEAR_MOTION_REBUILD);
                goto fail;
            }
        } else
            relative[i] = effective;
        if (i)
            for (unsigned axis = 0; axis < 3; ++axis)
                if (relative[i].translation[axis] !=
                    xg_render_motion_s16_translation(slot->accumulated[i].translation[axis]))
                    slot->pose.discontinuity = 1;
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned c = 0; c < 3; ++c)
                if (slot->accumulated[i].rotation[r][c] == INT16_MIN ||
                    slot->accumulated[i].rotation[r][c] == INT16_MAX)
                    slot->pose.discontinuity = 1;
    }
    if (slot->pose.discontinuity)
        xg_render_motion_note(XG_MOTION_TRANSLATION_STAGE_DISCRETE, skeleton);
    slot->rebuilding = true;
    ++pending_rebuilds;
    return;
fail:
    clear_slot(slot);
}

static bool matrices_match(CPUState *cpu, const GearConsumedPose *slot) {
    XgHost3dMatrix actual;
    for (uint32_t i = 0; i < slot->pose.node_count; ++i) {
        const uint32_t joint = slot->skeleton + i * JOINT_SIZE;
        if (cpu->read_word(joint) != slot->parent[i] ||
            cpu->read_half(joint + 8) != slot->geometry[i] ||
            !xg_render_runtime_capture_matrix(cpu, joint + 0xc, &actual) ||
            !matrix_equal(&actual, &slot->local[i]) ||
            !xg_render_runtime_capture_matrix(cpu, joint + 0x2c, &actual) ||
            !matrix_equal(&actual, &slot->accumulated[i]))
            return false;
    }
    return true;
}

static void rebuild_end(CPUState *cpu, uint32_t pc) {
    for (unsigned i = 0; pending_rebuilds && i < GEAR_SLOTS; ++i) {
        GearConsumedPose *slot = &slots[i];
        if (!slot->rebuilding || cpu->gpr[29] != slot->entry_sp ||
            !same_address(cpu->gpr[31], slot->return_pc) || slot->scaled != (pc == 0x801dcc2cu))
            continue;
        slot->rebuilding = false;
        --pending_rebuilds;
        slot->valid = cpu->gpr[2] == slot->pose.node_count && matrices_match(cpu, slot);
        if (slot->valid) {
            ++diagnostics.consumed_poses;
            diagnostics.last_consumed_revision = slot->revision;
        } else {
            reject(XG_RENDER_GEAR_MOTION_REBUILD);
            clear_slot(slot);
        }
        return;
    }
    reject(XG_RENDER_GEAR_MOTION_SCOPE);
}

static void render_begin(CPUState *cpu, uint32_t pc, const XgRenderGearMotionServices *services,
                          const XgRenderMotionSource *source) {
    cancel_render();
    render =
        (GearRenderScope){.entry_sp = cpu->gpr[29], .return_pc = cpu->gpr[31], .render_pc = pc,
                          .pending = true};
    const bool wrapper = pc == 0x801dcec8u;
    const uint32_t model = wrapper ? cpu->gpr[4] : 0;
    if (wrapper && !services->range(model, 0x64, 4, false)) {
        reject(XG_RENDER_GEAR_MOTION_RANGE);
        return;
    }
    const uint32_t mesh = wrapper ? cpu->read_word(model) : cpu->gpr[4];
    const uint32_t skeleton = wrapper ? cpu->read_word(model + 4) : cpu->gpr[5];
    GearConsumedPose *slot = find_slot(skeleton);
    render.consumed = slot;
    if (!slot || !slot->valid) {
        reject(XG_RENDER_GEAR_MOTION_REBUILD);
        return;
    }
    if (!source_matches(&slot->source, source)) {
        reject(XG_RENDER_GEAR_MOTION_AUTHORITY);
        return;
    }
    if (!services->range(skeleton, slot->pose.node_count * JOINT_SIZE, 4, false) ||
        !services->range(mesh, 8, 4, false) || !services->range(cpu->gpr[29], 0x1c, 4, true)) {
        reject(XG_RENDER_GEAR_MOTION_RANGE);
        return;
    }
    if (wrapper && cpu->read_byte(model + 0x5c) != 0xff) {
        reject(XG_RENDER_GEAR_MOTION_ATTACHMENT);
        return;
    }
    if (wrapper && !cpu->read_byte(model + 0x34)) {
        reject(XG_RENDER_GEAR_MOTION_REBUILD);
        return;
    }
    if (!matrices_match(cpu, slot)) {
        reject(XG_RENDER_GEAR_MOTION_REBUILD);
        return;
    }
    const uint32_t table = cpu->read_word(mesh), model_count = cpu->read_word(mesh + 4);
    const uint32_t buffer = cpu->read_word(cpu->gpr[29] + 0x18);
    const uint32_t mode = wrapper ? cpu->gpr[7] : cpu->read_word(cpu->gpr[29] + 0x10);
    if (!model_count || model_count > 1024 || buffer > 1 || mode != 1 ||
        !services->range(table, model_count * 4, 4, false)) {
        reject(XG_RENDER_GEAR_MOTION_GEOMETRY);
        return;
    }
    const uint32_t camera = wrapper ? cpu->gpr[5] : cpu->gpr[6];
    if (!services->range(camera, 0x20, 4, true) ||
        !xg_render_runtime_capture_matrix(cpu, camera, &render.camera) ||
        !xg_render_motion_camera_from_view(&render.camera, &slot->pose.camera)) {
        reject(XG_RENDER_GEAR_MOTION_ROTATION);
        return;
    }
    /* Both renderers use (camera * root +0xc) * bone-relative cache +0x2c.
     * Retain the exact two CompMatrix stages, not their TRS reconstruction. */
    XgHost3dMatrix root_view;
    if (!xg_host_3d_comp_matrix(&render.camera, &slot->local[0], &root_view)) {
        reject(XG_RENDER_GEAR_MOTION_ROTATION);
        return;
    }
    uint32_t geometry_key[XG_RENDER_MOTION_NODE_CAPACITY][5] = {0};
    uint32_t topology_sizes[XG_RENDER_MOTION_NODE_CAPACITY] = {0};
    for (uint32_t i = 1; i < slot->pose.node_count; ++i) {
        const uint32_t joint = skeleton + i * JOINT_SIZE;
        if (slot->geometry[i] == 0xffff)
            continue;
        if (!xg_host_3d_comp_matrix(&root_view, &slot->accumulated[i],
                                     &slot->pose.nodes[i].source_model_to_view)) {
            reject(XG_RENDER_GEAR_MOTION_ROTATION);
            return;
        }
        slot->pose.nodes[i].source_matrix_valid = 1;
        if (wrapper && (int16_t)cpu->read_half(joint + 0x52) > 0) {
            reject(XG_RENDER_GEAR_MOTION_BILLBOARD);
            return;
        }
        if (slot->geometry[i] >= model_count) {
            reject(XG_RENDER_GEAR_MOTION_GEOMETRY);
            return;
        }
        const uint32_t header = cpu->read_word(table + slot->geometry[i] * 4);
        if (!services->range(header, 0x38, 4, false)) {
            reject(XG_RENDER_GEAR_MOTION_RANGE);
            return;
        }
        const uint32_t vertices = cpu->read_word(header + 8),
                       topology = cpu->read_word(header + 0x10);
        const uint32_t count = cpu->read_half(header + 2), groups = cpu->read_half(header + 6);
        uint32_t cursor = topology;
        uint64_t packet_bytes = 0;
        if (!count || groups > 256 || !services->range(vertices, count * 8, 4, false)) {
            reject(XG_RENDER_GEAR_MOTION_GEOMETRY);
            return;
        }
        for (uint32_t g = 0; g < groups; ++g) {
            if (!services->range(cursor, 4, 2, false)) {
                reject(XG_RENDER_GEAR_MOTION_RANGE);
                return;
            }
            const uint32_t family = cpu->read_byte(cursor), n = cpu->read_half(cursor + 2);
            /* These are the shared FT3/FT4 source lanes. Reject the entire
             * skeleton if another geometry lane would leave a discrete limb. */
            if ((family != 5 && family != 13) || !n ||
                !services->range(cursor, 4 + n * 8, 2, false)) {
                reject(XG_RENDER_GEAR_MOTION_GEOMETRY);
                return;
            }
            for (uint32_t p = 0; p < n; ++p)
                for (uint32_t v = 0; v < (family == 5 ? 3u : 4u); ++v)
                    if (cpu->read_half(cursor + 4 + p * 8 + v * 2) >= count) {
                        reject(XG_RENDER_GEAR_MOTION_GEOMETRY);
                        return;
                    }
            packet_bytes += (uint64_t)n * (family == 5 ? 0x20u : 0x28u);
            cursor += 4 + n * 8;
        }
        render.models[i] = header;
        render.packets[i] = cpu->read_word(joint + 0x68 + buffer * 4);
        render.packet_sizes[i] = cpu->read_word(header + 0x34);
        if (packet_bytes > render.packet_sizes[i] ||
            !services->range(render.packets[i], render.packet_sizes[i], 4, false)) {
            reject(XG_RENDER_GEAR_MOTION_RANGE);
            return;
        }
        xg_render_motion_forget_range(render.packets[i], render.packet_sizes[i]);
        geometry_key[i][0] = header;
        geometry_key[i][1] = vertices;
        geometry_key[i][2] = topology;
        geometry_key[i][3] = count;
        geometry_key[i][4] = slot->geometry[i];
        topology_sizes[i] = cursor - topology;
    }
    slot->pose.entity_id = UINT64_C(0x4745415200000000) | (skeleton & 0x1fffffffu);
    slot->pose.geometry_id = xg_render_resource_digest(geometry_key, sizeof(geometry_key));
    slot->pose.geometry_generation = 1;
    slot->pose.camera_id = UINT64_C(0x4745415200000000) | (camera & 0x1fffffffu);
    slot->pose.geometry_scale = 1;
    slot->pose.screen_offset[0] = (int32_t)cpu->gte_ctrl[24] / 65536.0;
    slot->pose.screen_offset[1] = (int32_t)cpu->gte_ctrl[25] / 65536.0;
    slot->pose.projection_distance = (uint16_t)cpu->gte_ctrl[26];
    /* Camera-only frames may reuse a consumed skeleton revision. Publication
     * belongs to this source frame, not to the frame that last rebuilt bones. */
    XgRenderMotionSource publication = *source;
    publication.discontinuity |= slot->source.discontinuity;
    if (!xg_render_motion_publish(&publication, &slot->pose, &render.pose)) {
        reject(XG_RENDER_GEAR_MOTION_PUBLICATION);
        return;
    }
    slot->published = true;
    for (uint32_t i = 1; i < slot->pose.node_count; ++i) {
        if (!render.models[i])
            continue;
        const uint32_t addresses[3] = {render.models[i], geometry_key[i][1], geometry_key[i][2]};
        const uint32_t sizes[3] = {0x38, geometry_key[i][3] * 8, topology_sizes[i]};
        for (unsigned j = 0; j < 3; ++j) {
            if (!sizes[j])
                continue;
            if (!xg_render_motion_watch(render.pose, addresses[j], sizes[j])) {
                reject(XG_RENDER_GEAR_MOTION_PUBLICATION);
                return;
            }
            services->watch(addresses[j], sizes[j]);
        }
    }
    if (wrapper) {
        if (!xg_render_motion_watch(render.pose, model, 8)) {
            reject(XG_RENDER_GEAR_MOTION_PUBLICATION);
            return;
        }
        services->watch(model, 8);
    }
    render.valid = true;
    ++diagnostics.published_poses;
}

void xg_render_gear_motion_observe(CPUState *cpu, uint32_t pc,
                                    const XgRenderGearMotionServices *services) {
    XgRenderMotionSource source = {0};
    pc = (pc & UINT32_C(0x1fffffff)) | UINT32_C(0x80000000);
    if (!cpu || !cpu->read_word || !cpu->read_half || !cpu->read_byte || !services ||
        !services->source || !services->range || !services->watch ||
        !services->source(pc, &source)) {
        cancel_render();
        reject(XG_RENDER_GEAR_MOTION_AUTHORITY);
        return;
    }
    switch (pc) {
    case 0x801dc5c0u:
    case 0x801dc848u:
        rebuild_begin(cpu, pc, services, &source);
        break;
    case 0x801dc840u:
    case 0x801dcc2cu:
        rebuild_end(cpu, pc);
        break;
    case 0x801dcc3cu:
    case 0x801dcec8u:
        render_begin(cpu, pc, services, &source);
        if (!render.valid)
            cancel_render();
        break;
    case 0x801dcd84u:
    case 0x801ddbf0u:
        if (cpu->gpr[29] != render.entry_sp || !same_address(cpu->gpr[31], render.return_pc))
            cancel_render();
        render.valid = false;
        render.pending = false;
        render.consumed = NULL;
        render.pose = (XgRenderMotionRef){0};
        break;
    case 0x801dcd8cu: {
        GearConsumedPose *slot = find_slot(cpu->gpr[4]);
        if (slot)
            clear_slot(slot);
        break;
    }
    default:
        break;
    }
}

bool xg_render_gear_motion_bind(CPUState *cpu, XgRenderMotionRef *out_pose, uint32_t *out_part) {
    if (!cpu || !out_pose || !out_part || !render.valid || !render.consumed)
        return false;
    const uint32_t caller = cpu->gpr[31], skeleton = render.consumed->skeleton;
    const bool wrapper = same_address(caller, 0x801dd43cu);
    if ((!wrapper && !same_address(caller, 0x801dcd48u)) ||
        wrapper != (render.render_pc == 0x801dcec8u) ||
        cpu->gpr[29] != render.entry_sp - (wrapper ? 0xe0u : 0x38u))
        return false;
    const uint32_t joint = cpu->gpr[wrapper ? 18 : 17];
    const uint32_t delta = (joint & 0x1fffffffu) - (skeleton & 0x1fffffffu);
    const uint32_t part = delta / JOINT_SIZE;
    diagnostics.last_joint = joint;
    if (delta % JOINT_SIZE || !part || part >= render.consumed->pose.node_count ||
        !same_address(cpu->gpr[4], render.models[part]) ||
        !same_address(cpu->gpr[5], render.packets[part]) || cpu->gpr[7] != 1) {
        cancel_render();
        return reject(XG_RENDER_GEAR_MOTION_SCOPE);
    }
    *out_pose = render.pose;
    *out_part = part;
    ++diagnostics.bound_parts;
    return true;
}

void xg_render_gear_motion_reset(void) {
    if (!active_slots && !render.pending)
        return;
    cancel_render();
    for (unsigned i = 0; active_slots && i < GEAR_SLOTS; ++i)
        clear_slot(&slots[i]);
    /* Diagnostics and the monotonic rebuild revision survive reset, as before.
     * Invalid slot keys/scope prevent stale capture reuse without clearing payloads. */
}

void xg_render_gear_motion_invalidate(uint32_t address, uint32_t size) {
    /* Dynamic bone matrices and tracks are EXPECTED writes. Only executable
     * helper code invalidates consumed snapshots here; geometry has its own watches. */
    const uint64_t start = address & 0x1fffffffu, end = start + size;
    if (size && start < 0x1e8638u && end > 0x1dc000u)
        xg_render_gear_motion_reset();
}

void xg_render_gear_motion_diagnostics(XgRenderGearMotionDiagnostics *out) {
    if (out)
        *out = diagnostics;
}
