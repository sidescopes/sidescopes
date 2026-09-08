include(FetchContent)

# Configure the dependency in its own variable scope. Its BUILD_* options
# must not change SideScopes, GLFW, or the test suite.
function(sidescopes_add_face_network)
    # OpenCV lowers its policy version. Keep its cache declarations from
    # removing the normal variables that isolate shared BUILD_* options.
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    FetchContent_Declare(opencv
        URL https://github.com/opencv/opencv/archive/fe38fc608f6acb8b68953438a62305d8318f4fcd.tar.gz
        URL_HASH SHA256=6a7508554941c1a698c243b2212b2985ce59a65a3e0348d53c2158607d801e61
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        # Fetch first; the scoped configuration below supplies all options
        # before adding OpenCV's actual root directory.
        SOURCE_SUBDIR sidescopes-source-only)
    FetchContent_MakeAvailable(opencv)

    # Optional integrations are a closed set in this pinned source. Disable
    # them before enabling only the bundled ONNX parser dependency.
    file(STRINGS "${opencv_SOURCE_DIR}/CMakeLists.txt" options REGEX "^OCV_OPTION\\(WITH_")
    foreach(option IN LISTS options)
        string(REGEX MATCH "^OCV_OPTION\\((WITH_[A-Za-z0-9_]+)" matched "${option}")
        set(${CMAKE_MATCH_1} OFF)
    endforeach()
    set(WITH_PROTOBUF ON)
    set(BUILD_PROTOBUF ON)
    set(PROTOBUF_UPDATE_FILES OFF)
    set(BUILD_ZLIB ON)
    # These names belong only to OpenCV. Pin their cache values too because
    # its module configuration uses set(CACHE), rather than option().
    set(BUILD_LIST core,imgproc,dnn CACHE STRING "OpenCV modules used by face detection" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_opencv_gapi OFF)
    # Module discovery runs GAPI's initializer even when GAPI is disabled.
    set(WITH_ADE OFF CACHE BOOL "Face inference does not use the graph framework" FORCE)
    set(BUILD_opencv_apps OFF)
    set(BUILD_opencv_python2 OFF)
    set(BUILD_opencv_python3 OFF)
    set(BUILD_JAVA OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_PERF_TESTS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOCS OFF)
    set(BUILD_PACKAGE OFF)
    set(DNN_ENABLE_PLUGINS OFF CACHE BOOL "Face inference uses the built-in CPU backend" FORCE)
    set(PARALLEL_ENABLE_PLUGINS OFF CACHE BOOL "Face inference uses no external thread backend" FORCE)
    set(OPENCV_DNN_OPENCL OFF)
    set(OPENCV_DNN_CUDA OFF)
    set(OPENCV_DNN_OPENVINO OFF)
    set(ENABLE_CCACHE OFF)
    set(ENABLE_PRECOMPILED_HEADERS OFF)
    set(ENABLE_FAST_MATH OFF)
    set(ENABLE_LTO OFF)
    # Preserve the application's runtime choice, including the static CRT
    # used by portable release archives. Do not let OpenCV override it.
    set(BUILD_WITH_STATIC_CRT OFF)
    # Query the compiler's target, not the host running a cross build. Other
    # architectures retain OpenCV's own baseline and dispatch defaults.
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
        #if defined(_M_ARM64EC) || (!defined(_M_IX86) && !defined(_M_X64) && !defined(__i386__) && !defined(__x86_64__))
        #error Not an x86 target
        #endif
        int main() { return 0; }
    " SIDESCOPES_FACE_TARGET_X86)
    if(SIDESCOPES_FACE_TARGET_X86)
        set(CPU_BASELINE SSE2)
        set(CPU_DISPATCH "SSE4_1;SSE4_2;AVX;FP16;AVX2")
    endif()
    # These legacy cache settings escape directory and function scopes.
    # Keep the application's executable/package paths and install prefix.
    foreach(setting IN ITEMS EXECUTABLE_OUTPUT_PATH CMAKE_INSTALL_PREFIX)
        if(DEFINED CACHE{${setting}})
            set(saved_${setting}_defined TRUE)
            get_property(saved_${setting}_value CACHE ${setting} PROPERTY VALUE)
            get_property(saved_${setting}_type CACHE ${setting} PROPERTY TYPE)
            get_property(saved_${setting}_help CACHE ${setting} PROPERTY HELPSTRING)
        endif()
    endforeach()
    # OpenCV sets cache properties even when the caller supplied only a normal
    # build-type variable, as happens with multi-configuration generators.
    set(temporary_build_type_cache FALSE)
    if(NOT DEFINED CACHE{CMAKE_BUILD_TYPE})
        set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING "Build type")
        set(temporary_build_type_cache TRUE)
    endif()
    add_subdirectory("${opencv_SOURCE_DIR}" "${opencv_BINARY_DIR}" EXCLUDE_FROM_ALL)
    if(temporary_build_type_cache)
        unset(CMAKE_BUILD_TYPE CACHE)
    endif()
    foreach(setting IN ITEMS EXECUTABLE_OUTPUT_PATH CMAKE_INSTALL_PREFIX)
        if(saved_${setting}_defined)
            set(${setting} "${saved_${setting}_value}" CACHE
                "${saved_${setting}_type}" "${saved_${setting}_help}" FORCE)
        else()
            unset(${setting} CACHE)
        endif()
    endforeach()
    set(expected_modules opencv_core opencv_dnn opencv_imgproc)
    set(actual_modules ${OPENCV_MODULES_BUILD})
    list(SORT actual_modules)
    if(NOT actual_modules STREQUAL expected_modules OR
       DNN_ENABLE_PLUGINS OR PARALLEL_ENABLE_PLUGINS OR TARGET ade)
        message(FATAL_ERROR "Unexpected OpenCV face detection dependency configuration")
    endif()

    set(model "${CMAKE_SOURCE_DIR}/assets/models/face_detection_yunet.onnx")
    set(model_source "${CMAKE_CURRENT_BINARY_DIR}/generated/face_model_data.cpp")
    add_custom_command(OUTPUT "${model_source}"
        COMMAND ${CMAKE_COMMAND}
            "-DMODEL_INPUT=${model}"
            "-DMODEL_OUTPUT=${model_source}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFaceModel.cmake"
        DEPENDS "${model}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFaceModel.cmake"
        COMMENT "Embedding the face detection model"
        VERBATIM)
    add_library(sidescopes_face_network STATIC
        "${CMAKE_SOURCE_DIR}/src/platform/windows/face_network.cpp"
        "${model_source}")
    target_include_directories(sidescopes_face_network PRIVATE "${CMAKE_SOURCE_DIR}/src")
    # OpenCV's in-tree targets use directory includes, without exporting a
    # consumer include interface. Its public headers remain third-party code.
    target_include_directories(sidescopes_face_network SYSTEM PRIVATE
        "${opencv_SOURCE_DIR}/modules/core/include"
        "${opencv_SOURCE_DIR}/modules/imgproc/include"
        "${opencv_SOURCE_DIR}/modules/dnn/include"
        "${OPENCV_CONFIG_FILE_INCLUDE_DIR}")
    target_link_libraries(sidescopes_face_network PRIVATE sidescopes_core opencv_core opencv_imgproc opencv_dnn)
    sidescopes_target_defaults(sidescopes_face_network)
    # Distribution notices come from the same pinned source as the binary.
    set(SIDESCOPES_OPENCV_SOURCE "${opencv_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# OpenCV's bundled zlib is C. Enable it in the caller's directory before the
# scoped function configures the dependency.
enable_language(C)
sidescopes_add_face_network()
