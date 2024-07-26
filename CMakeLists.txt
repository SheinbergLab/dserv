cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)

project(dserv)

option(DSERV_HAVE_GPIO "using libgpiod to manage gpio lines" OFF)

set(CMAKE_BUILD_TYPE Release)
#set(CMAKE_BUILD_TYPE Debug)
find_library(LIBTCL NAMES "libtcl8.6.a")
find_library(LIBJANSSON NAMES "libjansson.a")
find_library(LIBZ z)
#SET(CMAKE_CXX_FLAGS_DEBUG "-g")

add_subdirectory(modules)
add_subdirectory(processors)
add_subdirectory(tools/essctrl)

if (DSERV_HAVE_GPIO)
   add_compile_definitions(HAVE_GPIO=1)
endif()
   add_executable(dserv src/dserv.cpp src/Base64.c src/Datapoint.c src/Dataserver.cpp src/SendClient.cpp src/LogClient.cpp src/LogTable.cpp src/TclServer.cpp src/dpoint_process.c src/mdns.c src/mdns_advertise.c)

# export symbols for shared objects loaded at runtime
set_property(TARGET dserv PROPERTY ENABLE_EXPORTS ON)

if(APPLE)
  include_directories("/usr/local/include")
  target_link_libraries(dserv PRIVATE
  "-framework CoreFoundation" ${LIBTCL} ${LIBJANSSON} ${LIBZ}
  )
elseif(WIN32)
  include_directories("/usr/local/include" "/usr/include/tcl")
  set(LIBTCL tcl8.6)
  set(LIBZ zlibstatic)
  set(LIBJANSSON jansson)
  target_link_libraries(dserv ${LIBTCL} ${LIBJANSSON} ${LIBZ})
else()
  include_directories("/usr/local/include" "/usr/include/tcl")
  target_link_libraries(dserv ${LIBTCL} ${LIBJANSSON} ${LIBZ} pthread gpiod rt dl)
endif()


install(TARGETS dserv DESTINATION dserv)
install(DIRECTORY config DESTINATION dserv)
install(DIRECTORY pkgs DESTINATION dserv)
install(DIRECTORY systems DESTINATION dserv)
install(DIRECTORY scripts DESTINATION dserv)
install(DIRECTORY www DESTINATION dserv)
