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
    src/core/preferences.cpp
    src/core/scopes/vectorscope.cpp
    src/core/scopes/waveform.cpp
    src/core/scopes/graticule.cpp
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
endif()

if(SIDESCOPES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
