#include "cpu_state.h"
#include "gpu.h"
#include "xg_field_particles.h"
#include "xg_field_projected.h"
#include "xg_render_backend.h"
#include "xg_render_primitive_utils.h"

#undef NDEBUG
#include <assert.h>
#include <string.h>

/* Exercise the real producers with guest source memory and recording sinks.
 * No GPU packets are used as an oracle for temporal identity. */
static unsigned char ram[0x200000];
static bool work_mode = true;
static struct {
    uint32_t packet, producer, primitive;
    GpuRenderSemantic semantic;
} draws[32];
static uint32_t draw_count, rejected;
static XgRenderTemporalComponent coverage;
static XgRenderTemporalSample samples[44];
static uint32_t sample_count;
static uint32_t hidden_producer, hidden_primitive;

static uint8_t read_byte(uint32_t address) {
    address &= 0x1fffffffu;
    assert(address < sizeof(ram));
    return ram[address];
}
static uint16_t read_half(uint32_t address) {
    return read_byte(address) | ((uint16_t)read_byte(address + 1u) << 8u);
}
static uint32_t read_word(uint32_t address) {
    return read_half(address) | ((uint32_t)read_half(address + 2u) << 16u);
}
static void write_byte(uint32_t address, uint8_t value) {
    address &= 0x1fffffffu;
    assert(address < sizeof(ram));
    ram[address] = value;
}
static void write_half(uint32_t address, uint16_t value) {
    write_byte(address, (uint8_t)value); write_byte(address + 1u, (uint8_t)(value >> 8u));
}
static void write_word(uint32_t address, uint32_t value) {
    write_half(address, (uint16_t)value); write_half(address + 2u, (uint16_t)(value >> 16u));
}
static CPUState cpu_state(void) {
    CPUState cpu = {0};
    cpu.read_byte = read_byte; cpu.read_half = read_half; cpu.read_word = read_word;
    cpu.write_byte = write_byte; cpu.write_half = write_half; cpu.write_word = write_word;
    cpu.gpr[29] = 0x801f0000u; cpu.gpr[31] = 0x80012340u;
    cpu.gte_ctrl[0] = cpu.gte_ctrl[2] = cpu.gte_ctrl[4] = 4096u;
    cpu.gte_ctrl[24] = 160u << 16u; cpu.gte_ctrl[25] = 112u << 16u;
    cpu.gte_ctrl[26] = 128u; cpu.gte_ctrl[30] = 256u;
    return cpu;
}
static void matrix(uint32_t address) {
    for (uint32_t i = 0u; i < 8u; ++i) write_word(address + i * 4u, 0u);
    write_half(address, 4096u); write_half(address + 8u, 4096u); write_half(address + 16u, 4096u);
}
void gpu_get_draw_state(GpuDrawState *out) {
    *out = (GpuDrawState){.right = 319u, .bottom = 239u};
}
bool xg_render_submission_native_work_mode(void) { return work_mode; }
uint64_t xg_render_submission_temporal_scene(void) { return 42u; }
static bool stage(const XgRenderIrNativePrimitive *p, uint32_t packet,
                  uint32_t source, uint32_t producer, uint32_t primitive) {
    (void)source;
    assert(draw_count < 32u);
    draws[draw_count].packet = packet;
    draws[draw_count].producer = producer;
    draws[draw_count].primitive = primitive;
    assert(xg_render_backend_translate_primitive(p, &draws[draw_count].semantic) == XG_RENDER_BACKEND_OK);
    xg_render_semantic_set_corner_identities(&draws[draw_count].semantic, producer, primitive);
    ++draw_count;
    return true;
}
static bool stage_active(const XgRenderIrNativePrimitive *p, uint32_t packet, uint32_t source,
        uint32_t bucket, uint8_t words, uint32_t producer, uint32_t primitive, uint32_t *failure) {
    (void)bucket; (void)words; (void)failure;
    return stage(p, packet, source, producer, primitive);
}
static bool temporal(const XgRenderIrNativePrimitive *p, uint32_t producer,
        uint32_t primitive, const GpuRenderTemporalCullPolicy *policy) {
    (void)p; (void)policy;
    hidden_producer = producer; hidden_primitive = primitive;
    return true;
}
static void reject(uint32_t reason) { rejected = reason; }
static bool not_active(void) { return false; }
static bool space_available(uint32_t count) { return count <= 11u; }
static bool prescene(const XgRenderPreScenePrimitive *p) {
    assert(!p->temporal_only);
    return stage(&p->primitive, p->packet_address, p->source_primitive_index,
        p->interpolation_producer_id, p->interpolation_primitive_id);
}
static bool begin_lifecycle(uint32_t pc, XgRenderProducerLifecycle *out) {
    (void)pc; *out = (XgRenderProducerLifecycle){0}; return true;
}
static bool has_template(uint32_t packet) { (void)packet; return true; }
static uint32_t template_space(void) { return 32u; }
static bool capture_template(const XgRenderFieldSpriteTemplateInput *p) {
    for (uint32_t i = 0u; i < draw_count; ++i)
        if (draws[i].packet == p->packet_address) {
            assert(draws[i].producer == p->interpolation_producer_id);
            assert(draws[i].primitive == p->interpolation_primitive_id);
            return true;
        }
    assert(false); return false;
}
bool xg_render_submission_publish_temporal_coverage(uint32_t scope,
        const XgRenderTemporalComponent *components, uint32_t component_count,
        const XgRenderTemporalSample *vertices, uint32_t vertex_count,
        const XgRenderTemporalCommandBinding *bindings, uint32_t binding_count) {
    assert(component_count == 1u && vertex_count <= 44u && binding_count == draw_count);
    assert(scope == components[0].producer_id);
    coverage = components[0]; sample_count = vertex_count;
    memcpy(samples, vertices, vertex_count * sizeof(*vertices));
    for (uint32_t i = 0u; i < binding_count; ++i) {
        assert(bindings[i].command_id == draws[i].packet + 4u);
        assert(bindings[i].component_id == coverage.component_id);
        for (uint32_t c = 0u; c < 4u; ++c) {
            const GpuRenderSemanticVertex *v = c < 3u ? &draws[i].semantic.triangles[0].vertices[c]
                : &draws[i].semantic.triangles[1].vertices[2];
            uint32_t j = 0u;
            while (j < vertex_count && vertices[j].vertex.interpolation_vertex_id != v->interpolation_vertex_id) ++j;
            assert(j < vertex_count && vertices[j].vertex.x == v->x && vertices[j].vertex.y == v->y);
        }
    }
    return true;
}

