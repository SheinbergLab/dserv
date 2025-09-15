cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 11)

set(CMAKE_BUILD_TYPE Release)

if(NOT DEFINED PROJECT_VERSION)
  set(PROJECT_VERSION 0.95)
endif()
project( essgui VERSION ${PROJECT_VERSION} DESCRIPTION "essgui app")

FIND_PACKAGE(FLTK REQUIRED NO-MODULE)
message("using FLTK libs: ${FLTK_LIBRARIES}")

include_directories( ${FLTK_INCLUDE_DIR} src ../include ../../../dlsh/src ../../../dlsh/src/lablib  )

if(WIN32)
    add_compile_options("/DFL_DLL")
    link_directories( c:/usr/local/lib/$ENV{VSCMD_ARG_TGT_ARCH} )
    set(LIBFLTK fltk_dll.lib)
    set(LIBTCL tcl90.lib)
    set(LIBDG dg.lib)
    set(LIBJANSSON jansson.lib)
    set(LIBTCLSTUB tclstub.lib)
    set(ZLIB zlibstatic.lib)
    set(LZ4LIB liblz4_static.lib)
    set(LIBJANSSON jansson.lib)
elseif(APPLE)
    # libtcl9.0 may be installed from source or by homebrew.
    include_directories("/usr/local/include" "/opt/homebrew/include/tcl-tk")
    link_directories("/usr/local/lib" "/opt/homebrew/lib")
    find_library(LIBZ z)
    find_library(LIBLZ4 NAMES "liblz4.a")
    find_library(LIBFLTK NAMES "libfltk.a")
    find_library(LIBFLTK_ZLIB NAMES "libfltk_z.a")
    find_library(LIBTCL "tcl9.0")
    set(LIBTCLSTUB "libtclstub.a")
    find_library(LIBJANSSON NAMES "libjansson.a")
    set(LIBDLSH "dg")

    # Locate TCL library files, as installed by homebrew.
    get_filename_component(LIBTCL_ACTUAL ${LIBTCL} REALPATH)
    get_filename_component(LIBTCL_DIR ${LIBTCL_ACTUAL} DIRECTORY)
else()
    link_directories( /usr/local/lib )
    find_library(LIBZ z)
    find_library(LIBLZ4 NAMES "liblz4.a")
    find_library(LIBFLTK NAMES "libfltk.a")
    find_library(LIBFLTK_ZLIB NAMES "libfltk_z.a")
    find_library(LIBTCL "tcl9.0")
    set(LIBTCLSTUB "/usr/local/lib/libtclstub.a")
    find_library(LIBJANSSON NAMES "libjansson.a")
    set(LIBDLSH "dg")
    
    # Check if XXHash is needed as a separate library (newer Debian/Ubuntu)
    # Try to find libxxhash
    find_library(LIBXXHASH NAMES "libxxhash.a" "xxhash")
    if(LIBXXHASH)
        message(STATUS "Found XXHash library: ${LIBXXHASH}")
    else()
        message(STATUS "XXHash library not found - assuming it's included in LZ4")
    endif()
endif()

include_directories( ../src ../../dlsh/src ../../dlsh/src/lablib )
link_directories( .. )

add_executable( essgui src/essgui.cxx src/setup_ui.cxx src/TclInterp.cxx src/Fl_Console.cxx src/Fl_DgTable.cxx src/mdns.c src/mdns_query.c src/fort.c src/b64.c src/Fl_OpDesk.cxx src/Fl_OpBox.cxx src/Fl_OpButton.cxx src/TclEditor.cxx src/MeshDiscovery.cpp src/EssEventTableWidget.cpp)

# expose functions in executable to loaded libs
set_property(TARGET essgui PROPERTY ENABLE_EXPORTS ON)

if(WIN32)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /NODEFAULTLIB:libcmt gdiplus.lib  comctl32.lib  ws2_32.lib  gdiplus.lib  kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib")
    target_link_libraries( essgui ${LIBFLTK} ${LIBFLTK_Z} ${LZ4LIB} ${LIBTCL} ${ZLIB} ${TCLLIB} ${LIBTCLSTUB} ${LIBJANSSON} ${LIBDG} )
