# Unit test for the send-cycle guard protocol (SendGuard.h, incl. the
# racing-mutual-senders Dekker property) and SharedQueue::wait_pop.
find_package(Threads REQUIRED)
add_executable(test_send_guard test_send_guard.cpp)
target_link_libraries(test_send_guard PRIVATE Threads::Threads)
add_test(NAME send_guard_unit COMMAND test_send_guard)
set_property(TEST send_guard_unit PROPERTY PASS_REGULAR_EXPRESSION "ALL PASS")

# End-to-end: a real dserv with two subprocess interps.  Pre-guard, the
# cycle send wedges both interp threads forever -- the TIMEOUT property
# turns that hang into a failure.  DSERV_SEND_TIMEOUT_MS shortens the
# timeout leg of the test.
add_test(
    NAME send_cycle
    COMMAND dserv -c "${CMAKE_SOURCE_DIR}/tests/test_send_cycle.tcl"
)
set_property(TEST send_cycle PROPERTY ENVIRONMENT "DSERV_SEND_TIMEOUT_MS=1000")
set_property(TEST send_cycle PROPERTY TIMEOUT 60)
# single self-asserted verdict line: listener bind-retry noise (another
# dserv holding the fixed ports) interleaves with script output, so a
# strict multi-line match is not robust
set_property(TEST send_cycle PROPERTY PASS_REGULAR_EXPRESSION "SEND CYCLE TEST: ALL PASS")

# End-to-end: tpool_queue (pull-based worker pool) -- every task done
# once, empty results not counted, -want quota exact, a failing task does
# not stop its worker, setup failure reported; and tpool_map unchanged.
add_test(
    NAME tpool_queue
    COMMAND dserv -c "${CMAKE_SOURCE_DIR}/tests/test_tpool_queue.tcl"
)
set_property(TEST tpool_queue PROPERTY TIMEOUT 120)
set_property(TEST tpool_queue PROPERTY PASS_REGULAR_EXPRESSION "TPOOL QUEUE TEST: ALL PASS")

