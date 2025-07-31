# third_party/QScintilla/CMakeLists.txt - Clean out-of-source build
cmake_minimum_required(VERSION 3.16)
project(qscintilla2_wrapper)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)

# Get Qt6 qmake
get_target_property(QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
message(STATUS "Using qmake: ${QMAKE_EXECUTABLE}")

# Determine source directory
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Qt4Qt5)
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Qt4Qt5)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set(QSCI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
else()
    message(FATAL_ERROR "Cannot find QScintilla source directory")
endif()

# Library name and path - in build directory
set(QSCI_LIB_NAME libqscintilla2_qt6.a)
set(QSCI_LIB_PATH ${CMAKE_CURRENT_BINARY_DIR}/${QSCI_LIB_NAME})

message(STATUS "QScintilla source: ${QSCI_SRC_DIR}")
message(STATUS "QScintilla build: ${CMAKE_CURRENT_BINARY_DIR}")
message(STATUS "QScintilla library: ${QSCI_LIB_PATH}")

# Build commands - shadow build in binary directory
add_custom_command(
    OUTPUT ${QSCI_LIB_PATH}
    COMMAND ${CMAKE_COMMAND} -E echo "Configuring QScintilla (shadow build)..."
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}
    COMMAND ${QMAKE_EXECUTABLE} 
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
    COMMAND ${CMAKE_COMMAND} -E echo "Building QScintilla..."
    COMMAND make
    COMMAND ${CMAKE_COMMAND} -E echo "QScintilla built successfully"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${QSCI_SRC_DIR}/qscintilla.pro
    COMMENT "Building QScintilla static library (out-of-source)"
    VERBATIM
)

# Create custom target that depends on the output
add_custom_target(QScintillaBuild ALL DEPENDS ${QSCI_LIB_PATH})

# Create imported library
add_library(qscintilla2_qt6 STATIC IMPORTED GLOBAL)
set_target_properties(qscintilla2_qt6 PROPERTIES
    IMPORTED_LOCATION ${QSCI_LIB_PATH}
)
add_dependencies(qscintilla2_qt6 QScintillaBuild)

# Set include directories (still from source)
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
