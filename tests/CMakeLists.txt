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

#
# ESS unit tests -- pure Tcl, dserv STUBBED.
#
# Unlike the tests above these need no server, no rig and no hardware: they
# source the .tm modules directly and drive them with fake datapoints and a
# fake clock. That is what makes the dial's geometry and its state machine
# testable at all, and both of the bugs they caught were invisible on a rig
# (a guard that returned before the cursor set out; an undeclared `variable`
# that Tcl silently makes a local).
#
# Guarded on tclsh being present so a build host without it still configures.
#
find_program(TCLSH_EXECUTABLE NAMES tclsh tclsh9.0 tclsh8.6)
if(TCLSH_EXECUTABLE)
  foreach(t ess_dial ess_dial_dpad ess_dial_sectors ess_binds ess_resolve)
    add_test(
        NAME ${t}
        COMMAND ${TCLSH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_${t}.tcl"
    )
    set_property(TEST ${t} PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")
  endforeach()
else()
  message(STATUS "tclsh not found -- skipping ESS unit tests")
endif()
