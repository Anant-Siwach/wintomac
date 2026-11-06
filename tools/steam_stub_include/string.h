/* Minimal libc declarations so tools/gen_steam.py can parse Steamworks headers for both targets
 * (x86_64-pc-windows-msvc and arm64-apple-macos) without either platform's SDK. Declarations only. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memset(void *, int, size_t); void *memcpy(void *, const void *, size_t); int memcmp(const void *, const void *, size_t);
int strcmp(const char *, const char *); size_t strlen(const char *); char *strncpy(char *, const char *, size_t);
char *strcpy(char *, const char *); int strncmp(const char *, const char *, size_t); char *strchr(const char *, int);
int stricmp(const char *, const char *); int _stricmp(const char *, const char *); int strcasecmp(const char *, const char *);
int sprintf(char *, const char *, ...); int snprintf(char *, size_t, const char *, ...); int _snprintf(char *, size_t, const char *, ...);
void *malloc(size_t); void free(void *);
#ifdef __cplusplus
}
#endif
