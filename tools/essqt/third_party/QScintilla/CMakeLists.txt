cmake_minimum_required(VERSION 3.16)
project(qscintilla2_wrapper)

# This is a wrapper that builds QScintilla using its native qmake build system

# Find Qt6 to get qmake
find_package(Qt6 REQUIRED COMPONENTS Core)
get_target_property(QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)

# Determine source directory
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Qt4Qt5)
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Qt4Qt5)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
else()
    message(FATAL_ERROR "Cannot find QScintilla source directory")
endif()

# Platform-specific library name
if(WIN32)
    set(QSCI_LIB_NAME qscintilla2_qt6.lib)
    set(QSCI_LIB_PATH ${QSCI_SRC_DIR}/release/${QSCI_LIB_NAME})
else()
    set(QSCI_LIB_NAME libqscintilla2_qt6.a)
    set(QSCI_LIB_PATH ${QSCI_SRC_DIR}/${QSCI_LIB_NAME})
endif()

# Custom command to build QScintilla using qmake
add_custom_command(
    OUTPUT ${QSCI_LIB_PATH}
    COMMAND ${QMAKE_EXECUTABLE} CONFIG+=staticlib CONFIG+=c++17
    COMMAND ${CMAKE_MAKE_PROGRAM}
    WORKING_DIRECTORY ${QSCI_SRC_DIR}
    COMMENT "Building QScintilla using qmake"
    VERBATIM
)

# Create a custom target
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
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)
target_link_libraries(qscintilla2_qt6 INTERFACE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
)
