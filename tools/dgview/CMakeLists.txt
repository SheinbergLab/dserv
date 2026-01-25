cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_BUILD_TYPE Release)

if(NOT DEFINED PROJECT_VERSION)
  set(PROJECT_VERSION 0.1.0)
endif()
project(dgview VERSION ${PROJECT_VERSION} DESCRIPTION "DG/DGZ File Viewer")

# Find FLTK
find_package(FLTK REQUIRED NO_MODULE)
message(STATUS "Using FLTK: ${FLTK_INCLUDE_DIRS}")

# Platform-specific setup
if(WIN32)
    add_compile_definitions(WIN32)
    
    # Find lz4
    find_package(lz4 CONFIG QUIET)
    if(lz4_FOUND)
        set(LZ4_LIBRARIES lz4::lz4)
    else()
        find_library(LZ4_LIBRARIES NAMES lz4_static liblz4_static lz4 liblz4 PATHS ${CMAKE_PREFIX_PATH}/lib)
    endif()
    
    # libdg - check for path hint or standard locations
    if(DEFINED LIBDG_PATH)
        link_directories(${LIBDG_PATH})
        find_library(LIBDG NAMES dg dg.lib PATHS ${LIBDG_PATH} NO_DEFAULT_PATH)
    else()
        link_directories(c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH})
        find_library(LIBDG NAMES dg dg.lib)
    endif()
    
    if(NOT LIBDG)
        set(LIBDG "dg")  # Fallback
    endif()
    
    # Find zlib static library (for gzip functions gzopen, gzread, etc.)
    find_library(ZLIB_STATIC NAMES zs zlibstatic zlibstaticd PATHS ${CMAKE_PREFIX_PATH}/lib)
    if(NOT ZLIB_STATIC)
        message(FATAL_ERROR "Could not find static zlib library (zs.lib or zlibstatic.lib)")
    endif()
    
    set(WIN_LIBS fltk::fltk ${ZLIB_STATIC} ${LZ4_LIBRARIES} ${LIBDG})
elseif(APPLE)
    include_directories("/usr/local/include" "/opt/homebrew/include")
    link_directories("/usr/local/lib" "/opt/homebrew/lib")
    find_library(LIBZ z)
    find_library(LIBLZ4 NAMES "liblz4.a" lz4)
    find_library(LIBFLTK NAMES "libfltk.a")
    find_library(LIBFLTK_ZLIB NAMES "libfltk_z.a")
    set(LIBDG "dg")
    
    # For app bundle
    set(MACOSX_BUNDLE_ICON_FILE dgview.icns)
else()
    # Linux
    link_directories(/usr/local/lib)
    find_library(LIBZ z)
    find_library(LIBLZ4 NAMES "liblz4.a" lz4)
    find_library(LIBFLTK NAMES "libfltk.a" fltk)
    find_library(LIBFLTK_ZLIB NAMES "libfltk_z.a" fltk_z)  # May not exist on all systems
    set(LIBDG "dg")
    
    # Check for XXHash (needed on newer Debian/Ubuntu)
    find_library(LIBXXHASH NAMES "libxxhash.a" "xxhash")
    if(LIBXXHASH)
        message(STATUS "Found XXHash library: ${LIBXXHASH}")
    endif()
endif()

# Include directories for dlsh/libdg headers
include_directories(
    ${FLTK_INCLUDE_DIRS}
    src
    ${CMAKE_SOURCE_DIR}/../../dlsh/src
    ${CMAKE_SOURCE_DIR}/../../dlsh/src/lablib
)

if(WIN32)
    include_directories(c:/usr/local/include)
    if(DEFINED LIBDG_PATH)
        include_directories(${LIBDG_PATH}/../src ${LIBDG_PATH}/../src/lablib)
    endif()
else()
    include_directories(/usr/local/include)
endif()

# Main executable
if(APPLE)
    add_executable(dgview
        src/dgview.cxx
        src/DgTable.cxx
        src/DgExport.cxx
        src/MacDropHandler.mm
    )
