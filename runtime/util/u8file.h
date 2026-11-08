/*
 * fopen with a UTF-8 path. On Windows, fopen only understands the legacy code page
 * (folders with accents or other alphabets fail); here the path is converted and opened
 * with _wfopen. On other systems it is plain fopen.
 */
#ifndef PCE_U8FILE_H
#define PCE_U8FILE_H

#include <stdio.h>

FILE *u8_fopen(const char *path, const char *mode);

#endif
