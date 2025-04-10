cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 20)

project(essctrl)

set(CMAKE_BUILD_TYPE Release)

add_executable(essctrl src/essctrl.c src/sockapi.c src/linenoise-ng/ConvertUTF.cpp src/linenoise-ng/linenoise.cpp src/linenoise-ng/wcwidth.cpp)

if(WIN32)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /INCREMENTAL /LTCG /NODEFAULTLIB:libcmt /NODEFAULTLIB:MSCVRT")
elseif(APPLE)
    set(CMAKE_XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--strict --timestamp --options=runtime")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_INJECT_BASE_ENTITLEMENTS "NO")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual")
else()
endif()

install(TARGETS essctrl DESTINATION bin COMPONENT essctrl)
