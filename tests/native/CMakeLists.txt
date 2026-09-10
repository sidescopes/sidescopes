if(TARGET sidescopes_face_network)
    add_executable(sidescopes_face_network_tests test_face_network.cpp)
    target_include_directories(sidescopes_face_network_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_include_directories(sidescopes_face_network_tests SYSTEM PRIVATE
        "${SIDESCOPES_OPENCV_SOURCE}/modules/core/include"
        "${SIDESCOPES_OPENCV_SOURCE}/modules/dnn/include"
        "${PROJECT_BINARY_DIR}")
    target_link_libraries(sidescopes_face_network_tests PRIVATE
        sidescopes_face_network Catch2::Catch2WithMain)
    sidescopes_target_defaults(sidescopes_face_network_tests)
    catch_discover_tests(sidescopes_face_network_tests
        TEST_PREFIX "Face network: " PROPERTIES LABELS face-network TIMEOUT 60)
endif()

if(APPLE)
    add_executable(sidescopes_native_tests test_macos_platform.mm)
    target_compile_options(sidescopes_native_tests PRIVATE -fobjc-arc)
elseif(WIN32)
    add_executable(sidescopes_native_tests test_windows_platform.cpp)
    target_compile_definitions(sidescopes_native_tests PRIVATE UNICODE _UNICODE)
else()
    return()
endif()

target_sources(sidescopes_native_tests PRIVATE test_face_detection.cpp)

target_include_directories(sidescopes_native_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support")
target_link_libraries(sidescopes_native_tests PRIVATE sidescopes_platform Catch2::Catch2WithMain)
sidescopes_target_defaults(sidescopes_native_tests)
catch_discover_tests(sidescopes_native_tests TEST_PREFIX "Native: " PROPERTIES LABELS native)

if(APPLE)
    add_executable(sidescopes_capture_completion_tests
        test_macos_capture_completion.mm ../support/allocation_failure.cpp)
    target_compile_options(sidescopes_capture_completion_tests PRIVATE -fobjc-arc)
    target_include_directories(sidescopes_capture_completion_tests PRIVATE ../support)
    target_link_libraries(sidescopes_capture_completion_tests PRIVATE sidescopes_platform Catch2::Catch2WithMain)
    sidescopes_target_defaults(sidescopes_capture_completion_tests)
    catch_discover_tests(sidescopes_capture_completion_tests
        TEST_PREFIX "Capture completion: " PROPERTIES LABELS native TIMEOUT 30)
endif()

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
        TEST_PREFIX "Native allocation: " PROPERTIES LABELS native TIMEOUT 30)
endif()

# Catch 3.7 flattens list-valued PROPERTIES across discovery. Add the second
# label only after its generated test lists have reached CTest.
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/allocation_labels.cmake")
