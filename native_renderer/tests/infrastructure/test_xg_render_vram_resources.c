#include "xg_render_vram_resources.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint8_t memory[0x10000];
static uint16_t canonical_vram[1024u * 512u];

static bool authorize_range(uint32_t address, uint32_t size,
                            uint32_t alignment, bool allow_scratchpad) {
    (void)allow_scratchpad;
    return size != 0u && alignment != 0u &&
        (address & (alignment - 1u)) == 0u &&
        address <= sizeof(memory) && size <= sizeof(memory) - address;
}

static uint16_t read_half(uint32_t address) {
    return (uint16_t)(memory[address] | (uint16_t)memory[address + 1u] << 8u);
}

static uint8_t read_byte(uint32_t address) {
    return memory[address];
}

static void write_half(uint32_t address, uint16_t value) {
    memory[address] = (uint8_t)value;
    memory[address + 1u] = (uint8_t)(value >> 8u);
}

static void prepare_upload(CPUState *cpu, uint32_t rect, uint32_t payload,
                           uint16_t x, uint16_t y,
                           uint16_t width, uint16_t height,
                           uint8_t seed) {
    const uint32_t byte_count = width * height * 2u;

    write_half(rect, x);
    write_half(rect + 2u, y);
    write_half(rect + 4u, width);
    write_half(rect + 6u, height);
    for (uint32_t index = 0u;
         index < byte_count && payload + index < sizeof(memory); ++index)
        memory[payload + index] = (uint8_t)(seed + index);
    if ((uint32_t)x + width <= 1024u && (uint32_t)y + height <= 512u &&
        payload <= sizeof(memory) && byte_count <= sizeof(memory) - payload) {
        for (uint32_t index = 0u; index < width * height; ++index) {
            canonical_vram[((uint32_t)y + index / width) * 1024u +
                           x + index % width] =
                (uint16_t)memory[payload + index * 2u] |
                (uint16_t)memory[payload + index * 2u + 1u] << 8u;
        }
    }
    cpu->gpr[4] = rect;
    cpu->gpr[5] = payload;
}

static void authorize_services(XgRenderVramResourceServices *services,
                               uint64_t owner_generation) {
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = 5001u,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = owner_generation,
        .source = {
            .source_class = XG_RENDER_RESOURCE_SOURCE_UI_GENERATED,
            .range_size = 1u,
            .range_content_digest = 5001u,
        },
    };

    metadata.source.identity.bytes[0] = 1u;
    assert(xg_render_resource_capability_register(
               &metadata, &services->provenance) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
}

