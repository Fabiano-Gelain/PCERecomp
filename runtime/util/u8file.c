#include "util/u8file.h"

#ifdef _WIN32
#include <stdlib.h>
#include <wchar.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

FILE *u8_fopen(const char *path, const char *mode)
{
    wchar_t wpath[1024], wmode[16];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, 1024) ||
        !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16))
        return fopen(path, mode); /* not valid UTF-8: try it as given */
    return _wfopen(wpath, wmode);
}
#else
FILE *u8_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}
#endif
