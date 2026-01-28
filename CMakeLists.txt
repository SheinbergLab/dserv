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

# Set shorter timeouts for FetchContent
set(FETCHCONTENT_QUIET OFF)  # Show progress
set(CMAKE_TIMEOUT 120)       # 2 minute timeout

FetchContent_Declare(
  uwebsockets
  GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
  GIT_TAG        v20.62.0
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
  TIMEOUT        120
)

FetchContent_Declare(
  usockets
  GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
  GIT_TAG        v0.8.8  
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""  # This prevents boringssl/lsquic from being cloned
  TIMEOUT        120
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

# Find OpenSSL for SSL/TLS support
find_package(OpenSSL REQUIRED)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    message(STATUS "  Include: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "  Libraries: ${OPENSSL_LIBRARIES}")
else()
    message(FATAL_ERROR "OpenSSL not found. Install with: apt-get install libssl-dev (Linux) or brew install openssl (macOS)")
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

# Add SSL-specific source files
if(EXISTS ${usockets_SOURCE_DIR}/src/crypto/openssl.c)
    list(APPEND USOCKETS_SOURCES ${usockets_SOURCE_DIR}/src/crypto/openssl.c)
    message(STATUS "Added OpenSSL crypto support")
endif()

if(EXISTS ${usockets_SOURCE_DIR}/src/crypto/sni_tree.cpp)
    list(APPEND USOCKETS_SOURCES ${usockets_SOURCE_DIR}/src/crypto/sni_tree.cpp)
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

# Enable SSL support in uSockets
target_compile_definitions(usockets PRIVATE LIBUS_USE_OPENSSL)
target_include_directories(usockets PRIVATE ${OPENSSL_INCLUDE_DIR})

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
target_compile_options(usockets PRIVATE 
    $<$<COMPILE_LANGUAGE:C>:-std=c11>
    -flto
)
endif()

# Include libuv headers if found
if(LIBUV_FOUND)
    target_include_directories(usockets PRIVATE ${LIBUV_INCLUDE_DIRS})
endif()

# Link OpenSSL to usockets
target_link_libraries(usockets PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# Web assets directory (for install, no longer embedded)
set(WEB_ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/www)
message(STATUS "Web assets directory: ${WEB_ASSETS_DIR}")
if(EXISTS ${WEB_ASSETS_DIR}/index.html)
    message(STATUS "  Found index.html - web interface ready")
else()
    message(STATUS "  index.html not found - web interface may not be configured")
endif()

add_subdirectory(modules)
add_subdirectory(processors)
add_subdirectory(tools/essctrl)

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_TAG_VERSION)
        set(dserv_VERSION ${GIT_TAG_VERSION})
    else()
        set(dserv_VERSION "0.0.0")
    endif()
else()
    set(dserv_VERSION "0.0.0")
endif()

# Configure the version header
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
    src/TclHttps.cpp    
    src/TclCompletion.cpp
    src/ErrorMonitor.cpp
    src/dpoint_process.c 
)

# export symbols for shared objects loaded at runtime
set_property(TARGET dserv PROPERTY ENABLE_EXPORTS ON)

# Include build directory for generated headers (dservConfig.h)
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
        OpenSSL::SSL
        OpenSSL::Crypto
    )
