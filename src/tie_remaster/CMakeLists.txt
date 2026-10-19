# Modern renderer for the game application.
add_library(tie_remaster STATIC
    remaster.c
    flight/cockpit/renderer.c
    flight/cockpit/layout.c
    flight/cockpit/text.c
    flight/backdrops.c
    flight/billboards.c
    flight/classic_lighting.c
    flight/renderer.c
    flight/classic_draw.c
    flight/mesh_draw.c
    flight/scene_mesh.c
    flight/line_draw.c
    flight/render_math.c
    flight/mesh_common.c
    flight/passes.c
    flight/xr_stereo.c
    flight/pipelines.c
    flight/classic_mesh.c
    flight/hyperstars.c
    flight/pbr.c
    flight/point_lights.c
    flight/sprite_cache.c
    flight/stars.c
    flight/world_ambient.c
    flight/xwa_cockpit.c
    integration/snapshot_adapter.c
    vr/vr_hands.c
    vr/vr_controllers.c
    vr/vr_ktx2_decode.cpp
    vr/vr_gloves.c
    vr/vr_flight_stick.c
    vr/vr_hand_overlay.c
    vr/vr_controller_labels.c
)
if(TIE_ENABLE_DEBUG_UI)
    target_sources(tie_remaster PRIVATE
        debug/debug_tools.cpp
        debug/camera_inspector.cpp
        debug/world_ambient_editor.cpp
        debug/gltf_mesh_inspector.cpp
        debug/motion_blur_inspector.cpp
        debug/hdr_inspector.cpp
        debug/cockpit_inspector.cpp)
endif()
target_include_directories(tie_remaster PUBLIC ${CMAKE_SOURCE_DIR}/src)
# VR controller models (vr/vr_controllers.c, from OpenXWA-VR): stb_image for
# their PNG/JPEG textures.
target_include_directories(tie_remaster PRIVATE ${CMAKE_SOURCE_DIR}/aeron/third_party/stb)
target_link_libraries(tie_remaster
    PUBLIC aeron aeron_scene aeron_asset tie_formats
    PRIVATE tie_core tie_remaster_scene2d lucas_opt PkgConfig::ZSTD tie_libm
        # The runtime's controller models: glTF parsing, KTX2 atlas,
        # cut-out meshes, Basis Universal textures.
        aeron_cgltf imgbake tie_meshopt tie_basisu)

# meshoptimizer's simplifier (MIT, third_party/meshoptimizer): light cut-out
# meshes of the VR controller models.
add_library(tie_meshopt STATIC
    ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/simplifier.cpp
    ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/allocator.cpp)
target_include_directories(tie_meshopt PUBLIC ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer)
# Basis Universal's transcoder (Apache 2.0, third_party/basisu): the KTX2
# textures of the runtime's controller models (button letters, shading), to
# RGBA. The GPU-format targets the game does not use are switched off.
add_library(tie_basisu STATIC ${CMAKE_SOURCE_DIR}/third_party/basisu/transcoder/basisu_transcoder.cpp)
target_include_directories(tie_basisu PUBLIC ${CMAKE_SOURCE_DIR}/third_party/basisu/transcoder)
target_compile_definitions(tie_basisu PUBLIC
    BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1
    BASISD_SUPPORT_DXT1=0 BASISD_SUPPORT_DXT5A=0 BASISD_SUPPORT_PVRTC1=0 BASISD_SUPPORT_PVRTC2=0
    BASISD_SUPPORT_ETC2_EAC_A8=0 BASISD_SUPPORT_ETC2_EAC_RG11=0 BASISD_SUPPORT_ATC=0 BASISD_SUPPORT_FXT1=0)
target_link_libraries(tie_basisu PRIVATE PkgConfig::ZSTD)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tie_basisu PRIVATE -w -O2)
endif()
target_compile_definitions(tie_remaster PRIVATE TIE_SHADER_DIR="${TIE_SHADER_BINARY_DIR}")
if(TIE_GPU_DEBUG_LABELS)
    target_compile_definitions(tie_remaster PRIVATE TIE_GPU_DEBUG_LABELS=1)
else()
    target_compile_definitions(tie_remaster PRIVATE TIE_GPU_DEBUG_LABELS=0)
endif()
tie_enable_warnings(tie_remaster)