static void test_rejected_loader_transactions(void) {
    XgRenderVramResourceServices services = {
        .authorize_guest_range = authorize_range,
    };
    CPUState cpu = {
        .read_half = read_half,
        .read_byte = read_byte,
    };
    XgRenderResourceDiagnostics diagnostics;
    XgRenderResourceView shared_view;
    XgSemanticResourceRef shared;
    const uint64_t owner_generation = 70u;

    xg_render_resource_repository_reset();
    xg_render_vram_resources_reset();
    authorize_services(&services, owner_generation);

    assert(xg_render_vram_image_begin(
               GUEST_RENDER_RENDER_NATIVE, owner_generation) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x100u, 0x200u, 32u, 32u, 4u, 2u, 1u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, owner_generation,
               &services) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x120u, 0x240u, 48u, 48u, 4u, 2u, 2u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, owner_generation,
               &services) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0xfff8u, 64u, 64u, 4u, 2u, 3u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, owner_generation,
               &services) == XG_RENDER_VRAM_RESOURCE_INVALID_RANGE);
    assert(xg_render_vram_resources_commit(owner_generation) ==
           XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED);
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.live_resources == 0u);

    for (uint32_t iteration = 0u;
         iteration < XG_RENDER_RESOURCE_REPOSITORY_CAPACITY + 1u;
         ++iteration) {
        assert(xg_render_vram_image_begin(
                   GUEST_RENDER_RENDER_NATIVE, owner_generation) ==
               XG_RENDER_VRAM_RESOURCE_OK);
        prepare_upload(&cpu, 0x100u, 0x200u, 96u, 96u, 4u, 2u, 0u);
        memcpy(&memory[0x200u], &iteration, sizeof(iteration));
        assert(xg_render_vram_resources_upload(
                   &cpu, XG_RENDER_RESOURCE_TEXTURE, owner_generation,
                   &services) == XG_RENDER_VRAM_RESOURCE_OK);
        prepare_upload(&cpu, 0x140u, 0xfff8u, 112u, 112u, 4u, 2u, 0u);
        assert(xg_render_vram_resources_upload(
                   &cpu, XG_RENDER_RESOURCE_TEXTURE, owner_generation,
                   &services) == XG_RENDER_VRAM_RESOURCE_INVALID_RANGE);
        assert(xg_render_vram_resources_commit(owner_generation) ==
               XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED);
    }
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.live_resources == 0u);

    assert(xg_render_vram_image_begin(
               GUEST_RENDER_RENDER_NATIVE, owner_generation) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x100u, 0x200u, 128u, 128u, 4u, 2u, 21u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, owner_generation,
               &services) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_commit(owner_generation) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 128u, 128u, 4u, 2u, &shared));

    assert(xg_render_vram_image_begin(
               GUEST_RENDER_RENDER_NATIVE, owner_generation) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x100u, 0x200u, 160u, 160u, 4u, 2u, 21u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, owner_generation,
               &services) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0xfff8u, 176u, 176u, 4u, 2u, 0u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, owner_generation,
               &services) == XG_RENDER_VRAM_RESOURCE_INVALID_RANGE);
    assert(xg_render_vram_resources_commit(owner_generation) ==
           XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED);
    assert(xg_render_resource_view(
               (XgRenderResourceHandle){
                   shared.resource_id, shared.generation},
               &shared_view) == XG_RENDER_RESOURCE_OK);
    assert(shared_view.current);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 128u, 128u, 4u, 2u, &shared));
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.live_resources == 1u);

    xg_render_vram_resources_reset();
    xg_render_resource_repository_reset();
}

