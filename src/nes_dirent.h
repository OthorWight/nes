#ifndef NES_DIRENT_H
#define NES_DIRENT_H
/* MSVC does not ship POSIX directory iteration. */
#ifdef _MSC_VER
#include <stdbool.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct dirent { char d_name[MAX_PATH]; };
typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    bool first;
    struct dirent entry;
} DIR;
static DIR *opendir(const char *path) {
    char pattern[4096];
    if (snprintf(pattern, sizeof(pattern), "%s/*", path) >= (int)sizeof(pattern)) return NULL;
    DIR *dir = calloc(1, sizeof(*dir));
    if (!dir) return NULL;
    dir->handle = FindFirstFileA(pattern, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) { free(dir); return NULL; }
    dir->first = true;
    return dir;
}
static struct dirent *readdir(DIR *dir) {
    if (!dir->first && !FindNextFileA(dir->handle, &dir->data)) return NULL;
    dir->first = false;
    strcpy(dir->entry.d_name, dir->data.cFileName);
    return &dir->entry;
}
static int closedir(DIR *dir) {
    BOOL result = FindClose(dir->handle);
    free(dir);
    return result ? 0 : -1;
}
#else
#include <dirent.h>
#endif
#endif
