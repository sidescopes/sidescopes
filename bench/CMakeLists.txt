include(FetchContent)

# Pinned to the test suite's Catch2; FetchContent de-duplicates when the tests
# are configured in the same tree.
FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(catch2)

add_executable(sidescopes_bench
    bench_scopes.cpp
    bench_color.cpp
)

# In dynamic module mode the engines live only in the loadable bundles, so the
# bench compiles their sources directly, as the tests do.
if(SIDESCOPES_MODULES_DYNAMIC)
    target_sources(sidescopes_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core/scopes/vectorscope.cpp
        ${CMAKE_SOURCE_DIR}/src/core/scopes/waveform.cpp
        ${CMAKE_SOURCE_DIR}/src/core/scopes/histogram.cpp)
endif()

target_link_libraries(sidescopes_bench PRIVATE
    ${SIDESCOPES_MODULE_LINK_LIBS} sidescopes_core Catch2::Catch2WithMain)

if(SIDESCOPES_MODULES_DYNAMIC)
    add_dependencies(sidescopes_bench
        sidescopes_module_vectorscope
        sidescopes_module_waveform
        sidescopes_module_histogram)
endif()

sidescopes_target_defaults(sidescopes_bench)
