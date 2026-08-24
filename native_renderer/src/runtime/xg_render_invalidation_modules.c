#include "xg_render_invalidation_dispatch.h"
#include "xg_render_invalidation_modules.h"
#include "xg_render_mutation_classifier.h"

#include "xg_field_particles.h"
#include "xg_field_projected.h"
#include "xg_field_zoom.h"
#include "xg_render_f4_sources.h"
#include "xg_render_field_character_pipeline.h"
#include "xg_render_field_polyline.h"
#include "xg_render_field_sprite.h"
#include "xg_render_instrumentation.h"
#include "xg_render_local_producer_auth.h"
#include "xg_render_model_repository.h"
#include "xg_render_model_sprite_pipeline.h"
#include "xg_render_overlay_ft4.h"
#include "xg_render_resident_line_f2.h"
#include "xg_render_residual.h"
#include "xg_render_resource_watch.h"
#include "xg_render_runtime_variant_auth.h"
#include "xg_render_submission.h"
#include "xg_render_ui_ot.h"
#include "xg_render_world_model_repository.h"
#include "xg_render_world_models_pipeline.h"
#include "xg_render_world_execution.h"
#include "xg_render_world_pending_services.h"
#include "xg_render_world_simple_producers.h"
#include "xg_render_world_sky_producer.h"

#include <stddef.h>

static const XgRenderInvalidationModule producer_modules[] = {
    { xg_render_ui_ot_handle_invalidation },
    { xg_render_instrumentation_handle_invalidation },
    { xg_render_field_character_handle_invalidation },
    { xg_render_submission_handle_invalidation },
    { xg_render_resource_watch_handle_invalidation },
    { xg_field_particles_handle_invalidation },
    { xg_field_zoom_handle_invalidation },
    { xg_render_local_producer_auth_handle_invalidation },
    { xg_field_projected_handle_invalidation },
    { xg_render_model_sprite_pipeline_handle_invalidation },
    { xg_render_field_sprite_handle_invalidation },
    { xg_render_f4_sources_handle_invalidation },
    { xg_render_model_repository_handle_invalidation },
    { xg_render_residual_handle_invalidation },
    { xg_render_overlay_ft4_handle_invalidation },
    { xg_render_field_polyline_handle_invalidation },
    { xg_render_resident_line_f2_handle_invalidation },
    { xg_render_world_simple_handle_invalidation },
    { xg_render_world_clouds_handle_invalidation },
    { xg_render_world_actor_handle_invalidation },
    { xg_render_world_models_handle_invalidation },
    { xg_render_world_model_repository_handle_invalidation },
    { xg_render_world_sky_handle_invalidation },
    { xg_render_world_execution_handle_invalidation },
    { xg_render_runtime_variant_handle_invalidation },
};

static const XgRenderMutationSource mutation_sources[] = {
    { xg_field_particles_classify_code_write,
      xg_field_particles_register_code_watches },
    { xg_field_zoom_classify_code_write,
      xg_field_zoom_register_code_watches },
    { xg_field_projected_classify_code_write,
      xg_field_projected_register_code_watches },
    { xg_render_model_sprite_pipeline_classify_code_write,
      xg_render_model_sprite_pipeline_register_code_watches },
    { xg_render_world_simple_classify_code_write,
      xg_render_world_simple_register_code_watches },
    { xg_render_world_sky_classify_code_write,
      xg_render_world_sky_register_code_watches },
    { xg_render_runtime_variant_classify_code_write,
      xg_render_runtime_variant_register_descriptor_code_watches },
};

bool xg_render_invalidation_modules_configure(void) {
    xg_render_invalidation_clear_modules();
    xg_render_mutation_classifier_clear_sources();
    for (uint32_t index = 0u;
         index < sizeof(producer_modules) / sizeof(producer_modules[0]);
         ++index)
        if (!xg_render_invalidation_register_module(&producer_modules[index]))
            goto rollback;
    for (uint32_t index = 0u;
         index < sizeof(mutation_sources) / sizeof(mutation_sources[0]);
         ++index)
        if (!xg_render_mutation_classifier_register_source(
                &mutation_sources[index]))
            goto rollback;
    return true;

rollback:
    xg_render_invalidation_clear_modules();
    xg_render_mutation_classifier_clear_sources();
    return false;
}
