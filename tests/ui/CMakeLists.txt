# Opt-in UI-test target built on the Dear ImGui Test Engine. Enabled only when
# SIDESCOPES_UI_TESTS=ON; with the option OFF (the local default)
# this directory is never added, so nothing here is fetched or built.
#
# The Test Engine (https://github.com/ocornut/imgui_test_engine) is NOT an OSI
# license. This project qualifies for its Free License (a natural-person,
# publicly released open-source project). It is fetched at configure time and
# never vendored, so this GPL-3 source tree never redistributes it.
#   Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO
#   License: imgui_test_engine/LICENSE.txt in the fetched tree.

include(FetchContent)

# Pinned to the tag matching the application's Dear ImGui (imgui GIT_TAG
# v1.92.8 in the top-level CMakeLists); the two versions stay in lockstep.
FetchContent_Declare(
    imgui_test_engine
    GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine.git
    GIT_TAG v1.92.8
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""  # skip the perf-suite's implot submodule; unused here
)
FetchContent_MakeAvailable(imgui_test_engine)

# A dedicated Dear ImGui build with the test-engine hooks compiled in, paired
# with Dear ImGui's official null (headless) backend. Kept entirely separate
# from the application's `imgui` target: IMGUI_ENABLE_TEST_ENGINE changes the
# ImGuiContext layout, so the hooked Dear ImGui, the engine, and the test
# executable must all share this one build, and the shipping app must not.
add_library(sidescopes_imgui_te STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_null.cpp
)
target_include_directories(sidescopes_imgui_te PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(sidescopes_imgui_te PUBLIC IMGUI_ENABLE_TEST_ENGINE)

# The Test Engine library. The std::thread coroutine implementation lets the
# engine drive tests without any app-provided scheduler. Linking imgui_te
# PUBLIC propagates the Dear ImGui headers and IMGUI_ENABLE_TEST_ENGINE so the
# engine and the hooked Dear ImGui see one identical ImGuiContext.
set(_ss_te_dir ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine)
add_library(sidescopes_imgui_test_engine STATIC
    ${_ss_te_dir}/imgui_te_context.cpp
    ${_ss_te_dir}/imgui_te_coroutine.cpp
    ${_ss_te_dir}/imgui_te_engine.cpp
    ${_ss_te_dir}/imgui_te_exporters.cpp
    ${_ss_te_dir}/imgui_te_perftool.cpp
    ${_ss_te_dir}/imgui_te_ui.cpp
    ${_ss_te_dir}/imgui_te_utils.cpp
    ${_ss_te_dir}/imgui_capture_tool.cpp
)
target_include_directories(sidescopes_imgui_test_engine PUBLIC
    ${imgui_test_engine_SOURCE_DIR})
target_compile_definitions(sidescopes_imgui_test_engine PUBLIC
    IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1)
target_link_libraries(sidescopes_imgui_test_engine PUBLIC sidescopes_imgui_te)

# The Dear ImGui-dependent app units, built a second time against the hooked
# Dear ImGui. Same sources as the app's sidescopes_app_imgui, one list in the
# top-level CMakeLists; only the ImGuiContext layout differs, and a suite links
# whichever variant matches its own Dear ImGui.
sidescopes_add_app_imgui_library(sidescopes_app_imgui_te sidescopes_imgui_te)
# Keep this variant out of the compile database. Its sources are already in
# there from the app's variant, and clang-tidy analyses every matching entry -
# two entries per file would double the tidy job over the whole app layer. The
# app's variant is the one worth analysing: it is what ships.
set_target_properties(sidescopes_app_imgui_te PROPERTIES EXPORT_COMPILE_COMMANDS OFF)

# The headless smoke test proving the harness runs.
add_executable(sidescopes_ui_tests test_smoke.cpp)
target_link_libraries(sidescopes_ui_tests PRIVATE sidescopes_imgui_test_engine)

add_test(NAME sidescopes_ui_smoke COMMAND sidescopes_ui_tests)

# Feature suites that drive the REAL scope pipeline and the real app-state
# classes through a representative headless UI. Each links the core pipeline,
# the scope modules and sidescopes_app exactly as the unit tests do (see
# tests/CMakeLists.txt), plus the hooked Dear ImGui and Test Engine. The
# sidescopes_app units have no Dear ImGui dependency, so they mix freely with
# the hooked build. ui_test_harness.cpp is the shared null-backend runner.
foreach(suite IN ITEMS pipeline view preset)
    add_executable(sidescopes_ui_${suite}_tests
        test_${suite}_ui.cpp
        ui_test_harness.cpp)
    target_include_directories(sidescopes_ui_${suite}_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
    target_link_libraries(sidescopes_ui_${suite}_tests PRIVATE
        sidescopes_imgui_test_engine sidescopes_app_imgui_te sidescopes_app
        ${SIDESCOPES_MODULE_LINK_LIBS} sidescopes_core)
    sidescopes_target_defaults(sidescopes_ui_${suite}_tests)
    add_test(NAME sidescopes_ui_${suite} COMMAND sidescopes_ui_${suite}_tests)
endforeach()

# The preset suite drives the real picker, whose rows reach two seams the
# other suites never pull from the static libraries: the shortcut context's
# platform queries (stubbed the same way the unit tests stub them) and the
# icon rasterizer (stubbed inside the suite - the null backend never renders,
# so the pixels would be discarded unread).
target_sources(sidescopes_ui_preset_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/support/desktop_stubs.cpp)

# The suites below compile Dear ImGui-dependent app translation units, which is
# why those cannot live in sidescopes_app: linking the test engine gives them
# the hooked build's headers, so they see the same ImGuiContext layout as
# everything else here, and the app's own Dear ImGui build would not.
#
# The seating-rule suite needs none of the pipeline: it asserts the arithmetic
# in src/app/row_layout.cpp.
add_executable(sidescopes_ui_layout_tests
    test_layout_ui.cpp
    ui_test_harness.cpp)
target_include_directories(sidescopes_ui_layout_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sidescopes_ui_layout_tests PRIVATE
    sidescopes_imgui_test_engine sidescopes_app_imgui_te)
sidescopes_target_defaults(sidescopes_ui_layout_tests)
add_test(NAME sidescopes_ui_layout COMMAND sidescopes_ui_layout_tests)

# The reorder suite drives the WHOLE drag gesture over a menu row list - press,
# move, release - and asserts the order that comes out. Three defects have
# shipped in that gesture, each past a test that asserted one PART of it, so
# this suite exists to assert it end to end and nothing else.
add_executable(sidescopes_ui_reorder_tests
    test_reorder_ui.cpp
    ui_test_harness.cpp)
target_include_directories(sidescopes_ui_reorder_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sidescopes_ui_reorder_tests PRIVATE
    sidescopes_imgui_test_engine sidescopes_app_imgui_te)
sidescopes_target_defaults(sidescopes_ui_reorder_tests)
add_test(NAME sidescopes_ui_reorder COMMAND sidescopes_ui_reorder_tests)

# The graticule suite drives the real overlay renderer against a hooked Dear
# ImGui and reads back what landed in the draw list, which is the only place the
# strength's whole path - style, render shim, toolkit - can be seen at once.
add_executable(sidescopes_ui_graticule_tests
    test_graticule_ui.cpp
    ui_test_harness.cpp)
target_include_directories(sidescopes_ui_graticule_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sidescopes_ui_graticule_tests PRIVATE
    sidescopes_imgui_test_engine sidescopes_app_imgui_te sidescopes_app)
sidescopes_target_defaults(sidescopes_ui_graticule_tests)
add_test(NAME sidescopes_ui_graticule COMMAND sidescopes_ui_graticule_tests)

# The interface-scale suite asserts applyInterfaceScale never compounds the
# style across repeated scale changes - the ImGui-only unit behind the UI
# Scaling menu, driven headlessly against the hooked build.
add_executable(sidescopes_ui_scale_tests
    test_scale_ui.cpp
    ui_test_harness.cpp)
target_include_directories(sidescopes_ui_scale_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sidescopes_ui_scale_tests PRIVATE
    sidescopes_imgui_test_engine sidescopes_app_imgui_te)
sidescopes_target_defaults(sidescopes_ui_scale_tests)
add_test(NAME sidescopes_ui_scale COMMAND sidescopes_ui_scale_tests)

# In the dynamic module configuration the engines live only in the loaded
# MODULE bundles, so - as in tests/CMakeLists.txt - compile their sources in to
# supply the symbols core's shared graticule references, and wait for the
# bundles the worker loads at runtime.
if(SIDESCOPES_MODULES_DYNAMIC)
    foreach(suite IN ITEMS pipeline view preset)
        target_sources(sidescopes_ui_${suite}_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/core/scopes/vectorscope.cpp
            ${CMAKE_SOURCE_DIR}/src/core/scopes/waveform.cpp
            ${CMAKE_SOURCE_DIR}/src/core/scopes/waveform_bins.cpp
            ${CMAKE_SOURCE_DIR}/src/core/scopes/histogram.cpp
            ${CMAKE_SOURCE_DIR}/src/core/scopes/histogram_bins.cpp)
        add_dependencies(sidescopes_ui_${suite}_tests
            sidescopes_module_vectorscope
            sidescopes_module_waveform
            sidescopes_module_histogram)
    endforeach()
endif()
