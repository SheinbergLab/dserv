#ifndef __DGFILE_H
#define __DGFILE_H

// use libdg to read dgz files
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#include <tcl.h>
#include <df.h>
#include <dynio.h>

extern "C" {
  int tclFindDynGroup(Tcl_Interp *interp, char *name, DYN_GROUP **dg);
}



class DGFile {
private:
  /*
   * Uncompress input to output then close both files.
   */
  
  static void gz_uncompress(gzFile in, FILE *out)
  {
    char buf[2048];
    int len;
    
    for (;;) {
      len = gzread(in, buf, sizeof(buf));
      if (len < 0) return;
      if (len == 0) break;
      
      if ((int)fwrite(buf, 1, (unsigned)len, out) != len) {
	return;
      }
    }
    if (fclose(out)) return;
    if (gzclose(in) != Z_OK) return;
  }
  
  static FILE *uncompress_file(char *filename, char *tempname, int sz)
  {
    FILE *fp;
    gzFile in;
#ifdef WIN32
    static char *tmpdir = "c:/windows/temp";
    char *fname;
#else
    int result;
    static char fname[9];
#endif
    
    if (!filename) return NULL;
    
    if (!(in = gzopen(filename, "rb"))) {
      snprintf(tempname, sz, "file %s not found", filename);
      return 0;
    }
    
#ifdef WIN32
    fname = tempnam(tmpdir, "dg");
#else
    strncpy(fname, "dgXXXXXX", 8);
    result = mkstemp(fname);
#endif
    if (!(fp = fopen(fname,"wb"))) {
      strncpy(tempname, "error opening temp file for decompression", sz);
      return 0;
    }
    
    gz_uncompress(in, fp);
    
    /* DONE in gz_uncompress!  fclose(fp); */
    
    fp = fopen(fname, "rb");
    if (tempname) strncpy(tempname, fname, sz);
    
    
#ifdef WIN32
    //  free(fname); ?? Apparently not, as it crashes when compiled with mingw
#endif
    
    return(fp);
  }  
  
public:

  static DYN_GROUP *read_dgz(char *filename)
  {
    DYN_GROUP *dg = NULL;
    FILE *fp;
    char *newname = NULL, *suffix;
    char tempname[128];
    
    if (!(dg = dfuCreateDynGroup(4))) {
      return NULL;
    }
    
    /* No need to uncompress a .dg file */
    if ((suffix = strrchr(filename, '.')) && strstr(suffix, "dg") &&
	!strstr(suffix, "dgz")) {
      fp = fopen(filename, "rb");
      if (!fp) {
	return NULL;
      }
      tempname[0] = 0;
    }
    
    else if ((suffix = strrchr(filename, '.')) &&
	     strlen(suffix) == 4 &&
	     ((suffix[1] == 'l' && suffix[2] == 'z' && suffix[3] == '4') ||
	      (suffix[1] == 'L' && suffix[2] == 'Z' && suffix[3] == '4'))) {
      if (dgReadDynGroup(filename, dg) == DF_OK) {
	goto process_dg;
      }
      else {
	return NULL;
      }
    }
    
    else if ((fp = uncompress_file(filename, tempname, sizeof(tempname))) == NULL) {
      char fullname[128];
      snprintf(fullname, sizeof(fullname), "%s.dg", filename);
      if ((fp = uncompress_file(fullname, tempname, sizeof(tempname))) == NULL) {
	snprintf(fullname, sizeof(fullname), "%s.dgz", filename);
	if ((fp = uncompress_file(fullname, tempname, sizeof(tempname))) == NULL) {
	  if (tempname[0] == 'f') { /* 'file not found...*/
	    return 0;
	  }
	  else {
	    return 0;
	  }
	  return 0;
	}
      }
    }
    
    if (!dguFileToStruct(fp, dg)) {
      fclose(fp);
      if (tempname[0]) unlink(tempname);
      return 0;
    }
    fclose(fp);
    if (tempname[0]) unlink(tempname);
    
  process_dg:
    return dg;
  }
};

#endif // DGFile