# End-to-end: `subprocess` duplicate-name error is loud, clean (no leaked
# int-parse text) and carries the re-source hint.
add_test(
    NAME subprocess_dup
    COMMAND dserv -c "${CMAKE_SOURCE_DIR}/tests/test_subprocess_dup.tcl"
)
set_property(TEST subprocess_dup PROPERTY TIMEOUT 60)
set_property(TEST subprocess_dup PROPERTY PASS_REGULAR_EXPRESSION "SUBPROCESS DUP TEST: ALL PASS")

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
# Real ess-2.0.tm under the dlsh ess_test harness against a fake stim2: a
# stim script that fails to source must be reported (warning, or a failed
# load when stim is required), never logged as "Loaded". Needs dlsh and a
# systems tree, so it is registered only where both exist (dev machines).
#
find_program(DLSH_EXECUTABLE NAMES dlsh)
if(DLSH_EXECUTABLE AND EXISTS "$ENV{HOME}/systems/ess/remap/doublestep")
  add_test(
      NAME ess_stim_source
      COMMAND ${DLSH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_ess_stim_source.tcl"
  )
  set_property(TEST ess_stim_source PROPERTY ENVIRONMENT "ESS_TEST_ESS_LIB=${CMAKE_SOURCE_DIR}/lib")
  set_property(TEST ess_stim_source PROPERTY TIMEOUT 120)
  set_property(TEST ess_stim_source PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")
else()
  message(STATUS "dlsh or ~/systems/ess not found -- skipping ess_stim_source")
endif()

#
# Swap-tag params arrive from stim2 as floats; stim_tag_evt must hand
# whole-number ones to an integer (short/long) event as integers, or the
# event is refused and lost (lib/ess-2.0.tm stim_tag_params).  Needs dlsh only.
#
if(DLSH_EXECUTABLE)
  add_test(
      NAME ess_stim_tag_params
      COMMAND ${DLSH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_ess_stim_tag_params.tcl"
  )
  set_property(TEST ess_stim_tag_params PROPERTY TIMEOUT 30)
  set_property(TEST ess_stim_tag_params PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")

  # stim2's flipwall is on the stim host's wall clock (UTC); mapping it with
  # ptp/d_us (TAI) put every stim-tagged event 37 s early on a PTP site
  # (lib/ess-2.0.tm stim_evt_time / stim_evt_wall_ok).
  add_test(
      NAME ess_stim_evt_time
      COMMAND ${DLSH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_ess_stim_evt_time.tcl"
  )
  set_property(TEST ess_stim_evt_time PROPERTY TIMEOUT 30)
  set_property(TEST ess_stim_evt_time PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")
endif()

#
# df::add_recorded_streams (lib/df-2.0.tm) on a synthetic obs DG: the
# streams a file's decode keys name reach the trials DG with no extractor
# involved. Needs dlsh for the dl_* commands, nothing else.
#
if(DLSH_EXECUTABLE)
  add_test(
      NAME df_recorded_streams
      COMMAND ${DLSH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_df_recorded_streams.tcl"
  )
  set_property(TEST df_recorded_streams PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")
  # df::add_display_times: events' flip and light columns (the evt_display
  # and photo_verify records, joined by swap)
  add_test(
      NAME df_display_times
      COMMAND ${DLSH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_df_display_times.tcl"
  )
  set_property(TEST df_display_times PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")
else()
  message(STATUS "dlsh not found -- skipping df_recorded_streams")
endif()

#
# Module unit tests -- pure Tcl, dserv STUBBED.
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
  foreach(t ess_dial ess_dial_dpad ess_dial_sectors ess_dial_threshold ess_roam ess_sling ess_binds ess_camera ess_resolve ess_live_params ess_boundary_stop ess_queue_hold ess_participant ess_transports ess_joystick_labels ess_workgroup_pair ess_sync_libs_gone ess_photo ess_photo_verify ess_photo_test ess_evt_display ess_obs_hold photopair extio_record extio_models settings rig_settings)
    add_test(
        NAME ${t}
        COMMAND ${TCLSH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_${t}.tcl"
    )
    set_property(TEST ${t} PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")
  endforeach()
else()
  message(STATUS "tclsh not found -- skipping module unit tests")
endif()

#
# processors/photo.c -- the photodiode edge detector -- on synthetic extio
# analog blocks with analytic edges: an LCD's exponential approach, a
# fast toggle, PWM ripple, a strobed (Display++) flash train, batched
# blocks, and the '*' in an attached name (src/dpoint_process.c).
#
add_executable(test_photo test_photo.c
    ${CMAKE_SOURCE_DIR}/processors/prmutil.c
    ${CMAKE_SOURCE_DIR}/src/dpoint_process.c)
target_include_directories(test_photo PRIVATE
    ${CMAKE_SOURCE_DIR}/processors ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_photo PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(NOT APPLE AND NOT WIN32)
  target_link_libraries(test_photo PRIVATE m)
endif()
add_test(NAME photo_processor COMMAND test_photo)
set_property(TEST photo_processor PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")
set_property(TEST photo_processor PROPERTY TIMEOUT 60)

#
# The rmtSend wrapper that records every stim2 command as a stim/cmd JSON
# datapoint (lib/ess-2.0.tm, stim_cmd_log block): a real dserv interp with
# the real rmt module loaded, against a fake stim2 (tests/fake_stim2.tcl,
# a child tclsh -- the dserv interps run no event loop, a socket server
# needs one). Needs tclsh for the fake only.
#
if(TCLSH_EXECUTABLE)
  add_test(
      NAME ess_stim_cmd
      COMMAND dserv -c "${CMAKE_SOURCE_DIR}/tests/test_ess_stim_cmd.tcl"
  )
  set_property(TEST ess_stim_cmd PROPERTY ENVIRONMENT
      "DSERV_RMT_MODULE=$<TARGET_FILE:rmt>;ESS_TEST_TCLSH=${TCLSH_EXECUTABLE}")
  set_property(TEST ess_stim_cmd PROPERTY TIMEOUT 60)
  set_property(TEST ess_stim_cmd PROPERTY PASS_REGULAR_EXPRESSION "STIM CMD TEST: ALL PASS")
endif()

#
# modules/mdns -- the `_dserv._tcp` DNS-SD advertisement.
#
# A C module, not a .tm, and it binds tclserver_* from whatever loads it, so
# tclsh cannot host it. test_mdns_host is a bare interp exporting inert
# stand-ins (ENABLE_EXPORTS puts them in the dynamic symbol table, which is
# where the module's undefined references resolve). The script's live
# checks need a responder and skip themselves where there is none, so this
# passes in a CI container as well as on a Mac or a rig.
#
if(TARGET mdns)
  add_executable(test_mdns_host test_mdns_host.c)
  target_include_directories(test_mdns_host PRIVATE
      ${CMAKE_SOURCE_DIR}/src ${TCL_H_DIR})
  target_link_libraries(test_mdns_host PRIVATE ${LIBTCL})
  set_target_properties(test_mdns_host PROPERTIES ENABLE_EXPORTS ON)
  add_test(
      NAME mdns
      COMMAND test_mdns_host "${CMAKE_SOURCE_DIR}/tests/test_mdns.tcl"
              $<TARGET_FILE:mdns>
  )
  set_property(TEST mdns PROPERTY PASS_REGULAR_EXPRESSION "all checks passed")
  set_property(TEST mdns PROPERTY TIMEOUT 30)
endif()
