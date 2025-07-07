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

# Add uWebSockets as external project
include(FetchContent)

# Fetch uWebSockets from GitHub
FetchContent_Declare(
  uwebsockets
  GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
  GIT_TAG        v20.62.0  # Latest stable version as of early 2024
)

FetchContent_Declare(
  usockets
  GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
  GIT_TAG        v0.8.8   # Match with uWebSockets version
)

# Make both available
FetchContent_MakeAvailable(usockets uwebsockets)

# Add uWebSockets include directories
include_directories(${uwebsockets_SOURCE_DIR}/src)
include_directories(${usockets_SOURCE_DIR}/src)

# Also add the parent directory for proper includes
include_directories(${uwebsockets_SOURCE_DIR})

# Platform-specific configuration
if(APPLE)
    # libtcl9.0 may be installed from source or by homebrew.
    include_directories("/usr/local/include" "/opt/homebrew/include/tcl-tk")
    link_directories("/usr/local/lib" "/opt/homebrew/lib")
    find_library(LIBTCL tcl9.0)
    find_library(LIBJANSSON NAMES "libjansson.a")
    find_library(LIBZ z)
    # macOS requires libuv for uSockets
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUV libuv)
        if(NOT LIBUV_FOUND)
            find_library(LIBUV_LIBRARIES NAMES uv libuv)
            if(LIBUV_LIBRARIES)
                set(LIBUV_FOUND TRUE)
            endif()
        endif()
    else()
        find_library(LIBUV_LIBRARIES NAMES uv libuv)
        if(LIBUV_LIBRARIES)
            set(LIBUV_FOUND TRUE)
        endif()
    endif()
    
    if(NOT LIBUV_FOUND)
        message(FATAL_ERROR "libuv is required on macOS. Install with: brew install libuv")
    endif()
    
    set(USOCKETS_LIBS ${LIBUV_LIBRARIES})
    set(WITH_LIBUV 1)
elseif (WIN32)
    include_directories("/usr/local/include")
    link_directories(c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH})
    set(LIBTCL tcl90)
    set(LIBJANSSON libjansson.lib)
    set(LIBZ zlibstatic.lib)
    # Windows specific for uSockets
    set(USOCKETS_LIBS ws2_32)
    set(WITH_LIBUV 1)
else()
    include_directories("/usr/local/include")
    find_library(LIBTCL tcl9.0)
    find_library(LIBJANSSON NAMES "libjansson.a")
    find_library(LIBZ z)
    # Linux doesn't need extra libs for uSockets
    set(USOCKETS_LIBS)
    set(WITH_LIBUV 0)
endif()

# Build uSockets using its own build system approach
# Collect the core source files
set(USOCKETS_SOURCES
    ${usockets_SOURCE_DIR}/src/bsd.c
    ${usockets_SOURCE_DIR}/src/context.c
    ${usockets_SOURCE_DIR}/src/loop.c
    ${usockets_SOURCE_DIR}/src/socket.c
)

# Add event loop implementation based on platform
if(WITH_LIBUV)
    list(APPEND USOCKETS_SOURCES ${usockets_SOURCE_DIR}/src/eventing/libuv.c)
else()
    list(APPEND USOCKETS_SOURCES ${usockets_SOURCE_DIR}/src/eventing/epoll_kqueue.c)
endif()

# Check for additional files that might exist in different versions
if(EXISTS ${usockets_SOURCE_DIR}/src/eventing/gcd.c)
    list(APPEND USOCKETS_SOURCES ${usockets_SOURCE_DIR}/src/eventing/gcd.c)
endif()

# Check for loop_data.c which contains the missing functions
if(EXISTS ${usockets_SOURCE_DIR}/src/loop_data.c)
    list(APPEND USOCKETS_SOURCES ${usockets_SOURCE_DIR}/src/loop_data.c)
endif()

# Check for internal directory
if(EXISTS ${usockets_SOURCE_DIR}/src/internal/internal.c)
    list(APPEND USOCKETS_SOURCES ${usockets_SOURCE_DIR}/src/internal/internal.c)
endif()

