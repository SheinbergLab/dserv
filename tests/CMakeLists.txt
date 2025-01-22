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
    COMMAND dserv --exit --tscript ../../tests/test_triggers.tcl
)
set_property(
    TEST trigger_script
    PROPERTY PASS_REGULAR_EXPRESSION "\
Handle test/foo: event = 42, latest = 42
Handle test/bar: event = 43, latest = 44"
)
