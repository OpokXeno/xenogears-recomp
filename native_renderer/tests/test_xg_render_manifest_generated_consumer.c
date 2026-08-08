#include "xg_render_manifest_generated.h"

int main(void) {
    const XgRenderManifestValidation *validation =
        &xg_render_manifest_validation;
    const XgRenderManifestRecord *records = xg_render_manifest_records;

    if (xg_render_manifest_record_count == 0u || records[0].record_id == 0u ||
        validation->producer_record_id == 0u || validation->site_record_id == 0u ||
        validation->producer_entry == 0u || validation->caller_site == 0u ||
        validation->static_callee == 0u || validation->return_site == 0u ||
        validation->instruction_window_start == 0u ||
        validation->instruction_window_size < 8u ||
        validation->required_jal_opcode != 3u ||
        validation->jal_target != validation->static_callee ||
        validation->required_delay_slot_instructions != 1u ||
        validation->required_delay_slot_non_control_transfer != 1u)
        return 1;
    return 0;
}