elseif(APPLE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -framework Cocoa -weak_framework UniformTypeIdentifiers -weak_framework ScreenCaptureKit")
    target_link_libraries( essgui ${LIBFLTK} ${LIBFLTK_Z} ${LIBDLSH} ${LIBLAB} ${LIBPDF} ${LIBZ} ${LIBLZ4} ${LIBTCL} ${LIBTCLSTUB} ${LIBJANSSON})
else()
    # Linux - include LIBXXHASH if it was found
    set(LINUX_LIBS ${LIBFLTK} ${LIBFLTK_Z} ${LIBLAB} ${LIBDLSH} ${LIBPDF} ${LIBZ} ${LIBLZ4})
    if(LIBXXHASH)
        list(APPEND LINUX_LIBS ${LIBXXHASH})
    endif()
    list(APPEND LINUX_LIBS ${LIBTCL} ${LIBTCLSTUB} ${LIBJANSSON} X11 Xext Xinerama Xfixes Xcursor Xft Xrender fontconfig pangoxft-1.0 pangoft2-1.0 pango-1.0 gobject-2.0 glib-2.0 harfbuzz freetype pangocairo-1.0 cairo gtk-3 gdk-3 gio-2.0 wayland-cursor wayland-client dbus-1 xkbcommon)
    target_link_libraries( essgui ${LINUX_LIBS})
endif()

set(CPACK_PACKAGE_NAME essgui)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "GUI app to go with dserv and related tools.")
set(CPACK_PACKAGE_CONTACT SheinbergLab)
set(CPACK_PACKAGE_VENDOR SheinbergLab)
set(CPACK_COMPONENTS_ALL essgui)

if(WIN32)
    # TODO
elseif(APPLE)
    # Wrap the essgui executable in an essgui.app bundle.
    set_target_properties(essgui PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME essgui
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_GUI_IDENTIFIER org.sheinberglab.essgui
        MACOSX_BUNDLE_ICON_FILE essgui
    )

    # Copy the app icon that will show in finder, dock, etc.
    target_sources(essgui PRIVATE ${CMAKE_SOURCE_DIR}/macos/essgui.icns)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/macos/essgui.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

    # "Fix up" the app bundle to make it relocatable.
    # Copy dylib dependencies into the bundle.
    # Configure the executable and dylibs to load dylibs from within the bundle.
    set(CMAKE_SKIP_RPATH TRUE)
    set(APP_BUNDLE ${CMAKE_BINARY_DIR}/Release/essgui.app)
    install(
        CODE "include(BundleUtilities)
        set(BU_CHMOD_BUNDLE_ITEMS ON)
        fixup_bundle(\"${APP_BUNDLE}\" \"\" \"\")"
        COMPONENT essgui
    )

    if(LIBTCL_DIR)
        # Copy TCL library files into the bundle as well.
        install(
            DIRECTORY ${LIBTCL_DIR}/tcl9.0/
            DESTINATION ${APP_BUNDLE}/Contents/lib/tcl9.0
            COMPONENT essgui
        )
    endif()

    if(DEFINED "CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM")
        # Configure for apple code signing and notarization.
        set(CMAKE_XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--strict --timestamp --options=runtime")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_INJECT_BASE_ENTITLEMENTS "NO")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application")

        # "Fixing up" the app bundle invalidates previously signed binaries.
        # Go back and sign things again: the executable, any relocated dylibs, and the bundle itself.
        set(CODESIGN_COMMAND /usr/bin/codesign --force --verify ${CMAKE_XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS} --entitlements ${CMAKE_SOURCE_DIR}/macos/essgui.entitlements --sign)
        install(
            CODE "file(GLOB BUNDLE_DYLIBS ${APP_BUNDLE}/Contents/Frameworks/*.dylib)"
            CODE "execute_process(COMMAND ${CODESIGN_COMMAND} \"${CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY}\" \$\{BUNDLE_DYLIBS\})"
            CODE "execute_process(COMMAND ${CODESIGN_COMMAND} \"${CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY}\" ${APP_BUNDLE}/Contents/MacOS/essgui)"
            CODE "execute_process(COMMAND ${CODESIGN_COMMAND} \"${CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY}\" ${APP_BUNDLE})"
            COMPONENT essgui
        )
    endif()

    # We want to install the "Fixed up" and re-signed bundle, so this instruction goes last.
    install(TARGETS essgui BUNDLE DESTINATION . COMPONENT essgui)
else()
    INSTALL(TARGETS essgui RUNTIME)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    # Update Debian dependencies to include libxxhash0 if needed
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libevdev2, libpq5, zlib1g, yajl-tools, libx11-6, libxinerama1, libxfixes3, libxcursor1, libpango-1.0-0, libpangocairo-1.0-0, libgtk-3-0, libxxhash0")
endif()

include(CPack)
cpack_add_component(essgui REQUIRED)
