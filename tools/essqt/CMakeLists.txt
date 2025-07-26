cmake_minimum_required(VERSION 3.16)
project(essqt LANGUAGES CXX C)  # Include C language for mDNS files
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# Enable Qt's automatic MOC, UIC, and RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Tcl/Tk detection (based on your FLTK version)
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
    set(LIBDLSH "dg")
    
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
    set(LIBDLSH "dg")
    
    # Include directories for your custom libraries
    include_directories(../include ../../../dlsh/src ../../../dlsh/src/lablib)
endif()

add_executable(essqt
    # Main application
    main.cpp
    MainWindow.cpp
    MainWindow.h
    
    # Terminal client
    TerminalClient.cpp
    TerminalClient.h
    
    # Code editor
    CodeEditor.cpp
    CodeEditor.h
    
    # Dserv networking
    DservListener.cpp
    DservListener.h
    DservClient.cpp
    DservClient.h
    EssClient.cpp
    EssClient.h
    ConnectionManager.cpp
    ConnectionManager.h
    DservEventParser.cpp
    DservEventParser.h
    
    # UI Components
    SystemConfigWidget.cpp
    SystemConfigWidget.h
    EssControlWidget.cpp
    EssControlWidget.h
    
    # Dock management system
    DockManager.cpp
    DockManager.h
    HostDiscoveryWidget.cpp
    HostDiscoveryWidget.h
    
    # Tcl Integration (copy from FLTK version)
    TclInterp.cpp
    TclInterp.h
    
    # Optional: Tcl Console Widget
    TclConsoleWidget.cpp
    TclConsoleWidget.h

    # Data Visualization
    DgTableWidget.cpp
    DgTableWidget.h
    DgFile.cpp
    DgFile.h

    dlfuncs.c
    dlfuncs.h
    
    # mDNS discovery (C files)
    mdns_query.c
    mdns.c
    mdns.h
    mdns_query_response.h
)

target_link_libraries(essqt
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

# Platform-specific networking libraries for mDNS
if(WIN32)
    target_link_libraries(essqt ws2_32 iphlpapi)
    # Add Tcl and other libraries
    target_link_libraries(essqt ${LIBTCL} ${ZLIB} ${LIBTCLSTUB} ${LIBDG} ${LZ4LIB})
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /NODEFAULTLIB:libcmt gdiplus.lib comctl32.lib ws2_32.lib gdiplus.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib")
    
elseif(APPLE)
    # macOS might need additional frameworks
    find_library(CORE_SERVICES CoreServices)
    if(CORE_SERVICES)
        target_link_libraries(essqt ${CORE_SERVICES})
    endif()
    # Add Tcl and other libraries
    target_link_libraries(essqt ${LIBDLSH} ${LIBZ} ${LIBLZ4} ${LIBTCL} ${LIBTCLSTUB} )
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -framework Cocoa -weak_framework UniformTypeIdentifiers -weak_framework ScreenCaptureKit")
    
else()
    # Linux
    target_link_libraries(essqt ${LIBDLSH} ${LIBZ} ${LIBLZ4} ${LIBTCL} ${LIBTCLSTUB} )
endif()

# Set up include directories
target_include_directories(essqt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Expose functions in executable to loaded libs (like FLTK version)
set_property(TARGET essqt PROPERTY ENABLE_EXPORTS ON)
