set(TIE_APP_SOURCES
    application.c
    application.h
    frame_loop.c
    frame_loop.h
    hotkeys.c
    hotkeys.h
    main.c
    midi_resources.c
    ui.c
    vr_keyboard.c
    vr_keyboard.h
    config/app_config.c
    config/keyboard_config.c
    config/controller_config.c
    settings/flight_options.c
	settings/audio_options.c
    settings/mouse_options.c
    settings/inflight_options.c
    settings/launch_options.c
    settings/video_options.c
    settings/settings.c
    settings/vr_options.c
    settings/keyboard_page.c
    settings/bindings_editor.c
    settings/controller_page.c
    setup/installation.c
    setup/installation_ui.c
    setup/setup.c
    quest/quest_app.c
    quest/quest_app.h
    test/test_harness.c
    test/test_harness.h
    test/test_wrap.c)
if(ANDROID)
    list(APPEND TIE_APP_SOURCES quest/xr_tier.c quest/crash_report.c)
endif()
# VR voice commands (speech recognizer dlopen'ed at run time).
list(APPEND TIE_APP_SOURCES
    quest/voice_commands.c
    quest/voice_commands.h
    quest/voice_core.c
    quest/voice_core.h
    quest/vr_voice.c
    quest/vr_voice.h)
if(ANDROID)
    # Quest: SDL's Java activity loads the game as libmain.so.
    add_library(tie SHARED ${TIE_APP_SOURCES})
    set_target_properties(tie PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
else()
    add_executable(tie ${TIE_APP_SOURCES})
endif()

# Aeron resolves generated shaders and staged resources relative to the
# executable. Keep the application beside those build-tree directories even
# though its sources are organized under src/tie_app.
set_target_properties(tie PROPERTIES
    OUTPUT_NAME "OpenTIE"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(ANDROID)
    set_target_properties(tie PROPERTIES OUTPUT_NAME "main")
endif()

if(TIE_ENABLE_DEBUG_UI)
    target_sources(tie PRIVATE
        debug/pbr_global_inspector.cpp
        debug/point_lights_editor.cpp
        debug/ssao_inspector.cpp
        debug/directional_shadow_inspector.cpp)
endif()

target_include_directories(tie PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
# third_party/sherpa-onnx/c-api.h (voice commands; the library is dlopen'ed).
set_source_files_properties(quest/voice_core.c PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}")
target_link_libraries(tie PRIVATE
    tie_core
    tie_remaster
    ${CMAKE_DL_LIBS})
target_compile_definitions(tie PRIVATE
    TIE_SHADER_RELATIVE_DIR="${TIE_SHADER_RELATIVE_DIR}"
    TIE_VERSION_STRING="${TIE_DISPLAY_VERSION}")
if(TIE_GPU_DEBUG_LABELS)
    target_compile_definitions(tie PRIVATE TIE_GPU_DEBUG_LABELS=1)
else()
    target_compile_definitions(tie PRIVATE TIE_GPU_DEBUG_LABELS=0)
endif()
tie_enable_warnings(tie)

# Headless test harness (src/tie_app/test): window capture and a fake XR
# headset need link-time wraps (GNU ld / lld). Inert unless TIE_TEST_* is set.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    set(TIE_TEST_WRAPPED_SYMBOLS
        SDL_ClaimWindowForGPUDevice
        SDL_WaitAndAcquireGPUSwapchainTexture
        SDL_SubmitGPUCommandBuffer
        Aeron_XrIsPresenting
        Aeron_XrIsUnfocused
        Aeron_XrStereoBegin
        Aeron_XrStereoPanelTarget
        Aeron_XrStereoCommit
        Aeron_XrStereoCommitHuds
        Aeron_XrStereoCommitPanels
        Aeron_XrStereoCommitConsole
        Aeron_XrStereoCommitCard
        Aeron_XrStereoCommitPanelLayer
        Aeron_XrStereoCommitEyeOverlay
        Aeron_XrStereoCommitEyeOverlayRect
        Aeron_XrStereoCommitBeams
        Aeron_XrStereoCommitBackdrop
        Aeron_XrVirtualScreenGeometry
        Aeron_XrGetHand
        Aeron_XrGetRenderModel
        Aeron_XrSetPointerEnabled
        Aeron_XrSetMenuKeys
        Aeron_XrSetFlightButtons
        Aeron_XrPollButton
        Aeron_XrTakeAPressed
        Aeron_XrTakeMenuBPressed
        Aeron_XrTakeMenuXPressed
        Aeron_XrTakeMenuYPressed
        Aeron_XrThrottleLever
        Aeron_XrSetThrottleLever
        Aeron_XrHapticPulse)
    target_compile_definitions(tie PRIVATE TIE_TEST_WRAP=1)
    foreach(symbol IN LISTS TIE_TEST_WRAPPED_SYMBOLS)
        target_link_options(tie PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
endif()

add_dependencies(tie flight_shaders)

if(NOT APPLE)
    set(TIE_AERON_SCENE3D_DEFAULTS
        "${CMAKE_SOURCE_DIR}/aeron/config/scene3d_defaults.yaml")
    add_custom_target(tie_aeron_scene3d_defaults
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:tie>/resources/aeron"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TIE_AERON_SCENE3D_DEFAULTS}"
            "$<TARGET_FILE_DIR:tie>/resources/aeron/scene3d_defaults.yaml"
        DEPENDS "${TIE_AERON_SCENE3D_DEFAULTS}"
        COMMENT "Staging Aeron Scene3D defaults for TIE"
        VERBATIM)
    add_dependencies(tie tie_aeron_scene3d_defaults)
    add_custom_command(TARGET tie POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources"
            "$<TARGET_FILE_DIR:tie>/resources"
        COMMENT "Staging OpenTIE application resources"
        VERBATIM)
endif()

if(APPLE)
    set(TIE_MACOS_BUILD_VERSION "1" CACHE STRING "OpenTIE macOS bundle build version")
    if(NOT DEFINED TIE_MACOS_BUNDLE_IDENTIFIER)
        set(TIE_MACOS_BUNDLE_IDENTIFIER "org.totallyopen.opentie" CACHE STRING
            "OpenTIE macOS bundle identifier")
    elseif(TIE_MACOS_BUNDLE_IDENTIFIER STREQUAL "org.totallyopen.tie")
        set(TIE_MACOS_BUNDLE_IDENTIFIER "org.totallyopen.opentie" CACHE STRING
            "OpenTIE macOS bundle identifier" FORCE)
    endif()
    set_property(CACHE TIE_MACOS_BUNDLE_IDENTIFIER PROPERTY HELPSTRING
        "OpenTIE macOS bundle identifier")
    set_target_properties(tie PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/macos/Info.plist.in"
        INSTALL_RPATH "@executable_path/../Frameworks")
    set(TIE_MACOS_ICON "${CMAKE_SOURCE_DIR}/cmake/macos/TIE.icns")
    target_sources(tie PRIVATE "${TIE_MACOS_ICON}")
    set_source_files_properties("${TIE_MACOS_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    set(TIE_MACOS_ROOT_RESOURCE_FILES
        "${CMAKE_SOURCE_DIR}/resources/config.yaml")
    target_sources(tie PRIVATE ${TIE_MACOS_ROOT_RESOURCE_FILES})
    set_source_files_properties(${TIE_MACOS_ROOT_RESOURCE_FILES} PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources/resources)

    set(TIE_MACOS_FLIGHT_RESOURCE_FILES
        "${CMAKE_SOURCE_DIR}/resources/flight/tie98-models.yaml")
    target_sources(tie PRIVATE ${TIE_MACOS_FLIGHT_RESOURCE_FILES})
    set_source_files_properties(${TIE_MACOS_FLIGHT_RESOURCE_FILES} PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources/resources/flight)

    set(TIE_MACOS_FONT_RESOURCE_FILES
        "${CMAKE_SOURCE_DIR}/resources/assets/fonts/menu.fnt"
        "${CMAKE_SOURCE_DIR}/resources/assets/fonts/menu.png")
    target_sources(tie PRIVATE ${TIE_MACOS_FONT_RESOURCE_FILES})
    set_source_files_properties(${TIE_MACOS_FONT_RESOURCE_FILES} PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources/resources/assets/fonts)

    set(TIE_MACOS_AERON_RESOURCE_FILES
        "${CMAKE_SOURCE_DIR}/aeron/config/scene3d_defaults.yaml")
    target_sources(tie PRIVATE ${TIE_MACOS_AERON_RESOURCE_FILES})
    set_source_files_properties(${TIE_MACOS_AERON_RESOURCE_FILES} PROPERTIES
        HEADER_FILE_ONLY TRUE
        MACOSX_PACKAGE_LOCATION Resources/resources/aeron)

    add_custom_command(TARGET tie POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DTIE_BUNDLE_CONTENT_DIR=$<TARGET_BUNDLE_CONTENT_DIR:tie>"
            "-DTIE_SHADER_SOURCE_DIR=${TIE_SHADER_BINARY_DIR}"
            -P "${CMAKE_SOURCE_DIR}/cmake/macos/StageBundle.cmake"
        COMMENT "Staging OpenTIE macOS shaders"
        VERBATIM)
    install(TARGETS tie BUNDLE DESTINATION .)
    set(TIE_MACOS_DEPENDENCY_DIRS)
    foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND TIE_MACOS_DEPENDENCY_DIRS "${prefix}/lib")
    endforeach()
    list(JOIN TIE_MACOS_DEPENDENCY_DIRS ";" TIE_MACOS_DEPENDENCY_DIRS)
    configure_file("${CMAKE_SOURCE_DIR}/cmake/macos/FixupBundle.cmake.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/FixupTieBundle.cmake" @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/FixupTieBundle.cmake")
endif()

if(WIN32)
    set_target_properties(tie PROPERTIES WIN32_EXECUTABLE TRUE)
    enable_language(RC)
    if(TIE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(TIE_VERSION_MAJOR ${CMAKE_MATCH_1})
        set(TIE_VERSION_MINOR ${CMAKE_MATCH_2})
        set(TIE_VERSION_PATCH ${CMAKE_MATCH_3})
    else()
        set(TIE_VERSION_MAJOR 0)
        set(TIE_VERSION_MINOR 0)
        set(TIE_VERSION_PATCH 0)
    endif()
    set(TIE_WINDOWS_ICON "${CMAKE_SOURCE_DIR}/cmake/windows/tie.ico")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/windows/tie.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/tie.rc" @ONLY)
    target_sources(tie PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tie.rc")
endif()

# Voice command matcher test (host): ninja tie_voice_test && ./tie_voice_test
if(NOT ANDROID)
    add_executable(tie_voice_test EXCLUDE_FROM_ALL
        ${CMAKE_SOURCE_DIR}/tests/voice_commands_test.c
        quest/voice_commands.c)
    target_include_directories(tie_voice_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/aeron/include)
    set_target_properties(tie_voice_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
