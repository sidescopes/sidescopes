# Opt-in UI-test target built on the Dear ImGui Test Engine. Enabled only when
# SIDESCOPES_UI_TESTS=ON; with the option OFF (the default and what CI uses)
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
# v1.92.8 in the top-level CMakeLists); the two version in lockstep.
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

# The headless smoke test proving the harness runs.
add_executable(sidescopes_ui_tests test_smoke.cpp)
target_link_libraries(sidescopes_ui_tests PRIVATE sidescopes_imgui_test_engine)

add_test(NAME sidescopes_ui_smoke COMMAND sidescopes_ui_tests)

# Feature suites that drive the REAL scope pipeline and the real app-state
# classes through a representative headless UI. Each links the core pipeline
# and the scope modules exactly as the unit tests do (see tests/CMakeLists.txt),
# plus the hooked Dear ImGui and Test Engine, and compiles the app-state
# translation units directly - they have no Dear ImGui dependency, so they mix
# freely with the hooked build. ui_test_harness.cpp is the shared null-backend
# runner.
foreach(suite IN ITEMS pipeline view)
    add_executable(sidescopes_ui_${suite}_tests
        test_${suite}_ui.cpp
        ui_test_harness.cpp
        ${CMAKE_SOURCE_DIR}/src/app/scope_registry.cpp
        ${CMAKE_SOURCE_DIR}/src/app/pane_layout.cpp
        ${CMAKE_SOURCE_DIR}/src/app/scope_stack.cpp
        ${CMAKE_SOURCE_DIR}/src/app/scope_view.cpp
        ${CMAKE_SOURCE_DIR}/src/app/stack_tokens.cpp
        ${CMAKE_SOURCE_DIR}/src/app/trace_params.cpp
        ${CMAKE_SOURCE_DIR}/src/app/pin_board.cpp
        ${CMAKE_SOURCE_DIR}/src/app/capture_controller.cpp)
    target_include_directories(sidescopes_ui_${suite}_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
    target_link_libraries(sidescopes_ui_${suite}_tests PRIVATE
        sidescopes_imgui_test_engine ${SIDESCOPES_MODULE_LINK_LIBS} sidescopes_core)
    sidescopes_target_defaults(sidescopes_ui_${suite}_tests)
    add_test(NAME sidescopes_ui_${suite} COMMAND sidescopes_ui_${suite}_tests)
endforeach()

# The seating-rule suite needs none of the pipeline: it asserts the arithmetic
# in src/app/row_layout.cpp, the one app translation unit that DOES depend on
# Dear ImGui - linking the test engine gives it the hooked build's headers, so
# it sees the same ImGuiContext layout as everything else here.
add_executable(sidescopes_ui_layout_tests
    test_layout_ui.cpp
    ui_test_harness.cpp
    ${CMAKE_SOURCE_DIR}/src/app/row_layout.cpp)
target_include_directories(sidescopes_ui_layout_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sidescopes_ui_layout_tests PRIVATE sidescopes_imgui_test_engine)
sidescopes_target_defaults(sidescopes_ui_layout_tests)
add_test(NAME sidescopes_ui_layout COMMAND sidescopes_ui_layout_tests)

# In the dynamic module configuration the engines live only in the loaded
# MODULE bundles, so - as in tests/CMakeLists.txt - compile their sources in to
# supply the symbols core's shared graticule references, and wait for the
# bundles the worker loads at runtime.
if(SIDESCOPES_MODULES_DYNAMIC)
    foreach(suite IN ITEMS pipeline view)
        target_sources(sidescopes_ui_${suite}_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/core/scopes/vectorscope.cpp
            ${CMAKE_SOURCE_DIR}/src/core/scopes/waveform.cpp
            ${CMAKE_SOURCE_DIR}/src/core/scopes/histogram.cpp)
        add_dependencies(sidescopes_ui_${suite}_tests
            sidescopes_module_vectorscope
            sidescopes_module_waveform
            sidescopes_module_histogram)
    endforeach()
endif()
