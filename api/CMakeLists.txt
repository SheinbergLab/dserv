cmake_minimum_required(VERSION 3.15)

project(dserv)

set(CMAKE_BUILD_TYPE Release)

add_definitions(-DUSE_TCL_STUBS)

set(TCLLIB tclstub)
find_library(LIBJANSSON NAMES "libjansson.a")

if(WIN32)
	include_directories(c:/usr/local/include ../src)
	link_directories(c:/usr/local/lib)
elseif(APPLE)
	add_compile_options(-fPIC)
	include_directories(/usr/local/include ../src)
	link_directories(/usr/local/lib)
else()
	add_compile_options(-fPIC)
	include_directories(${APP_DIR} /usr/local/include ../src)
	link_directories(/usr/local/lib)
endif()

include_directories(.)

###############################
# dserv
###############################
add_library(dserv SHARED tcl_dserv.c dservapi.c dpoint_tclobj.c ../src/Datapoint.c ../src/Base64.c)

if(WIN32)

elseif(APPLE)
	set(BUNDLE_LOAD "-dynamiclib")
endif()

target_link_libraries(dserv ${TCLLIB} ${LIBJANSSON})

