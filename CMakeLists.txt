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
    src/core/photo_region_detector.cpp
    src/core/preferences.cpp
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

    add_executable(SideScopes MACOSX_BUNDLE src/app/main.mm)
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
    # Screen-recording permission attaches to the bundle's code signature;
    # an ad-hoc signature gives development builds a stable-enough identity.
    add_custom_command(TARGET SideScopes POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:SideScopes>
    )
endif()

if(SIDESCOPES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
