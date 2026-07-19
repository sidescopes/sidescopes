include(FetchContent)

FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(catch2)

add_executable(sidescopes_tests
    test_frame.cpp
    test_color_lab.cpp
    test_parallel_for.cpp
    test_frame_mailbox.cpp
    test_region_hash.cpp
    test_trace_intensity.cpp
    test_marker_smoother.cpp
    test_analysis_worker.cpp
    test_graticule.cpp
    test_histogram.cpp
    test_preferences.cpp
    test_region_suggestions.cpp
    test_vectorscope.cpp
    test_modules.cpp
    test_module_loader.cpp
    test_waveform.cpp
    test_pin_board.cpp
    test_param_menu.cpp
    test_overlay_style.cpp
    test_scope_registry.cpp
    test_scope_view.cpp
    test_capture_controller.cpp
    test_attach_controller.cpp
    test_face_pin.cpp
    test_region_geometry.cpp
    test_icons.cpp
    test_window_suggestions.cpp
    test_version.cpp
)

# The shared test support headers (frame builders, image probes, temp files).
target_include_directories(sidescopes_tests PRIVATE support)

# These are application-level state, not part of the core library the tests
# link, so their sources compile directly into the test binary.
target_sources(sidescopes_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/app/attach_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/app/capture_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/app/face_pin.cpp
    ${CMAKE_SOURCE_DIR}/src/app/overlay_style.cpp
    ${CMAKE_SOURCE_DIR}/src/app/param_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/app/pin_board.cpp
    ${CMAKE_SOURCE_DIR}/src/app/scope_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/app/scope_view.cpp
    ${CMAKE_SOURCE_DIR}/src/app/version.cpp
    ${CMAKE_SOURCE_DIR}/src/app/window_suggestions.cpp)

# The region geometry (src/platform/region_geometry.cpp) is pure,
# cross-platform logic; its tests compile straight into the test binary on
# every platform. It needs only core, so the tests never link the
# GUI/capture platform library.
target_sources(sidescopes_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/platform/region_geometry.cpp)
target_sources(sidescopes_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/platform/icons.cpp)
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/platform/icons.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4702>")
target_include_directories(sidescopes_tests PRIVATE ${nanosvg_SOURCE_DIR}/src)

# The rest of the Windows platform layer's pure logic - UTF-8/UTF-16
# conversion and display-id parsing - is unit-tested on the Windows runner.
# The remainder of that layer (DXGI capture, GDI+ overlays, WinRT faces,
# enumeration) needs a live desktop and is verified by running the app.
if(WIN32)
    target_sources(sidescopes_tests PRIVATE test_windows_platform.cpp)
endif()

# The engine tests (test_vectorscope/waveform/histogram/graticule) are
# white-box: they include the engine headers and link the engine directly,
# which in the static build comes in through the module targets. In dynamic
# mode those engines compile only into the MODULE bundles (not linkable
# archives), so the test binary compiles the engine sources directly. This
# also supplies Vectorscope::Project for core's shared graticule.cpp. The
# module boundary itself is still exercised by test_modules.cpp through the
# loaded dylibs.
if(SIDESCOPES_MODULES_DYNAMIC)
    target_sources(sidescopes_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core/scopes/vectorscope.cpp
        ${CMAKE_SOURCE_DIR}/src/core/scopes/vectorscope_graticule.cpp
        ${CMAKE_SOURCE_DIR}/src/core/scopes/waveform.cpp
        ${CMAKE_SOURCE_DIR}/src/core/scopes/histogram.cpp)
else()
    # The module loader compiles into core only in the dynamic configuration;
    # test_module_loader.cpp exercises it in either, so the static build
    # compiles it straight into the test binary.
    target_sources(sidescopes_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/modules/module_loader.cpp)
endif()
# The module libraries link ahead of core (see the top-level comment on
# link order). In dynamic mode SIDESCOPES_MODULE_LINK_LIBS is empty and the
# modules are loaded from disk at runtime, so the test binary only needs to
# be built after them.
target_link_libraries(sidescopes_tests PRIVATE
    ${SIDESCOPES_MODULE_LINK_LIBS} sidescopes_core Catch2::Catch2WithMain)
if(SIDESCOPES_MODULES_DYNAMIC)
    add_dependencies(sidescopes_tests
        sidescopes_module_vectorscope
        sidescopes_module_waveform
        sidescopes_module_histogram)
endif()
sidescopes_target_defaults(sidescopes_tests)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(sidescopes_tests)
