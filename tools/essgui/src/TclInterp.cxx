#include <iostream>
#include <cassert>
#include <tcl.h>
#include "TclInterp.h"

#ifdef _MSC_VER
int setenv(const char *name, const char *value, int overwrite)
{
    int errcode = 0;
    if(!overwrite) {
        size_t envsize = 0;
        errcode = getenv_s(&envsize, NULL, 0, name);
        if(errcode || envsize) return errcode;
    }
    return _putenv_s(name, value);
}
#endif

extern "C" {
int Dlsh_Init(Tcl_Interp *interp);
}

int TclInterp::DlshAppInit(Tcl_Interp *interp) {
  setenv("TCLLIBPATH", "", 1);

  if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;
  if (Dlsh_Init(interp) == TCL_ERROR) return(TCL_ERROR);
  Tcl_VarEval(interp, "set dlshroot [file join [zipfs root] dlsh]", NULL);
  Tcl_VarEval(interp, "zipfs mount /usr/local/dlsh/dlsh.zip $dlshroot", NULL);
  Tcl_VarEval(interp, "set auto_path [linsert $auto_path [set auto_path 0] $dlshroot/lib]", NULL);
  Tcl_VarEval(interp, "source $dlshroot/lib/dlsh/dlsh.tcl", NULL);
  return TCL_OK;
}

TclInterp::TclInterp(int argc, char *argv[]) {
  _interp = Tcl_CreateInterp();
  assert(_interp != NULL);
#ifdef _MSC_VER
  TclZipfs_AppHook(&argc, (wchar_t ***) &argv);
#else
  TclZipfs_AppHook(&argc, &argv);
#endif
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