/* The translation unit also contains renderer submission entry points; these
 * tests only call its pure translation functions. Unexpected submission fails. */
GpuRenderTransactionStatus gr_commit_validate(GpuRenderTransactionId id, uint64_t generation, const GpuRenderPresent *present) { (void)id; (void)generation; (void)present; assert(false); return GPU_RENDER_TRANSACTION_BACKEND_ERROR; }
GpuRenderTransactionStatus gr_draw_semantic(GpuRenderTransactionId id, const GpuRenderSemantic *s) { (void)id; (void)s; assert(false); return GPU_RENDER_TRANSACTION_BACKEND_ERROR; }
GpuRenderTransactionStatus gr_ordering_barrier(GpuRenderTransactionId id) { (void)id; assert(false); return GPU_RENDER_TRANSACTION_BACKEND_ERROR; }
GpuRenderTransactionStatus gr_rollback(GpuRenderTransactionId id) { (void)id; assert(false); return GPU_RENDER_TRANSACTION_BACKEND_ERROR; }
GpuRenderTransactionStatus gr_transaction_begin(GpuRenderTransactionId id, uint64_t generation) { (void)id; (void)generation; assert(false); return GPU_RENDER_TRANSACTION_BACKEND_ERROR; }

static void test_particles(void) {
    const XgFieldParticlePipelineServices services = {stage_active, temporal, reject};
    const int16_t table[12] = {-1,-2,1,2,10,20,30,40,50,60,70,80};
    CPUState cpu = cpu_state();
    xg_field_particles_reset();
    matrix(0x80012000u);
    for (uint32_t i = 0u; i < 12u; ++i) write_half(0x800af27cu + i * 2u, (uint16_t)table[i]);
    for (uint32_t i = 0u; i < 3u; ++i) write_word(0x80013000u + i * 4u, 4096u);
    write_word(0x800523f0u, 4096u << 16u);
    write_word(cpu.gpr[29] + 0x10u, 0x80013000u);
    write_word(0x800c426cu, 0x80015000u);
    uint32_t first_owner = 0u, first_life = 0u;
    for (uint32_t instance = 0u; instance < 3u; ++instance) {
        const uint32_t object = instance == 1u ? 0x80020200u : 0x80020000u;
        cpu.gpr[4] = object; cpu.gpr[5] = 0u; cpu.gpr[6] = 0u;
        assert(xg_field_particles_observe_initializer(&cpu, GUEST_RENDER_RENDER_NATIVE, true, 0x800a8eacu));
        XgRenderParticleSource source;
        assert(xg_field_particles_lookup(object, &source));
        for (uint32_t v = 0u; v < 4u; ++v) {
            write_half(object + 0xa0u + v * 8u, (uint16_t)source.x[v]);
            write_half(object + 0xa2u + v * 8u, (uint16_t)source.y[v]);
        }
        for (uint32_t v = 0u; v < 3u; ++v) write_half(object + 0x38u + v * 2u, 4096u);
        write_word(object + 0x10u, 1000u * 4096u);
        cpu.gpr[5] = 0x80012000u; cpu.gpr[7] = 0u;
        for (uint32_t parity = 0u; parity < 2u; ++parity) {
            draw_count = 0u;
            write_word(0x800adb08u, parity);
            write_word(object + 0x50u + parity * 0x28u, 0x09000000u);
            assert(xg_field_particles_cutover(&cpu, 0x800a9b54u, &services));
            assert(draw_count == 1u && draws[0].producer == object);
            assert(draws[0].primitive == source.generation);
            if (!instance && !parity) { first_owner = draws[0].producer; first_life = draws[0].primitive; }
            if (instance == 1u) assert(draws[0].producer != first_owner);
            if (instance == 2u) assert(draws[0].primitive != first_life);
        }
        cpu.gpr[7] = 1u; write_word(object + 0x10u, 0u);
        draw_count = 0u;
        assert(xg_field_particles_cutover(&cpu, 0x800a9b54u, &services));
        assert(!draw_count && hidden_producer == object && hidden_primitive == source.generation);
    }
}

