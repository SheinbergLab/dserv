# Locate Tcl 9.x so that the headers and the library always agree.
#
# The minor version is read from the tcl.h the compiler will see -- the first
# hit in TCL9_INCLUDE_HINTS, which must list the same directories, in the same
# order, as the project's include_directories() -- and the library looked up is
# exactly libtcl<major>.<minor>. Asking for "tcl9.1 or tcl9.0" instead would
# happily pair one minor's headers with another's library on any machine that
# has both installed.
#
# Sets: TCL_H_DIR, TCL_MM (e.g. "9.1"), TCL_PATCH (e.g. "9.1.0"), LIBTCL.

if(NOT DEFINED TCL9_INCLUDE_HINTS)
    if(APPLE)
        set(TCL9_INCLUDE_HINTS /usr/local/include /opt/homebrew/include/tcl-tk)
    else()
        set(TCL9_INCLUDE_HINTS /usr/local/include)
    endif()
endif()

# The hints first and on their own: macOS would otherwise prefer the SDK's
# Tcl.framework (8.5) over any of them.
find_path(TCL_H_DIR tcl.h PATHS ${TCL9_INCLUDE_HINTS} NO_DEFAULT_PATH)
find_path(TCL_H_DIR tcl.h)
if(NOT TCL_H_DIR)
    message(FATAL_ERROR "tcl.h not found (looked in ${TCL9_INCLUDE_HINTS} and the default paths)")
endif()

file(STRINGS "${TCL_H_DIR}/tcl.h" _tcl_version_line REGEX "define[ \t]+TCL_VERSION[ \t]")
string(REGEX MATCH "9\\.[0-9]+" TCL_MM "${_tcl_version_line}")
file(STRINGS "${TCL_H_DIR}/tcl.h" _tcl_patch_line REGEX "define[ \t]+TCL_PATCH_LEVEL[ \t]")
string(REGEX MATCH "9\\.[0-9]+[.ab][0-9]+" TCL_PATCH "${_tcl_patch_line}")
if(NOT TCL_MM)
    message(FATAL_ERROR "${TCL_H_DIR}/tcl.h is not a Tcl 9 header")
endif()

find_library(LIBTCL NAMES tcl${TCL_MM})
if(NOT LIBTCL)
    message(FATAL_ERROR "${TCL_H_DIR}/tcl.h is Tcl ${TCL_PATCH} but no libtcl${TCL_MM} was found to match it")
endif()
message(STATUS "Tcl ${TCL_PATCH}: headers ${TCL_H_DIR}, library ${LIBTCL}")
