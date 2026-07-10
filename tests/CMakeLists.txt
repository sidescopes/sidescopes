include(FetchContent)

FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(catch2)

add_executable(sidescopes_tests
    test_frame.cpp
    test_vectorscope.cpp
)
target_link_libraries(sidescopes_tests PRIVATE sidescopes_core Catch2::Catch2WithMain)
sidescopes_target_defaults(sidescopes_tests)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(sidescopes_tests)
