cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(dserv VERSION 1.3)

link_directories(/usr/local/lib)

set(CMAKE_BUILD_TYPE Release)
#set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_CXX_FLAGS_DEBUG "-g")
#set(CMAKE_C_FLAGS_DEBUG "-g")
#find_library(LIBTCL NAMES "libtcl9.0.a")
if (WIN32)
link_directories(c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH})
set(LIBTCL tcl90)
set(LIBJANSSON libjansson.lib)
set(LIBZ zlibstatic.lib)
else()
set(LIBTCL tcl9.0)
find_library(LIBJANSSON NAMES "libjansson.a")
find_library(LIBZ z)
endif()

add_subdirectory(modules)
add_subdirectory(processors)
add_subdirectory(tools/essctrl)

configure_file(src/dservConfig.h.in ../src/dservConfig.h)

add_executable(dserv src/dserv.cpp src/Base64.c src/Datapoint.c src/Dataserver.cpp src/SendClient.cpp src/LogClient.cpp src/LogTable.cpp src/TclServer.cpp src/dpoint_process.c src/mdns.c src/mdns_advertise.c)

# export symbols for shared objects loaded at runtime
set_property(TARGET dserv PROPERTY ENABLE_EXPORTS ON)

if(APPLE)
  include_directories("/usr/local/include")
  target_link_libraries(dserv PRIVATE
  "-framework CoreFoundation" ${LIBTCL} ${LIBJANSSON} ${LIBZ}
  )
elseif(WIN32)
  include_directories("/usr/local/include")
  set(LIBTCL tcl90)
  set(LIBZ zlibstatic)
  set(LIBJANSSON jansson)
  target_link_libraries(dserv ${LIBTCL} ${LIBJANSSON} ${LIBZ})
else()
  include_directories("/usr/local/include")
  target_link_libraries(dserv ${LIBTCL} ${LIBJANSSON} ${LIBZ} pthread rt dl)
endif()


install(TARGETS dserv DESTINATION dserv)
install(DIRECTORY config DESTINATION dserv)
install(DIRECTORY lib DESTINATION dserv)
install(DIRECTORY db DESTINATION dserv)
install(DIRECTORY local DESTINATION dserv)
install(DIRECTORY systems DESTINATION dserv)
install(DIRECTORY systemd DESTINATION dserv)
install(DIRECTORY scripts DESTINATION dserv)
install(DIRECTORY www DESTINATION dserv)

enable_testing()
add_subdirectory("tests")