static void test_projected_bands(void) {
    const uint32_t object = 0x80030000u;
    const XgFieldProjectedPipelineServices services = {
        .proof_active = not_active, .stage_active = stage_active, .stage_standalone = stage,
        .pre_scene_available = space_available, .stage_pre_scene = prescene, .stage_temporal = temporal,
        .begin_lifecycle = begin_lifecycle, .has_template = has_template,
        .available_template_capacity = template_space, .capture_template = capture_template, .reject_policy = reject,
    };
    CPUState cpu = cpu_state();
    xg_field_projected_reset();
    write_word(cpu.gpr[29] + 0x10u, 240u); write_word(cpu.gpr[29] + 0x14u, 240u);
    write_word(cpu.gpr[29] + 0x24u, 0x80016000u);
    xg_field_projected_observe_initializer_begin(&cpu, GUEST_RENDER_RENDER_NATIVE);
    cpu.gpr[29] -= 0xa0u; cpu.gpr[2] = object;
    write_word(cpu.gpr[29] + 0x9cu, cpu.gpr[31]);
    xg_field_projected_observe_initializer_commit(&cpu);
    cpu.gpr[29] += 0xa0u;
    for (uint32_t parity = 0u; parity < 2u; ++parity) {
        for (uint32_t i = 0u; i < 8u; ++i) write_word(object + parity * 0x140u + i * 0x28u, 0x09000000u);
        write_word(object + 0x280u + parity * 0x18u, 0x05000000u);
        write_word(object + 0x2b0u + parity * 0x18u, 0x05000000u);
        write_word(object + 0x2e0u + parity * 0x24u, 0x08000000u);
    }
    write_word(object + 0x328u, 256u); write_word(object + 0x32cu, 64u);
    write_half(object + 0x344u, 1u); write_half(object + 0x346u, 256u);
    matrix(0x80012000u);
    write_half(0x80016104u, 1000u);
    cpu.gpr[4] = object; cpu.gpr[5] = 0x80016020u; cpu.gpr[6] = 0x80016100u; cpu.gpr[7] = 0x80012000u;
    write_word(0x800ccb00u, 0x80014000u);
    write_word(0x800c426cu, 0x80015000u);
    write_word(cpu.gpr[29] + 0x10u, 0x80014074u);
    uint64_t geometry = 0u;
    for (uint32_t pass = 0u; pass < 3u; ++pass) {
        draw_count = 0u;
        cpu.gte_ctrl[25] = (pass == 1u ? 32u : 112u) << 16u;
        write_word(cpu.gpr[29] + 0x14u, pass & 1u);
        if (pass == 2u) write_word(cpu.gpr[29] + 0x10u, 0x800150d0u); /* Field OT. */
        assert(xg_field_projected_cutover(&cpu, 0x800273c4u, &services));
        /* Centered 256-pixel panorama repeats across 320 pixels: 32/256/32.
         * Only the upper band disappears; all six source quads remain known. */
        assert(draw_count == (pass == 1u ? 5u : 6u) && sample_count == 24u);
        if (!pass) geometry = coverage.geometry_id;
        assert(coverage.geometry_id == geometry);
        assert(draws[draw_count - 1u].primitive == 10u); /* Lower band, even when upper disappears. */
        bool upper_sample = false;
        for (uint32_t i = 0u; i < sample_count; ++i)
            upper_sample |= samples[i].vertex.interpolation_vertex_id == 8u * 4u;
        assert(upper_sample);
    }
}
int main(void) {
    test_particles();
    memset(ram, 0, sizeof(ram));
    test_projected_bands();
    assert(!rejected);
    return 0;
}
