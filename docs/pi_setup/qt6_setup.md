#

## Linux packages
qt6-base-dev-tools qt6-tools-dev-tools qt6-wayland-dev-tools libqscintilla2-qt6-dev 

## CmakeLists.txt example
```
project(MyQScintillaApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Find QScintilla2 include directory
find_path(QSCINTILLA_INCLUDE_DIR
    NAMES Qsci/qsciscintilla.h
    PATHS /usr/include/aarch64-linux-gnu/qt6/
    PATH_SUFFIXES Qsci
)

# Find QScintilla2 library
find_library(QSCINTILLA_LIBRARY
    NAMES qscintilla2_qt6 # Adjust name if different (e.g., qscintilla2_qt6)
    PATHS /usr/lib/aarch64-linux-gnu
)

# Check if found and provide helpful messages
if(NOT QSCINTILLA_INCLUDE_DIR)
    message(FATAL_ERROR "QScintilla2 include directory not found!")
endif()
if(NOT QSCINTILLA_LIBRARY)
    message(FATAL_ERROR "QScintilla2 library not found!")
endif()

# Set up standard Qt project configuration (optional but recommended for modern Qt projects)
qt_standard_project_setup()


add_executable(MyQScintillaApp src/main.cpp)

target_link_libraries(MyQScintillaApp PRIVATE
    Qt6::Widgets
)
target_link_libraries(MyQScintillaApp PRIVATE ${QSCINTILLA_LIBRARY})
target_include_directories(MyQScintillaApp PRIVATE ${QSCINTILLA_INCLUDE_DIR})
```


