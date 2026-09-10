foreach(test IN LISTS
    sidescopes_capture_completion_tests_TESTS
    sidescopes_native_allocation_tests_TESTS)
    set_tests_properties("${test}" PROPERTIES LABELS "native;allocation")
endforeach()
