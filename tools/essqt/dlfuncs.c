#include <tcl.h>
#include <df.h>
#include <dynio.h>
#include <string.h>
#include <stdlib.h>
#include "dlfuncs.h"

/*
 * Tmplist stack structure for pushing & popping tmp list names
 */

static const char *DLSH_ASSOC_DATA_KEY = "dlsh";

typedef struct {
  int size;
  int index;
  int increment;
  DYN_LIST **lists;
} TMPLIST_STACK;


#define TMPLIST_SIZE(t)        ((t)->size)
#define TMPLIST_INDEX(t)       ((t)->index)
#define TMPLIST_INC(t)         ((t)->increment)
#define TMPLIST_TMPLISTS(t)    ((t)->lists)

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

/*****************************************************************************
 *
 * FUNCTION
 *    tclFindDynGroup
 *
 * ARGS
 *    Tcl_Interp *interp, char *name, DYN_GROUP **
 *
 * DESCRIPTION
 *    Searches for dyngroup called name.  If dg is not null, it places
 * a pointer to the dyngroup there.
 *
 *****************************************************************************/

int tclFindDynGroup(Tcl_Interp *interp, char *name, DYN_GROUP **dg)
{
  DYN_GROUP *g;
  Tcl_HashEntry *entryPtr;

  DLSHINFO *dlinfo = (DLSHINFO *) Tcl_GetAssocData(interp, DLSH_ASSOC_DATA_KEY, NULL);
  if (!dlinfo) return TCL_ERROR;
  
  if ((entryPtr = Tcl_FindHashEntry(&dlinfo->dgTable, name))) {
    g = (DYN_GROUP *) Tcl_GetHashValue(entryPtr);
    if (!g) {
      Tcl_SetResult(interp, "bad dyngroup ptr in hash table", TCL_STATIC);
      return TCL_ERROR;
    }
    if (dg) *dg = g;
    return TCL_OK;
  }
  else {
    char outname[64];
    strncpy(outname, name, sizeof(outname));
    Tcl_AppendResult(interp, "dyngroup \"", outname, "\" not found", 
		     (char *) NULL);
    return TCL_ERROR;
  }
}

