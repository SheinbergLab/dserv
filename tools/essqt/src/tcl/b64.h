
/*****************************************************************************
 *
 * FUNCTIONS
 *    base64encode/base64decode
 *
 * DESCRIPTION
 *    Move to/from b64 encoding
 *
 * SOURCE
 *    http://en.wikibooks.org/wiki/Algorithm_Implementation/Miscellaneous/Base64
 *
 *****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif
  
int base64encode(const void* data_buf,
		 int dataLength, char* result, int resultSize);
int base64decode (char *in, unsigned int inLen,
		  unsigned char *out, unsigned int *outLen);

#ifdef __cplusplus
}
#endif
  
