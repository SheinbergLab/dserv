cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)

project(essctrl)

set(CMAKE_BUILD_TYPE Release)
if(WIN32)
#include_directories(c:/usr/local/include)
#link_directories(c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH})
else()
#include_directories(/usr/local/include)
#link_directories(/usr/local/lib)
endif()

add_executable(essctrl src/essctrl.c src/sockapi.c src/linenoise-ng/ConvertUTF.cpp src/linenoise-ng/linenoise.cpp src/linenoise-ng/wcwidth.cpp)

if(WIN32)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /INCREMENTAL /LTCG /NODEFAULTLIB:libcmt /NODEFAULTLIB:MSCVRT")
elseif(APPLE)
else()
endif()

install(TARGETS essctrl DESTINATION bin)

