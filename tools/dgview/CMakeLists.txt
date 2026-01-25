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
    link_directories(c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH})
    set(LIBFLTK fltk.lib)
    set(LIBDG dg.lib)
    set(ZLIB zlibstatic.lib)
    set(LZ4LIB liblz4_static.lib)
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
    /usr/local/include
)

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
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /NODEFAULTLIB:libcmt comctl32.lib ws2_32.lib kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib comdlg32.lib advapi32.lib")
    target_link_libraries(dgview ${LIBFLTK} ${LZ4LIB} ${ZLIB} ${LIBDG})
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
        if(WAYLAND_FOUND)
            list(APPEND LINUX_LIBS ${WAYLAND_LIBRARIES})
        endif()
        if(GTK3_FOUND)
            list(APPEND LINUX_LIBS ${GTK3_LIBRARIES})
        endif()
        if(CAIRO_FOUND)
            list(APPEND LINUX_LIBS ${CAIRO_LIBRARIES})
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
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "zlib1g, liblz4-1, libx11-6, libxinerama1, libxfixes3, libxcursor1")
endif()

include(CPack)
cpack_add_component(dgview REQUIRED)
