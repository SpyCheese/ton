set(TONLIB_TESTS
    test-ffi-event-loop
    test-ffi-awaitable
)

foreach(TEST_NAME IN LISTS TONLIB_TESTS)
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)

    target_link_libraries(${TEST_NAME} PRIVATE
        tdactor
        tdutils
        tonlibjson_objects
        td-test-main
    )

    ton_test(${TEST_NAME})
endforeach()
