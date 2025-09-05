#pragma once

#include <cgraph.h>
#include <gbuf.h>
#include <df.h>
#include <dynio.h>
#include <tcl.h>
#include "b64.h"

#ifdef __cplusplus
extern "C" {
#endif
  DYN_GROUP *decode_dg(const char *data, int length);
  DYN_LIST *findDynListInGroup(DYN_GROUP *dg, char *name);

  int tclPutDynGroup(Tcl_Interp *interp, DYN_GROUP *dg);
  int tclFindDynGroup(Tcl_Interp *interp, char *name, DYN_GROUP **dg);
  int tclFindDynList(Tcl_Interp *interp, char *name, DYN_LIST **dl);
#ifdef __cplusplus
}
#endif


struct dl_ps_ctx {
  FRAME fr ;
  GBUF_DATA gb ;
  struct dl_ps_ctx * next ;
} ;

typedef struct dl_ps_ctx dpc ;

