#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>

int main(int argc, char *argv[])
{
  typedef int (*processfunc)( int, uint16_t *v, uint64_t, char ** );
  void* handle;
  processfunc process;
  int n = 2;
  uint64_t timestamp = 1234567890;
  uint16_t vals[2] = { 4000, 3900 };
  char *resultstr;
  int rc;
  
  if (argc < 2) {
    printf("usage: %s shared_lib\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  
  /* Open a shared library. */
  handle = dlopen( argv[1], RTLD_NOW|RTLD_GROUP|RTLD_GLOBAL|RTLD_WORLD);
  if (!handle) {
    printf("dlopen error: %s\n", dlerror());
    exit(EXIT_FAILURE);
  }

  /* Find the address of a function and a global integer. */
  process = (processfunc) dlsym( handle, "onProcess" );

  /* Invoke the function and print the int. */
  rc = (*process)( n, vals, 12345678, &resultstr );
  if (rc) printf("%s", resultstr);
  else printf("pass\n");
 
  return 0;

}
