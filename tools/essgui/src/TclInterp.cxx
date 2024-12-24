#include <iostream>
#include <cassert>
#include <tcl.h>
#include "TclInterp.h"

#define DLSH_ASSOC_DATA_KEY "dlsh"

/*
 * Tmplist stack structure for pushing & popping tmp list names
 */

typedef struct {
  int size;
  int index;
  int increment;
  DYN_LIST **lists;
} TMPLIST_STACK;


typedef struct _dlshinfo {
  /*
   * Local tables for holding dynGroups and dynLists
   */
  Tcl_HashTable dlTable;	/* stores dynLists  */
  Tcl_HashTable dgTable;	/* stores dynGroups */

  /*
   * amounts to grow by dynamically
   */
  int DefaultListIncrement;
  int DefaultGroupIncrement;
  
  /*
   * variables used to manage groups and lists
   */
  int dgCount;		/* increasing count of dynGroups */
  int dlCount;	        /* increasing count of dynLists  */
  int localCount;       /* for naming local variables    */
  int returnCount;      /* for naming returned lists     */

  /*
   * for managing temporary list storage
   */
  TMPLIST_STACK *TmpListStack;
  DYN_LIST *TmpListRecordList;
    
  
} DLSHINFO;


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
  Tcl_StaticPackage(interp, "dlsh", Dlsh_Init, NULL);

  Tcl_VarEval(interp, 
        "proc load_local_packages {} {\n"
        " global auto_path\n"
        " set f [file dirname [info nameofexecutable]]\n"
        " if [file exists [file join $f dlsh.zip]] { set dlshzip [file join $f dlsh.zip] } {"
#ifdef _WIN32
        "   set dlshzip c:/usr/local/dlsh/dlsh.zip }\n"
#else
        "   set dlshzip /usr/local/dlsh/dlsh.zip }\n"
#endif
        " set dlshroot [file join [zipfs root] dlsh]\n"
        " zipfs unmount $dlshroot\n"
        " zipfs mount $dlshzip $dlshroot\n"
	" set auto_path [linsert $auto_path [set auto_path 0] $dlshroot/lib]\n"
	" package require dlsh\n"
        "}\n"
        "load_local_packages\n",
        NULL);
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

#ifdef _MSC_VER
  Tcl_VarEval(_interp,
	      "source [file join [zipfs root] dlsh lib dlsh dlsh.tcl]", NULL);
#endif
  
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

DYN_LIST *TclInterp::findDynList(DYN_GROUP *dg, char *name)
{
  int i;

  for (i = 0; i < DYN_GROUP_N(dg); i++) {
    if (!strcmp(DYN_LIST_NAME(DYN_GROUP_LIST(dg,i)), name)) {
      return(DYN_GROUP_LIST(dg,i));
    }
  }

  return(NULL);
}

int TclInterp::tclPutGroup(DYN_GROUP *dg) 
{
  Tcl_Interp *interp = _interp;
  Tcl_HashEntry *entryPtr;
  int newentry;
  char groupname[64];

  DLSHINFO *dlinfo = (DLSHINFO *) Tcl_GetAssocData(interp, DLSH_ASSOC_DATA_KEY, NULL);

  if (!dlinfo) return TCL_ERROR;
  
  if (!dg) return 0;

  if (!DYN_GROUP_NAME(dg)[0]) {
    snprintf(groupname, sizeof(groupname), "group%d", dlinfo->dgCount++);
    strcpy(DYN_GROUP_NAME(dg), groupname);
  }
  else {
    strcpy(groupname, DYN_GROUP_NAME(dg));
  }

  if ((entryPtr = Tcl_FindHashEntry(&dlinfo->dgTable, groupname))) {
    char resultstr[128];
    snprintf(resultstr, sizeof(resultstr),
	     "tclPutGroup: group %s already exists", groupname);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(resultstr, -1));
    return TCL_ERROR;
  }

  /*
   * Add to hash table which contains list of open dyngroups
   */
  entryPtr = Tcl_CreateHashEntry(&dlinfo->dgTable, groupname, &newentry);
  Tcl_SetHashValue(entryPtr, dg);

  Tcl_SetResult(interp, groupname, TCL_VOLATILE);
  return TCL_OK;
}
