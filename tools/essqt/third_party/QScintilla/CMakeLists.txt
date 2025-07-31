# third_party/QScintilla/CMakeLists.txt - Fixed for Linux Qt6 paths
cmake_minimum_required(VERSION 3.16)
project(qscintilla2_wrapper)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)

# Get Qt6 tools
get_target_property(QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_target_property(MOC_EXECUTABLE Qt6::moc IMPORTED_LOCATION)
get_target_property(RCC_EXECUTABLE Qt6::rcc IMPORTED_LOCATION)
get_target_property(UIC_EXECUTABLE Qt6::uic IMPORTED_LOCATION)

# Get Qt6 paths
get_target_property(QT6_INSTALL_PREFIX Qt6::Core INTERFACE_INSTALL_PREFIX)
set(QT6_INSTALL_BINS ${QT6_INSTALL_PREFIX}/bin)

message(STATUS "Using qmake: ${QMAKE_EXECUTABLE}")
message(STATUS "Using moc: ${MOC_EXECUTABLE}")
message(STATUS "Qt6 bin dir: ${QT6_INSTALL_BINS}")

# On Linux, especially Debian/Ubuntu, we might need to find qmake explicitly
if(UNIX AND NOT APPLE AND NOT QMAKE_EXECUTABLE)
    find_program(QMAKE_EXECUTABLE 
        NAMES qmake6 qmake-qt6 qmake
        PATHS 
            /usr/lib/qt6/bin
            /usr/lib/x86_64-linux-gnu/qt6/bin
            /usr/lib/aarch64-linux-gnu/qt6/bin
        NO_DEFAULT_PATH
    )
endif()

# Determine source directory
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Qt4Qt5)
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Qt4Qt5)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
else()
    message(FATAL_ERROR "Cannot find QScintilla source directory")
endif()

# Library name and path
set(QSCI_LIB_NAME libqscintilla2_qt6.a)
set(QSCI_LIB_PATH ${CMAKE_CURRENT_BINARY_DIR}/${QSCI_LIB_NAME})

# Platform-specific make command
if(WIN32)
    set(MAKE_CMD nmake)
else()
    set(MAKE_CMD make)
endif()

# Build commands - with explicit Qt paths for Linux
add_custom_command(
    OUTPUT ${QSCI_LIB_PATH}
    COMMAND ${CMAKE_COMMAND} -E echo "Configuring QScintilla (shadow build)..."
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E env 
            "PATH=${QT6_INSTALL_BINS}:$ENV{PATH}"
            "QTDIR=${QT6_INSTALL_PREFIX}"
            "QT_SELECT=qt6"
            ${QMAKE_EXECUTABLE} 
            ${QSCI_SRC_DIR}/qscintilla.pro 
            CONFIG+=staticlib 
            CONFIG+=c++17 
            CONFIG+=release 
            CONFIG-=debug
            TARGET=qscintilla2_qt6
            DESTDIR=${CMAKE_CURRENT_BINARY_DIR}
            OBJECTS_DIR=${CMAKE_CURRENT_BINARY_DIR}/.obj
            MOC_DIR=${CMAKE_CURRENT_BINARY_DIR}/.moc
            RCC_DIR=${CMAKE_CURRENT_BINARY_DIR}/.rcc
            "QMAKE_MOC=${MOC_EXECUTABLE}"
            "QMAKE_RCC=${RCC_EXECUTABLE}"
            "QMAKE_UIC=${UIC_EXECUTABLE}"
    COMMAND ${CMAKE_COMMAND} -E echo "Building QScintilla..."
    COMMAND ${CMAKE_COMMAND} -E env 
            "PATH=${QT6_INSTALL_BINS}:$ENV{PATH}"
            ${MAKE_CMD}
    COMMAND ${CMAKE_COMMAND} -E echo "QScintilla built successfully"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${QSCI_SRC_DIR}/qscintilla.pro
    COMMENT "Building QScintilla static library (out-of-source)"
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
