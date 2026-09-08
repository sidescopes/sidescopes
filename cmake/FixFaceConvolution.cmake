# Use the existing bounds-checked path for singleton depthwise spatial axes.
function(sidescopes_fix_face_convolution source_dir output_dir)
    set(original "${source_dir}/modules/dnn/src/layers/cpu_kernels/convolution.cpp")
    file(SHA256 "${original}" source_hash)
    if(NOT source_hash STREQUAL "3b66c26387974050cc84857abaca94d685cbed24204b150a71be443d609fc046")
        message(FATAL_ERROR "Review singleton depthwise handling for changed OpenCV")
    endif()
    file(READ "${original}" source)
    string(FIND "${source}" "void runFastConv(InputArray _input," begin)
    string(FIND "${source}" "\nstatic inline void convBlockMR1NoSIMD(" end)
    math(EXPR length "${end} - ${begin}")
    string(SUBSTRING "${source}" 0 ${begin} prefix)
    string(SUBSTRING "${source}" ${begin} ${length} body)
    string(SUBSTRING "${source}" ${end} -1 suffix)
    string(REPLACE "conv->conv_type" "conv_type" body "${body}")
    set(selector [=[    // The specialized 3x3 depthwise border kernels require both spatial
    // dimensions to exceed one. The generic depthwise path uses the same
    // packed weights and handles simultaneous opposite padding boundaries.
    const bool singleton2D = conv_dim == CONV_2D &&
            (input.size[2] == 1 || input.size[3] == 1);
    const int conv_type = conv->conv_type == CONV_TYPE_DEPTHWISE && singleton2D
            ? CONV_TYPE_DEPTHWISE_REMAIN : conv->conv_type;

]=])
    string(REPLACE "    const bool useFP16 = conv->useFP16;\n"
        "${selector}    const bool useFP16 = conv->useFP16;\n" body "${body}")
    set(candidate "${prefix}${body}${suffix}")
    string(SHA256 candidate_hash "${candidate}")
    if(NOT candidate_hash STREQUAL "0798b18b4108ebcc858f34f284ac127c370dd7714cd62f809524557736bec817")
        message(FATAL_ERROR "Unexpected singleton depthwise source transformation")
    endif()
    file(MAKE_DIRECTORY "${output_dir}")
    set(generated "${output_dir}/face_convolution.cpp")
    file(CONFIGURE OUTPUT "${generated}" CONTENT "${candidate}" @ONLY NEWLINE_STYLE UNIX)
    sidescopes_replace_dnn_source("${original}" "${generated}")
endfunction()
