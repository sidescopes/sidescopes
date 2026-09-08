# Collect notices from the same dependency sources that produce the binary.
# Native apps embed their notices. The browser copies them after linking so
# Emscripten's port is present even on a cold build.
function(sidescopes_add_notices target)
    set(output "")
    if(EMSCRIPTEN)
        set(output "$<TARGET_FILE_DIR:${target}>/licenses")
    endif()
    set(opencv "")
    if(WIN32)
        set(opencv "${SIDESCOPES_OPENCV_SOURCE}")
    endif()
    set(arguments
        "-DNOTICE_OUTPUT=${output}"
        "-DNOTICE_SOURCE=${CMAKE_SOURCE_DIR}"
        "-DNOTICE_IMGUI=${imgui_SOURCE_DIR}"
        "-DNOTICE_NANOSVG=${nanosvg_SOURCE_DIR}"
        "-DNOTICE_GLFW=${glfw_SOURCE_DIR}"
        "-DNOTICE_OPENCV=${opencv}"
        "-DNOTICE_EMSCRIPTEN=${EMSCRIPTEN_ROOT_PATH}"
        "-DNOTICE_PORTS=${EMSCRIPTEN_SYSROOT}/../ports")
    file(GLOB notices CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/licenses/*.txt")
    set(inputs
        "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedNotices.cmake"
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${imgui_SOURCE_DIR}/LICENSE.txt"
        "${imgui_SOURCE_DIR}/imstb_rectpack.h"
        "${imgui_SOURCE_DIR}/imstb_textedit.h"
        "${imgui_SOURCE_DIR}/imstb_truetype.h"
        "${nanosvg_SOURCE_DIR}/LICENSE.txt"
        ${notices})
    if(EMSCRIPTEN)
        list(APPEND inputs
            "${CMAKE_SOURCE_DIR}/src/web/fonts/Inter-OFL.txt"
            "${CMAKE_SOURCE_DIR}/src/web/fonts/RobotoMono-OFL.txt")
    else()
        list(APPEND inputs "${glfw_SOURCE_DIR}/LICENSE.md")
    endif()
    if(opencv)
        list(APPEND inputs
            "${opencv}/LICENSE"
            "${opencv}/3rdparty/protobuf/LICENSE"
            "${opencv}/3rdparty/zlib/LICENSE"
            "${opencv}/modules/core/src/softfloat.cpp")
    endif()
    if(EMSCRIPTEN)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} ${arguments} -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            VERBATIM)
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${inputs})
    else()
        set(cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}_notices.cpp")
        add_custom_command(OUTPUT "${cpp}"
            COMMAND ${CMAKE_COMMAND} ${arguments} "-DNOTICE_CPP=${cpp}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            DEPENDS ${inputs}
            VERBATIM)
        target_sources(sidescopes_app PRIVATE "${cpp}")
    endif()
endfunction()

if(NOT CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

function(copy_notice source name)
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Missing distribution notice: ${source}")
    endif()
    if(NOTICE_CPP)
        file(READ "${source}" hex HEX)
        embed_notice("${name}" "${hex}")
    else()
        configure_file("${source}" "${NOTICE_OUTPUT}/${name}.txt" COPYONLY)
    endif()
endfunction()

function(write_notice name content)
    if(NOTICE_CPP)
        # Preserve file(WRITE)'s host line endings, as in the text packages.
        set(temporary "${NOTICE_CPP}.notice")
        file(WRITE "${temporary}" "${content}")
        copy_notice("${temporary}" "${name}")
        file(REMOVE "${temporary}")
    else()
        file(WRITE "${NOTICE_OUTPUT}/${name}.txt" "${content}")
    endif()
endfunction()

if(NOTICE_CPP)
    include("${CMAKE_CURRENT_LIST_DIR}/EmbedNotices.cmake")
    begin_embedded_notices()
else()
    file(MAKE_DIRECTORY "${NOTICE_OUTPUT}")
endif()
copy_notice("${NOTICE_SOURCE}/LICENSE" SideScopes-GPL)
copy_notice("${NOTICE_IMGUI}/LICENSE.txt" Dear-ImGui)
copy_notice("${NOTICE_NANOSVG}/LICENSE.txt" NanoSVG)
copy_notice("${NOTICE_SOURCE}/licenses/Lucide.txt" Lucide)
copy_notice("${NOTICE_SOURCE}/licenses/ProggyClean.txt" ProggyClean)
copy_notice("${NOTICE_SOURCE}/licenses/ProggyForever.txt" ProggyForever)
if(NOTICE_OPENCV)
    copy_notice("${NOTICE_OPENCV}/LICENSE" OpenCV)
    copy_notice("${NOTICE_OPENCV}/3rdparty/protobuf/LICENSE" protobuf)
    copy_notice("${NOTICE_OPENCV}/3rdparty/zlib/LICENSE" zlib)
    copy_notice("${NOTICE_SOURCE}/licenses/YuNet.txt" YuNet)
    # OpenCV's core also contains these separately licensed arithmetic
    # implementations. Preserve their complete upstream notice prefix, not
    # only OpenCV's top-level Apache license.
    file(READ "${NOTICE_OPENCV}/modules/core/src/softfloat.cpp" softfloat_source)
    string(FIND "${softfloat_source}" "#include" notice_end)
    if(notice_end LESS 0)
        message(FATAL_ERROR "Cannot locate the OpenCV SoftFloat notice boundary")
    endif()
    string(SUBSTRING "${softfloat_source}" 0 ${notice_end} softfloat_notice)
    if(NOT softfloat_notice MATCHES "University of California" OR
       NOT softfloat_notice MATCHES "Copyright \\(C\\) 1993 by Sun Microsystems" OR
       NOT softfloat_notice MATCHES "Copyright \\(C\\) 2004 by Sun Microsystems")
        message(FATAL_ERROR "OpenCV SoftFloat/FDLIBM notices changed; review the distribution notice")
    endif()
    write_notice(OpenCV-SoftFloat-FDLIBM "${softfloat_notice}")
endif()

# All three stb headers currently carry the same text, but keep each one's
# actual notice so a dependency update cannot silently change that assumption.
foreach(component rectpack textedit truetype)
    file(READ "${NOTICE_IMGUI}/imstb_${component}.h" header)
    string(FIND "${header}" "This software is available under 2 licenses" begin)
    if(begin LESS 0)
        message(FATAL_ERROR "Missing stb notice in imstb_${component}.h")
    endif()
    string(SUBSTRING "${header}" ${begin} -1 notice)
    string(FIND "${notice}" "*/" end)
    if(end LESS 0)
        message(FATAL_ERROR "Unterminated stb notice in imstb_${component}.h")
    endif()
    string(SUBSTRING "${notice}" 0 ${end} notice)
    write_notice("stb-${component}" "${notice}")
endforeach()

if(NOT NOTICE_EMSCRIPTEN)
    copy_notice("${NOTICE_GLFW}/LICENSE.md" GLFW)
    if(NOTICE_CPP)
        finish_embedded_notices()
    endif()
    return()
endif()

copy_notice("${NOTICE_SOURCE}/src/web/fonts/Inter-OFL.txt" Inter-OFL)
copy_notice("${NOTICE_SOURCE}/src/web/fonts/RobotoMono-OFL.txt" RobotoMono-OFL)
copy_notice("${NOTICE_PORTS}/contrib.glfw3/LICENSE.txt" Emscripten-GLFW)
file(READ "${NOTICE_PORTS}/contrib.glfw3/external/GLFW/glfw3.h" header)
string(FIND "${header}" "*/" end)
if(end LESS 0)
    message(FATAL_ERROR "Missing GLFW header notice")
endif()
math(EXPR end "${end} + 2")
string(SUBSTRING "${header}" 0 ${end} notice)
file(WRITE "${NOTICE_OUTPUT}/GLFW.txt" "${notice}\n")

# Homebrew places these package files beside libexec; emsdk keeps them at
# the toolchain root. Runtime libraries are linked into the WebAssembly file.
set(emscripten_package "${NOTICE_EMSCRIPTEN}")
if(NOT EXISTS "${emscripten_package}/LICENSE")
    get_filename_component(emscripten_package "${emscripten_package}" DIRECTORY)
endif()
copy_notice("${emscripten_package}/LICENSE" Emscripten)
copy_notice("${emscripten_package}/AUTHORS" Emscripten-Authors)
copy_notice("${NOTICE_EMSCRIPTEN}/system/lib/libc/musl/COPYRIGHT" musl)
foreach(component libcxx libcxxabi compiler-rt)
    copy_notice("${NOTICE_EMSCRIPTEN}/system/lib/${component}/LICENSE.TXT" "${component}")
endforeach()
