#ifndef HEADER_StringBuffer
#define HEADER_StringBuffer


#include <stdlib.h>
#include <string.h>

#define STRINGBUFFER_BLOCK 256


typedef struct StringBuffer_
{
	char* buffer;
	int bufferSize;
	int usedSize;
} StringBuffer;


StringBuffer* StringBuffer_new(char* data);

void StringBuffer_delete(StringBuffer* this);

void StringBuffer_makeRoom(StringBuffer* this, int neededSize);

void StringBuffer_addChar(StringBuffer* this, char ch);

void StringBuffer_add(StringBuffer* this, char* str);

void StringBuffer_prepend(StringBuffer* this, char* str);

void StringBuffer_set(StringBuffer* this, const char* str);

void StringBuffer_addAll(StringBuffer* this, int n, ...);

#ifdef __GNUC__
#define PRINTF_FUNCTION __attribute__ ((format (printf, 2, 3)))
#else
#define PRINTF_FUNCTION
#endif

void StringBuffer_printf(StringBuffer* this, char* format, ...) PRINTF_FUNCTION;

void StringBuffer_addPrintf(StringBuffer* this, char* format, ...) PRINTF_FUNCTION;

char* StringBuffer_getCopy(StringBuffer* this);

char* StringBuffer_getBuffer(StringBuffer* this);

char* StringBuffer_getRef(StringBuffer* this);

void StringBuffer_prune(StringBuffer* this);

#endif