elseif(WIN32)
    target_link_libraries(dserv 
        ${LIBTCL} 
        ${LIBJANSSON} 
        ${LIBZ}
        usockets
        ${USOCKETS_LIBS}
        OpenSSL::SSL
        OpenSSL::Crypto
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
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()

enable_testing()
add_subdirectory("tests")

# ============================================================================
# dserv-agent (Go-based management agent)
# ============================================================================
find_program(GO_EXECUTABLE go)

if(GO_EXECUTABLE)
    message(STATUS "Found Go: ${GO_EXECUTABLE}")
    execute_process(
        COMMAND ${GO_EXECUTABLE} version
        OUTPUT_VARIABLE GO_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "  ${GO_VERSION_OUTPUT}")
    
    set(DSERV_AGENT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dserv-agent)
    # Put binary in bin/ subdirectory to avoid circular dependency warning
    set(DSERV_AGENT_BINARY ${CMAKE_CURRENT_BINARY_DIR}/bin/dserv-agent)
    
    if(EXISTS ${DSERV_AGENT_SOURCE_DIR}/main.go)
        message(STATUS "Found dserv-agent source at ${DSERV_AGENT_SOURCE_DIR}")
        
        # Determine GOARCH
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            set(GOARCH "arm64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
            set(GOARCH "amd64")
        else()
            set(GOARCH "amd64")
        endif()
        
        # Determine GOOS
        if(APPLE)
            set(GOOS "darwin")
        elseif(WIN32)
            set(GOOS "windows")
        else()
            set(GOOS "linux")
        endif()
        
        message(STATUS "  Building for GOOS=${GOOS} GOARCH=${GOARCH}")
        
        # Ensure output directory exists
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
        
        # Collect embedded files that should trigger rebuild
        file(GLOB DSERV_AGENT_WEB_FILES ${DSERV_AGENT_SOURCE_DIR}/web/*)
        set(DSERV_AGENT_COMPONENTS_JSON ${DSERV_AGENT_SOURCE_DIR}/components.json)
        
        add_custom_command(
            OUTPUT ${DSERV_AGENT_BINARY}
            COMMAND ${CMAKE_COMMAND} -E env 
                GOOS=${GOOS} 
                GOARCH=${GOARCH}
                CGO_ENABLED=0
                ${GO_EXECUTABLE} build -o ${DSERV_AGENT_BINARY} .
            WORKING_DIRECTORY ${DSERV_AGENT_SOURCE_DIR}
            DEPENDS 
                ${DSERV_AGENT_SOURCE_DIR}/main.go
                ${DSERV_AGENT_WEB_FILES}
                ${DSERV_AGENT_COMPONENTS_JSON}
            COMMENT "Building dserv-agent for ${GOOS}/${GOARCH}"
            VERBATIM
        )
        
        add_custom_target(dserv-agent ALL DEPENDS ${DSERV_AGENT_BINARY})
        
        # Install binary to /usr/local/bin
        install(PROGRAMS ${DSERV_AGENT_BINARY} 
            DESTINATION bin 
            COMPONENT dserv-agent
        )
        
        # Install systemd service file
        if(EXISTS ${DSERV_AGENT_SOURCE_DIR}/dserv-agent.service)
            install(FILES ${DSERV_AGENT_SOURCE_DIR}/dserv-agent.service
                DESTINATION dserv/systemd
                COMPONENT dserv-agent
            )
        endif()
        
        # Install components.json config
        if(EXISTS ${DSERV_AGENT_SOURCE_DIR}/components.json)
            install(FILES ${DSERV_AGENT_SOURCE_DIR}/components.json
                DESTINATION dserv/config
                COMPONENT dserv-agent
            )
        endif()
        
        set(DSERV_AGENT_FOUND TRUE)
    else()
        message(STATUS "dserv-agent source not found at ${DSERV_AGENT_SOURCE_DIR}")
        set(DSERV_AGENT_FOUND FALSE)
    endif()
else()
    message(STATUS "Go not found - dserv-agent will not be built")
    set(DSERV_AGENT_FOUND FALSE)
endif()


install(TARGETS dserv DESTINATION dserv COMPONENT dserv)
install(DIRECTORY config DESTINATION dserv COMPONENT dserv)
install(DIRECTORY lib DESTINATION dserv COMPONENT dserv)
install(DIRECTORY db DESTINATION dserv COMPONENT dserv)
install(DIRECTORY local DESTINATION dserv COMPONENT dserv)
install(DIRECTORY systems DESTINATION dserv COMPONENT dserv)
install(DIRECTORY systemd DESTINATION dserv COMPONENT dserv)
install(DIRECTORY scripts DESTINATION dserv COMPONENT dserv)
install(DIRECTORY www DESTINATION dserv COMPONENT dserv)
install(DIRECTORY ssl DESTINATION dserv COMPONENT dserv)
install(DIRECTORY soundfonts DESTINATION dserv COMPONENT dserv)

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
    if(DSERV_AGENT_FOUND)
       list(APPEND CPACK_COMPONENTS_ALL dserv-agent)
    endif()
    
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
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libevdev2, libpq5, zlib1g, yajl-tools, fluidsynth")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/dpkg/postinst)
    install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/dpkg/install-dserv-service.sh DESTINATION bin)
endif()

include(CPack)
cpack_add_component(dserv essctrl REQUIRED)
if(DSERV_AGENT_FOUND)
    cpack_add_component(dserv-agent)
endif()
