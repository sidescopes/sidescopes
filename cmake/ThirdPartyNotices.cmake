# Collect notices from the same dependency sources that produce the binary.
# Called after linking so Emscripten's port is present even on a cold build.
function(sidescopes_add_notices target)
    set(output "$<TARGET_FILE_DIR:${target}>/licenses")
    if(APPLE AND NOT EMSCRIPTEN)
        set(output "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/Licenses")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DNOTICE_OUTPUT=${output}"
            "-DNOTICE_SOURCE=${CMAKE_SOURCE_DIR}"
            "-DNOTICE_IMGUI=${imgui_SOURCE_DIR}"
            "-DNOTICE_NANOSVG=${nanosvg_SOURCE_DIR}"
            "-DNOTICE_GLFW=${glfw_SOURCE_DIR}"
            "-DNOTICE_EMSCRIPTEN=${EMSCRIPTEN_ROOT_PATH}"
            "-DNOTICE_PORTS=${EMSCRIPTEN_SYSROOT}/../ports"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        VERBATIM)
    file(GLOB notices CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/licenses/*.txt")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${imgui_SOURCE_DIR}/LICENSE.txt"
        "${nanosvg_SOURCE_DIR}/LICENSE.txt"
        ${notices})
    if(EMSCRIPTEN)
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_SOURCE_DIR}/src/web/fonts/Inter-OFL.txt"
            "${CMAKE_SOURCE_DIR}/src/web/fonts/RobotoMono-OFL.txt")
    else()
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${glfw_SOURCE_DIR}/LICENSE.md")
    endif()
endfunction()

if(NOT CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

function(copy_notice source name)
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Missing distribution notice: ${source}")
    endif()
    configure_file("${source}" "${NOTICE_OUTPUT}/${name}.txt" COPYONLY)
endfunction()

file(MAKE_DIRECTORY "${NOTICE_OUTPUT}")
copy_notice("${NOTICE_SOURCE}/LICENSE" SideScopes-GPL)
copy_notice("${NOTICE_IMGUI}/LICENSE.txt" Dear-ImGui)
copy_notice("${NOTICE_NANOSVG}/LICENSE.txt" NanoSVG)
copy_notice("${NOTICE_SOURCE}/licenses/Lucide.txt" Lucide)
copy_notice("${NOTICE_SOURCE}/licenses/ProggyClean.txt" ProggyClean)
copy_notice("${NOTICE_SOURCE}/licenses/ProggyForever.txt" ProggyForever)

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
    file(WRITE "${NOTICE_OUTPUT}/stb-${component}.txt" "${notice}")
endforeach()

if(NOT NOTICE_EMSCRIPTEN)
    copy_notice("${NOTICE_GLFW}/LICENSE.md" GLFW)
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
