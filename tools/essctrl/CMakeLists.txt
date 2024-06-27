cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)

project(essctrl)

set(CMAKE_BUILD_TYPE Release)

add_executable(essctrl src/essctrl.c src/sockapi.c src/linenoise.c)

install(TARGETS essctrl DESTINATION bin)

