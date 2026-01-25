#ifndef DGFILE_H
#define DGFILE_H

/**
 * DgFile - Standalone DG/DGZ file loader
 * 
 * Reads dg, dgz, and lz4 compressed dynamic group files.
 * No Tcl interpreter dependency - uses libdg directly.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <zlib.h>

#include <df.h>
#include <dynio.h>

class DgFile {
public:
    /**
     * Load a DG/DGZ file and return the DYN_GROUP
     * Caller owns the returned pointer and must free with dfuFreeDynGroup()
     * Returns nullptr on error
     */
    static DYN_GROUP* load(const char* filename, std::string* errorMsg = nullptr) {
        if (!filename || !filename[0]) {
            if (errorMsg) *errorMsg = "No filename provided";
            return nullptr;
        }

        DYN_GROUP* dg = dfuCreateDynGroup(4);
        if (!dg) {
            if (errorMsg) *errorMsg = "Failed to allocate DYN_GROUP";
            return nullptr;
        }

        // Check file extension
        const char* suffix = strrchr(filename, '.');
        
        // Try LZ4 format first
        if (suffix && strlen(suffix) == 4 &&
            ((suffix[1] == 'l' && suffix[2] == 'z' && suffix[3] == '4') ||
             (suffix[1] == 'L' && suffix[2] == 'Z' && suffix[3] == '4'))) {
            if (dgReadDynGroup(const_cast<char*>(filename), dg) == DF_OK) {
                return dg;
            } else {
                dfuFreeDynGroup(dg);
                if (errorMsg) *errorMsg = "Failed to read LZ4 file";
                return nullptr;
            }
        }

        // Plain .dg file (no compression)
        if (suffix && strstr(suffix, "dg") && !strstr(suffix, "dgz")) {
            FILE* fp = fopen(filename, "rb");
            if (!fp) {
                dfuFreeDynGroup(dg);
                if (errorMsg) *errorMsg = "Could not open file";
                return nullptr;
            }
            
            if (!dguFileToStruct(fp, dg)) {
                fclose(fp);
                dfuFreeDynGroup(dg);
                if (errorMsg) *errorMsg = "Failed to parse DG file";
                return nullptr;
            }
            fclose(fp);
            return dg;
        }

        // Try gzip compressed (.dgz or other)
        char tempname[256] = {0};
        FILE* fp = uncompressFile(filename, tempname, sizeof(tempname));
        
        if (!fp) {
            // Try with .dg suffix
            std::string altname = std::string(filename) + ".dg";
            fp = uncompressFile(altname.c_str(), tempname, sizeof(tempname));
            
            if (!fp) {
                // Try with .dgz suffix
                altname = std::string(filename) + ".dgz";
                fp = uncompressFile(altname.c_str(), tempname, sizeof(tempname));
            }
        }

        if (!fp) {
            dfuFreeDynGroup(dg);
            if (errorMsg) *errorMsg = "Could not open or decompress file";
            return nullptr;
        }

        if (!dguFileToStruct(fp, dg)) {
            fclose(fp);
            if (tempname[0]) unlink(tempname);
            dfuFreeDynGroup(dg);
            if (errorMsg) *errorMsg = "Failed to parse decompressed DG data";
            return nullptr;
        }

        fclose(fp);
        if (tempname[0]) unlink(tempname);

        return dg;
    }

    /**
     * Get basic info about a loaded DYN_GROUP
     */
    static int getListCount(DYN_GROUP* dg) {
        return dg ? DYN_GROUP_N(dg) : 0;
    }

    static const char* getName(DYN_GROUP* dg) {
        return dg ? DYN_GROUP_NAME(dg) : "";
    }

    static int getMaxRows(DYN_GROUP* dg) {
        if (!dg) return 0;
        int maxRows = 0;
        for (int i = 0; i < DYN_GROUP_N(dg); i++) {
            DYN_LIST* dl = DYN_GROUP_LIST(dg, i);
            if (DYN_LIST_N(dl) > maxRows) {
                maxRows = DYN_LIST_N(dl);
            }
        }
        return maxRows;
    }

private:
    /**
     * Decompress gzipped file to temp file, return FILE* to temp
     */
    static FILE* uncompressFile(const char* filename, char* tempname, int tempnameSize) {
        gzFile in = gzopen(filename, "rb");
        if (!in) return nullptr;

#ifdef WIN32
        char* fname = tempnam("c:/windows/temp", "dg");
#else
        char fname[16];
        strncpy(fname, "/tmp/dgXXXXXX", sizeof(fname));
        int fd = mkstemp(fname);
        if (fd < 0) {
            gzclose(in);
            return nullptr;
        }
        close(fd);
#endif

        FILE* fp = fopen(fname, "wb");
        if (!fp) {
            gzclose(in);
            return nullptr;
        }

        // Decompress
        char buf[4096];
        int len;
        while ((len = gzread(in, buf, sizeof(buf))) > 0) {
            if (fwrite(buf, 1, len, fp) != (size_t)len) {
                fclose(fp);
                gzclose(in);
                unlink(fname);
                return nullptr;
            }
        }

        fclose(fp);
        gzclose(in);

        // Reopen for reading
        fp = fopen(fname, "rb");
        if (tempname && tempnameSize > 0) {
            strncpy(tempname, fname, tempnameSize - 1);
            tempname[tempnameSize - 1] = '\0';
        }

        return fp;
    }
};

#endif // DGFILE_H
