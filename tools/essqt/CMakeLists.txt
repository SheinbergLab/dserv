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

# Determine platform-specific output path
if(WIN32)
    # Set up QScintilla paths for Windows
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/QScintilla/src)
    
    # Determine the correct library name and path based on build type
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(QSCI_LIB_NAME qscintilla2_qt6d)  # Debug library usually has 'd' suffix
        set(QSCI_LIB_PATH ${QSCI_SRC_DIR}/debug/${QSCI_LIB_NAME}.lib)
    else()
        set(QSCI_LIB_NAME qscintilla2_qt6)
        set(QSCI_LIB_PATH ${QSCI_SRC_DIR}/release/${QSCI_LIB_NAME}.lib)
    endif()
    
    # Check if the library exists
    if(NOT EXISTS ${QSCI_LIB_PATH})
        message(FATAL_ERROR "QScintilla library not found at: ${QSCI_LIB_PATH}")
    endif()
    
    # Add the library directory to link directories
    link_directories(${QSCI_SRC_DIR}/release ${QSCI_SRC_DIR}/debug)
    
else()
    # Non-Windows platforms
    add_subdirectory(third_party/QScintilla)
    set(QSCI_LIB_NAME qscintilla2_qt6)
endif()

# Tcl/Tk detection
if(WIN32)
    add_compile_options("/DFL_DLL")
    link_directories(c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH})
    set(LIBTCL tcl90.lib)
    set(LIBDG dg.lib)
    set(LIBTCLSTUB tclstub.lib)
    set(ZLIB zlibstatic.lib)
    set(LZ4LIB liblz4_static.lib)
    
    # Include directories for Tcl and your custom libraries
    include_directories(
        c:/usr/local/include 
        ../include 
        ../../../dlsh/src 
        ../../../dlsh/src/lablib 
        ${QSCI_SRC_DIR}
    )

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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components/host_discovery
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components/experiment_control
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components/script_editor
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components/dg_viewer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components/visualization
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/components/state_system
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
    src/ui/EssWorkspaceManager.cpp
    src/ui/components/terminal/EssTerminalWidget.cpp
    src/ui/components/terminal/CommandHistory.cpp
    src/ui/components/console/EssOutputConsole.cpp
    src/ui/components/dpoint_table/EssDatapointTableWidget.cpp
    src/ui/components/event_table/EssEventTableWidget.cpp    
    src/ui/components/host_discovery/EssHostDiscoveryWidget.cpp
    src/ui/components/experiment_control/EssExperimentControlWidget.cpp
    src/ui/components/script_editor/EssCodeEditor.cpp
    src/ui/components/script_editor/EssScriptEditorWidget.cpp
    src/ui/components/dg_viewer/EssDynGroupViewer.cpp
    src/ui/components/dg_viewer/EssStimDgWidget.cpp
    src/ui/components/visualization/EssVisualizationWidget.cpp
    src/ui/components/visualization/EssWindowIndicator.cpp
    src/ui/components/visualization/EssEyeTouchVisualizer.cpp
    src/ui/components/visualization/EssEyeTouchVisualizerWidget.cpp
    src/ui/components/state_system/EssStateSystemWidget.cpp
    src/ui/components/state_system/EssStateDebugData.cpp
)

set(OPTIONAL_SOURCES
    # src/tcl/TclInterp.cpp
    src/communication/mdns_query.c
    src/communication/mdns.c
    src/tcl/dlfuncs.c
    src/tcl/b64.c
    src/tcl/TclUtils.cpp
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
    # For Windows, we need to use the full path or just the library name if link_directories is set
    target_link_libraries(essqt PRIVATE
        ${LIBTCL} 
        ${ZLIB} 
        ${LIBTCLSTUB} 
        ${LIBDG} 
        ${LZ4LIB}
        ${QSCI_LIB_NAME}  # Use just the library name since we added link_directories
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
        ${QSCI_LIB_NAME}
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
        ${QSCI_LIB_NAME}
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

# Add this to your CMakeLists.txt after the install() command
if(WIN32)
    # Find windeployqt
    get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")

    # Add a custom command to run windeployqt after building
    add_custom_command(TARGET essqt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E env PATH="${_qt_bin_dir}" 
        ${WINDEPLOYQT_EXECUTABLE} "$<IF:$<CONFIG:Debug>,--debug,--release>" "$<TARGET_FILE:essqt>"
    )
    
    # Install runtime dependencies
    install(CODE "
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env PATH=\"${_qt_bin_dir}\" 
            \"${WINDEPLOYQT_EXECUTABLE}\" \"$<TARGET_FILE:essqt>\"
        )
    ")
endif()