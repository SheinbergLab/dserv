cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_BUILD_TYPE Release)

if(WIN32)
	add_definitions(-DUSE_INTERP_RESULT -DUSE_TCL_STUBS)
	include_directories(c:/usr/local/include ../src)
	link_directories(c:/usr/local/lib)
	set(TCLLIB c:/usr/local/lib/tclstub86.lib)
	set(CMAKE_SHARED_LINKER_FLAGS "/NODEFAULTLIB:MSCVRT")
elseif(APPLE)
	add_definitions(-DMACOS -Dunix -DLINUX -fPIC -DUSE_INTERP_RESULT -DUSE_TCL_STUBS)
	include_directories(../src /usr/local/include)
	link_directories(/usr/local/lib)
	set(CMAKE_MODULE_LINKER_FLAGS "-undefined dynamic_lookup")
	set(TCLLIB tclstub8.6)
	set(CMAKE_SHARED_MODULE_SUFFIX .dylib)
elseif(CMAKE_SYSTEM_NAME STREQUAL "QNX")
	add_definitions(-DUSE_INTERP_RESULT -DUSE_TCL_STUBS)
	include_directories(../local/include)
	link_directories(../local/aarch64/lib)
	set(TCLLIB tclstub8.6)
else()
	add_definitions(-DUSE_INTERP_RESULT -DUSE_TCL_STUBS -DLINUX)
	include_directories(../src /usr/local/include)
	link_directories(/usr/local/lib)
	set(TCL_INCLUDE_DIR /usr/include/tcl8.6)
	set(TCLLIB tclstub8.6)
endif()

include_directories(${TCL_INCLUDE_DIR} .)

##
## ain
##
set(MODULE ain)
project(${MODULE})
add_library(${MODULE} MODULE ${MODULE}/${MODULE}.c)
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} pthread)

##
## juicer
##
set(MODULE juicer)
project(${MODULE})
add_library(${MODULE} MODULE ${MODULE}/${MODULE}.c)
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} pthread)

##
## gpio_input
##
set(MODULE gpio_input)
project(${MODULE})
add_library(${MODULE} MODULE ${MODULE}/${MODULE}.c)
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} pthread)

##
## gpio_output
##
set(MODULE gpio_output)
project(${MODULE})
add_library(${MODULE} MODULE ${MODULE}/${MODULE}.c)
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} pthread)

##
## rmt
##
set(MODULE rmt)
project(${MODULE})
add_library(${MODULE} MODULE ${MODULE}/${MODULE}.c)
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} pthread)

##
## eventlog
##
set(MODULE eventlog)
project(${MODULE})
add_library(${MODULE} MODULE ${MODULE}/${MODULE}.c)
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} pthread)

##
## sound
##
set(MODULE sound)
project(${MODULE})
add_library(${MODULE} MODULE ${MODULE}/${MODULE}.c)
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} pthread)

##
## touch
##
set(MODULE touch)
project(${MODULE})
if (UNIX AND NOT APPLE)
set (EVDEVLIB evdev)
include_directories(/usr/include/libevdev-1.0)
endif()
add_library(${MODULE} MODULE ${MODULE}/${MODULE}.c)
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} ${EVDEVLIB} pthread)


