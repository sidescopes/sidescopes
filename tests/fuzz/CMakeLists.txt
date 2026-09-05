foreach(boundary IN ITEMS preferences geometry modules)
    if(boundary STREQUAL "modules")
        set(helper module)
    else()
        set(helper ${boundary})
    endif()
    add_executable(sidescopes_fuzz_${boundary}
        ${boundary}.cpp ../support/${helper}_properties.cpp)
    target_include_directories(sidescopes_fuzz_${boundary} PRIVATE ../support)
    target_link_libraries(sidescopes_fuzz_${boundary} PRIVATE
        sidescopes_app ${SIDESCOPES_MODULE_LINK_LIBS} sidescopes_core)
    target_link_options(sidescopes_fuzz_${boundary} PRIVATE -fsanitize=fuzzer)
    sidescopes_target_defaults(sidescopes_fuzz_${boundary})
endforeach()
target_sources(sidescopes_fuzz_geometry PRIVATE
    ${CMAKE_SOURCE_DIR}/src/platform/region_geometry.cpp)
