if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(XgNativeSemanticRenderer C CXX)
    include(CTest)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    find_package(SDL2 REQUIRED)
    set(PSX_SDL_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}")
    set(PSX_SDL_LIBRARIES SDL2::SDL2)
endif()

get_filename_component(XG_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(XG_RENDER_PRIVATE_INCLUDE_DIRS
    src/auth
    src/core
    src/infrastructure
    src/producers/battle
    src/producers/battling
    src/producers/field
    src/producers/menu
    src/producers/model
    src/producers/resident
    src/producers/world
    src/runtime)

if(MSVC)
    # xg_render_auth_runtime.c / xg_render_instrumentation.c use <stdatomic.h>
    # (atomic_flag); MSVC only supports it under the experimental C11 atomics
    # switch (same scoping psxrecomp/runtime/runtime.cmake uses for
    # audio_trace.c). Source-file scoped so it reaches every target that
    # compiles these two files (the xg_render_auth library and the
    # test_xg_render_static_auth test executable alike).
    set_property(SOURCE
        src/runtime/xg_render_auth_runtime.c
        src/infrastructure/xg_render_instrumentation.c
        APPEND PROPERTY COMPILE_OPTIONS /experimental:c11atomics)
endif()

add_library(xg_visual_state STATIC src/core/xg_visual_state.c)
target_include_directories(xg_visual_state PUBLIC include)
target_compile_features(xg_visual_state PUBLIC c_std_11)

add_library(xg_host_3d STATIC src/core/xg_host_3d.c)
target_include_directories(xg_host_3d PUBLIC include PRIVATE
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_host_3d PUBLIC c_std_11)

add_library(xg_native_view STATIC src/core/xg_native_view.c)
target_include_directories(xg_native_view PUBLIC include)
target_compile_features(xg_native_view PUBLIC c_std_11)
target_link_libraries(xg_native_view PUBLIC xg_host_3d)

add_library(xg_render_ir STATIC src/core/xg_render_ir.c)
target_include_directories(xg_render_ir PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_render_ir PUBLIC c_std_11)

add_library(xg_render_quad_builder STATIC src/core/xg_render_quad_builder.c)
target_include_directories(xg_render_quad_builder PUBLIC include)
target_compile_features(xg_render_quad_builder PUBLIC c_std_11)
target_link_libraries(xg_render_quad_builder PUBLIC xg_render_ir)

add_library(xg_world_sky STATIC src/producers/world/xg_world_sky.c)
target_include_directories(xg_world_sky PUBLIC include)
target_compile_features(xg_world_sky PUBLIC c_std_11)
target_link_libraries(xg_world_sky PUBLIC xg_host_3d xg_render_quad_builder)

add_library(xg_world_horizon STATIC
    src/producers/world/xg_world_horizon.c
    src/producers/world/xg_world_horizon_source_capture.c)
target_include_directories(xg_world_horizon PUBLIC include)
target_compile_features(xg_world_horizon PUBLIC c_std_11)
target_link_libraries(xg_world_horizon PUBLIC
    xg_host_3d xg_native_view xg_render_quad_builder)

add_library(xg_world_effects STATIC
    src/producers/world/xg_world_effects.c
    src/producers/world/xg_world_effects_source_capture.c)
target_include_directories(xg_world_effects PUBLIC include)
target_compile_features(xg_world_effects PUBLIC c_std_11)
target_link_libraries(xg_world_effects PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_terrain_water STATIC
    src/producers/world/xg_world_terrain_water.c
    src/producers/world/xg_world_terrain_water_source_capture.c
    src/producers/world/xg_world_terrain_water_shadow.c)
target_include_directories(xg_world_terrain_water PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_terrain_water PUBLIC c_std_11)
target_link_libraries(xg_world_terrain_water PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_entity_shadows STATIC
    src/producers/world/xg_world_entity_shadows.c
    src/producers/world/xg_world_entity_shadows_source_capture.c
    src/producers/world/xg_world_entity_shadows_shadow.c)
target_include_directories(xg_world_entity_shadows PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_entity_shadows PUBLIC c_std_11)
target_link_libraries(xg_world_entity_shadows PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_decorations STATIC
    src/producers/world/xg_world_decorations.c
    src/producers/world/xg_world_decorations_source_capture.c
    src/producers/world/xg_world_decorations_shadow.c)
target_include_directories(xg_world_decorations PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_decorations PUBLIC c_std_11)
target_link_libraries(xg_world_decorations PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_clouds STATIC
    src/producers/world/xg_world_clouds.c
    src/producers/world/xg_world_clouds_source_capture.c
    src/producers/world/xg_world_clouds_shadow.c)
target_include_directories(xg_world_clouds PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_clouds PUBLIC c_std_11)
target_link_libraries(xg_world_clouds PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_minimap STATIC
    src/producers/world/xg_world_minimap.c
    src/producers/world/xg_world_minimap_source_capture.c
    src/producers/world/xg_world_minimap_shadow.c)
target_include_directories(xg_world_minimap PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_minimap PUBLIC c_std_11)
target_link_libraries(xg_world_minimap PUBLIC xg_host_3d xg_render_ir)

add_library(xg_model_ft4_raw STATIC src/producers/model/xg_model_ft4_raw.c)
target_include_directories(xg_model_ft4_raw PUBLIC include)
target_compile_features(xg_model_ft4_raw PUBLIC c_std_11)
target_link_libraries(xg_model_ft4_raw PUBLIC xg_host_3d xg_render_quad_builder)

add_library(xg_sprite_ft4 STATIC src/producers/model/xg_sprite_ft4.c)
target_include_directories(xg_sprite_ft4 PUBLIC include)
target_compile_features(xg_sprite_ft4 PUBLIC c_std_11)
target_link_libraries(xg_sprite_ft4 PUBLIC xg_host_3d xg_render_quad_builder)

add_library(xg_render_model_resources STATIC
    src/producers/model/xg_render_model_resources.c)
target_include_directories(xg_render_model_resources PUBLIC include)
target_compile_features(xg_render_model_resources PUBLIC c_std_11)
target_link_libraries(xg_render_model_resources PUBLIC xg_semantic_presentation)

add_library(xg_world_models STATIC
    src/producers/world/xg_world_models.c
    src/producers/world/xg_world_models_native.c)
target_include_directories(xg_world_models PUBLIC include)
target_compile_features(xg_world_models PUBLIC c_std_11)
target_link_libraries(xg_world_models PUBLIC xg_model_ft4_raw xg_host_3d)

add_library(xg_world_actor_sprites STATIC
    src/producers/world/xg_world_actor_sprites.c
    src/producers/world/xg_world_actor_sprites_source_capture.c)
target_include_directories(xg_world_actor_sprites PUBLIC include)
target_compile_features(xg_world_actor_sprites PUBLIC c_std_11)
target_link_libraries(xg_world_actor_sprites PUBLIC
    xg_sprite_ft4 xg_host_3d xg_render_quad_builder)

add_library(xg_render_backend STATIC src/core/xg_render_backend.c)
target_include_directories(xg_render_backend PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_render_backend PUBLIC c_std_11)
target_link_libraries(xg_render_backend PUBLIC xg_render_ir)

set(XG_RENDER_UI_OWNER_CATALOG_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/xg_render_ui_owner_catalog.c")
add_custom_command(
    OUTPUT "${XG_RENDER_UI_OWNER_CATALOG_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}"
        "${XG_REPOSITORY_ROOT}/tools/native_render_ui_owners.py" generate-c
        --catalog "${CMAKE_CURRENT_SOURCE_DIR}/xg_render_ui_owners.toml"
        --enum-header "${CMAKE_CURRENT_SOURCE_DIR}/include/xg_render_ui_resources.h"
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/xg_render_manifest.toml"
        --overlay-index "${XG_REPOSITORY_ROOT}/annotations/overlays/index.toml"
        --output "${XG_RENDER_UI_OWNER_CATALOG_SOURCE}"
    DEPENDS
        "${XG_REPOSITORY_ROOT}/tools/native_render_ui_owners.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/xg_render_ui_owners.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/xg_render_manifest.toml"
        "${XG_REPOSITORY_ROOT}/annotations/overlays/index.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/xg_render_ui_resources.h"
    VERBATIM)
set_source_files_properties("${XG_RENDER_UI_OWNER_CATALOG_SOURCE}"
    PROPERTIES GENERATED TRUE)

add_library(xg_semantic_presentation STATIC
    "${XG_RENDER_UI_OWNER_CATALOG_SOURCE}"
    src/core/xg_render_motion.c
    src/infrastructure/xg_render_semantic_compositor.c
    src/infrastructure/xg_render_source_frame.c
    src/infrastructure/xg_render_source_commit.c
    src/infrastructure/xg_render_native_work.c
    src/infrastructure/xg_render_semantic_presentation.c
    src/runtime/xg_render_presentation_host.c
    src/infrastructure/xg_render_resource_repository.c
    src/infrastructure/xg_render_surface_graph.c
    src/infrastructure/xg_render_ui_layout.c
    src/infrastructure/xg_render_vram_journal.c
    src/infrastructure/xg_render_ui_resources.c
    src/producers/movie/xg_render_movie_publisher.c)
target_include_directories(xg_semantic_presentation PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" src/runtime)
target_compile_features(xg_semantic_presentation PUBLIC c_std_11)
target_link_libraries(xg_semantic_presentation PUBLIC xg_host_3d)
if(NOT MSVC)
    target_link_libraries(xg_semantic_presentation PUBLIC m)
endif()

add_library(xg_render_resident_capture STATIC
    src/producers/resident/xg_render_resident_capture.c)
target_include_directories(xg_render_resident_capture PUBLIC include)
target_compile_features(xg_render_resident_capture PUBLIC c_std_11)
target_link_libraries(xg_render_resident_capture PUBLIC
    xg_render_quad_builder)

add_library(xg_render_battle_fx STATIC
    src/producers/battle/xg_render_battle_fx.c)
target_include_directories(xg_render_battle_fx PUBLIC include)
target_compile_features(xg_render_battle_fx PUBLIC c_std_11)
target_link_libraries(xg_render_battle_fx PUBLIC
    xg_render_ir xg_host_3d)

add_library(xg_render_fragment_runtime STATIC
    src/runtime/xg_render_fragment_runtime.c)
target_include_directories(xg_render_fragment_runtime PUBLIC include)
target_compile_features(xg_render_fragment_runtime PUBLIC c_std_11)
target_link_libraries(xg_render_fragment_runtime PUBLIC
    xg_semantic_presentation)

add_library(xg_guest_render_transaction STATIC
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_transaction.c"
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_native_stream.c")
target_include_directories(xg_guest_render_transaction PUBLIC
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_guest_render_transaction PUBLIC c_std_11)

add_library(xg_field_character_shadow STATIC
    src/producers/field/field_character_shadow.c
    src/producers/field/field_character_shadow_observe.c)
target_include_directories(xg_field_character_shadow PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_field_character_shadow PUBLIC c_std_11)

add_library(xg_field_character_adapter STATIC
    src/producers/field/xg_field_character_adapter.c)
target_include_directories(xg_field_character_adapter PUBLIC include)
target_compile_features(xg_field_character_adapter PUBLIC c_std_11)
target_link_libraries(xg_field_character_adapter PUBLIC xg_render_ir)
target_link_libraries(xg_field_character_adapter PUBLIC xg_render_quad_builder)

add_library(xg_field_character_source STATIC
    src/producers/field/xg_field_character_source.c
    src/producers/field/xg_field_character_source_adapter.c
    src/producers/field/xg_field_character_source_capture.c)
target_include_directories(xg_field_character_source PUBLIC include
    PRIVATE ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
target_compile_features(xg_field_character_source PUBLIC c_std_11)
target_link_libraries(xg_field_character_source PUBLIC
    xg_field_character_adapter xg_host_3d)

add_library(xg_field_character_capture STATIC
    src/producers/field/xg_field_character_capture.c)
target_include_directories(xg_field_character_capture PUBLIC include
    PRIVATE ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
target_compile_features(xg_field_character_capture PUBLIC c_std_11)
target_link_libraries(xg_field_character_capture PUBLIC xg_field_character_adapter)

add_library(xg_render_private_compare STATIC
    src/producers/field/xg_render_private_compare.c)
target_include_directories(xg_render_private_compare PUBLIC include
    PRIVATE ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
target_compile_features(xg_render_private_compare PUBLIC c_std_11)
target_link_libraries(xg_render_private_compare
    PUBLIC xg_field_character_adapter xg_render_ir)

add_library(xg_field_character_runtime STATIC
    src/producers/field/xg_field_character_runtime.c)
target_include_directories(xg_field_character_runtime PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
    PRIVATE ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
target_compile_features(xg_field_character_runtime PUBLIC c_std_11)
target_link_libraries(xg_field_character_runtime PUBLIC
    xg_field_character_capture
    xg_field_character_source
    xg_render_private_compare
    xg_render_backend
    xg_guest_render_transaction)

set(XG_RENDER_OVERLAY_CUTOVERS_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/xg_render_overlay_cutovers_generated.c")
add_custom_command(
    OUTPUT "${XG_RENDER_OVERLAY_CUTOVERS_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}"
        "${XG_REPOSITORY_ROOT}/tools/native_render_overlay_ranges.py"
        emit-cold-cutover-table
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/xg_render_overlay_ranges.toml"
        --output "${XG_RENDER_OVERLAY_CUTOVERS_SOURCE}"
    DEPENDS
        "${XG_REPOSITORY_ROOT}/tools/native_render_overlay_ranges.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/xg_render_overlay_ranges.toml"
    VERBATIM)
set_source_files_properties("${XG_RENDER_OVERLAY_CUTOVERS_SOURCE}"
    PROPERTIES GENERATED TRUE)
add_custom_target(xg_render_overlay_cutovers_generated
    DEPENDS "${XG_RENDER_OVERLAY_CUTOVERS_SOURCE}")

add_library(xg_render_auth STATIC
    "${XG_RENDER_OVERLAY_CUTOVERS_SOURCE}"
    src/auth/xg_render_auth.c
    src/runtime/xg_render_auth_runtime.c
    src/runtime/xg_render_runtime_host_services.c
    src/runtime/xg_render_runtime_composition.c
    src/runtime/xg_render_cutover_dispatch.c
    src/producers/resident/xg_render_resident_text.c
    src/producers/resident/xg_render_native_target.c
    src/producers/world/xg_render_world_coordinator.c
    src/producers/field/xg_render_field_character_pipeline.c
    src/producers/field/xg_field_render_services.c
    src/infrastructure/xg_render_address_lookup.c
    src/producers/field/xg_render_f4_sources.c
    src/producers/field/xg_render_field_polyline.c
    src/producers/field/xg_render_field_sprite.c
    src/infrastructure/xg_render_instrumentation.c
    src/runtime/xg_render_invalidation_dispatch.c
    src/runtime/xg_render_invalidation_modules.c
    src/runtime/xg_render_mutation_classifier.c
    src/auth/xg_render_local_producer_auth.c
    src/producers/model/xg_render_model_repository.c
    src/producers/model/xg_render_model_sprite_pipeline.c
    src/producers/battle/xg_render_battle_geometry.c
    src/producers/model/xg_render_gear_motion.c
    src/producers/world/xg_render_world_model_repository.c
    src/producers/world/xg_render_world_models_pipeline.c
    src/producers/field/xg_render_overlay_ft4.c
    src/infrastructure/xg_render_primitive_utils.c
    src/producers/field/xg_render_residual.c
    src/infrastructure/xg_render_resolver_registry.c
    src/infrastructure/xg_render_shared_packet_resolver.c
    src/infrastructure/xg_render_shared_packet_gpu_environment.c
    src/producers/field/xg_render_resident_line_f2.c
    src/infrastructure/xg_render_resource_watch.c
    src/infrastructure/xg_render_submission.c
    src/infrastructure/xg_render_temporal_submission.c
    src/infrastructure/xg_render_ui_ot.c
    src/producers/world/xg_render_world_simple_producers.c
    src/producers/world/xg_render_world_actor_sprites.c
    src/producers/world/xg_render_world_clouds.c
    src/producers/world/xg_render_world_sky_producer.c
    src/producers/world/xg_render_world_execution.c
    src/producers/field/xg_field_particles.c
    src/producers/field/xg_field_zoom.c
    src/producers/field/xg_field_projected.c
    src/producers/field/xg_field_compass.c
    src/infrastructure/xg_render_vram_resources.c
    src/auth/xg_render_runtime_variant_auth.c
    src/auth/xg_render_static_auth_metadata.c)
add_dependencies(xg_render_auth xg_render_overlay_cutovers_generated)
target_include_directories(xg_render_auth PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
    PRIVATE ${XG_RENDER_PRIVATE_INCLUDE_DIRS}
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(xg_render_auth PUBLIC c_std_11)
target_link_libraries(xg_render_auth PUBLIC
    xg_host_3d
    xg_native_view
    xg_render_quad_builder
    xg_render_ir
    xg_render_backend
    xg_render_fragment_runtime
    xg_render_resident_capture
    xg_render_battle_fx
    xg_semantic_presentation
    xg_render_model_resources
    xg_model_ft4_raw
    xg_sprite_ft4
    xg_world_actor_sprites
    xg_world_clouds
    xg_world_decorations
    xg_world_effects
    xg_world_entity_shadows
    xg_world_horizon
    xg_world_minimap
    xg_world_models
    xg_world_sky
    xg_world_terrain_water
    xg_guest_render_transaction
    xg_field_character_shadow
    xg_field_character_runtime)

add_library(xg_render_static_auth STATIC
    src/auth/xg_render_static_auth.c)
target_include_directories(xg_render_static_auth PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
    PRIVATE ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
target_compile_features(xg_render_static_auth PUBLIC c_std_11)
target_link_libraries(xg_render_static_auth PUBLIC xg_render_auth)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_library(xg_visual_state_test_impl STATIC src/core/xg_visual_state.c)
    target_include_directories(xg_visual_state_test_impl PUBLIC include)
    target_compile_features(xg_visual_state_test_impl PUBLIC c_std_11)
    target_compile_definitions(xg_visual_state_test_impl
        PUBLIC XG_VISUAL_STATE_COUNTER_MAX=4ULL)

    add_library(xg_render_ir_test_impl STATIC src/core/xg_render_ir.c)
    target_include_directories(xg_render_ir_test_impl PUBLIC
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_compile_features(xg_render_ir_test_impl PUBLIC c_std_11)
    target_compile_definitions(xg_render_ir_test_impl PUBLIC
        XG_RENDER_IR_ITEM_CAPACITY=4)

    add_executable(test_xg_visual_state tests/core/test_xg_visual_state.c)
    target_include_directories(test_xg_visual_state PRIVATE include)
    target_link_libraries(test_xg_visual_state PRIVATE xg_visual_state_test_impl)
    add_test(NAME xg_visual_state COMMAND test_xg_visual_state)

    add_executable(test_xg_render_ir tests/core/test_xg_render_ir.c)
    target_include_directories(test_xg_render_ir PRIVATE include)
    target_link_libraries(test_xg_render_ir PRIVATE xg_render_ir_test_impl)
    add_test(NAME xg_render_ir COMMAND test_xg_render_ir)

    add_executable(test_xg_host_3d_math tests/core/test_xg_host_3d_math.c)
    target_link_libraries(test_xg_host_3d_math PRIVATE xg_host_3d)
    add_test(NAME xg_host_3d_math COMMAND test_xg_host_3d_math)
    add_test(NAME xg_host_3d_dependency_audit
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/test_xg_host_3d_dependencies.py")

    add_executable(test_xg_render_backend
        tests/core/test_xg_render_backend.c)
    target_include_directories(test_xg_render_backend PRIVATE
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_link_libraries(test_xg_render_backend PRIVATE xg_render_backend)
    add_test(NAME xg_render_backend COMMAND test_xg_render_backend)

    add_executable(test_xg_world_sky tests/world/test_xg_world_sky.c)
    target_link_libraries(test_xg_world_sky PRIVATE xg_world_sky)
    add_test(NAME xg_world_sky COMMAND test_xg_world_sky)

    add_executable(test_xg_world_horizon tests/world/test_xg_world_horizon.c)
    target_link_libraries(test_xg_world_horizon PRIVATE xg_world_horizon)
    add_test(NAME xg_world_horizon COMMAND test_xg_world_horizon)

    add_executable(test_xg_world_effects tests/world/test_xg_world_effects.c)
    target_link_libraries(test_xg_world_effects PRIVATE xg_world_effects)
    add_test(NAME xg_world_effects COMMAND test_xg_world_effects)

    add_executable(test_xg_world_terrain_water
        tests/world/test_xg_world_terrain_water.c)
    target_link_libraries(test_xg_world_terrain_water PRIVATE
        xg_world_terrain_water)
    add_test(NAME xg_world_terrain_water COMMAND test_xg_world_terrain_water)

    add_executable(test_xg_world_terrain_water_shadow
        tests/world/test_xg_world_terrain_water_shadow.c)
    target_link_libraries(test_xg_world_terrain_water_shadow PRIVATE
        xg_world_terrain_water)
    add_test(NAME xg_world_terrain_water_shadow
        COMMAND test_xg_world_terrain_water_shadow)

    add_executable(test_xg_world_entity_shadows
        tests/world/test_xg_world_entity_shadows.c)
    target_link_libraries(test_xg_world_entity_shadows PRIVATE
        xg_world_entity_shadows)
    add_test(NAME xg_world_entity_shadows COMMAND test_xg_world_entity_shadows)

    add_executable(test_xg_world_entity_shadows_shadow
        tests/world/test_xg_world_entity_shadows_shadow.c)
    target_link_libraries(test_xg_world_entity_shadows_shadow PRIVATE
        xg_world_entity_shadows)
    add_test(NAME xg_world_entity_shadows_shadow
        COMMAND test_xg_world_entity_shadows_shadow)

    add_executable(test_xg_world_decorations
        tests/world/test_xg_world_decorations.c)
    target_link_libraries(test_xg_world_decorations PRIVATE
        xg_world_decorations)
    add_test(NAME xg_world_decorations COMMAND test_xg_world_decorations)

    add_executable(test_xg_world_decorations_shadow
        tests/world/test_xg_world_decorations_shadow.c)
    target_link_libraries(test_xg_world_decorations_shadow PRIVATE
        xg_world_decorations)
    add_test(NAME xg_world_decorations_shadow
        COMMAND test_xg_world_decorations_shadow)

    add_executable(test_xg_world_clouds tests/world/test_xg_world_clouds.c)
    target_link_libraries(test_xg_world_clouds PRIVATE xg_world_clouds)
    add_test(NAME xg_world_clouds COMMAND test_xg_world_clouds)

    add_executable(test_xg_world_clouds_shadow
        tests/world/test_xg_world_clouds_shadow.c)
    target_link_libraries(test_xg_world_clouds_shadow PRIVATE xg_world_clouds)
    add_test(NAME xg_world_clouds_shadow COMMAND test_xg_world_clouds_shadow)

    add_executable(test_xg_world_minimap tests/world/test_xg_world_minimap.c)
    target_link_libraries(test_xg_world_minimap PRIVATE xg_world_minimap)
    add_test(NAME xg_world_minimap COMMAND test_xg_world_minimap)

    add_executable(test_xg_world_minimap_shadow
        tests/world/test_xg_world_minimap_shadow.c)
    target_link_libraries(test_xg_world_minimap_shadow PRIVATE xg_world_minimap)
    add_test(NAME xg_world_minimap_shadow COMMAND test_xg_world_minimap_shadow)

    add_executable(test_xg_model_ft4_raw tests/model/test_xg_model_ft4_raw.c)
    target_link_libraries(test_xg_model_ft4_raw PRIVATE xg_model_ft4_raw)
    add_test(NAME xg_model_ft4_raw COMMAND test_xg_model_ft4_raw)

    add_executable(test_xg_sprite_ft4 tests/model/test_xg_sprite_ft4.c)
    target_link_libraries(test_xg_sprite_ft4 PRIVATE xg_sprite_ft4)
    add_test(NAME xg_sprite_ft4 COMMAND test_xg_sprite_ft4)

    add_executable(test_xg_world_models tests/world/test_xg_world_models.c)
    target_link_libraries(test_xg_world_models PRIVATE xg_world_models)
    add_test(NAME xg_world_models COMMAND test_xg_world_models)

    add_executable(test_xg_world_actor_sprites
        tests/world/test_xg_world_actor_sprites.c)
    target_link_libraries(test_xg_world_actor_sprites PRIVATE
        xg_world_actor_sprites)
    add_test(NAME xg_world_actor_sprites COMMAND test_xg_world_actor_sprites)

    add_executable(test_xg_render_static_auth
        tests/auth/test_xg_render_static_auth.c
        tests/auth/test_xg_render_auth_compat_abi.c
        "${XG_RENDER_OVERLAY_CUTOVERS_SOURCE}"
        src/auth/xg_render_static_auth.c
        src/auth/xg_render_static_auth_metadata.c
        src/runtime/xg_render_auth_runtime.c
        src/runtime/xg_render_runtime_host_services.c
        tests/auth/xg_render_auth_runtime_test_adapter.c
        src/runtime/xg_render_runtime_composition.c
        src/runtime/xg_render_cutover_dispatch.c
        src/producers/resident/xg_render_resident_text.c
        src/producers/world/xg_render_world_coordinator.c
        src/producers/field/xg_render_field_character_pipeline.c
        src/producers/field/xg_field_render_services.c
        src/infrastructure/xg_render_address_lookup.c
        src/producers/field/xg_render_f4_sources.c
        src/producers/field/xg_render_field_polyline.c
        src/producers/field/xg_render_field_sprite.c
        src/infrastructure/xg_render_instrumentation.c
        src/runtime/xg_render_invalidation_dispatch.c
        src/runtime/xg_render_invalidation_modules.c
        src/runtime/xg_render_mutation_classifier.c
        src/auth/xg_render_local_producer_auth.c
        src/producers/model/xg_render_model_repository.c
        src/producers/model/xg_render_model_sprite_pipeline.c
        src/producers/battle/xg_render_battle_geometry.c
        src/producers/world/xg_render_world_model_repository.c
        src/producers/world/xg_render_world_models_pipeline.c
        src/producers/field/xg_render_overlay_ft4.c
        src/infrastructure/xg_render_primitive_utils.c
        src/producers/field/xg_render_residual.c
        src/infrastructure/xg_render_resolver_registry.c
        src/infrastructure/xg_render_shared_packet_resolver.c
        src/infrastructure/xg_render_shared_packet_gpu_environment.c
        src/producers/field/xg_render_resident_line_f2.c
        src/infrastructure/xg_render_resource_watch.c
        src/infrastructure/xg_render_submission.c
        src/infrastructure/xg_render_temporal_submission.c
        src/infrastructure/xg_render_ui_ot.c
        src/producers/world/xg_render_world_simple_producers.c
        src/producers/world/xg_render_world_actor_sprites.c
        src/producers/world/xg_render_world_clouds.c
        src/producers/world/xg_render_world_sky_producer.c
        src/producers/world/xg_render_world_execution.c
        src/producers/field/xg_field_particles.c
        src/producers/field/xg_field_zoom.c
        src/producers/field/xg_field_projected.c
        src/producers/field/xg_field_compass.c
        src/infrastructure/xg_render_vram_resources.c
        src/auth/xg_render_runtime_variant_auth.c
        src/auth/xg_render_auth.c
        src/core/xg_render_ir.c
        tests/auth/xg_render_runtime_variant_test_table.c
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/gte_attribution.cpp"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/psx_sha256.c"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_transaction.c"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_native_stream.c"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_bridge.c")
    target_include_directories(test_xg_render_static_auth PRIVATE
        include
        ${XG_RENDER_PRIVATE_INCLUDE_DIRS}
        tests/auth
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
        ${PSX_SDL_INCLUDE_DIRS})
    add_dependencies(test_xg_render_static_auth
        xg_render_overlay_cutovers_generated)
    target_compile_definitions(test_xg_render_static_auth PRIVATE
        GUEST_RENDER_BRIDGE_TESTING=1
        GUEST_RENDER_BRIDGE_TEST_BINDING_CAPACITY=256
        GUEST_RENDER_NATIVE_STREAM_TESTING=1
        XG_RENDER_RUNTIME_COMPOSITION_TESTING=1
        GUEST_RENDER_TRANSACTION_TESTING=1
        GUEST_RENDER_TRANSACTION_PENDING_CAPACITY=256)
    target_link_libraries(test_xg_render_static_auth PRIVATE
        xg_host_3d xg_native_view xg_field_character_shadow
        xg_field_character_runtime
        xg_render_fragment_runtime
        xg_semantic_presentation
        xg_render_model_resources
        xg_model_ft4_raw xg_sprite_ft4 xg_world_clouds xg_world_decorations
        xg_world_effects xg_world_entity_shadows xg_world_horizon
        xg_world_minimap xg_world_models xg_world_actor_sprites xg_world_sky
        xg_world_terrain_water ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_render_static_auth COMMAND test_xg_render_static_auth)
    set_tests_properties(xg_render_static_auth PROPERTIES
        ENVIRONMENT "PSX_OVERLAY_NO_GATES=0")

    add_executable(test_field_character_shadow
        tests/field/test_field_character_shadow.c
        tests/field/field_character_shadow_test_adapter.c)
    target_include_directories(test_field_character_shadow PRIVATE
        include
        tests/field
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_link_libraries(test_field_character_shadow PRIVATE
        xg_field_character_shadow)
    add_test(NAME field_character_shadow COMMAND test_field_character_shadow)

    add_executable(test_xg_field_character_adapter
        tests/field/test_xg_field_character_adapter.c)
    target_link_libraries(test_xg_field_character_adapter PRIVATE
        xg_field_character_adapter)
    add_test(NAME xg_field_character_adapter
        COMMAND test_xg_field_character_adapter)

    add_executable(test_xg_field_character_capture
        tests/field/test_xg_field_character_capture.c)
    target_include_directories(test_xg_field_character_capture PRIVATE
        ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
    target_link_libraries(test_xg_field_character_capture PRIVATE
        xg_field_character_capture)
    add_test(NAME xg_field_character_capture
        COMMAND test_xg_field_character_capture)

    add_executable(test_xg_field_character_source
        tests/field/test_xg_field_character_source.c)
    target_include_directories(test_xg_field_character_source PRIVATE
        ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
    target_link_libraries(test_xg_field_character_source PRIVATE
        xg_field_character_source)
    add_test(NAME xg_field_character_source
        COMMAND test_xg_field_character_source)

    add_executable(test_xg_render_private_compare
        tests/field/test_xg_render_private_compare.c)
    target_include_directories(test_xg_render_private_compare PRIVATE
        ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
    target_link_libraries(test_xg_render_private_compare PRIVATE
        xg_render_private_compare)
    add_test(NAME xg_render_private_compare
        COMMAND test_xg_render_private_compare)

    add_executable(test_xg_field_character_runtime
        tests/field/test_xg_field_character_runtime.c)
    target_include_directories(test_xg_field_character_runtime PRIVATE
        ${XG_RENDER_PRIVATE_INCLUDE_DIRS})
    target_link_libraries(test_xg_field_character_runtime PRIVATE
        xg_field_character_runtime)
    add_test(NAME xg_field_character_runtime
        COMMAND test_xg_field_character_runtime)

    add_executable(test_xg_render_auth
        tests/auth/test_xg_render_auth.c
        src/auth/xg_render_auth.c
        src/core/xg_render_ir.c
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_bridge.c")
    target_include_directories(test_xg_render_auth PRIVATE
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
        ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(test_xg_render_auth PRIVATE
        GUEST_RENDER_BRIDGE_TESTING=1)
    target_link_libraries(test_xg_render_auth PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_render_auth COMMAND test_xg_render_auth)

    add_executable(test_xg_render_shared_packet_resolver
        tests/infrastructure/test_xg_render_shared_packet_resolver.c
        src/infrastructure/xg_render_shared_packet_resolver.c)
    target_include_directories(test_xg_render_shared_packet_resolver PRIVATE
        ${XG_RENDER_PRIVATE_INCLUDE_DIRS}
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    add_test(NAME xg_render_shared_packet_resolver
        COMMAND test_xg_render_shared_packet_resolver)
    set_tests_properties(xg_render_shared_packet_resolver PROPERTIES
        LABELS "shared-packet-resolver;synthetic;native-only")

    add_executable(test_xg_render_instrumentation
        tests/infrastructure/test_xg_render_instrumentation.c
        src/infrastructure/xg_render_instrumentation.c)
    target_include_directories(test_xg_render_instrumentation PRIVATE
        include
        ${XG_RENDER_PRIVATE_INCLUDE_DIRS}
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_compile_definitions(test_xg_render_instrumentation PRIVATE
        XG_RENDER_INSTRUMENTATION_TESTING=1)
    add_test(NAME xg_render_instrumentation
        COMMAND test_xg_render_instrumentation)
    set_tests_properties(xg_render_instrumentation PROPERTIES
        LABELS "instrumentation;synthetic;native-only")

    add_executable(test_guest_render_native_stream
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/tests/test_guest_render_native_stream.c"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_native_stream.c")
    target_include_directories(test_guest_render_native_stream PRIVATE
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_compile_definitions(test_guest_render_native_stream PRIVATE
        GUEST_RENDER_NATIVE_STREAM_TESTING=1
        GUEST_RENDER_NATIVE_STREAM_CAPACITY=4)
    add_test(NAME guest_render_native_stream
        COMMAND test_guest_render_native_stream)
    set_tests_properties(guest_render_native_stream PROPERTIES
        LABELS "native-stream;synthetic;native-only")

    add_executable(test_xg_semantic_presentation
        tests/infrastructure/test_xg_semantic_presentation.c)
    target_link_libraries(test_xg_semantic_presentation PRIVATE
        xg_semantic_presentation)
    add_dependencies(test_xg_semantic_presentation
        test_guest_render_native_stream
        test_xg_render_instrumentation)
    add_test(NAME xg_semantic_presentation
        COMMAND test_xg_semantic_presentation)
    set_tests_properties(xg_semantic_presentation PROPERTIES
        LABELS "semantic-presentation;synthetic;native-only")

    add_executable(test_xg_render_presentation_host
        tests/infrastructure/test_xg_render_presentation_host.c)
    target_link_libraries(test_xg_render_presentation_host PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_presentation_host
        COMMAND test_xg_render_presentation_host)
    set_tests_properties(xg_render_presentation_host PROPERTIES
        LABELS "presentation-host;synthetic;native-only")

    add_executable(test_xg_render_source_timeline
        tests/infrastructure/test_xg_render_source_timeline.c)
    target_link_libraries(test_xg_render_source_timeline PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_source_timeline COMMAND test_xg_render_source_timeline)
    set_tests_properties(xg_render_source_timeline PROPERTIES
        LABELS "source-timeline;synthetic;native-only")

    add_executable(test_xg_render_temporal_submission
        tests/infrastructure/test_xg_render_temporal_submission.c
        src/infrastructure/xg_render_temporal_submission.c)
    target_include_directories(test_xg_render_temporal_submission PRIVATE
        include
        src/infrastructure
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_compile_definitions(test_xg_render_temporal_submission PRIVATE
        XG_RENDER_IR_ITEM_CAPACITY=2)
    target_link_libraries(test_xg_render_temporal_submission PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_temporal_submission COMMAND test_xg_render_temporal_submission)
    set_tests_properties(xg_render_temporal_submission PROPERTIES
        LABELS "temporal-submission;synthetic;native-only")

    add_executable(test_xg_render_resource_repository
        tests/infrastructure/test_xg_render_resource_repository.c)
    target_link_libraries(test_xg_render_resource_repository PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_resource_repository
        COMMAND test_xg_render_resource_repository)

    add_executable(test_xg_render_model_resources
        tests/infrastructure/test_xg_render_model_resources.c)
    target_link_libraries(test_xg_render_model_resources PRIVATE
        xg_render_model_resources)
    add_test(NAME xg_render_model_resources
        COMMAND test_xg_render_model_resources)

    add_executable(test_xg_render_vram_resources
        tests/infrastructure/test_xg_render_vram_resources.c
        src/infrastructure/xg_render_vram_resources.c
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/psx_sha256.c")
    target_include_directories(test_xg_render_vram_resources PRIVATE
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_link_libraries(test_xg_render_vram_resources PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_vram_resources
        COMMAND test_xg_render_vram_resources)

    add_executable(test_xg_render_ui_movie
        tests/infrastructure/test_xg_render_ui_movie.c)
    target_link_libraries(test_xg_render_ui_movie PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_ui_movie COMMAND test_xg_render_ui_movie)
    add_test(NAME xg_render_ui_owner_contract
        COMMAND test_xg_render_ui_movie --ui-only)

    add_executable(test_xg_render_ui_layout
        tests/infrastructure/test_xg_render_ui_layout.c)
    target_link_libraries(test_xg_render_ui_layout PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_ui_layout COMMAND test_xg_render_ui_layout)
    set_tests_properties(xg_render_ui_layout PROPERTIES
        LABELS "ui-layout;synthetic;native-only")

    add_executable(test_xg_render_display_options
        tests/infrastructure/test_xg_render_display_options.c)
    target_link_libraries(test_xg_render_display_options PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_display_options COMMAND test_xg_render_display_options)

    add_executable(test_xg_render_ui_source_commit
        tests/infrastructure/test_xg_render_ui_source_commit.c)
    target_link_libraries(test_xg_render_ui_source_commit PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_ui_source_commit
        COMMAND test_xg_render_ui_source_commit)
    set_tests_properties(xg_render_ui_source_commit PROPERTIES
        LABELS "ui-source-commit;synthetic;native-only")

    add_executable(test_xg_render_movie_publisher
        tests/infrastructure/test_xg_render_movie_publisher.c)
    target_link_libraries(test_xg_render_movie_publisher PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_movie_publisher
        COMMAND test_xg_render_movie_publisher)

    add_executable(test_xg_render_surface_graph
        tests/infrastructure/test_xg_render_surface_graph.c)
    target_link_libraries(test_xg_render_surface_graph PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_surface_graph
        COMMAND test_xg_render_surface_graph)

    add_executable(test_xg_render_semantic_presentation_synthetic
        tests/integration/test_xg_render_semantic_presentation_synthetic.c)
    target_link_libraries(test_xg_render_semantic_presentation_synthetic PRIVATE
        xg_semantic_presentation)
    add_test(NAME xg_render_semantic_presentation_synthetic
        COMMAND test_xg_render_semantic_presentation_synthetic)
    set_tests_properties(xg_render_semantic_presentation_synthetic PROPERTIES
        LABELS "semantic-presentation;field-world;resident-menu;battle;movie;synthetic;native-only")

    foreach(assert_target IN ITEMS
            test_xg_render_shared_packet_resolver
            test_xg_render_instrumentation
            test_xg_semantic_presentation
            test_xg_render_presentation_host
            test_xg_render_source_timeline
            test_xg_render_temporal_submission
            test_xg_render_resource_repository
            test_xg_render_model_resources
            test_xg_render_vram_resources
            test_xg_render_ui_movie
            test_xg_render_ui_layout
            test_xg_render_ui_source_commit
            test_xg_render_display_options
            test_xg_render_movie_publisher
            test_xg_render_surface_graph
            test_xg_render_semantic_presentation_synthetic)
        if(MSVC)
            target_compile_options(${assert_target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${assert_target} PRIVATE -UNDEBUG)
        endif()
    endforeach()

    add_executable(test_xg_native_render_baseline_adapter
        tests/integration/test_xg_native_render_baseline_adapter.c
        src/core/xg_native_render_baseline.c)
    target_include_directories(test_xg_native_render_baseline_adapter PRIVATE
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    add_test(NAME xg_native_render_baseline_adapter
        COMMAND test_xg_native_render_baseline_adapter)
    add_test(NAME xg_render_manifest
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/test_native_render_manifest.py")
    add_test(NAME xg_render_manifest_field_identity
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/test_native_render_manifest_field_identity.py")
    add_test(NAME xg_render_manifest_auth_metadata
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/test_native_render_manifest_auth_metadata.py")
    add_test(NAME xg_render_static_census
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/test_native_render_static_census.py")
    add_test(NAME xg_render_runtime_variants
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/test_native_render_runtime_variants.py")
    add_test(NAME xg_warm_overlay_source_observation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/test_warm_overlay_source_observation.py")
    add_test(NAME xg_render_overlay_ranges
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${XG_REPOSITORY_ROOT}/tools/test_native_render_overlay_ranges.py")
    add_test(NAME xg_render_ui_owner_catalog
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${XG_REPOSITORY_ROOT}/tools/test_native_render_ui_owners.py")
    add_test(NAME xg_native_render_replay
        COMMAND "${Python3_EXECUTABLE}" "${XG_REPOSITORY_ROOT}/tools/test_native_render_replay.py")
    add_test(NAME xg_disc_overlay_census
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${XG_REPOSITORY_ROOT}/tools/test_census_disc_overlays.py")
    set_tests_properties(
        xg_render_static_census
        PROPERTIES LABELS "static-census;static;native-only")
endif()
