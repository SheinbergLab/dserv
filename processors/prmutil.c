/*
 * NAME
 *   prmutil.h
 *
 * DESCRIPTION 
 *   Utilities for reading and writing parameter lists.
 *
 * AUTHOR 
 *   DLS, 6/94
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "prmutil.h"

static int sizes[] = { 
  0,
  sizeof(char), 
  sizeof(short),
  sizeof(int),
  sizeof(float),
  sizeof(double)
};

static char *ifmt[]={
  "", 
  "%c", 
  "%hd", 
  "%d", 
  "%f", 
  "%lf"
};

static char *ofmt[]={
  "", 
  "%c", 
  "%hd", 
  "%d", 
  "%7.4f", 
  "%lf"
};

/*--------------------------------------------------------------------
 * FUNCTION:    puSetParamEntry
 * ARGS:        PARAM_ENTRY *table, char *name, int nvals, char **vals
 * DESCRIPTION: Sets parameter called "name" to equal vals
 * RETURNS:     Number of variables set:
 *               (1 for basic types, >= 1 for arrays)
 *--------------------------------------------------------------------*/

int puSetParamEntry(PARAM_ENTRY *table, char *name, int nvals, char **vals)
{
  int i;
  PARAM_ENTRY *p = &table[0];
  char *val = vals[0];
  float **fptr;
  int **lptr;

  if (!nvals) return(0);

  while (p->type != PU_NULL) {
#if defined(__linux__) || defined(__APPLE__) || defined(FREEBSD) || defined(__QNX__)
    if (!strcasecmp(name,p->name)) {
#else
    if (!stricmp(name,p->name)) {
#endif
    switch(p->type) {
      case PU_CHAR:
	*p->n = sscanf(val, ifmt[PU_CHAR], (char *)p->value);
	return(1);
	break;
      case PU_SHORT:
	*p->n = sscanf(val, ifmt[PU_SHORT], (short *)p->value);
	return(1);
	break;
      case PU_LONG:
	*p->n = sscanf(val, ifmt[PU_LONG], (int *)p->value);
	return(1);
	break;
      case PU_FLOAT:
	*p->n = sscanf(val, ifmt[PU_FLOAT], (float *)p->value);
	return(1);
	break;
      case PU_DOUBLE:
	*p->n = sscanf(val, ifmt[PU_DOUBLE], (double *)p->value);
	return(1);
	break;
      case PU_LONG_ARRAY:
	lptr = (int **) (p->value);
	if (*lptr) free((void *) *lptr);
	*lptr = (int *) calloc(nvals, sizeof(int));
	*p->n = nvals;
	for (i = 0; i < nvals; i++) {
	  sscanf(vals[i], ifmt[PU_LONG], &((*lptr)[i]));
	}
	return(nvals);
      case PU_FLOAT_ARRAY:
	fptr = (float **) (p->value);
	if (*fptr) free((void *) *fptr);
	*fptr = (float *) calloc(nvals, sizeof(float));
	*p->n = nvals;
	for (i = 0; i < nvals; i++) {
	  sscanf(vals[i], ifmt[PU_FLOAT], &((*fptr)[i]));
	}
	return(nvals);
      }
    }
    p++;
  }
  return(0);
}

/*--------------------------------------------------------------------
 * FUNCTION:    puGetParamEntry
 * ARGS:        PARAM_ENTRY *table, char *name
 * DESCRIPTION: Returns a string corresponding to value of vals
 * RETURNS:     1 if successful, 0 on error
 *--------------------------------------------------------------------*/

char *puGetParamEntry(PARAM_ENTRY *table, char *name)
{
  int i;
  PARAM_ENTRY *p = &table[0];
  static char buffer[128], tmpbuf[24];
  float **fptr;
  int **lptr;

  while (p->type != PU_NULL) {

#if defined(__linux__) || defined(__APPLE__) || defined(FREEBSD) || defined(__QNX__)
    if (!strcasecmp(name,p->name)) {
#else
    if (!stricmp(name,p->name)) {
#endif
      switch(p->type) {
      case PU_CHAR:
	sprintf(buffer, ofmt[PU_CHAR], *((char *)p->value));
	break;
      case PU_SHORT:
	sprintf(buffer, ofmt[PU_SHORT], *((short *)p->value));
	break;
      case PU_LONG:
	sprintf(buffer, ofmt[PU_LONG], *((int *)p->value));
	break;
      case PU_FLOAT:
	sprintf(buffer, ofmt[PU_FLOAT], *((float *)p->value));
	break;
      case PU_DOUBLE:
	sprintf(buffer, ofmt[PU_DOUBLE], *((double *)p->value));
	break;
      case PU_LONG_ARRAY:
	lptr = (int **) (p->value);
	buffer[0] = 0;
	for (i = 0; i < *p->n; i++) {
	  sprintf(tmpbuf, ofmt[PU_LONG], (*lptr)[i]);
	  if (i) strcat(buffer, " ");
	  strcat(buffer, tmpbuf);
	}
	break;
      case PU_FLOAT_ARRAY:
	fptr = (float **) (p->value);
	buffer[0] = 0;
	for (i = 0; i < *p->n; i++) {
	  sprintf(tmpbuf, ofmt[PU_FLOAT], (*fptr)[i]);
	  if (i) strcat(buffer, " ");
	  strcat(buffer, tmpbuf);
	}
	break;
      }
      return(buffer);
    }
    p++;
  }
  return(NULL);
}

/*--------------------------------------------------------------------
 * FUNCTION:    puVarList
 * ARGS:        PARAM_ENTRY *table
 * DESCRIPTION: Returns a string of all settable entries in table
 * RETURNS:     1 if successful, 0 on error
 *--------------------------------------------------------------------*/

char *puVarList(PARAM_ENTRY *table)
{
  PARAM_ENTRY *p = &table[0];
  int nbytes = 0;
  char *buffer;

  while (p->type != PU_NULL) {
#if defined(LINUX) || defined(MACOS)
    nbytes += (strlen(p->name)+1);
#else
    nbytes += (strlen(p->name)+2);
#endif
    p++;
  }
  buffer = (char *) calloc(nbytes, sizeof(char));

  p = &table[0];
  while (p->type != PU_NULL) {
    if (p != &table[0]) strcat(buffer, " ");
    strcat(buffer,p->name);
    p++;
  }
  return(buffer);
}

#ifdef TEST1

float x = 10.0, y = 3.14;
int numbers[] = { 0, 1, 2, 3, 4 };
int n_numbers = sizeof(numbers) / sizeof(int);
char verbose = 0;

PARAM params[] = {
  &x, 1, PU_FLOAT,
  &y, 1, PU_FLOAT,
  numbers, sizeof(numbers) / sizeof(int), PU_INT,
  &verbose, 1, PU_CHAR,
  0, 0, PU_NULL
};
  
main()
{
  int i;
  printf("Before...\n");
  printf("verbose: %d\n", verbose);
  printf("x: %6.3f\ty: %6.3f\n", x, y);
  printf("numbers:\t");
  for (i = 0; i < n_numbers; i++) {
    printf("%d ", numbers[i]);
  }
  printf("\n");

  for (i = 0; i < n_numbers; i++) numbers[i] = 10-i;
  verbose = 1;
  x = 50.0; y = -10.0;

  puWriteParams(params, "test.dmp");

  printf("After...\n");
  puReadParams(params, "test.dmp");
  printf("verbose: %d\n", verbose);
  printf("x: %6.3f\ty: %6.3f\n", x, y);
  printf("numbers:\t");
  for (i = 0; i < n_numbers; i++) {
    printf("%d ", numbers[i]);
  }
  printf("\n");
  
}

#endif

#ifdef TEST2

typedef struct {
  float floatval;
  double doubleval;
  char charval;
  short shortval;
  int longval;
  int nfloatlist;
  float *floatlist;
  int nintlist;
  int *intlist;
  int dummyInt;
} SAMPLE_STRUCT;

SAMPLE_STRUCT TestData;

PARAM_ENTRY params[] = {
  "charval",   &TestData.charval,    &TestData.dummyInt,   PU_CHAR,
  "longval",   &TestData.longval,    &TestData.dummyInt,   PU_LONG,
  "shortval",  &TestData.shortval,   &TestData.dummyInt,   PU_SHORT,
  "floatval",  &TestData.floatval,   &TestData.dummyInt,   PU_FLOAT,
  "doubleval", &TestData.doubleval,  &TestData.dummyInt,   PU_DOUBLE,
  "floatlist", &TestData.floatlist,  &TestData.nfloatlist, PU_FLOAT_ARRAY,
  "intlist",   &TestData.intlist,    &TestData.nintlist,   PU_LONG_ARRAY,
  "", NULL, NULL, PU_NULL
};


main(int argc, char *argv[])
{
  char buffer[64], name[32], val[32], *retval;
  int result;
  switch (argc) {
  case 1:
    printf("Variable names: %s\n", puVarList(params));
    break;
  case 2:
    printf("%s:\t%s\n", argv[1], puGetParamEntry(params, argv[1]));
    break;
  default:
    result = puSetParamEntry(params, argv[1], argc-2, &argv[2]);
    printf("%s:\t%s\n", argv[1], puGetParamEntry(params, argv[1]));
    break;
  }
}
#endif
