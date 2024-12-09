#include <iostream>
#include <cassert>
#include <tcl.h>
#include "TclInterp.h"

extern "C" {
int Dlsh_Init(Tcl_Interp *interp);
}

int TclInterp::DlshAppInit(Tcl_Interp *interp) {
  setenv("TCLLIBPATH", "", 1);

  if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;
  if (Dlsh_Init(interp) == TCL_ERROR) return(TCL_ERROR);
  Tcl_VarEval(interp, "lappend auto_path [file dir [info nameofexecutable]]/../lib", NULL);
  Tcl_VarEval(interp, "set dlshroot [file join [zipfs root] dlsh]");
  Tcl_VarEval(interp, "zipfs mount /usr/local/dlsh/dlsh.zip $dlshroot", NULL);
  Tcl_VarEval(interp, "set auto_path [linsert $auto_path [set auto_path 0] $dlshroot/lib]");
  Tcl_VarEval(interp, "source $dlshroot/lib/dlsh/dlsh.tcl", NULL);
  return TCL_OK;
}

TclInterp::TclInterp(int argc, char *argv[]) {
  _interp = Tcl_CreateInterp();
  assert(_interp != NULL);
  TclZipfs_AppHook(&argc, &argv);

  /*
   * Invoke application-specific initialization.
   */

  if (DlshAppInit(_interp) != TCL_OK) {
    std::cerr << "application-specific initialization failed: ";
    std::cerr << Tcl_GetStringResult(_interp) << std::endl;
  }
  else {
    Tcl_SourceRCFile(_interp);
  }
}

TclInterp::~TclInterp(void) {
  /* Free the interpreter */
  Tcl_DeleteInterp(_interp);
}

Tcl_Interp * TclInterp::interp(void) {
  return _interp;
}

int TclInterp::eval(const char *command, std::string &resultstr) {
  int len;
  int result = Tcl_Eval(_interp, command);
  char *rstr = Tcl_GetStringResult(_interp);
  resultstr = std::string(rstr);
  return result;
}

std::string TclInterp::eval(const char *command) {
  int len;
  int result = Tcl_Eval(_interp, command);
  char *rstr = Tcl_GetStringResult(_interp);
  return std::string(rstr);
}
