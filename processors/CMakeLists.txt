cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)

if(APPLE)
set(CMAKE_SHARED_MODULE_SUFFIX .dylib)
endif()

set(PROCESSOR windows)
project(${PROCESSOR})
include_directories(-I.. -I. -I ../src)
add_library(${PROCESSOR} MODULE ${PROCESSOR}.c prmutil.c)
set_target_properties(${PROCESSOR} PROPERTIES PREFIX "")

set(PROCESSOR touch_windows)
project(${PROCESSOR})
include_directories(-I.. -I. -I ../src)
add_library(${PROCESSOR} MODULE ${PROCESSOR}.c prmutil.c)
set_target_properties(${PROCESSOR} PROPERTIES PREFIX "")

set(PROCESSOR up_down_left_right)
project(${PROCESSOR})
include_directories(-I.. -I. -I ../src)
add_library(${PROCESSOR} MODULE ${PROCESSOR}.c prmutil.c)
set_target_properties(${PROCESSOR} PROPERTIES PREFIX "")

set(PROCESSOR in_out)
project(${PROCESSOR})
include_directories(-I.. -I. -I ../src)
add_library(${PROCESSOR} MODULE ${PROCESSOR}.c prmutil.c)
set_target_properties(${PROCESSOR} PROPERTIES PREFIX "")

install(TARGETS touch_windows windows in_out up_down_left_right DESTINATION dserv/processors)