int main(void) {
    XgRenderVramResourceServices services = {
        .authorize_guest_range = authorize_range,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = 5001u,
        },
    };
    CPUState cpu = {
        .read_half = read_half,
        .read_byte = read_byte,
    };
    XgRenderVramResourceSnapshot snapshot;
    XgSemanticResourceRef clut;
    XgSemanticResourceRef texture;
    XgRenderResourceView view;
    XgRenderVramResourceResolvedResources resolved;
    XgSemanticResourceRef saved_texture;
    XgSemanticResourceRef future_texture;
    XgSemanticResourceRef restored_texture;
    uint8_t *checkpoint;
    size_t checkpoint_size;
    uint32_t checkpoint_publications;
    uint64_t publication_count;
    uint64_t completed_loaders;
    uint64_t rejected_operations;
    uint64_t checkpoint_capability;
    uint32_t published_cluts;
    XgRenderVramResourceServices unauthenticated_services = services;
    XgRenderIrNativePrimitive primitive = {
        .material = {
            .texture_page_x = 10u,
            .texture_page_y = 1u,
            .clut_x = 256u,
            .clut_y = 243u,
            .texture_depth = XG_RENDER_IR_TEXTURE_4_BIT,
            .textured = true,
        },
        .triangle_count = 1u,
        .triangles = {{
            .vertices = {
                {.u = 4 << 16u, .v = 0 << 16u},
                {.u = 11 << 16u, .v = 0 << 16u},
                {.u = 4 << 16u, .v = 1 << 16u},
            },
        }},
    };

    test_rejected_loader_transactions();
    xg_render_resource_repository_reset();
    xg_render_vram_resources_reset();
    authorize_services(&services, 7u);
    cpu.gpr[4] = 0x100u;
    assert(xg_render_vram_resources_begin(
               &cpu, GUEST_RENDER_RENDER_NATIVE, 7u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x120u, 0x200u, 256u, 243u, 16u, 1u, 3u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_CLUT, 7u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_CLUT, 256u, 243u, 16u, 1u, &clut));
    prepare_upload(&cpu, 0x140u, 0x240u, 640u, 256u, 4u, 2u, 9u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, 7u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 640u, 256u, 1u, 1u, &texture));
    assert(xg_render_vram_resources_commit(7u) == XG_RENDER_VRAM_RESOURCE_OK);
    xg_render_vram_resources_snapshot(&snapshot);
    assert(!snapshot.loader_active && !snapshot.loader_blocked);
    assert(snapshot.completed_loaders == 1u);
    assert(snapshot.published_images == 1u);
    assert(snapshot.published_cluts == 1u);
    assert(snapshot.publication_count == 2u);

    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_CLUT, 256u, 243u, 16u, 1u, &clut));
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 641u, 257u, 2u, 1u, &texture));
    assert(texture.resource_id != clut.resource_id);
    assert(xg_render_resource_view(
               (XgRenderResourceHandle){texture.resource_id,
                                        texture.generation}, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current && view.has_identity);
    assert(view.kind == XG_RENDER_RESOURCE_TEXTURE);
    assert(view.descriptor.version == XG_RENDER_RESOURCE_DESCRIPTOR_VERSION);
    assert(view.descriptor.pixel_format ==
           XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555);
    assert(view.descriptor.width == 4u && view.descriptor.height == 2u);
    assert(view.descriptor.row_pitch == 8u);
    assert(view.descriptor.vram_x == 640u && view.descriptor.vram_y == 256u);
    assert(view.descriptor.vram_width == 4u &&
           view.descriptor.vram_height == 2u);
    assert(view.descriptor.sampler == XG_RENDER_RESOURCE_SAMPLER_NEAREST);
    assert(view.descriptor.wrap_u == XG_RENDER_RESOURCE_WRAP_CLAMP &&
           view.descriptor.wrap_v == XG_RENDER_RESOURCE_WRAP_CLAMP);
    assert((view.descriptor.flags &
            XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u);
    assert(view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(view.provenance.receipt == 5001u);
    assert(!view.provenance.synthetic);
    assert(xg_render_vram_resources_resolve_draw(&primitive, &resolved));
    assert(resolved.has_texture && resolved.has_clut);
    assert(resolved.texture.resource_id == texture.resource_id);
    assert(resolved.clut.resource_id == clut.resource_id);

    cpu.gpr[4] = 0x100u;
    assert(xg_render_vram_resources_begin(
               &cpu, GUEST_RENDER_RENDER_NATIVE, 7u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x280u, 642u, 257u, 1u, 1u, 33u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, 7u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_commit(7u) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 641u, 257u, 2u, 1u, &texture));
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 642u, 257u, 1u, 1u, &texture));

    authorize_services(&services, 8u);
    cpu.gpr[4] = 0x100u;
    assert(xg_render_vram_resources_begin(
               &cpu, GUEST_RENDER_RENDER_NATIVE, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0xfff8u, 640u, 256u, 4u, 2u, 9u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_INVALID_RANGE);
    assert(xg_render_vram_resources_commit(8u) ==
           XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 640u, 256u, 1u, 1u, &texture));
    xg_render_vram_resources_snapshot(&snapshot);
    assert(snapshot.rejected_operations == 2u);

    assert(xg_render_vram_image_begin(
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x280u, 768u, 300u, 4u, 2u, 55u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 768u, 300u, 1u, 1u, &texture));
    assert(xg_render_vram_resources_commit(8u) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 768u, 300u, 1u, 1u, &texture));

    assert(xg_render_vram_clut_begin(
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x280u, 128u, 250u, 128u, 1u, 77u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_CLUT, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_commit(8u) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_CLUT, 144u, 250u, 16u, 1u, &clut));

    assert(xg_render_vram_stream_begin(
               XG_RENDER_RESOURCE_TEXTURE,
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x280u, 512u, 320u, 8u, 2u, 91u);
    assert(xg_render_vram_stream_upload(&cpu, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x300u, 512u, 322u, 8u, 3u, 123u);
    assert(xg_render_vram_stream_upload(&cpu, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 512u, 321u, 8u, 2u, &texture));
    assert(xg_render_vram_stream_finish(8u) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 512u, 321u, 8u, 2u, &texture));
    assert(xg_render_resource_view(
               (XgRenderResourceHandle){texture.resource_id,
                                        texture.generation}, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.byte_count == 8u * 5u * 2u);
    assert(((const uint8_t *)view.bytes)[0] == 91u);
    assert(((const uint8_t *)view.bytes)[8u * 2u * 2u] == 123u);

    assert(xg_render_vram_stream_begin(
               XG_RENDER_RESOURCE_CLUT,
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x280u, 256u, 260u, 16u, 1u, 17u);
    assert(xg_render_vram_stream_upload(&cpu, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x300u, 256u, 262u, 16u, 1u, 19u);
    assert(xg_render_vram_stream_upload(&cpu, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_INVALID_RANGE);
    assert(xg_render_vram_stream_finish(8u) ==
           XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_CLUT, 256u, 260u, 16u, 1u, &clut));

    assert(xg_render_vram_clut_begin(
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x1000u, 0u, 432u, 256u, 64u, 29u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_CLUT, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_CLUT, 0u, 432u, 256u, 64u, &clut));
    assert(xg_render_vram_resources_commit(8u) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_CLUT, 0u, 432u, 256u, 64u, &clut));
    assert(xg_render_resource_view(
               (XgRenderResourceHandle){clut.resource_id, clut.generation},
               &view) == XG_RENDER_RESOURCE_OK);
    assert(view.byte_count == 256u * 64u * 2u);
    assert(view.descriptor.pixel_format ==
           XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555);
    assert(view.descriptor.vram_x == 0u && view.descriptor.vram_y == 432u);
    assert(view.descriptor.vram_width == 256u &&
           view.descriptor.vram_height == 64u);
    assert(((const uint8_t *)view.bytes)[0] == 29u);

    xg_render_vram_resources_snapshot(&snapshot);
    publication_count = snapshot.publication_count;
    published_cluts = snapshot.published_cluts;
    assert(xg_render_vram_clut_begin(
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x180u, 0x2000u, 0u, 496u, 256u, 15u, 41u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_CLUT, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_CLUT, 0u, 496u, 256u, 15u, &clut));
    assert(xg_render_vram_resources_commit(8u) == XG_RENDER_VRAM_RESOURCE_OK);
    xg_render_vram_resources_snapshot(&snapshot);
    assert(snapshot.publication_count == publication_count + 1u);
    assert(snapshot.published_cluts == published_cluts + 1u);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_CLUT, 0u, 496u, 256u, 15u, &clut));
    assert(xg_render_resource_view(
               (XgRenderResourceHandle){clut.resource_id, clut.generation},
               &view) == XG_RENDER_RESOURCE_OK);
    assert(view.byte_count == 256u * 15u * 2u);
    assert(((const uint8_t *)view.bytes)[0] == 41u);

    xg_render_vram_resources_snapshot(&snapshot);
    completed_loaders = snapshot.completed_loaders;
    rejected_operations = snapshot.rejected_operations;
    assert(xg_render_vram_image_begin(
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_commit_optional(8u) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    xg_render_vram_resources_snapshot(&snapshot);
    assert(!snapshot.loader_active);
    assert(snapshot.completed_loaders == completed_loaders);
    assert(snapshot.rejected_operations == rejected_operations);

    assert(xg_render_vram_image_begin(
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x280u, 320u, 128u, 8u, 2u, 101u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x180u, 0x300u, 384u, 160u, 4u, 3u, 151u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u, &texture));
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 384u, 160u, 4u, 3u, &texture));
    assert(xg_render_vram_resources_commit_optional(8u) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u, &texture));
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 384u, 160u, 4u, 3u, &texture));

    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u, &saved_texture));
    xg_render_vram_resources_snapshot(&snapshot);
    checkpoint_publications = snapshot.publication_count;
    checkpoint_size = xg_render_vram_resources_checkpoint_size();
    checkpoint = (uint8_t *)malloc(checkpoint_size);
    assert(checkpoint != NULL);
    assert(xg_render_vram_resources_checkpoint_write(
               50u, checkpoint, checkpoint_size) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_resource_view(
               (XgRenderResourceHandle){saved_texture.resource_id,
                                        saved_texture.generation},
               &view) == XG_RENDER_RESOURCE_OK);
    checkpoint_capability = view.provenance.capability;

    assert(xg_render_vram_image_begin(
               GUEST_RENDER_RENDER_NATIVE, 8u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x280u, 320u, 128u, 8u, 2u, 201u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, 8u, &services) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_commit(8u) == XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u, &future_texture));
    assert(future_texture.generation != saved_texture.generation);

    xg_render_resource_repository_reset();
    xg_render_vram_resources_reset();
    assert(xg_render_vram_resources_checkpoint_restore(
               checkpoint, checkpoint_size, 51u, 9u) ==
           XG_RENDER_VRAM_RESOURCE_OK);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u,
        &restored_texture));
    assert(restored_texture.generation != saved_texture.generation);
    assert(restored_texture.generation != future_texture.generation);
    assert(xg_render_resource_view(
               (XgRenderResourceHandle){restored_texture.resource_id,
                                        restored_texture.generation},
               &view) == XG_RENDER_RESOURCE_OK);
    assert(((const uint8_t *)view.bytes)[0] == 101u);
    assert(view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(view.provenance.receipt == 5001u);
    assert(view.provenance.capability != checkpoint_capability);
    assert(!view.provenance.synthetic);
    assert(view.descriptor.version == XG_RENDER_RESOURCE_DESCRIPTOR_VERSION);
    assert(view.descriptor.pixel_format ==
           XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555);
    assert(view.descriptor.vram_x == 320u && view.descriptor.vram_y == 128u);
    assert(view.descriptor.width == 8u && view.descriptor.height == 2u);
    assert((uint8_t)canonical_vram[128u * 1024u + 320u] == 201u);
    xg_render_vram_resources_note_vram_mutation(
        320u, 128u, 8u, 2u, 52u, view.content_digest, true);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u,
        &restored_texture));
    xg_render_vram_resources_snapshot(&snapshot);
    assert(snapshot.vram_generation == 52u);
    assert(snapshot.owner_generation == 9u);
    assert(snapshot.publication_count == checkpoint_publications);
    xg_render_vram_resources_note_vram_mutation(
        0u, 0u, 1u, 1u, 53u, 0u, false);
    assert(xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u,
        &restored_texture));
    xg_render_vram_resources_note_vram_mutation(
        319u, 128u, 2u, 1u, 54u, 0u, false);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u, &texture));

    xg_render_resource_repository_reset();
    xg_render_vram_resources_reset();
    checkpoint[40u + 108u] ^= 1u;
    assert(xg_render_vram_resources_checkpoint_restore(
               checkpoint, checkpoint_size, 52u, 10u) ==
           XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED);
    checkpoint[40u + 108u] ^= 1u;
    checkpoint[checkpoint_size - 1u] ^= 1u;
    assert(xg_render_vram_resources_checkpoint_restore(
               checkpoint, checkpoint_size, 52u, 10u) ==
           XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED);
    assert(!xg_render_vram_resources_lookup(
        XG_RENDER_RESOURCE_TEXTURE, 320u, 128u, 8u, 2u, &texture));
    free(checkpoint);

    xg_render_vram_resources_reset();
    memset(&unauthenticated_services.provenance, 0,
           sizeof(unauthenticated_services.provenance));
    cpu.gpr[4] = 0x100u;
    assert(xg_render_vram_resources_begin(
               &cpu, GUEST_RENDER_RENDER_NATIVE, 11u,
               &unauthenticated_services) ==
           XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT);
    assert(xg_render_vram_image_begin(
               GUEST_RENDER_RENDER_NATIVE, 11u) == XG_RENDER_VRAM_RESOURCE_OK);
    prepare_upload(&cpu, 0x140u, 0x280u, 640u, 256u, 4u, 2u, 9u);
    assert(xg_render_vram_resources_upload(
               &cpu, XG_RENDER_RESOURCE_TEXTURE, 11u,
               &unauthenticated_services) ==
           XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT);
    assert(xg_render_vram_resources_commit(11u) ==
           XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED);
    return 0;
}
