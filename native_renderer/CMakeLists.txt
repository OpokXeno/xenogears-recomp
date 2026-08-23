get_filename_component(XG_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(xg_visual_state STATIC src/xg_visual_state.c)
target_include_directories(xg_visual_state PUBLIC include)
target_compile_features(xg_visual_state PUBLIC c_std_11)

add_library(xg_host_3d STATIC src/xg_host_3d.c)
target_include_directories(xg_host_3d PUBLIC include)
target_compile_features(xg_host_3d PUBLIC c_std_11)

add_library(xg_native_view STATIC src/xg_native_view.c)
target_include_directories(xg_native_view PUBLIC include)
target_compile_features(xg_native_view PUBLIC c_std_11)
target_link_libraries(xg_native_view PUBLIC xg_host_3d)

add_library(xg_render_ir STATIC src/xg_render_ir.c)
target_include_directories(xg_render_ir PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_render_ir PUBLIC c_std_11)

add_library(xg_render_quad_builder STATIC src/xg_render_quad_builder.c)
target_include_directories(xg_render_quad_builder PUBLIC include)
target_compile_features(xg_render_quad_builder PUBLIC c_std_11)
target_link_libraries(xg_render_quad_builder PUBLIC xg_render_ir)

add_library(xg_world_sky STATIC src/xg_world_sky.c)
target_include_directories(xg_world_sky PUBLIC include)
target_compile_features(xg_world_sky PUBLIC c_std_11)
target_link_libraries(xg_world_sky PUBLIC xg_host_3d xg_render_quad_builder)

add_library(xg_world_horizon STATIC
    src/xg_world_horizon.c
    src/xg_world_horizon_source_capture.c)
target_include_directories(xg_world_horizon PUBLIC include)
target_compile_features(xg_world_horizon PUBLIC c_std_11)
target_link_libraries(xg_world_horizon PUBLIC
    xg_host_3d xg_native_view xg_render_quad_builder)

add_library(xg_world_effects STATIC
    src/xg_world_effects.c
    src/xg_world_effects_source_capture.c)
target_include_directories(xg_world_effects PUBLIC include)
target_compile_features(xg_world_effects PUBLIC c_std_11)
target_link_libraries(xg_world_effects PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_terrain_water STATIC
    src/xg_world_terrain_water.c
    src/xg_world_terrain_water_source_capture.c
    src/xg_world_terrain_water_shadow.c)
target_include_directories(xg_world_terrain_water PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_terrain_water PUBLIC c_std_11)
target_link_libraries(xg_world_terrain_water PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_entity_shadows STATIC
    src/xg_world_entity_shadows.c
    src/xg_world_entity_shadows_source_capture.c
    src/xg_world_entity_shadows_shadow.c)
target_include_directories(xg_world_entity_shadows PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_entity_shadows PUBLIC c_std_11)
target_link_libraries(xg_world_entity_shadows PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_decorations STATIC
    src/xg_world_decorations.c
    src/xg_world_decorations_source_capture.c
    src/xg_world_decorations_shadow.c)
target_include_directories(xg_world_decorations PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_decorations PUBLIC c_std_11)
target_link_libraries(xg_world_decorations PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_clouds STATIC
    src/xg_world_clouds.c
    src/xg_world_clouds_source_capture.c
    src/xg_world_clouds_shadow.c)
target_include_directories(xg_world_clouds PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_clouds PUBLIC c_std_11)
target_link_libraries(xg_world_clouds PUBLIC
    xg_host_3d xg_render_quad_builder)

add_library(xg_world_minimap STATIC
    src/xg_world_minimap.c
    src/xg_world_minimap_source_capture.c
    src/xg_world_minimap_shadow.c)
target_include_directories(xg_world_minimap PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_world_minimap PUBLIC c_std_11)
target_link_libraries(xg_world_minimap PUBLIC xg_host_3d xg_render_ir)

add_library(xg_model_ft4_raw STATIC src/xg_model_ft4_raw.c)
target_include_directories(xg_model_ft4_raw PUBLIC include)
target_compile_features(xg_model_ft4_raw PUBLIC c_std_11)
target_link_libraries(xg_model_ft4_raw PUBLIC xg_host_3d xg_render_quad_builder)

add_library(xg_sprite_ft4 STATIC src/xg_sprite_ft4.c)
target_include_directories(xg_sprite_ft4 PUBLIC include)
target_compile_features(xg_sprite_ft4 PUBLIC c_std_11)
target_link_libraries(xg_sprite_ft4 PUBLIC xg_host_3d xg_render_quad_builder)

add_library(xg_world_models STATIC
    src/xg_world_models.c
    src/xg_world_models_native.c)
target_include_directories(xg_world_models PUBLIC include)
target_compile_features(xg_world_models PUBLIC c_std_11)
target_link_libraries(xg_world_models PUBLIC xg_model_ft4_raw xg_host_3d)

add_library(xg_world_actor_sprites STATIC
    src/xg_world_actor_sprites.c
    src/xg_world_actor_sprites_source_capture.c)
target_include_directories(xg_world_actor_sprites PUBLIC include)
target_compile_features(xg_world_actor_sprites PUBLIC c_std_11)
target_link_libraries(xg_world_actor_sprites PUBLIC
    xg_sprite_ft4 xg_host_3d xg_render_quad_builder)

add_library(xg_render_backend STATIC src/xg_render_backend.c)
target_include_directories(xg_render_backend PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_render_backend PUBLIC c_std_11)
target_link_libraries(xg_render_backend PUBLIC xg_render_ir)

add_library(xg_guest_render_transaction STATIC
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_transaction.c"
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_native_stream.c")
target_include_directories(xg_guest_render_transaction PUBLIC
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_guest_render_transaction PUBLIC c_std_11)

add_library(xg_field_character_shadow STATIC
    src/field_character_shadow.c
    src/field_character_shadow_observe.c)
target_include_directories(xg_field_character_shadow PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_field_character_shadow PUBLIC c_std_11)

add_library(xg_field_character_adapter STATIC
    src/xg_field_character_adapter.c)
target_include_directories(xg_field_character_adapter PUBLIC include)
target_compile_features(xg_field_character_adapter PUBLIC c_std_11)
target_link_libraries(xg_field_character_adapter PUBLIC xg_render_ir)
target_link_libraries(xg_field_character_adapter PUBLIC xg_render_quad_builder)

add_library(xg_field_character_source STATIC
    src/xg_field_character_source.c
    src/xg_field_character_source_adapter.c
    src/xg_field_character_source_capture.c)
target_include_directories(xg_field_character_source PUBLIC include PRIVATE src)
target_compile_features(xg_field_character_source PUBLIC c_std_11)
target_link_libraries(xg_field_character_source PUBLIC
    xg_field_character_adapter xg_host_3d)

add_library(xg_field_character_capture STATIC
    src/xg_field_character_capture.c)
target_include_directories(xg_field_character_capture PUBLIC include PRIVATE src)
target_compile_features(xg_field_character_capture PUBLIC c_std_11)
target_link_libraries(xg_field_character_capture PUBLIC xg_field_character_adapter)

add_library(xg_render_private_compare STATIC
    src/xg_render_private_compare.c)
target_include_directories(xg_render_private_compare PUBLIC include PRIVATE src)
target_compile_features(xg_render_private_compare PUBLIC c_std_11)
target_link_libraries(xg_render_private_compare
    PUBLIC xg_field_character_adapter xg_render_ir)

add_library(xg_field_character_runtime STATIC
    src/xg_field_character_runtime.c)
target_include_directories(xg_field_character_runtime PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
    PRIVATE src)
target_compile_features(xg_field_character_runtime PUBLIC c_std_11)
target_link_libraries(xg_field_character_runtime PUBLIC
    xg_field_character_capture
    xg_field_character_source
    xg_render_private_compare
    xg_render_backend
    xg_guest_render_transaction)

set(XG_RENDER_OVERLAY_CUTOVERS_INC
    "${CMAKE_CURRENT_BINARY_DIR}/generated/xg_render_overlay_cutovers.inc")
add_custom_command(
    OUTPUT "${XG_RENDER_OVERLAY_CUTOVERS_INC}"
    COMMAND "${Python3_EXECUTABLE}"
        "${XG_REPOSITORY_ROOT}/tools/native_render_overlay_ranges.py"
        emit-cold-cutover-table
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/xg_render_overlay_ranges.toml"
        --output "${XG_RENDER_OVERLAY_CUTOVERS_INC}"
    DEPENDS
        "${XG_REPOSITORY_ROOT}/tools/native_render_overlay_ranges.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/xg_render_overlay_ranges.toml"
    VERBATIM)
set_source_files_properties("${XG_RENDER_OVERLAY_CUTOVERS_INC}"
    PROPERTIES HEADER_FILE_ONLY TRUE)
add_custom_target(xg_render_overlay_cutovers_generated
    DEPENDS "${XG_RENDER_OVERLAY_CUTOVERS_INC}")

add_library(xg_render_auth STATIC
    "${XG_RENDER_OVERLAY_CUTOVERS_INC}"
    src/xg_render_auth.c
    src/xg_render_auth_runtime.c
    src/xg_field_particles.c
    src/xg_field_zoom.c
    src/xg_field_projected.c
    src/xg_field_compass.c
    src/xg_render_runtime_variant_auth.c
    src/xg_render_static_auth_metadata.c)
add_dependencies(xg_render_auth xg_render_overlay_cutovers_generated)
target_include_directories(xg_render_auth PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
    PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(xg_render_auth PUBLIC c_std_11)
target_link_libraries(xg_render_auth PUBLIC
    xg_host_3d
    xg_render_quad_builder
    xg_render_ir
    xg_render_backend
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
    src/xg_render_static_auth.c)
target_include_directories(xg_render_static_auth PUBLIC
    include
    "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
target_compile_features(xg_render_static_auth PUBLIC c_std_11)
target_link_libraries(xg_render_static_auth PUBLIC xg_render_auth)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_library(xg_visual_state_test_impl STATIC src/xg_visual_state.c)
    target_include_directories(xg_visual_state_test_impl PUBLIC include)
    target_compile_features(xg_visual_state_test_impl PUBLIC c_std_11)
    target_compile_definitions(xg_visual_state_test_impl
        PUBLIC XG_VISUAL_STATE_COUNTER_MAX=4ULL)

    add_library(xg_render_ir_test_impl STATIC src/xg_render_ir.c)
    target_include_directories(xg_render_ir_test_impl PUBLIC
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_compile_features(xg_render_ir_test_impl PUBLIC c_std_11)
    target_compile_definitions(xg_render_ir_test_impl PUBLIC
        XG_RENDER_IR_ITEM_CAPACITY=4
        XG_RENDER_IR_TESTING=1)

    add_executable(test_xg_visual_state tests/test_xg_visual_state.c)
    target_include_directories(test_xg_visual_state PRIVATE include)
    target_link_libraries(test_xg_visual_state PRIVATE xg_visual_state_test_impl)
    add_test(NAME xg_visual_state COMMAND test_xg_visual_state)

    add_executable(test_xg_render_ir tests/test_xg_render_ir.c)
    target_include_directories(test_xg_render_ir PRIVATE include)
    target_link_libraries(test_xg_render_ir PRIVATE xg_render_ir_test_impl)
    add_test(NAME xg_render_ir COMMAND test_xg_render_ir)

    add_executable(test_xg_host_3d_math tests/test_xg_host_3d_math.c)
    target_link_libraries(test_xg_host_3d_math PRIVATE xg_host_3d)
    add_test(NAME xg_host_3d_math COMMAND test_xg_host_3d_math)
    add_test(NAME xg_host_3d_dependency_audit
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_xg_host_3d_dependencies.py")

    add_executable(test_xg_render_backend
        tests/test_xg_render_backend.c)
    target_include_directories(test_xg_render_backend PRIVATE
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_link_libraries(test_xg_render_backend PRIVATE xg_render_backend)
    add_test(NAME xg_render_backend COMMAND test_xg_render_backend)

    add_executable(test_xg_world_sky tests/test_xg_world_sky.c)
    target_link_libraries(test_xg_world_sky PRIVATE xg_world_sky)
    add_test(NAME xg_world_sky COMMAND test_xg_world_sky)

    add_executable(test_xg_world_horizon tests/test_xg_world_horizon.c)
    target_link_libraries(test_xg_world_horizon PRIVATE xg_world_horizon)
    add_test(NAME xg_world_horizon COMMAND test_xg_world_horizon)

    add_executable(test_xg_world_effects tests/test_xg_world_effects.c)
    target_link_libraries(test_xg_world_effects PRIVATE xg_world_effects)
    add_test(NAME xg_world_effects COMMAND test_xg_world_effects)

    add_executable(test_xg_world_terrain_water
        tests/test_xg_world_terrain_water.c)
    target_link_libraries(test_xg_world_terrain_water PRIVATE
        xg_world_terrain_water)
    add_test(NAME xg_world_terrain_water COMMAND test_xg_world_terrain_water)

    add_executable(test_xg_world_terrain_water_shadow
        tests/test_xg_world_terrain_water_shadow.c)
    target_link_libraries(test_xg_world_terrain_water_shadow PRIVATE
        xg_world_terrain_water)
    add_test(NAME xg_world_terrain_water_shadow
        COMMAND test_xg_world_terrain_water_shadow)

    add_executable(test_xg_world_entity_shadows
        tests/test_xg_world_entity_shadows.c)
    target_link_libraries(test_xg_world_entity_shadows PRIVATE
        xg_world_entity_shadows)
    add_test(NAME xg_world_entity_shadows COMMAND test_xg_world_entity_shadows)

    add_executable(test_xg_world_entity_shadows_shadow
        tests/test_xg_world_entity_shadows_shadow.c)
    target_link_libraries(test_xg_world_entity_shadows_shadow PRIVATE
        xg_world_entity_shadows)
    add_test(NAME xg_world_entity_shadows_shadow
        COMMAND test_xg_world_entity_shadows_shadow)

    add_executable(test_xg_world_decorations
        tests/test_xg_world_decorations.c)
    target_link_libraries(test_xg_world_decorations PRIVATE
        xg_world_decorations)
    add_test(NAME xg_world_decorations COMMAND test_xg_world_decorations)

    add_executable(test_xg_world_decorations_shadow
        tests/test_xg_world_decorations_shadow.c)
    target_link_libraries(test_xg_world_decorations_shadow PRIVATE
        xg_world_decorations)
    add_test(NAME xg_world_decorations_shadow
        COMMAND test_xg_world_decorations_shadow)

    add_executable(test_xg_world_clouds tests/test_xg_world_clouds.c)
    target_link_libraries(test_xg_world_clouds PRIVATE xg_world_clouds)
    add_test(NAME xg_world_clouds COMMAND test_xg_world_clouds)

    add_executable(test_xg_world_clouds_shadow
        tests/test_xg_world_clouds_shadow.c)
    target_link_libraries(test_xg_world_clouds_shadow PRIVATE xg_world_clouds)
    add_test(NAME xg_world_clouds_shadow COMMAND test_xg_world_clouds_shadow)

    add_executable(test_xg_world_minimap tests/test_xg_world_minimap.c)
    target_link_libraries(test_xg_world_minimap PRIVATE xg_world_minimap)
    add_test(NAME xg_world_minimap COMMAND test_xg_world_minimap)

    add_executable(test_xg_world_minimap_shadow
        tests/test_xg_world_minimap_shadow.c)
    target_link_libraries(test_xg_world_minimap_shadow PRIVATE xg_world_minimap)
    add_test(NAME xg_world_minimap_shadow COMMAND test_xg_world_minimap_shadow)

    add_executable(test_xg_model_ft4_raw tests/test_xg_model_ft4_raw.c)
    target_link_libraries(test_xg_model_ft4_raw PRIVATE xg_model_ft4_raw)
    add_test(NAME xg_model_ft4_raw COMMAND test_xg_model_ft4_raw)

    add_executable(test_xg_sprite_ft4 tests/test_xg_sprite_ft4.c)
    target_link_libraries(test_xg_sprite_ft4 PRIVATE xg_sprite_ft4)
    add_test(NAME xg_sprite_ft4 COMMAND test_xg_sprite_ft4)

    add_executable(test_xg_world_models tests/test_xg_world_models.c)
    target_link_libraries(test_xg_world_models PRIVATE xg_world_models)
    add_test(NAME xg_world_models COMMAND test_xg_world_models)

    add_executable(test_xg_world_actor_sprites
        tests/test_xg_world_actor_sprites.c)
    target_link_libraries(test_xg_world_actor_sprites PRIVATE
        xg_world_actor_sprites)
    add_test(NAME xg_world_actor_sprites COMMAND test_xg_world_actor_sprites)

    add_executable(test_xg_render_static_auth
        tests/test_xg_render_static_auth.c
        src/xg_render_static_auth.c
        src/xg_render_static_auth_metadata.c
        src/xg_render_auth_runtime.c
        src/xg_field_particles.c
        src/xg_field_zoom.c
        src/xg_field_projected.c
        src/xg_field_compass.c
        src/xg_render_runtime_variant_auth.c
        src/xg_render_auth.c
        src/xg_render_ir.c
        tests/xg_render_runtime_variant_test_table.c
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/gte_attribution.cpp"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/psx_sha256.c"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_transaction.c"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_native_stream.c"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_bridge.c")
    target_include_directories(test_xg_render_static_auth PRIVATE
        include
        src
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
        ${PSX_SDL_INCLUDE_DIRS})
    add_dependencies(test_xg_render_static_auth
        xg_render_overlay_cutovers_generated)
    target_compile_definitions(test_xg_render_static_auth PRIVATE
        GUEST_RENDER_BRIDGE_TESTING=1
        GUEST_RENDER_BRIDGE_TEST_BINDING_CAPACITY=256
        GUEST_RENDER_NATIVE_STREAM_TESTING=1
        GUEST_RENDER_TRANSACTION_TESTING=1
        PSX_XG_RENDER_AUTH_RUNTIME_TESTING=1
        GUEST_RENDER_TRANSACTION_PENDING_CAPACITY=256)
    target_link_libraries(test_xg_render_static_auth PRIVATE
        xg_host_3d xg_field_character_shadow xg_field_character_runtime
        xg_model_ft4_raw xg_sprite_ft4 xg_world_clouds xg_world_decorations
        xg_world_effects xg_world_entity_shadows xg_world_horizon
        xg_world_minimap xg_world_models xg_world_actor_sprites xg_world_sky
        xg_world_terrain_water ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_render_static_auth COMMAND test_xg_render_static_auth)
    set_tests_properties(xg_render_static_auth PROPERTIES
        ENVIRONMENT "PSX_OVERLAY_NO_GATES=0")

    add_executable(test_field_character_shadow
        tests/test_field_character_shadow.c
        src/field_character_shadow.c
        src/field_character_shadow_observe.c)
    target_include_directories(test_field_character_shadow PRIVATE
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    target_compile_definitions(test_field_character_shadow PRIVATE
        FIELD_CHARACTER_SHADOW_TESTING=1)
    add_test(NAME field_character_shadow COMMAND test_field_character_shadow)

    add_executable(test_xg_field_character_adapter
        tests/test_xg_field_character_adapter.c)
    target_link_libraries(test_xg_field_character_adapter PRIVATE
        xg_field_character_adapter)
    add_test(NAME xg_field_character_adapter
        COMMAND test_xg_field_character_adapter)

    add_executable(test_xg_field_character_capture
        tests/test_xg_field_character_capture.c)
    target_include_directories(test_xg_field_character_capture PRIVATE src)
    target_link_libraries(test_xg_field_character_capture PRIVATE
        xg_field_character_capture)
    add_test(NAME xg_field_character_capture
        COMMAND test_xg_field_character_capture)

    add_executable(test_xg_field_character_source
        tests/test_xg_field_character_source.c)
    target_include_directories(test_xg_field_character_source PRIVATE src)
    target_link_libraries(test_xg_field_character_source PRIVATE
        xg_field_character_source)
    add_test(NAME xg_field_character_source
        COMMAND test_xg_field_character_source)

    add_executable(test_xg_render_private_compare
        tests/test_xg_render_private_compare.c)
    target_include_directories(test_xg_render_private_compare PRIVATE src)
    target_link_libraries(test_xg_render_private_compare PRIVATE
        xg_render_private_compare)
    add_test(NAME xg_render_private_compare
        COMMAND test_xg_render_private_compare)

    add_executable(test_xg_field_character_runtime
        tests/test_xg_field_character_runtime.c)
    target_include_directories(test_xg_field_character_runtime PRIVATE src)
    target_link_libraries(test_xg_field_character_runtime PRIVATE
        xg_field_character_runtime)
    add_test(NAME xg_field_character_runtime
        COMMAND test_xg_field_character_runtime)

    add_executable(test_xg_render_auth
        tests/test_xg_render_auth.c
        src/xg_render_auth.c
        src/xg_render_ir.c
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/src/guest_render_bridge.c")
    target_include_directories(test_xg_render_auth PRIVATE
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include"
        ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(test_xg_render_auth PRIVATE
        GUEST_RENDER_BRIDGE_TESTING=1)
    target_link_libraries(test_xg_render_auth PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_render_auth COMMAND test_xg_render_auth)

    add_executable(test_xg_native_render_baseline_adapter
        tests/test_xg_native_render_baseline_adapter.c
        src/xg_native_render_baseline.c)
    target_include_directories(test_xg_native_render_baseline_adapter PRIVATE
        include
        "${XG_REPOSITORY_ROOT}/psxrecomp/runtime/include")
    add_test(NAME xg_native_render_baseline_adapter
        COMMAND test_xg_native_render_baseline_adapter)
    add_test(NAME xg_render_manifest
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_render_manifest.py")
    add_test(NAME xg_render_manifest_build
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_render_manifest_build.py")
    add_test(NAME xg_render_manifest_field_identity
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_render_manifest_field_identity.py")
    add_test(NAME xg_render_runtime_variants
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_render_runtime_variants.py")
    add_test(NAME xg_warm_overlay_source_observation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_warm_overlay_source_observation.py")
    add_test(NAME xg_render_overlay_ranges
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${XG_REPOSITORY_ROOT}/tools/test_native_render_overlay_ranges.py")
    add_test(NAME xg_native_render_replay
        COMMAND "${Python3_EXECUTABLE}" "${XG_REPOSITORY_ROOT}/tools/test_native_render_replay.py")
endif()
