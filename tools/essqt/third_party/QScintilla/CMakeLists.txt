# third_party/QScintilla/CMakeLists.txt - Simple working version
cmake_minimum_required(VERSION 3.16)
project(qscintilla2_wrapper)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)

# Get Qt6 qmake
get_target_property(QMAKE_EXECUTABLE_DEFAULT Qt6::qmake IMPORTED_LOCATION)

# On Linux, find qmake explicitly if needed
if(UNIX AND NOT APPLE)
    find_program(QMAKE_EXECUTABLE 
        NAMES qmake6 qmake-qt6 qmake
        PATHS 
            /usr/lib/qt6/bin
            /usr/lib/x86_64-linux-gnu/qt6/bin
            /usr/lib/aarch64-linux-gnu/qt6/bin
        NO_DEFAULT_PATH
    )
    if(NOT QMAKE_EXECUTABLE)
        set(QMAKE_EXECUTABLE ${QMAKE_EXECUTABLE_DEFAULT})
    endif()
else()
    set(QMAKE_EXECUTABLE ${QMAKE_EXECUTABLE_DEFAULT})
endif()

message(STATUS "Using qmake: ${QMAKE_EXECUTABLE}")

# Determine source directory
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Qt4Qt5)
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Qt4Qt5)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
else()
    message(FATAL_ERROR "Cannot find QScintilla source directory")
endif()

# Library name and path - in source directory
set(QSCI_LIB_NAME libqscintilla2_qt6.a)
set(QSCI_LIB_PATH ${QSCI_SRC_DIR}/${QSCI_LIB_NAME})

# Simple build command - exactly what worked before
add_custom_command(
    OUTPUT ${QSCI_LIB_PATH}
    COMMAND ${CMAKE_COMMAND} -E echo "Configuring QScintilla..."
    COMMAND ${QMAKE_EXECUTABLE} CONFIG+=staticlib CONFIG+=c++17 CONFIG+=release TARGET=qscintilla2_qt6
    COMMAND ${CMAKE_COMMAND} -E echo "Building QScintilla..."
    COMMAND make
    WORKING_DIRECTORY ${QSCI_SRC_DIR}
    DEPENDS ${QSCI_SRC_DIR}/qscintilla.pro
    COMMENT "Building QScintilla static library"
    VERBATIM
)

# Create custom target
add_custom_target(QScintillaBuild ALL DEPENDS ${QSCI_LIB_PATH})

# Create imported library
add_library(qscintilla2_qt6 STATIC IMPORTED GLOBAL)
set_target_properties(qscintilla2_qt6 PROPERTIES
    IMPORTED_LOCATION ${QSCI_LIB_PATH}
)
add_dependencies(qscintilla2_qt6 QScintillaBuild)

# Set include directories
target_include_directories(qscintilla2_qt6 INTERFACE
    ${QSCI_SRC_DIR}
    ${QSCI_SRC_DIR}/Qsci
)

# Link Qt libraries
target_link_libraries(qscintilla2_qt6 INTERFACE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
)
