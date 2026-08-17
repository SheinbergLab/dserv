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
    COMMAND dserv --tscript "${CMAKE_SOURCE_DIR}/tests/test_triggers.tcl"
)
set_property(TEST trigger_script PROPERTY PASS_REGULAR_EXPRESSION "\
Start trigger test.
End trigger test.
Handle test/foo: event = 42, latest = 42
Handle test/bar: event = 43, latest = 44
Done processing datapoints."
)

add_test(
    NAME private_dpoints
    COMMAND dserv --tscript "${CMAKE_SOURCE_DIR}/tests/test_private.tcl"
)
set_property(TEST private_dpoints PROPERTY PASS_REGULAR_EXPRESSION "\
Start private test.
public get: hello
keys has secret: 1
get denied: 1
deny message: dpoint \"test/secret\" is private
timestamp visible: 1
copy denied: 1 private
End private test.
TRIGGER test/pub = world
Private test done."
)

add_test(
    NAME private_logger
    COMMAND dserv --cscript "${CMAKE_SOURCE_DIR}/tests/test_private_logger.tcl"
)
set_property(TEST private_logger PROPERTY PASS_REGULAR_EXPRESSION "\
Start private logger test.
End private logger test.
DPOINT test/pub = logged_public
Checking private log.
log secret: private val=logged_secret
log public: public val=logged_public
Private logger test done."
)

add_test(
    NAME logger_script
    COMMAND dserv --cscript "${CMAKE_SOURCE_DIR}/scripts/tcl/test_logger.tcl"
)
set_property(TEST logger_script PROPERTY PASS_REGULAR_EXPRESSION "\
Checking log contents.
Log contains 101 varnames \\(expecting 101\\)
varname 0 is logger:open \\(expecting logger:open\\)
Checked 101 varnames.
Log contains 101 vals \\(expecting 101\\)
vals 0 is 3 \\(expecting 3\\)
Checked 101 vals."
)
