#ifndef TclInterp_H
#define TclInterp_H

#include <tcl.h>
#include <df.h>
#include <dynio.h>

class TclInterp {
  Tcl_Interp *_interp;
public:
  int DlshAppInit(Tcl_Interp *interp);
  TclInterp(int argc, char *argv[]);
  ~TclInterp(void);
  Tcl_Interp * interp(void);
  int eval(const char *command, std::string &resultstr);
  std::string eval(const char *command);
  int tclPutGroup(DYN_GROUP *dg); 
  DYN_LIST *findDynList(DYN_GROUP *dg, char *); 
};

#endif
