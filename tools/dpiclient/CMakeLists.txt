cmake_minimum_required(VERSION 3.12)
project(DPIClient)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add executable
add_executable(dpi_client dpiclient.cpp)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(dpi_client ws2_32)
endif()

# Compiler-specific flags
if(MSVC)
    target_compile_options(dpi_client PRIVATE /W3)
else()
    target_compile_options(dpi_client PRIVATE -Wall -Wextra -pedantic)
endif()

# Set output directory
set_target_properties(dpi_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
