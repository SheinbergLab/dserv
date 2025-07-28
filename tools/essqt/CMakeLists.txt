cmake_minimum_required(VERSION 3.16)
project(essqt VERSION 0.1.0 LANGUAGES CXX C)  # Include C for dlfuncs.c and mdns if needed

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

# Enable Qt MOC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Tcl/Tk detection (from your working version)
if(WIN32)
    add_compile_options("/DFL_DLL")
    link_directories(c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH})
    set(LIBTCL tcl90.lib)
    set(LIBDG dg.lib)
    set(LIBTCLSTUB tclstub.lib)
    set(ZLIB zlibstatic.lib)
    set(LZ4LIB liblz4_static.lib)
    
    # Include directories for Tcl and your custom libraries
    include_directories(c:/usr/local/include ../include ../../../dlsh/src ../../../dlsh/src/lablib)
    
elseif(APPLE)
    # Tcl may be installed from source or by homebrew
    include_directories("/usr/local/include" "/opt/homebrew/include/tcl-tk")
    link_directories("/usr/local/lib" "/opt/homebrew/lib")
    
    find_library(LIBZ z)
    find_library(LIBLZ4 NAMES "liblz4.a")
    find_library(LIBTCL "tcl9.0")
    set(LIBTCLSTUB "libtclstub.a")
    set(LIBDG "dg")
    
    # Locate TCL library files, as installed by homebrew
    get_filename_component(LIBTCL_ACTUAL ${LIBTCL} REALPATH)
    get_filename_component(LIBTCL_DIR ${LIBTCL_ACTUAL} DIRECTORY)
    
    # Include directories for your custom libraries
    include_directories(../include ../../../dlsh/src ../../../dlsh/src/lablib)
    
else()
    # Linux
    link_directories(/usr/local/lib)
    find_library(LIBZ z)
    find_library(LIBLZ4 NAMES "liblz4.a")
    find_library(LIBTCL "tcl9.0")
    set(LIBTCLSTUB "/usr/local/lib/libtclstub.a")
    set(LIBDG "dg")
    
    # Include directories for your custom libraries
    include_directories(../include ../../../dlsh/src ../../../dlsh/src/lablib)
endif()

# Set up include directories for the essqt structure
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components/terminal    
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components/console 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/communication
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl
)

# Collect source files
set(CORE_SOURCES
    src/core/EssApplication.cpp
    src/core/EssConfig.cpp
    src/core/EssCommandInterface.cpp
    src/core/EssDataProcessor.cpp
    src/core/EssEventProcessor.cpp    
)

set(COMM_SOURCES
    src/communication/DservClient.cpp
    src/communication/DservListener.cpp
    src/communication/EssClient.cpp
    src/communication/DservEventParser.cpp
)

set(UI_SOURCES
    src/ui/EssMainWindow.cpp
    src/ui/components/terminal/EssTerminalWidget.cpp
    src/ui/components/terminal/CommandHistory.cpp
    src/ui/components/console/EssOutputConsole.cpp
    src/ui/components/dpoint_table/EssDatapointTableWidget.cpp
    src/ui/components/event_table/EssEventTableWidget.cpp    
)

# Optional: Add these if you have them
set(OPTIONAL_SOURCES
    # src/dlfuncs.c
    # src/tcl/TclInterp.cpp
    # src/communication/mdns_query.c
    # src/communication/mdns.c
)

# Main executable
qt_add_executable(essqt
    src/main.cpp
    ${CORE_SOURCES}
    ${COMM_SOURCES}
    ${UI_SOURCES}
    ${OPTIONAL_SOURCES}
)

# Link libraries based on platform
target_link_libraries(essqt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

# Platform-specific linking
if(WIN32)
    target_link_libraries(essqt PRIVATE
        ${LIBTCL} 
        ${ZLIB} 
        ${LIBTCLSTUB} 
        ${LIBDG} 
        ${LZ4LIB}
        ws2_32 
        iphlpapi
        gdiplus
        comctl32
        kernel32
        user32
        gdi32
        winspool
        shell32
        ole32
        oleaut32
        uuid
        comdlg32
        advapi32
    )
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /NODEFAULTLIB:libcmt")
    
elseif(APPLE)
    target_link_libraries(essqt PRIVATE
        ${LIBDG} 
        ${LIBZ} 
        ${LIBLZ4} 
        ${LIBTCL} 
        ${LIBTCLSTUB}
    )
    
    # macOS frameworks
    find_library(CORE_SERVICES CoreServices)
    if(CORE_SERVICES)
        target_link_libraries(essqt PRIVATE ${CORE_SERVICES})
    endif()
    
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -framework Cocoa -weak_framework UniformTypeIdentifiers -weak_framework ScreenCaptureKit")
    
else()
    # Linux
    target_link_libraries(essqt PRIVATE
        ${LIBDG} 
        ${LIBZ} 
        ${LIBLZ4} 
        ${LIBTCL} 
        ${LIBTCLSTUB}
    )
endif()

# Platform-specific executable properties
if(WIN32)
    set_target_properties(essqt PROPERTIES
        WIN32_EXECUTABLE TRUE
        ENABLE_EXPORTS ON  # Expose functions to loaded libs
    )
elseif(APPLE)
    set_target_properties(essqt PROPERTIES
        MACOSX_BUNDLE TRUE
        ENABLE_EXPORTS ON  # Expose functions to loaded libs
    )
else()
    set_target_properties(essqt PROPERTIES
        ENABLE_EXPORTS ON  # Expose functions to loaded libs
    )
endif()

# Installation
install(TARGETS essqt
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
