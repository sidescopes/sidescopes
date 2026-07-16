cmake_minimum_required(VERSION 3.24)

project(sidescopes VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # Compile each target's sources in parallel; MSBuild alone only
    # parallelizes across targets, and most sources here live in a few.
    # Directory-scoped so the fetched dependencies build the same way.
    add_compile_options(/MP)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SIDESCOPES_BUILD_TESTS "Build the test suite" ON)
option(SIDESCOPES_BENCH "Build the opt-in benchmark suite" OFF)
option(SIDESCOPES_WERROR "Treat compiler warnings as errors" OFF)
option(SIDESCOPES_MODULES_DYNAMIC "Load built-in scope modules dynamically" OFF)
option(SIDESCOPES_UI_TESTS "Build the Dear ImGui Test Engine UI-test target" OFF)
set(SIDESCOPES_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers to build and link with (e.g. address,undefined or thread)")

if(SIDESCOPES_SANITIZE)
    add_compile_options(-fsanitize=${SIDESCOPES_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${SIDESCOPES_SANITIZE})
endif()

function(sidescopes_target_defaults target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(SIDESCOPES_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(SIDESCOPES_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# The hourglass in build form: each scope engine and its module shim live
# in a per-module static (or, in dynamic mode, MODULE) library; core keeps
# only shared machinery. frame.cpp and graticule.cpp stay in core because
# the module shims call their symbols and duplicating them across modules
# would clash at link. The registry is host machinery and stays in core;
# its extern references to the module entries resolve at the final link.
add_library(sidescopes_core STATIC
    src/core/frame.cpp
    src/core/color_lab.cpp
    src/core/region_hash.cpp
    src/core/trace_intensity.cpp
    src/core/marker_smoother.cpp
    src/core/analysis_worker.cpp
    src/core/preferences.cpp
    src/core/region_suggestions.cpp
    src/core/scopes/graticule.cpp
    src/modules/module_registry.cpp
)
target_include_directories(sidescopes_core PUBLIC src include)
sidescopes_target_defaults(sidescopes_core)

# --- scope modules ---
# STATIC by default; MODULE shared objects in the dynamic dev/CI mode. Each
# pairs a module shim with its engine. Engine headers under src are private;
# only include/sidescopes/module.h is the public boundary.
if(SIDESCOPES_MODULES_DYNAMIC)
    set(SIDESCOPES_MODULE_KIND MODULE)
else()
    set(SIDESCOPES_MODULE_KIND STATIC)
endif()

function(sidescopes_add_module name)
    add_library(${name} ${SIDESCOPES_MODULE_KIND} ${ARGN})
    target_include_directories(${name} PRIVATE src PUBLIC include)
    # The shims reference frame/graticule symbols that live in core; core
    # never links the modules, so this dependency edge stays acyclic.
    target_link_libraries(${name} PRIVATE sidescopes_core)
    sidescopes_target_defaults(${name})
    if(SIDESCOPES_MODULES_DYNAMIC)
        # Hidden visibility with a single default-visible ss_module_entry,
        # dropped alongside the loader's search directory.
        set_target_properties(${name} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules)
        # Multi-config generators (MSVC) append a per-config subdirectory to the
        # output directory, which the loader's fixed SIDESCOPES_MODULES_DIR would
        # then miss. Pin every config's module output to the one directory the
        # loader scans. No-op on the single-config generators, whose
        # CMAKE_CONFIGURATION_TYPES is empty.
        foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${config}" config_upper)
            set_target_properties(${name} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/modules
                RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/modules)
        endforeach()
        if(APPLE)
            # CMake gives MODULE bundles the .so suffix on macOS; the loader
            # scans for the platform-native .dylib, so match it.
            set_target_properties(${name} PROPERTIES SUFFIX ".dylib")
        endif()
        # Singular per-module flag that gates the module's ss_module_entry
        # export; distinct from the plural SIDESCOPES_MODULES_DYNAMIC that
        # switches the core registry over to the dynamic loader.
        target_compile_definitions(${name} PRIVATE SIDESCOPES_MODULE_DYNAMIC)
    endif()
endfunction()

# BuildVectorscopeGraticule projects colors through the vectorscope engine, so
# it ships in the vectorscope module beside that engine. Every module then links
# self-contained: none references a symbol defined only in another, which is
# what lets the MODULE bundles link on toolchains without lazy symbol binding.
sidescopes_add_module(sidescopes_module_vectorscope
    src/modules/vectorscope_module.cpp
    src/core/scopes/vectorscope.cpp
    src/core/scopes/vectorscope_graticule.cpp)
sidescopes_add_module(sidescopes_module_waveform
    src/modules/waveform_module.cpp
    src/core/scopes/waveform.cpp)
sidescopes_add_module(sidescopes_module_histogram
    src/modules/histogram_module.cpp
    src/core/scopes/histogram.cpp)

# Consumers (app, tests) link these ahead of core. In static mode the module
# libs carry the built-in entries the registry references and the engines the
# shims use; in dynamic mode SIDESCOPES_MODULE_LINK_LIBS is empty and the
# entries come from the loaded shared objects instead, so nothing links them
# and the registry is told where to load them from.
if(SIDESCOPES_MODULES_DYNAMIC)
    target_compile_definitions(sidescopes_core PRIVATE
        SIDESCOPES_MODULES_DYNAMIC
        "SIDESCOPES_MODULES_DIR=\"${CMAKE_BINARY_DIR}/modules\"")
    target_sources(sidescopes_core PRIVATE src/modules/module_loader.cpp)
    set(SIDESCOPES_MODULE_LINK_LIBS "")
else()
    # core and the module libs depend on each other: the registry references
    # each module's entry, while each shim references core's frame/graticule
    # symbols. GNU ld's single left-to-right pass cannot resolve that cycle
    # from link order alone, so force every module object into the link.
    # WHOLE_ARCHIVE maps to --whole-archive / -force_load / /WHOLEARCHIVE per
    # toolchain (ld64 and MSVC already tolerated the cycle; GNU ld did not).
    set(SIDESCOPES_MODULE_LINK_LIBS
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,sidescopes_module_vectorscope,sidescopes_module_waveform,sidescopes_module_histogram>")
endif()

if(APPLE)
    enable_language(OBJCXX)
    add_library(sidescopes_platform STATIC
        src/platform/region_geometry.cpp
        src/platform/macos/desktop.mm
        src/platform/macos/face_detection.mm
        src/platform/macos/native_menu.mm
        src/platform/macos/region_selection.mm
        src/platform/macos/screen_capture.mm
    )
    target_include_directories(sidescopes_platform PUBLIC src)
    target_compile_options(sidescopes_platform PRIVATE -fobjc-arc)
    target_link_libraries(sidescopes_platform
        PUBLIC
        sidescopes_core
        "-framework AppKit"
        "-framework CoreGraphics"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework ScreenCaptureKit"
        "-framework Vision"
    )
    sidescopes_target_defaults(sidescopes_platform)
endif()

if(WIN32)
    # The Windows platform layer builds on every push so the port meets a
    # compiler continuously; the application target follows with the port
    # itself.
    add_library(sidescopes_platform STATIC
        src/platform/region_geometry.cpp
        src/platform/windows/desktop.cpp
        src/platform/windows/face_detection.cpp
        src/platform/windows/native_menu.cpp
        src/platform/windows/region_selection.cpp
        src/platform/windows/screen_capture.cpp
    )
    target_include_directories(sidescopes_platform PUBLIC src)
    # The layer calls wide APIs explicitly; UNICODE makes the
    # resource-id macros (IDC_*) match them.
    target_compile_definitions(sidescopes_platform PRIVATE UNICODE _UNICODE)
    target_link_libraries(sidescopes_platform
        PUBLIC
        sidescopes_core
        d3d11
        dxgi
        dwmapi
        gdiplus
        shell32  # ShellExecuteW for opening URLs
        WindowsApp  # C++/WinRT (the in-box face detector)
    )
    sidescopes_target_defaults(sidescopes_platform)
endif()

if(APPLE OR WIN32)
    # --- application ---
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.8
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    )
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC glfw)
    if(APPLE)
        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm)
        set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    else()
        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    endif()

    # The graphics backends live with the platform code but compile into
    # the application: they own the ImGui render backends, and only the
    # application links ImGui.
    if(APPLE)
        add_executable(SideScopes MACOSX_BUNDLE
            src/app/main.cpp
            src/platform/macos/graphics.mm
            assets/icon/sidescopes.icns
        )
        set_source_files_properties(assets/icon/sidescopes.icns
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_source_files_properties(src/platform/macos/graphics.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set_target_properties(SideScopes PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
            MACOSX_BUNDLE_BUNDLE_NAME "SideScopes"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.sidescopes.app"
            MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        )
        target_link_libraries(SideScopes PRIVATE
            ${SIDESCOPES_MODULE_LINK_LIBS}
            sidescopes_platform
            imgui
            glfw
            "-framework Metal"
            "-framework QuartzCore"
        )
        sidescopes_target_defaults(SideScopes)

        # The screen-recording permission is bound to the bundle's code-signing
        # identity. An ad-hoc signature's identity is the binary hash, so it
        # changes on every build and the granted permission is lost each time.
        # Signing with a stable identity keeps a single grant valid across
        # rebuilds. If a self-signed "SideScopes Dev" certificate is present (see
        # CONTRIBUTING), use it; otherwise fall back to ad-hoc. Releases override
        # this with a Developer ID.
        if(NOT DEFINED SIDESCOPES_CODESIGN_IDENTITY)
            execute_process(
                COMMAND security find-certificate -c "SideScopes Dev"
                RESULT_VARIABLE _sidescopes_no_dev_cert
                OUTPUT_QUIET ERROR_QUIET)
            if(_sidescopes_no_dev_cert EQUAL 0)
                set(SIDESCOPES_CODESIGN_IDENTITY "SideScopes Dev")
            else()
                set(SIDESCOPES_CODESIGN_IDENTITY "-")
            endif()
        endif()
        message(STATUS "SideScopes code-signing identity: ${SIDESCOPES_CODESIGN_IDENTITY}")
        add_custom_command(TARGET SideScopes POST_BUILD
            COMMAND codesign --force --sign "${SIDESCOPES_CODESIGN_IDENTITY}"
                    $<TARGET_BUNDLE_DIR:SideScopes>
        )
    else()
        # WIN32_EXECUTABLE hides the console; main() stays the portable
        # entry point, so the linker is told where to find it. The manifest
        # declares per-monitor DPI awareness before the first window-system
        # call. The version resource carries the icon and the metadata the
        # shell shows in Properties and Task Manager.
        enable_language(RC)  # Ninja does not enable resource compilation on its own
        set(SIDESCOPES_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/icon/sidescopes.ico")
        configure_file(cmake/sidescopes.rc.in sidescopes.rc @ONLY)
        add_executable(SideScopes WIN32
            src/app/main.cpp
            src/platform/windows/graphics.cpp
            cmake/sidescopes.manifest
            ${CMAKE_CURRENT_BINARY_DIR}/sidescopes.rc
        )
        target_link_options(SideScopes PRIVATE /ENTRY:mainCRTStartup)
        target_link_libraries(SideScopes PRIVATE
            ${SIDESCOPES_MODULE_LINK_LIBS}
            sidescopes_platform
            imgui
            glfw
            opengl32
        )
        sidescopes_target_defaults(SideScopes)
    endif()

    # Application logic shared by both platforms: cohesive controllers lifted
    # out of main.
    target_sources(SideScopes PRIVATE
        src/app/capture_controller.cpp
        src/app/overlay_render.cpp
        src/app/overlay_style.cpp
        src/app/param_menu.cpp
        src/app/pin_board.cpp
        src/app/scope_registry.cpp
        src/app/scope_view.cpp
        src/app/version.cpp)

    # Build-time version stamp. The custom target regenerates the header from
    # `git describe` whenever it changes, so every build reflects the checked-
    # out commit without relinking on a no-op build. Only the app includes it.
    set(SIDESCOPES_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/sidescopes_version.h)
    add_custom_target(sidescopes_version
        BYPRODUCTS ${SIDESCOPES_VERSION_HEADER}
        COMMAND ${CMAKE_COMMAND}
            -DSIDESCOPES_VERSION=${PROJECT_VERSION}
            -DSIDESCOPES_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DSIDESCOPES_OUTPUT=${SIDESCOPES_VERSION_HEADER}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenerateVersion.cmake
        COMMENT "Stamping build version")
    add_dependencies(SideScopes sidescopes_version)
    target_include_directories(SideScopes PRIVATE ${CMAKE_BINARY_DIR}/generated)

    if(SIDESCOPES_MODULES_DYNAMIC)
        # Nothing links the modules in dynamic mode; make the app wait for
        # the shared objects it loads at runtime. The app itself depends only
        # on the module boundary and the registry - it draws every scope
        # through a loaded instance and no longer compiles any engine source.
        add_dependencies(SideScopes
            sidescopes_module_vectorscope
            sidescopes_module_waveform
            sidescopes_module_histogram)
    endif()
endif()

if(SIDESCOPES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Opt-in Catch2 micro-benchmarks. Everything the suite needs is reached only
# here, so the default build fetches and configures nothing extra.
if(SIDESCOPES_BENCH)
    add_subdirectory(bench)
endif()

# Opt-in UI tests on the Dear ImGui Test Engine. Everything the engine needs
# (its fetch, a hooked Dear ImGui build, the test executable) lives under
# tests/ui and is reached only here, so the default build is unaffected. The
# engine builds against the same Dear ImGui the app fetches, so it is only
# available where that is built.
if(SIDESCOPES_UI_TESTS)
    if(NOT (APPLE OR WIN32))
        message(FATAL_ERROR "SIDESCOPES_UI_TESTS requires the Dear ImGui build (APPLE or WIN32).")
    endif()
    enable_testing()
    add_subdirectory(tests/ui)
endif()
