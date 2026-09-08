# Limit the private DNN registry to the pinned face model. Layer implementations,
# importer transformations, CPU dispatch and arithmetic stay upstream code.
function(sidescopes_generate_face_registry source_dir model output)
    set(guards
        "modules/dnn/src/init.cpp|4706fdf18aa3ada28a2cd5acff3ac64f55b0fbcde10a4407183584ec4b9fc9c4"
        "modules/dnn/src/onnx/onnx_importer.cpp|571edc78958f7604d57a6ebf3b6f013b27345a4b0d0bb217babad95772253376"
        "modules/dnn/src/onnx/onnx_graph_simplifier.cpp|d4d155904ce7c1cde25641338e503b861deb7e74ce9bd0f5e6dbf3f25c7f60a9"
        "modules/dnn/src/net_impl.cpp|86b3e6e55496060cffdb7330f292192a6bc9fa19c87cd5d9bd7448024cf7e0ff"
        "modules/dnn/src/net_impl_fuse.cpp|42261ae80f5e22a0a06d98f4a0b0b5a37cc9a3ac376f57e5f58cd819777a7d7d")
    foreach(guard IN LISTS guards)
        string(REPLACE "|" ";" fields "${guard}")
        list(GET fields 0 relative)
        list(GET fields 1 expected)
        file(SHA256 "${source_dir}/${relative}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Review face layer requirements for changed OpenCV: ${relative}")
        endif()
    endforeach()
    file(SHA256 "${model}" model_hash)
    if(NOT model_hash STREQUAL "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4")
        message(FATAL_ERROR "Review layer requirements for the changed face model")
    endif()

    # Eight imported model types, plus constant inputs and named outputs.
    set(required Convolution Pooling ReLU Reshape Resize Sigmoid NaryEltwise Permute Const Identity)
    file(STRINGS "${source_dir}/modules/dnn/src/init.cpp" lines)
    set(registry "")
    set(found "")
    set(registrations 0)
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*CV_DNN_REGISTER_LAYER_CLASS\\(([A-Za-z0-9_]+),[ \t]*[A-Za-z0-9_]+\\);?([ \t]*//.*)?$")
            set(name "${CMAKE_MATCH_1}")
            math(EXPR registrations "${registrations} + 1")
            if(NOT name IN_LIST required)
                continue()
            endif()
            list(APPEND found "${name}")
        endif()
        string(APPEND registry "${line}\n")
    endforeach()
    list(SORT found)
    list(SORT required)
    if(NOT registrations EQUAL 149 OR NOT found STREQUAL required)
        message(FATAL_ERROR "Unexpected OpenCV face layer registry")
    endif()
    get_filename_component(output_dir "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${registry}" @ONLY NEWLINE_STYLE UNIX)
endfunction()

function(sidescopes_trim_face_layers source_dir model output_dir)
    set(original "${source_dir}/modules/dnn/src/init.cpp")
    set(generated "${output_dir}/face_layer_registry.cpp")
    sidescopes_generate_face_registry("${source_dir}" "${model}" "${generated}")
    sidescopes_replace_dnn_source("${original}" "${generated}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${model}"
        "${source_dir}/modules/dnn/src/onnx/onnx_importer.cpp"
        "${source_dir}/modules/dnn/src/onnx/onnx_graph_simplifier.cpp"
        "${source_dir}/modules/dnn/src/net_impl.cpp"
        "${source_dir}/modules/dnn/src/net_impl_fuse.cpp")
endfunction()
