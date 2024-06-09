/*
 * prmutil.h - Definitions for parameter utilities
 */

typedef struct {
  char *name;
  void *value;
  int *n;
  int type;
} PARAM_ENTRY;

typedef struct {
  void *value;
  int n;
  int type;
} PARAM;

#define PU_NULL    0
#define PU_CHAR    1
#define PU_SHORT   2
#define PU_LONG    3
#define PU_INT     PU_LONG
#define PU_FLOAT   4
#define PU_DOUBLE  5
#define PU_LONG_ARRAY  6
#define PU_FLOAT_ARRAY 7

#define PU_OK          0
#define PU_FOPEN_ERR   1
#define PU_WRITE_ERR   2
#define PU_READ_ERR    3

int puWriteParams(PARAM *, char *);
int puReadParams(PARAM *, char *);

int puSetParamEntry(PARAM_ENTRY *table, char *name, int n, char **val);
char *puGetParamEntry(PARAM_ENTRY *table, char *name);
char *puVarList(PARAM_ENTRY *table);