# Check for other potential source files
file(GLOB USOCKETS_INTERNAL_SOURCES ${usockets_SOURCE_DIR}/src/internal/*.c)
if(USOCKETS_INTERNAL_SOURCES)
    list(APPEND USOCKETS_SOURCES ${USOCKETS_INTERNAL_SOURCES})
endif()

# Print what we're building
message(STATUS "Building uSockets with sources: ${USOCKETS_SOURCES}")

# Create the library
add_library(usockets STATIC ${USOCKETS_SOURCES})

# Set uSockets compile definitions (no SSL)
target_compile_definitions(usockets PRIVATE LIBUS_NO_SSL)

# Platform-specific event loop selection
if(WIN32)
    # Windows uses libuv by default
    target_compile_definitions(usockets PRIVATE LIBUS_USE_LIBUV)
elseif(APPLE)
    # macOS uses libuv
    target_compile_definitions(usockets PRIVATE LIBUS_USE_LIBUV)
    target_include_directories(usockets PRIVATE ${LIBUV_INCLUDE_DIRS})
else()
    # Linux uses epoll
    target_compile_definitions(usockets PRIVATE LIBUS_USE_EPOLL)
endif()

# Add compile flags that uSockets expects
if(NOT WIN32)
    target_compile_options(usockets PRIVATE -std=c11 -flto)
endif()

# Include libuv headers if found
if(LIBUV_FOUND)
    target_include_directories(usockets PRIVATE ${LIBUV_INCLUDE_DIRS})
endif()

# Create directory for web assets
set(WEB_ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/www)
file(MAKE_DIRECTORY ${WEB_ASSETS_DIR})

# Function to embed files as C++ headers
function(embed_resource input_file output_file variable_name)
    add_custom_command(
        OUTPUT ${output_file}
        COMMAND ${CMAKE_COMMAND} -E echo "// Auto-generated file. Do not edit!" > ${output_file}
        COMMAND ${CMAKE_COMMAND} -E echo "#pragma once" >> ${output_file}
        COMMAND ${CMAKE_COMMAND} -E echo "" >> ${output_file}
        COMMAND ${CMAKE_COMMAND} -E echo "namespace embedded {" >> ${output_file}
        COMMAND ${CMAKE_COMMAND} -E echo "const char* ${variable_name} = R\"EMBEDDED(" >> ${output_file}
        COMMAND ${CMAKE_COMMAND} -E cat ${input_file} >> ${output_file}
        COMMAND ${CMAKE_COMMAND} -E echo ")EMBEDDED\";" >> ${output_file}
        COMMAND ${CMAKE_COMMAND} -E echo "} // namespace embedded" >> ${output_file}
        DEPENDS ${input_file}
        COMMENT "Embedding ${input_file} as ${variable_name}"
        VERBATIM
    )
endfunction()

embed_resource(
    ${WEB_ASSETS_DIR}/terminal.html
    ${CMAKE_CURRENT_BINARY_DIR}/embedded_terminal.h
    terminal_html
)

add_subdirectory(modules)
add_subdirectory(processors)
add_subdirectory(tools/essctrl)

configure_file(src/dservConfig.h.in ../src/dservConfig.h)

add_executable(dserv 
    src/dserv.cpp 
    src/Base64.c 
    src/Datapoint.c 
    src/Dataserver.cpp 
    src/SendClient.cpp 
    src/LogClient.cpp 
    src/LogTable.cpp 
    src/TclServer.cpp 
    src/dpoint_process.c 
    src/mdns.c 
    src/mdns_advertise.c
)

# export symbols for shared objects loaded at runtime
set_property(TARGET dserv PROPERTY ENABLE_EXPORTS ON)

target_sources(dserv PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/embedded_terminal.h)
target_include_directories(dserv PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Link libraries based on platform
if(APPLE)
    set_target_properties(dserv PROPERTIES LINK_FLAGS "-Wl,-export_dynamic")
    target_link_libraries(dserv PRIVATE 
        "-framework CoreFoundation" 
        ${LIBTCL} 
        ${LIBJANSSON} 
        ${LIBZ}
        usockets
        ${USOCKETS_LIBS}
    )
elseif(WIN32)
    target_link_libraries(dserv 
        ${LIBTCL} 
        ${LIBJANSSON} 
        ${LIBZ}
        usockets
        ${USOCKETS_LIBS}
    )
else()
    target_link_libraries(dserv 
        ${LIBTCL} 
        ${LIBJANSSON} 
        ${LIBZ} 
        pthread 
        rt 
        dl
        usockets
        ${USOCKETS_LIBS}
    )
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

    if(DEFINED CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM)
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
