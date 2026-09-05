if(APPLE)
    add_executable(sidescopes_native_tests test_macos_platform.mm)
    target_compile_options(sidescopes_native_tests PRIVATE -fobjc-arc)
elseif(WIN32)
    add_executable(sidescopes_native_tests test_windows_platform.cpp)
    target_compile_definitions(sidescopes_native_tests PRIVATE UNICODE _UNICODE)
else()
    return()
endif()

target_include_directories(sidescopes_native_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support")
target_link_libraries(sidescopes_native_tests PRIVATE sidescopes_platform Catch2::Catch2WithMain)
sidescopes_target_defaults(sidescopes_native_tests)
catch_discover_tests(sidescopes_native_tests TEST_PREFIX "Native: " PROPERTIES LABELS native)

if(WIN32)
    add_executable(sidescopes_native_allocation_tests
        test_windows_allocations.cpp
        ../support/allocation_failure.cpp)
    target_include_directories(sidescopes_native_allocation_tests PRIVATE ../support)
    target_compile_definitions(sidescopes_native_allocation_tests PRIVATE UNICODE _UNICODE)
    target_link_libraries(sidescopes_native_allocation_tests PRIVATE
        sidescopes_platform Catch2::Catch2WithMain)
    sidescopes_target_defaults(sidescopes_native_allocation_tests)
    catch_discover_tests(sidescopes_native_allocation_tests
        TEST_PREFIX "Native allocation: " PROPERTIES LABELS "native;allocation" TIMEOUT 30)
endif()
