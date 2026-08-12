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

# Make all available (usockets and uwebsockets only now)
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
    src/TpoolMap.cpp
    src/TclHttps.cpp    
    src/TclSha256.cpp    
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

    # Load the libtcl9.0.so we SHIP, not the distro's.
    #
    # The deb installs our own libtcl9.0.so into /usr/local/lib (see
    # InstallRequiredSystemLibraries below), built from the core-9-0-N tag
    # deps/tcl is pinned to. But on Debian both copies land in the ld cache and
    # the multiarch one is listed first -- /etc/ld.so.conf.d/*.conf is globbed
    # alphabetically, so aarch64-linux-gnu.conf beats libc.conf, which is what
    # contributes /usr/local/lib. With no RUNPATH on the binary, apt's
    # libtcl9.0 won and the shipped one sat unused: measured on officepi and
    # hm-0 (trixie, libtcl9.0 9.0.1+dfsg-2) with dserv built against 9.0.4.
    #
    # DT_RUNPATH is searched before the cache, so this makes build-time and
    # run-time Tcl the same library again. Note it is deliberately narrow --
    # RUNPATH does not propagate to transitive deps, and modules dlopened later
    # reuse the libtcl9.0.so already loaded under that SONAME.
    set_target_properties(dserv PROPERTIES INSTALL_RPATH "/usr/local/lib")
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
        
        # Collect sources that should trigger a rebuild: all Go sources plus
        # the embedded web assets and components.json. CONFIGURE_DEPENDS makes
        # CMake re-glob when files are added/removed.
        file(GLOB DSERV_AGENT_GO_FILES CONFIGURE_DEPENDS ${DSERV_AGENT_SOURCE_DIR}/*.go)
        file(GLOB DSERV_AGENT_WEB_FILES CONFIGURE_DEPENDS ${DSERV_AGENT_SOURCE_DIR}/web/*)
        set(DSERV_AGENT_COMPONENTS_JSON ${DSERV_AGENT_SOURCE_DIR}/components.json)

        add_custom_command(
            OUTPUT ${DSERV_AGENT_BINARY}
            COMMAND ${CMAKE_COMMAND} -E env
                GOOS=${GOOS}
                GOARCH=${GOARCH}
                CGO_ENABLED=0
                ${GO_EXECUTABLE} build
                    -ldflags "-X main.version=${PROJECT_VERSION}"
                    -o ${DSERV_AGENT_BINARY} .
            WORKING_DIRECTORY ${DSERV_AGENT_SOURCE_DIR}
            DEPENDS
                ${DSERV_AGENT_GO_FILES}
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
        #
        # COMPONENT dserv, NOT dserv-agent, and deliberately so. This is the
        # list the panel offers, and the ONLY channel that reaches a box which
        # does not yet have the agent package is the dserv package it already
        # has. Shipping it with the agent would mean you needed the agent
        # package to learn that the agent package exists -- and worse, an
        # existing box upgrading dserv would LOSE this file (owned by the old
        # dserv, absent from the new one) and fall back to its stale built-in
        # list, so the panel could never offer the thing.
        if(EXISTS ${DSERV_AGENT_SOURCE_DIR}/components.json)
            install(FILES ${DSERV_AGENT_SOURCE_DIR}/components.json
                DESTINATION dserv/config
                COMPONENT dserv
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

# ============================================================================
# dservctl (Go-based CLI tool)
# ============================================================================
if(GO_EXECUTABLE)
    set(DSERVCTL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/dservctl)
    set(DSERVCTL_BINARY ${CMAKE_CURRENT_BINARY_DIR}/bin/dservctl)

    if(EXISTS ${DSERVCTL_SOURCE_DIR}/main.go)
        message(STATUS "Found dservctl source at ${DSERVCTL_SOURCE_DIR}")

        # Collect Go source files that should trigger rebuild
        file(GLOB DSERVCTL_GO_SOURCES ${DSERVCTL_SOURCE_DIR}/*.go)

        add_custom_command(
            OUTPUT ${DSERVCTL_BINARY}
            COMMAND ${CMAKE_COMMAND} -E env
                GOOS=${GOOS}
                GOARCH=${GOARCH}
                CGO_ENABLED=0
                ${GO_EXECUTABLE} build -o ${DSERVCTL_BINARY} .
            WORKING_DIRECTORY ${DSERVCTL_SOURCE_DIR}
            DEPENDS ${DSERVCTL_GO_SOURCES}
            COMMENT "Building dservctl for ${GOOS}/${GOARCH}"
            VERBATIM
        )

        add_custom_target(dservctl ALL DEPENDS ${DSERVCTL_BINARY})

        install(PROGRAMS ${DSERVCTL_BINARY}
            DESTINATION bin
            COMPONENT dservctl
        )

        # Create essctrl -> dservctl symlink for backward compatibility
        install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink dservctl \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/essctrl)"
            COMPONENT dservctl
        )

        # Unit tests run under ctest (the CI harness already runs `ctest`), so
        # the Go tool gets coverage with no workflow change.
        add_test(
            NAME dservctl-gotest
            COMMAND ${GO_EXECUTABLE} test ./...
            WORKING_DIRECTORY ${DSERVCTL_SOURCE_DIR}
        )

        set(DSERVCTL_FOUND TRUE)
    else()
        message(STATUS "dservctl source not found at ${DSERVCTL_SOURCE_DIR}")
        set(DSERVCTL_FOUND FALSE)
    endif()
else()
    set(DSERVCTL_FOUND FALSE)
endif()

# ============================================================================
# extio-setup (Go-based extio box manager: serial + dserv drivers, embedded UI)
# ============================================================================
if(GO_EXECUTABLE)
    set(EXTIO_SETUP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/extio-setup)
    set(EXTIO_SETUP_BINARY ${CMAKE_CURRENT_BINARY_DIR}/bin/extio-setup)

    if(EXISTS ${EXTIO_SETUP_SOURCE_DIR}/main.go)
        message(STATUS "Found extio-setup source at ${EXTIO_SETUP_SOURCE_DIR}")

        # Rebuild triggers: Go sources plus the go:embed'd web/ assets (like
        # dserv-agent, the UI is embedded, so a web change must relink).
        file(GLOB EXTIO_SETUP_GO_SOURCES CONFIGURE_DEPENDS ${EXTIO_SETUP_SOURCE_DIR}/*.go)
        file(GLOB EXTIO_SETUP_WEB_FILES CONFIGURE_DEPENDS ${EXTIO_SETUP_SOURCE_DIR}/web/*)

        # cgo policy differs from dservctl because the serial driver enumerates
        # USB boxes by identity (2e8a:100b) via go.bug.st/serial's enumerator:
        #   macOS -> IOKit, which is cgo. CGO_ENABLED=1, and it can only build
        #            natively (needs the target C toolchain), not cross like
        #            dservctl. Fine: release_macos builds on a native runner.
        #   Linux -> pure-Go sysfs (/sys/class/tty), no libudev. Keep
        #            CGO_ENABLED=0 so the binary stays static/cross-compilable
        #            and the .deb needs no extra runtime dependency.
        if(APPLE)
            set(EXTIO_SETUP_CGO 1)
        else()
            set(EXTIO_SETUP_CGO 0)
        endif()
        add_custom_command(
            OUTPUT ${EXTIO_SETUP_BINARY}
            COMMAND ${CMAKE_COMMAND} -E env
                GOOS=${GOOS}
                GOARCH=${GOARCH}
                CGO_ENABLED=${EXTIO_SETUP_CGO}
                ${GO_EXECUTABLE} build -o ${EXTIO_SETUP_BINARY} .
            WORKING_DIRECTORY ${EXTIO_SETUP_SOURCE_DIR}
            DEPENDS ${EXTIO_SETUP_GO_SOURCES} ${EXTIO_SETUP_WEB_FILES}
            COMMENT "Building extio-setup for ${GOOS}/${GOARCH} (CGO=${EXTIO_SETUP_CGO})"
            VERBATIM
        )

        add_custom_target(extio-setup ALL DEPENDS ${EXTIO_SETUP_BINARY})

        install(PROGRAMS ${EXTIO_SETUP_BINARY}
            DESTINATION bin
            COMPONENT extio-setup
        )

        # Go unit tests (frame/%get parsing, push pipeline, write guard) under ctest.
        add_test(
            NAME extio-setup-gotest
            COMMAND ${GO_EXECUTABLE} test ./...
            WORKING_DIRECTORY ${EXTIO_SETUP_SOURCE_DIR}
        )

        set(EXTIO_SETUP_FOUND TRUE)
    else()
        message(STATUS "extio-setup source not found at ${EXTIO_SETUP_SOURCE_DIR}")
        set(EXTIO_SETUP_FOUND FALSE)
    endif()
else()
    set(EXTIO_SETUP_FOUND FALSE)
endif()

# Linux: ship a udev rule for the extio box's USB identity (2e8a:100b) so the
# CDC is dialout-accessible without root and ModemManager leaves it alone.
# Staged into the payload; the .deb postinst copies it into /etc/udev/rules.d
# and reloads (the /usr/local package prefix is not a path udev scans). No-op
# on macOS/Windows.
if(EXTIO_SETUP_FOUND AND NOT APPLE AND NOT WIN32)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/dpkg/60-extio.rules
        DESTINATION dserv/udev
        COMPONENT extio-setup)
endif()

install(TARGETS dserv DESTINATION dserv COMPONENT dserv)
install(DIRECTORY config DESTINATION dserv COMPONENT dserv)
install(DIRECTORY lib DESTINATION dserv COMPONENT dserv)

# docs.db is built from db/docs.sql (the committed source of truth) so the
# binary doesn't churn in git. Requires sqlite3 on the build machine — which
# is already a transitive dep of the docs subprocess. The .sql is shipped
# alongside for recovery / re-init scenarios.
find_program(SQLITE3_EXECUTABLE sqlite3 REQUIRED)

# Building the full-text index needs sqlite3 compiled with the fts5 module.
# The sqlite3 first on PATH doesn't always have it (notably on some macOS CI
# runners), so verify and fall back to known-good locations. If none has
# fts5 the build still proceeds (see cmake/build_docs_db.cmake) — the docs
# subprocess rebuilds the index at runtime with dlsh's fts5-capable sqlite.
function(_sqlite3_has_fts5 exe outvar)
    execute_process(
        COMMAND "${exe}" :memory: "CREATE VIRTUAL TABLE t USING fts5(x);"
        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if(_rc EQUAL 0)
        set(${outvar} TRUE PARENT_SCOPE)
    else()
        set(${outvar} FALSE PARENT_SCOPE)
    endif()
endfunction()

_sqlite3_has_fts5("${SQLITE3_EXECUTABLE}" _have_fts5)
if(NOT _have_fts5)
    foreach(_cand /usr/bin/sqlite3
                  /opt/homebrew/opt/sqlite/bin/sqlite3
                  /usr/local/opt/sqlite/bin/sqlite3)
        if(EXISTS "${_cand}")
            _sqlite3_has_fts5("${_cand}" _cand_fts5)
            if(_cand_fts5)
                set(SQLITE3_EXECUTABLE "${_cand}")
                set(_have_fts5 TRUE)
                break()
            endif()
        endif()
    endforeach()
endif()
if(_have_fts5)
    message(STATUS "docs.db: using ${SQLITE3_EXECUTABLE} (fts5 available)")
else()
    message(WARNING "docs.db: no fts5-capable sqlite3 found; full-text index "
                    "will be rebuilt at runtime by the docs subprocess")
endif()

set(DOCS_SQL ${CMAKE_CURRENT_SOURCE_DIR}/db/docs.sql)
set(DOCS_DB  ${CMAKE_CURRENT_BINARY_DIR}/db/docs.db)
add_custom_command(
    OUTPUT ${DOCS_DB}
    # Driven through a -P helper (not a shell) so it works under every
    # generator and so a missing fts5 module degrades gracefully instead
    # of failing the build.
    COMMAND ${CMAKE_COMMAND}
            -DSQLITE3=${SQLITE3_EXECUTABLE}
            -DSQL=${DOCS_SQL}
            -DDB=${DOCS_DB}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_docs_db.cmake
    DEPENDS ${DOCS_SQL} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_docs_db.cmake
    COMMENT "Building docs.db from db/docs.sql"
    VERBATIM
)
add_custom_target(docs_db ALL DEPENDS ${DOCS_DB})
install(FILES ${DOCS_DB}                              DESTINATION dserv/db COMPONENT dserv)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/db/docs.sql DESTINATION dserv/db COMPONENT dserv)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/db/README   DESTINATION dserv/db COMPONENT dserv OPTIONAL)

install(DIRECTORY local DESTINATION dserv COMPONENT dserv)
install(DIRECTORY systems DESTINATION dserv COMPONENT dserv)
install(DIRECTORY systemd DESTINATION dserv COMPONENT dserv)
# USE_SOURCE_PERMISSIONS, and it is not cosmetic. install(DIRECTORY) defaults
# every file to 644, so `make install` strips the executable bit off every script
# in here -- boot_soak.sh, certify.sh, export-docs.sh, the PTP helpers, all of
# them. The .deb never showed it because dpkg/postinst chmod 0755's the two PTP
# scripts by hand, so the packaged path worked and the from-source path did not.
#
# Found 2026-08-09 after `sudo make install` on two rigs: dserv-ptp-setup
# answered "Permission denied" on both. The quieter half is worse --
# dserv-ptp-select-phc runs from the ptp4l units' ExecStartPre with a `-` prefix,
# so a non-executable copy is SKIPPED SILENTLY and ptp4l binds whatever clock the
# kernel offers first, which is the wrong-PHC failure that script exists to
# prevent. git carries the bit; honour it.
install(DIRECTORY scripts DESTINATION dserv COMPONENT dserv USE_SOURCE_PERMISSIONS)
install(DIRECTORY www DESTINATION dserv COMPONENT dserv)
install(DIRECTORY ssl DESTINATION dserv COMPONENT dserv)
install(DIRECTORY soundfonts DESTINATION dserv COMPONENT dserv)

set(CPACK_PACKAGE_NAME dserv)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Data server system for data acquisition and control")
set(CPACK_PACKAGE_CONTACT SheinbergLab)
set(CPACK_PACKAGE_VENDOR SheinbergLab)

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${LIBTCL})

# This module generates an install() rule of its own, and without a component
# it lands in "Unspecified" -- which, once CPACK_DEB_COMPONENT_INSTALL is on,
# is not packaged at all. That is exactly how 0.51.0 shipped a dserv deb with
# an empty /usr/local/lib: libtcl9.0.so silently vanished and the installed
# binary could not start ("error while loading shared libraries: libtcl9.0.so").
#
# Worth remembering the shape of the miss: the audit that preceded the split
# checked every install() call written in these files and found only one
# untagged. This rule is not written in these files -- the module emits it --
# so no amount of reading install( lines would have found it. Component
# packaging turns "untagged" from harmless into "silently absent from the
# package", which makes any implicitly-generated install rule a hazard.
# NOTE the name: CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT, with no LIBS. The
# variable that NAMES the libraries has LIBS in it, the one that assigns their
# component does not, and a wrong guess here fails exactly like no setting at
# all -- silently, in the package rather than at configure time.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT dserv)
include(InstallRequiredSystemLibraries)

if(WIN32)
    # TODO
elseif(APPLE)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_COMPONENTS_ALL dserv)
    if(DSERV_AGENT_FOUND)
       list(APPEND CPACK_COMPONENTS_ALL dserv-agent)
    endif()
    if(DSERVCTL_FOUND)
       list(APPEND CPACK_COMPONENTS_ALL dservctl)
    endif()
    if(EXTIO_SETUP_FOUND)
       list(APPEND CPACK_COMPONENTS_ALL extio-setup)
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

    # Split dserv-agent into its own .deb.
    #
    # The agent exists to manage a box when dserv is down or absent, but it
    # shipped INSIDE dserv's package -- so a display box (stim2 + dlsh, no
    # dserv) had no way to get one, and no way to update the one it had. That
    # vacuum is what the hand-rolled stim2-agent.service grew into.
    #
    # ONE_PER_GROUP, not one-per-component: only the agent splits out. dservctl
    # and extio-setup stay inside dserv, because moving files out of a package
    # makes dpkg delete them on upgrade -- a box that took the new dserv but
    # declined the merely-Recommended agent would silently lose dservctl, and
    # with it essctrl, which is a symlink to dservctl in the same component.
    #
    # The agent does call dservctl (getDservStatus shells out to `essctrl -c`),
    # which argues for moving it -- but that call only ever answers when there
    # is a running dserv to answer it, and a box with a dserv already has
    # dservctl. On a display box it would be a client with nothing to talk to.
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_GROUPING ONE_PER_GROUP)
    set(CPACK_COMPONENTS_ALL dserv)
    set(CPACK_COMPONENT_DSERV_GROUP "dserv")
    if(DSERV_AGENT_FOUND)
        list(APPEND CPACK_COMPONENTS_ALL dserv-agent)
        set(CPACK_COMPONENT_DSERV-AGENT_GROUP "dserv-agent")
    endif()
    if(DSERVCTL_FOUND)
        list(APPEND CPACK_COMPONENTS_ALL dservctl)
        set(CPACK_COMPONENT_DSERVCTL_GROUP "dserv")
    endif()
    if(EXTIO_SETUP_FOUND)
        list(APPEND CPACK_COMPONENTS_ALL extio-setup)
        set(CPACK_COMPONENT_EXTIO-SETUP_GROUP "dserv")
    endif()

    # Per-group control fields. Package name must be set explicitly or CPack
    # derives it from the group and emits dserv-dserv-agent.
    set(CPACK_DEBIAN_DSERV_PACKAGE_NAME "dserv")
    set(CPACK_DEBIAN_DSERV_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/dpkg/preinst;${CMAKE_CURRENT_SOURCE_DIR}/dpkg/postinst")

    # Recommends, NOT Depends -- decided against a hard dependency once the
    # deployed fleet was accounted for.
    #
    # The hazard being defended against is real: dserv used to own
    # /usr/local/bin/dserv-agent, and dpkg deletes files the old version owned
    # that the new one does not ship, so an upgrade would remove the agent out
    # from under a box, by an update the agent itself launched. But a hard
    # Depends fixes that by making apt REFUSE the upgrade on every box that
    # does not yet have the agent as a package -- which is every box deployed
    # today. On an in-cage box in another building, a failed update with a
    # dependency message is not better than a silent one; it is the same
    # stranded box plus a support call.
    #
    # dpkg/preinst + dpkg/postinst handle it directly instead: stash the binary
    # before dpkg can remove it, put it back if nothing else provides it. The
    # upgrade cannot fail on a dependency it does not declare, and the agent
    # cannot vanish. Recommends keeps the relationship documented and is never
    # fatal -- dpkg ignores it, and apt treats it as best-effort.
    #
    # Revisit once the fleet actually carries the dserv-agent package: a hard
    # Depends is the better long-term expression, and is safe to add the moment
    # it can be satisfied everywhere.
    set(CPACK_DEBIAN_DSERV_PACKAGE_DEPENDS "libevdev2, libpq5, zlib1g, yajl-tools, fluidsynth")
    # chrony is RECOMMENDS and deliberately not DEPENDS. Every dserv host needs a
    # correct wall clock -- not for acquisition (timestamps ride CLOCK_MONOTONIC)
    # but because clock_epoch_offset_us() is captured ONCE at startup, so a host
    # that boots with a wrong clock poisons every timestamp for the life of the
    # process and nothing self-heals it.
    #
    # WHICH daemon, not whether. Measured on the i.MX95 grandmaster 2026-08-08,
    # 120 samples of its own phc2sys: chrony sd 92 ns, systemd-timesyncd sd
    # 53,042 ns, no NTP at all sd 77 ns -- timesyncd is ~690x worse than having
    # NO time source, because it is an SNTP client with no filtering and phc2sys
    # pushes its frequency thrash straight into the PHC and out to every box.
    #
    # Not DEPENDS because installing chrony REMOVES systemd-timesyncd, and
    # rearranging a host's time configuration is exactly the class of decision
    # dpkg/postinst refuses to make for PTP roles. Recommends is installed by
    # default and stays overridable with --no-install-recommends.
    set(CPACK_DEBIAN_DSERV_PACKAGE_RECOMMENDS "dserv-agent, chrony")

    set(CPACK_DEBIAN_DSERV-AGENT_PACKAGE_NAME "dserv-agent")
    # The agent's three files used to belong to dserv. Without Replaces, dpkg
    # refuses to unpack ("trying to overwrite ..., which is also in package
    # dserv") on every box that already has one. "<< PROJECT_VERSION" is right
    # because the split ships in a release strictly newer than any monolithic
    # dserv, so every version that owned those files is covered.
    set(CPACK_DEBIAN_DSERV-AGENT_PACKAGE_REPLACES "dserv (<< ${PROJECT_VERSION})")

    # NO Breaks, though the file-move idiom normally pairs it with Replaces.
    # Breaks would refuse to configure dserv-agent while an older dserv is
    # installed -- which is every deployed box, and precisely the state the
    # bootstrap is in when step_install_agent runs FIRST, before the components.
    # It would block the very path used to migrate a box. Replaces alone lets
    # the agent package take the file and leaves the old dserv working, which
    # it does: nothing in dserv executes that binary, the agent is a separate
    # service. The cost is that reinstalling an OLD dserv over a box that has
    # the agent package would conflict -- a downgrade, loud, and not a routine
    # update.


    # COMPONENT is not optional once component install is on: an untagged
    # install() lands in "Unspecified" and becomes a stray package.
    install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/dpkg/install-dserv-service.sh
        DESTINATION bin COMPONENT dserv)
endif()

include(CPack)
cpack_add_component(dserv REQUIRED)
if(DSERV_AGENT_FOUND)
    cpack_add_component(dserv-agent)
endif()
if(DSERVCTL_FOUND)
    cpack_add_component(dservctl)
endif()
if(EXTIO_SETUP_FOUND)
    cpack_add_component(extio-setup)
endif()
