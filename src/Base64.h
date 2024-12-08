#ifndef BASE64_H
#define BASE64_H

#ifdef __cplusplus
extern "C" {
#endif

  int base64encode(const void* data_buf, int dataLength,
		   char* result, int resultSize);
  int base64decode (char *in, unsigned int inLen,
		    unsigned char *out, unsigned int *outLen);
  int base64size(int len);

#ifdef __cplusplus
}
#endif
  
#endif
