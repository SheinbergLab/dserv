#include <gbuf.h>
#include <cgraph.h>

#ifdef __cplusplus
extern "C" {
#endif
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

