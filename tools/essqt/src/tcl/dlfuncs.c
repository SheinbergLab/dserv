#include <tcl.h>
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

  DLSHINFO *dlinfo =
    (DLSHINFO *) Tcl_GetAssocData(interp, DLSH_ASSOC_DATA_KEY, NULL);
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


int tclPutDynGroup(Tcl_Interp *interp, DYN_GROUP *dg) 
{
  Tcl_HashEntry *entryPtr;
  int newentry;
  char groupname[64];

  DLSHINFO *dlinfo =
    (DLSHINFO *) Tcl_GetAssocData(interp, DLSH_ASSOC_DATA_KEY, NULL);

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


DYN_GROUP *decode_dg(const char *data, int length)
{
  DYN_GROUP *dg;
  unsigned int decoded_length = length;
  unsigned char *decoded_data;
  int result;

  if (!(dg = dfuCreateDynGroup(4))) {
    return NULL;
  }
  
  decoded_data = (unsigned char *) calloc(decoded_length, sizeof(char));
  result = base64decode((char *) data, length, decoded_data, &decoded_length);
  
  if (result) {
    free(decoded_data);
    return NULL;
  }
  
  if (dguBufferToStruct(decoded_data, decoded_length, dg) != DF_OK) {
    free(decoded_data);
    return NULL;
  }
  
  free(decoded_data);
  return dg;
}


DYN_LIST *findDynListInGroup(DYN_GROUP *dg, char *name)
{
  int i;
  
  for (i = 0; i < DYN_GROUP_N(dg); i++) {
    if (!strcmp(DYN_LIST_NAME(DYN_GROUP_LIST(dg,i)), name)) {
      return(DYN_GROUP_LIST(dg,i));
    }
  }

  return(NULL);
}
