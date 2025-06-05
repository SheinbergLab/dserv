cmake_minimum_required(VERSION 3.12)
project(DPIClient)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add executable
add_executable(dpi_client dpiclient.cpp)

include(FetchContent)

FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json)

target_include_directories(dpi_client PRIVATE ${nlohmann_json_SOURCE_DIR}/include/nlohmann)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(dpi_client PRIVATE ws2_32 nlohmann_json::nlohmann_json)
else ()
    target_link_libraries(dpi_client PRIVATE nlohmann_json::nlohmann_json)
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
