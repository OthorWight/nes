#ifndef NES_FILE_BROWSER_H
#define NES_FILE_BROWSER_H
#include "host.h"
#include "menu_bar.h"
enum { BROWSER_PATH = 4096, BROWSER_FILES = 1024 };
typedef struct BrowserEntry { char name[256]; bool directory; } BrowserEntry;
typedef struct FileBrowser {
    bool active, editing, replace_text, save_new;
    char path[BROWSER_PATH], location[BROWSER_PATH], error[96];
    const char *title, *extension;
    BrowserEntry entries[BROWSER_FILES];
    int count, selected, scroll, rows, last_click;
    double last_click_time;
    MenuRect bounds;
} FileBrowser;
bool file_browser_open(FileBrowser *, const char *path, const char *title, const char *extension, bool save_new);
bool file_browser_scan(FileBrowser *, const char *path);
void file_browser_resize(FileBrowser *, int width, int height);
void file_browser_draw(const FileBrowser *, const MenuPainter *);
/* Mouse coordinates are UI logical pixels. A selected file is returned as a full path. */
bool file_browser_event(FileBrowser *, const HostEvent *, double now, char output[BROWSER_PATH]);
#endif
