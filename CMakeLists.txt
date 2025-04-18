cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)

if(NOT DEFINED PROJECT_VERSION)
  set(PROJECT_VERSION 1.3)
endif()
project(dserv VERSION ${PROJECT_VERSION})

set(CMAKE_BUILD_TYPE Release)
#set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_CXX_FLAGS_DEBUG "-g")
#set(CMAKE_C_FLAGS_DEBUG "-g")
#find_library(LIBTCL NAMES "libtcl9.0.a")
if(APPLE)
    # libtcl9.0 may be installed from source or by homebrew.
    include_directories("/usr/local/include" "/opt/homebrew/include/tcl-tk")
    link_directories("/usr/local/lib" "/opt/homebrew/lib")
    find_library(LIBTCL tcl9.0)
    find_library(LIBJANSSON NAMES "libjansson.a")
    find_library(LIBZ z)
elseif (WIN32)
    include_directories("/usr/local/include")
    link_directories(c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH})
    set(LIBTCL tcl90)
    set(LIBJANSSON libjansson.lib)
    set(LIBZ zlibstatic.lib)
else()
    include_directories("/usr/local/include")
    find_library(LIBTCL tcl9.0)
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
    target_link_libraries(dserv PRIVATE "-framework CoreFoundation" ${LIBTCL} ${LIBJANSSON} ${LIBZ})
elseif(WIN32)
    target_link_libraries(dserv ${LIBTCL} ${LIBJANSSON} ${LIBZ})
else()
    target_link_libraries(dserv ${LIBTCL} ${LIBJANSSON} ${LIBZ} pthread rt dl)
endif()

enable_testing()
add_subdirectory("tests")

install(TARGETS dserv DESTINATION dserv COMPONENT dserv)
install(DIRECTORY config DESTINATION dserv COMPONENT dserv)
install(DIRECTORY lib DESTINATION dserv COMPONENT dserv)
install(DIRECTORY db DESTINATION dserv COMPONENT dserv)
install(DIRECTORY local DESTINATION dserv COMPONENT dserv)
install(DIRECTORY systems DESTINATION dserv COMPONENT dserv)
install(DIRECTORY systemd DESTINATION dserv COMPONENT dserv)
install(DIRECTORY scripts DESTINATION dserv COMPONENT dserv)
install(DIRECTORY www DESTINATION dserv COMPONENT dserv)

set(CPACK_PACKAGE_NAME dserv)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Data server system for data acquisition and control")
set(CPACK_PACKAGE_CONTACT SheinbergLab)
set(CPACK_PACKAGE_VENDOR SheinbergLab)

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${LIBTCL})
include(InstallRequiredSystemLibraries)

if(WIN32)
    # TODO
elseif(APPLE)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_COMPONENTS_ALL dserv essctrl)

    if(DEFINED "CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM")
        # Configure for apple code signing, notarization, and gatekeeper/entitlements.
        set(CMAKE_XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--strict --timestamp --options=runtime")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_INJECT_BASE_ENTITLEMENTS "NO")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")
        set(CMAKE_SKIP_RPATH TRUE)

        # These entitlements paths seem redundant.
        # It seems cmake uses the first for building, and cpack uses the second for packaging.
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${CMAKE_CURRENT_SOURCE_DIR}/macos/dserv.entitlements)
        set(CODE_SIGN_ENTITLEMENTS ${CMAKE_CURRENT_SOURCE_DIR}/macos/dserv.entitlements)

        # Set up .pkg metadata.
        list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/macos)
        set(CPACK_PRODUCTBUILD_IDENTIFIER "org.sheinberglab")
        set(CPACK_RESOURCE_FILE_WELCOME ${CMAKE_SOURCE_DIR}/macos/welcome.txt)
        set(CPACK_RESOURCE_FILE_README ${CMAKE_SOURCE_DIR}/macos/readme.txt)
        set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/macos/license.txt)

        # Specify a postinstall script (eg relax permissions on /usr/local/dserv/db/).
        set(CPACK_POSTFLIGHT_DSERV_SCRIPT ${CMAKE_SOURCE_DIR}/macos/postinstall)
    endif()
else()
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libevdev2, libpq5, zlib1g, yajl-tools")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/dpkg/postinst)
    install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/dpkg/install-dserv-service.sh DESTINATION bin)
endif()

include(CPack)
cpack_add_component(dserv essctrl REQUIRED)
