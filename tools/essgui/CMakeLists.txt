cmake_minimum_required(VERSION 3.15)
set(CMAKE_CXX_STANDARD 14)

set(CMAKE_BUILD_TYPE Release)

project( essgui VERSION 0.95 DESCRIPTION "essgui app")

FIND_PACKAGE(FLTK REQUIRED NO-MODULE)
message("using FLTK libs: ${FLTK_LIBRARIES}")

include_directories( ${FLTK_INCLUDE_DIR} src ../include ../../../dlsh/src ../../../dlsh/src/lablib  )

if(WIN32)
  link_directories( c:/usr/local/lib )
  set(LIBZ zlibstatic.lib)
  set(LIBLZ4 liblz4_static64.lib)
  set(LIBFLTK fltk.lib)
  set(LIBTCL tcl86.lib)
  set(LIBDLSH dlsh-static.lib)
else()
  link_directories( /usr/local/lib )
  set (CMAKE_INSTALL_PREFIX /usr/local)
  find_library(LIBZ z)
  find_library(LIBLZ4 NAMES "liblz4.a")
  find_library(LIBFLTK NAMES "libfltk.a")
  find_library(LIBFLTK_ZLIB NAMES "libfltk_z.a")
  set(LIBTCL "tcl9.0")
  set(LIBTCLSTUB "/usr/local/lib/libtclstub.a")
  set(LIBDLSH "dlsh-static")
  set(LIBPDF "/usr/local/lib/libhpdf.a")
  find_library(LIBJANSSON NAMES "libjansson.a")	
endif()

include_directories( ../src ../../dlsh/src ../../dlsh/src/lablib )
link_directories( .. )
add_executable( essgui src/essgui.cxx src/setup_ui.cxx src/TclInterp.cxx src/Fl_Console.cxx src/Fl_DgTable.cxx src/linenoise-fltk.cxx src/mdns.c src/mdns_query.c src/fort.c)

if(WIN32)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}   gdiplus.lib  comctl32.lib  ws2_32.lib  gdiplus.lib  kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib")
endif()

if(APPLE)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -framework Cocoa -weak_framework UniformTypeIdentifiers -weak_framework ScreenCaptureKit")
 target_link_libraries( essgui ${LIBFLTK} ${LIBFLTK_Z} ${LIBDLSH} ${LIBLAB} ${LIBPDF} ${LIBZ} ${LIBLZ4} ${LIBTCL} ${LIBTCLSTUB} ${LIBJANSSON})
else()
 target_link_libraries( essgui ${LIBFLTK} ${LIBFLTK_Z} ${LIBLAB} ${LIBDLSH} ${LIBPDF} ${LIBZ} ${LIBLZ4} ${LIBTCL} ${LIBTCLSTUB} ${LIBJANSSON} X11 Xext Xinerama Xfixes Xcursor Xft Xrender fontconfig pangoxft-1.0 pangoft2-1.0 pango-1.0 gobject-2.0 glib-2.0 harfbuzz freetype pangocairo-1.0 cairo gtk-3 gdk-3 gio-2.0 wayland-cursor wayland-client dbus-1 xkbcommon)
endif()

if(WIN32)

else()

INSTALL(TARGETS essgui
        RUNTIME DESTINATION /usr/local/bin
        ARCHIVE DESTINATION /usr/local/lib
)
endif()