elseif(WIN32)
    # Include resource file for Windows icon
    set(WIN_RESOURCES "")
    if(EXISTS ${CMAKE_SOURCE_DIR}/windows/dgview.rc)
        set(WIN_RESOURCES ${CMAKE_SOURCE_DIR}/windows/dgview.rc)
    endif()
    add_executable(dgview WIN32
        src/dgview.cxx
        src/DgTable.cxx
        src/DgExport.cxx
        ${WIN_RESOURCES}
    )
else()
    # Linux
    add_executable(dgview
        src/dgview.cxx
        src/DgTable.cxx
        src/DgExport.cxx
    )
endif()

# Link libraries
if(WIN32)
    target_link_libraries(dgview ${WIN_LIBS} 
        comctl32 ws2_32 kernel32 user32 gdi32 shell32 ole32 comdlg32 advapi32)
elseif(APPLE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -framework Cocoa -weak_framework UniformTypeIdentifiers -weak_framework ScreenCaptureKit")
    target_link_libraries(dgview ${LIBFLTK} ${LIBFLTK_ZLIB} ${LIBDG} ${LIBZ} ${LIBLZ4})
    
    # App bundle configuration
    set_target_properties(dgview PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "DG Viewer"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_GUI_IDENTIFIER org.sheinberglab.dgview
        MACOSX_BUNDLE_ICON_FILE dgview
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/macos/Info.plist
    )
    
    # Copy icon if it exists
    if(EXISTS ${CMAKE_SOURCE_DIR}/macos/dgview.icns)
        target_sources(dgview PRIVATE ${CMAKE_SOURCE_DIR}/macos/dgview.icns)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/macos/dgview.icns 
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    endif()
else()
    # Linux
    set(LINUX_LIBS ${LIBFLTK} ${LIBDG} ${LIBZ} ${LIBLZ4})
    if(LIBFLTK_ZLIB)
        list(APPEND LINUX_LIBS ${LIBFLTK_ZLIB})
    endif()
    if(LIBXXHASH)
        list(APPEND LINUX_LIBS ${LIBXXHASH})
    endif()
    
    # Core X11 libraries
    list(APPEND LINUX_LIBS X11 Xext Xinerama Xfixes Xcursor Xft Xrender fontconfig pthread dl)
    
    # Wayland/GTK libraries (needed if FLTK was built with Wayland support)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(WAYLAND wayland-client wayland-cursor)
        pkg_check_modules(GTK3 gtk+-3.0)
        pkg_check_modules(CAIRO cairo)
        pkg_check_modules(DBUS dbus-1)
        if(WAYLAND_FOUND)
            list(APPEND LINUX_LIBS ${WAYLAND_LIBRARIES})
        endif()
        if(GTK3_FOUND)
            list(APPEND LINUX_LIBS ${GTK3_LIBRARIES})
        endif()
        if(CAIRO_FOUND)
            list(APPEND LINUX_LIBS ${CAIRO_LIBRARIES})
        endif()
        if(DBUS_FOUND)
            list(APPEND LINUX_LIBS ${DBUS_LIBRARIES})
        endif()
    endif()
    
    # Additional libraries that may be needed
    find_library(LIBDECOR decor-0)
    find_library(LIBXKBCOMMON xkbcommon)
    if(LIBDECOR)
        list(APPEND LINUX_LIBS ${LIBDECOR})
    endif()
    if(LIBXKBCOMMON)
        list(APPEND LINUX_LIBS ${LIBXKBCOMMON})
    endif()
    
    target_link_libraries(dgview ${LINUX_LIBS})
endif()

# Installation
set(CPACK_PACKAGE_NAME dgview)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Viewer for DG/DGZ data files")
set(CPACK_PACKAGE_CONTACT SheinbergLab)
set(CPACK_PACKAGE_VENDOR SheinbergLab)

if(WIN32)
    # Windows NSIS installer configuration
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "DG Viewer")
    set(CPACK_NSIS_PACKAGE_NAME "dgview")
    set(CPACK_NSIS_MODIFY_PATH ON)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    
    # File associations for .dg and .dgz
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
        WriteRegStr HKCR '.dg' '' 'dgview.Document'
        WriteRegStr HKCR '.dgz' '' 'dgview.Document'
        WriteRegStr HKCR 'dgview.Document' '' 'DG Data File'
        WriteRegStr HKCR 'dgview.Document\\\\DefaultIcon' '' '$INSTDIR\\\\bin\\\\dgview.exe,0'
        WriteRegStr HKCR 'dgview.Document\\\\shell\\\\open\\\\command' '' '$INSTDIR\\\\bin\\\\dgview.exe \\\"%1\\\"'
    ")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
        DeleteRegKey HKCR '.dg'
        DeleteRegKey HKCR '.dgz'
        DeleteRegKey HKCR 'dgview.Document'
    ")
    
    install(TARGETS dgview RUNTIME DESTINATION bin COMPONENT dgview)
