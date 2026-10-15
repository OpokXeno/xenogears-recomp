#include "xg_render_model_repository.h"

#undef NDEBUG
#include <assert.h>
#include <stddef.h>

int main(void) {
    XgRenderModelFt4Template crossing = {
        .packet_address = UINT32_C(0x8009eff0),
        .descriptor_address = UINT32_C(0x80060ffc),
        .valid = true,
    };
    XgRenderModelFt4Template neighbour = {
        .packet_address = UINT32_C(0x8009f080),
        .descriptor_address = UINT32_C(0x80062040),
        .valid = true,
    };
    xg_render_model_repository_reset_templates();
    assert(xg_render_model_repository_store_template(&crossing));
    assert(xg_render_model_repository_store_template(&neighbour));
    /* The byte immediately after the packet shares its last page. */
    xg_render_model_repository_invalidate_overlapping(
        crossing.packet_address + 0x28u, 1u, NULL);
    assert(xg_render_model_repository_packet_template_present(crossing.packet_address));
    /* Its last byte is on the next page; KSEG1 names the same RAM. */
    xg_render_model_repository_invalidate_overlapping(
        UINT32_C(0xa009f017), 1u, NULL);
    assert(!xg_render_model_repository_packet_template_present(crossing.packet_address));
    assert(!xg_render_model_repository_descriptor_template_present(crossing.descriptor_address));
    assert(xg_render_model_repository_packet_template_present(neighbour.packet_address));

    xg_render_model_repository_reset_templates();
    assert(xg_render_model_repository_store_template(&crossing));
    assert(xg_render_model_repository_store_template(&neighbour));
    xg_render_model_repository_invalidate_overlapping(
        UINT32_C(0xa0061007), 1u, NULL);
    assert(!xg_render_model_repository_packet_template_present(crossing.packet_address));
    assert(!xg_render_model_repository_descriptor_template_present(crossing.descriptor_address));
    assert(xg_render_model_repository_packet_template_present(neighbour.packet_address));
    assert(xg_render_model_repository_descriptor_template_present(neighbour.descriptor_address));
    xg_render_model_repository_reset_templates();
    return 0;
}
