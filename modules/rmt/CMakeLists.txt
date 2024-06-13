cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)

set(MODULE rmt)

project(${MODULE})

set(CMAKE_BUILD_TYPE Release)

if(WIN32)
	add_definitions(-DUSE_INTERP_RESULT -DUSE_TCL_STUBS)
	include_directories(c:/usr/local/include ../../src)
	link_directories(c:/usr/local/lib)
elseif(APPLE)
	add_definitions(-DMACOS -Dunix -DLINUX -fPIC -DUSE_INTERP_RESULT -DUSE_TCL_STUBS)
	include_directories(../../src /usr/local/include)
	link_directories(/usr/local/lib)
elseif(CMAKE_SYSTEM_NAME STREQUAL "QNX")
	add_definitions(-DUSE_INTERP_RESULT -DUSE_TCL_STUBS)
	include_directories(../local/include)
	link_directories(../local/aarch64/lib)
	set(TCLLIB tclstub8.6)
else()
	add_definitions(-DUSE_INTERP_RESULT -DUSE_TCL_STUBS -DLINUX)
	include_directories(../../src /usr/local/include)
	link_directories(/usr/local/lib)
	set(TCL_INCLUDE_DIR /usr/include/tcl8.6)
endif()

include_directories(${TCL_INCLUDE_DIR} .)

###############################
# ${MODULE}
###############################

add_library(${MODULE} SHARED src/${MODULE}.c)

if(WIN32)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(TCLLIB c:/usr/local/lib/tclstub86.lib)
	set(CMAKE_SHARED_LINKER_FLAGS "/NODEFAULTLIB:MSCVRT")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
	set(TCLLIB tclstub86.lib)
	set(CMAKE_SHARED_LINKER_FLAGS "/NODEFAULTLIB:MSCVRT")
endif()
elseif(APPLE)
	set(CMAKE_SHARED_LINKER_FLAGS "-dynamiclib -undefined dynamic_lookup")
	set(TCLLIB tclstub8.6)
else()
	set(TCLLIB tclstub8.6)
endif()
set_target_properties(${MODULE} PROPERTIES PREFIX "dserv_")
target_link_libraries(${MODULE} ${DLSH} ${TCLLIB} pthread)