elseif(APPLE)
    set(CMAKE_SKIP_RPATH TRUE)
    set(APP_BUNDLE ${CMAKE_BINARY_DIR}/Release/dgview.app)
    
    install(
        CODE "include(BundleUtilities)
        set(BU_CHMOD_BUNDLE_ITEMS ON)
        fixup_bundle(\"${APP_BUNDLE}\" \"\" \"\")"
        COMPONENT dgview
    )
    
    if(DEFINED CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM)
        set(CMAKE_XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--strict --timestamp --options=runtime")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_INJECT_BASE_ENTITLEMENTS "NO")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application")
        
        set(CODESIGN_COMMAND /usr/bin/codesign --force --verify 
            ${CMAKE_XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS} 
            --entitlements ${CMAKE_SOURCE_DIR}/macos/dgview.entitlements --sign)
        
        install(
            CODE "file(GLOB BUNDLE_DYLIBS ${APP_BUNDLE}/Contents/Frameworks/*.dylib)"
            CODE "execute_process(COMMAND ${CODESIGN_COMMAND} \"${CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY}\" \$\{BUNDLE_DYLIBS\})"
            CODE "execute_process(COMMAND ${CODESIGN_COMMAND} \"${CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY}\" ${APP_BUNDLE}/Contents/MacOS/dgview)"
            CODE "execute_process(COMMAND ${CODESIGN_COMMAND} \"${CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY}\" ${APP_BUNDLE})"
            COMPONENT dgview
        )
    endif()
    
    install(TARGETS dgview BUNDLE DESTINATION . COMPONENT dgview)
else()
    # Linux installation
    install(TARGETS dgview RUNTIME DESTINATION bin)
    
    # Install desktop file for menu integration
    install(FILES ${CMAKE_SOURCE_DIR}/linux/dgview.desktop 
            DESTINATION share/applications)
    
    # Install icons at various sizes (if they exist)
    foreach(SIZE 16 32 48 64 128 256 512)
        if(EXISTS ${CMAKE_SOURCE_DIR}/linux/icons/${SIZE}x${SIZE}/dgview.png)
            install(FILES ${CMAKE_SOURCE_DIR}/linux/icons/${SIZE}x${SIZE}/dgview.png
                    DESTINATION share/icons/hicolor/${SIZE}x${SIZE}/apps)
        endif()
    endforeach()
    
    # Fallback: install single icon
    if(EXISTS ${CMAKE_SOURCE_DIR}/linux/dgview.png)
        install(FILES ${CMAKE_SOURCE_DIR}/linux/dgview.png
                DESTINATION share/pixmaps)
    endif()
    
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    # Full dependency list based on ldd output
    set(CPACK_DEBIAN_PACKAGE_DEPENDS 
        "libc6, libstdc++6, libgcc-s1, zlib1g, libx11-6, libxext6, libxinerama1, libxfixes3, libxcursor1, libxrender1, libxft2, libfontconfig1, libgtk-3-0, libgdk-pixbuf-2.0-0, libpango-1.0-0, libpangocairo-1.0-0, libcairo2, libglib2.0-0, libgobject-2.0-0, libgio-2.0-0, libwayland-client0, libwayland-cursor0, libdbus-1-3, libxkbcommon0, libatk1.0-0, libharfbuzz0b")
endif()

include(CPack)
cpack_add_component(dgview REQUIRED)
