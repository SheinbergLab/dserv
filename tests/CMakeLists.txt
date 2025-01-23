# Run several end-to-end tests via the dserv executable, looking for expected std out.
add_test(
    NAME not_an_option
    COMMAND dserv --not_an_option
)
set_property(TEST not_an_option PROPERTY PASS_REGULAR_EXPRESSION "Option 'not_an_option' does not exist")

add_test(
    NAME help
    COMMAND dserv --help
)
set_property(TEST help PROPERTY PASS_REGULAR_EXPRESSION "Data server")

add_test(
    NAME trigger_script
    COMMAND dserv --exit --tscript "${CMAKE_SOURCE_DIR}/tests/test_triggers.tcl"
)
set_property(TEST trigger_script PROPERTY PASS_REGULAR_EXPRESSION "\
Start trigger test.
End trigger test.
Handle test/foo: event = 42, latest = 42
Handle test/bar: event = 43, latest = 44
Exiting dserv instead of waiting for messages.
Shutting down..."
)

# Obtain GoogleTest for testing C++ code.
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Run a unit test for datapoint serialization and deserialization.
include_directories("${CMAKE_SOURCE_DIR}/src")
add_executable(datapoint_serialization "datapoint_serialization.cpp" "${CMAKE_SOURCE_DIR}/src/Datapoint.c" "${CMAKE_SOURCE_DIR}/src/Base64.c")
target_link_libraries(datapoint_serialization ${LIBJANSSON} GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(datapoint_serialization)
