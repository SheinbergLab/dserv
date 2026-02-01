/*
 * TclSha256.cpp - SHA256 hash command for Tcl using OpenSSL
 *
 * Provides:
 *   sha256 $string        - Returns hex-encoded SHA256 hash of string
 *   sha256 -file $path    - Returns hex-encoded SHA256 hash of file contents
 *
 * Uses OpenSSL which is already linked into dserv.
 * Add to TclServer.cpp's add_tcl_commands():
 *   TclSha256_RegisterCommands(interp);
 *
 * Build: Already links OpenSSL, just include this file in the build.
 */

#include <tcl.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#include <cstdio>
#include <cstring>

/*
 * Convert binary hash to hex string
 */
static void hashToHex(const unsigned char* hash, int len, char* hex) {
    for (int i = 0; i < len; i++) {
        snprintf(hex + (i * 2), 3, "%02x", hash[i]);
    }
    hex[len * 2] = '\0';
}

/*
 * sha256 $string
 * sha256 -file $path
 *
 * Returns lowercase hex-encoded SHA256 hash (64 characters)
 */
static int Sha256Cmd(ClientData clientData, Tcl_Interp *interp,
                     int objc, Tcl_Obj *const objv[]) {
    
    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "string | -file path");
        return TCL_ERROR;
    }
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    
    // Check for -file option
    if (objc == 3) {
        const char* opt = Tcl_GetString(objv[1]);
        if (strcmp(opt, "-file") != 0) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("unknown option, expected -file", -1));
            return TCL_ERROR;
        }
        
        const char* path = Tcl_GetString(objv[2]);
        
        // Open file
        FILE* fp = fopen(path, "rb");
        if (!fp) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("cannot open file: %s", path));
            return TCL_ERROR;
        }
        
        // Use EVP interface for streaming hash
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            fclose(fp);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("failed to create hash context", -1));
            return TCL_ERROR;
        }
        
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        
        // Read and hash file in chunks
        unsigned char buffer[8192];
        size_t bytesRead;
        
        while ((bytesRead = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
            EVP_DigestUpdate(ctx, buffer, bytesRead);
        }
        
        fclose(fp);
        
        unsigned int hashLen;
        EVP_DigestFinal_ex(ctx, hash, &hashLen);
        EVP_MD_CTX_free(ctx);
        
        hashToHex(hash, SHA256_DIGEST_LENGTH, hex);
        
    } else {
        // Hash string directly
        Tcl_Size len;
        const unsigned char* data = (const unsigned char*)Tcl_GetStringFromObj(objv[1], &len);
        
        SHA256(data, len, hash);
        hashToHex(hash, SHA256_DIGEST_LENGTH, hex);
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(hex, SHA256_DIGEST_LENGTH * 2));
    return TCL_OK;
}

/*
 * Register commands with interpreter
 * Call from add_tcl_commands() in TclServer.cpp:
 *   TclSha256_RegisterCommands(interp);
 */
extern "C" {
    int TclSha256_RegisterCommands(Tcl_Interp *interp) {
        Tcl_CreateObjCommand(interp, "sha256", Sha256Cmd, NULL, NULL);
        return TCL_OK;
    }
}
