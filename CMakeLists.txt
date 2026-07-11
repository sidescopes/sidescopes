cmake_minimum_required(VERSION 3.24)

project(sidescopes VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SIDESCOPES_BUILD_TESTS "Build the test suite" ON)
option(SIDESCOPES_WERROR "Treat compiler warnings as errors" OFF)
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

add_library(sidescopes_core STATIC
    src/core/frame.cpp
    src/core/region_hash.cpp
    src/core/trace_intensity.cpp
    src/core/marker_smoother.cpp
    src/core/analysis_worker.cpp
    src/core/preferences.cpp
    src/core/region_suggestions.cpp
    src/core/scopes/vectorscope.cpp
    src/core/scopes/waveform.cpp
    src/core/scopes/graticule.cpp
    src/core/scopes/histogram.cpp
)
target_include_directories(sidescopes_core PUBLIC src)
sidescopes_target_defaults(sidescopes_core)

if(APPLE)
    enable_language(OBJCXX)
    add_library(sidescopes_platform STATIC
        src/platform/macos/desktop.mm
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
        ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
    )
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC glfw)
    set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

    add_executable(SideScopes MACOSX_BUNDLE
        src/app/main.mm
        assets/icon/sidescopes.icns
    )
    set_source_files_properties(assets/icon/sidescopes.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(src/app/main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(SideScopes PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "SideScopes"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.sidescopes.app"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    )
    target_link_libraries(SideScopes PRIVATE
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
endif()

if(WIN32)
    # The Windows platform layer builds on every push so the port meets a
    # compiler continuously; the application target follows with the port
    # itself.
    add_library(sidescopes_platform STATIC
        src/platform/windows/desktop.cpp
        src/platform/windows/screen_capture.cpp
    )
    target_include_directories(sidescopes_platform PUBLIC src)
    target_link_libraries(sidescopes_platform
        PUBLIC
        sidescopes_core
        d3d11
        dxgi
        dwmapi
    )
    sidescopes_target_defaults(sidescopes_platform)
endif()

if(SIDESCOPES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
