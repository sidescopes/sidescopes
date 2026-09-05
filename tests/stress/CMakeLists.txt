add_executable(sidescopes_lifecycle_stress
    test_region_lifecycle.cpp
    ../support/desktop_stubs.cpp
    ../support/region_overlay_stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/region_geometry.cpp)
target_include_directories(sidescopes_lifecycle_stress PRIVATE ../support)
target_link_libraries(sidescopes_lifecycle_stress PRIVATE
    sidescopes_app ${SIDESCOPES_MODULE_LINK_LIBS} sidescopes_core Catch2::Catch2WithMain)
if(SIDESCOPES_MODULES_DYNAMIC)
    add_dependencies(sidescopes_lifecycle_stress
        sidescopes_module_vectorscope
        sidescopes_module_waveform
        sidescopes_module_histogram)
endif()
sidescopes_target_defaults(sidescopes_lifecycle_stress)
catch_discover_tests(sidescopes_lifecycle_stress
    TEST_PREFIX "Stress: " PROPERTIES LABELS stress TIMEOUT 300)

if(APPLE)
    add_executable(sidescopes_native_stress test_macos_lifecycle.mm)
    target_link_libraries(sidescopes_native_stress PRIVATE
        sidescopes_platform Catch2::Catch2WithMain)
    target_compile_options(sidescopes_native_stress PRIVATE -fobjc-arc)
    sidescopes_target_defaults(sidescopes_native_stress)
    catch_discover_tests(sidescopes_native_stress
        TEST_PREFIX "Native stress: " PROPERTIES LABELS stress TIMEOUT 300)
endif()
