
/************************************************************************
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
 ************************************************************************/

/************************************************************************
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
 ************************************************************************/

// how big a buffer is needed for the b64 encoded data?
int base64size(int len)
{
  if (!len)
    return 0;
  else
    return ((4 * len / 3) + 3) & ~3;
}

int base64encode(const void* data_buf, int dataLength, char* result,
		 int resultSize)
{
   const char base64chars[] =
     "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   const unsigned char *data = (const unsigned char *)data_buf;
   int resultIndex = 0;
   int x;
   unsigned int n = 0;
   int padCount = dataLength % 3;
   unsigned char n0, n1, n2, n3;

   /* increment over the length of the string, three characters at a time */
   for (x = 0; x < dataLength; x += 3) 
   {
      /* these three 8-bit (ASCII) characters become one 24-bit number */
      n = data[x] << 16;
 
      if((x+1) < dataLength)
         n += data[x+1] << 8;
 
      if((x+2) < dataLength)
         n += data[x+2];
 
      /* this 24-bit number gets separated into four 6-bit numbers */
      n0 = (unsigned char)(n >> 18) & 63;
      n1 = (unsigned char)(n >> 12) & 63;
      n2 = (unsigned char)(n >> 6) & 63;
      n3 = (unsigned char)n & 63;
 
      /*
       * if we have one byte available, then its encoding is spread
       * out over two characters
       */
      if(resultIndex >= resultSize) return 0;   /* indicate failure: buffer too small */
      result[resultIndex++] = base64chars[n0];
      if(resultIndex >= resultSize) return 0;   /* indicate failure: buffer too small */
      result[resultIndex++] = base64chars[n1];
 
      /*
       * if we have only two bytes available, then their encoding is
       * spread out over three chars
       */
      if((x+1) < dataLength)
      {
         if(resultIndex >= resultSize) return 0;   /* indicate failure: buffer too small */
         result[resultIndex++] = base64chars[n2];
      }
 
      /*
       * if we have all three bytes available, then their encoding is spread
       * out over four characters
       */
      if((x+2) < dataLength)
      {
         if(resultIndex >= resultSize) return 0;   /* indicate failure: buffer too small */
         result[resultIndex++] = base64chars[n3];
      }
   }  
 
   /*
    * create and add padding that is required if we did not have a multiple of 3
    * number of characters available
    */
   if (padCount > 0) 
   { 
      for (; padCount < 3; padCount++) 
      { 
         if(resultIndex >= resultSize) return 0;   /* indicate failure: buffer too small */
         result[resultIndex++] = '=';
      } 
   }
   if(resultIndex >= resultSize) return 0;   /* indicate failure: buffer too small */
   result[resultIndex] = 0;
   return 1;   /* indicate success */
}

#define B64_WHITESPACE 64
#define B64_EQUALS     65
#define B64_INVALID    66
 
static const unsigned char d[] = {
    66,66,66,66,66,66,66,66,66,64,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,
    66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,62,66,66,66,63,52,53,
    54,55,56,57,58,59,60,61,66,66,66,65,66,66,66, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,66,66,66,66,66,66,26,27,28,
    29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,66,66,
    66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,
    66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,
    66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,
    66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,
    66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,66,
    66,66,66,66,66,66
};
 
int base64decode (char *in, unsigned int inLen, unsigned char *out, unsigned int *outLen) { 
  char *end = in + inLen;
  int buf = 1;
  unsigned int len = 0;
  
  while (in < end) {
    unsigned char c = d[(int)(*in++)];
    
    switch (c) {
    case B64_WHITESPACE: continue;   /* skip whitespace */
    case B64_INVALID:    return 1;   /* invalid input, return error */
    case B64_EQUALS:                 /* pad character, end of data */
      in = end;
      continue;
    default:
      buf = buf << 6 | c;
      
      /* If the buffer is full, split it into bytes */
      if (buf & 0x1000000) {
	if ((len += 3) > *outLen) return 1; /* buffer overflow */
	*out++ = buf >> 16;
	*out++ = buf >> 8;
	*out++ = buf;
	buf = 1;
      }   
        }
  }
  
  if (buf & 0x40000) {
    if ((len += 2) > *outLen) return 1; /* buffer overflow */
    *out++ = buf >> 10;
    *out++ = buf >> 2;
  }
  else if (buf & 0x1000) {
    if (++len > *outLen) return 1; /* buffer overflow */
    *out++ = buf >> 4;
    }
  
  *outLen = len; /* modify to reflect the actual output size */
  return 0;
}
