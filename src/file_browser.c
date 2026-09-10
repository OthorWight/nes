#include "file_browser.h"
#include "nes_dirent.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif
#ifdef _WIN32
#include <direct.h>
#define browser_getcwd _getcwd
#else
#include <unistd.h>
#define browser_getcwd getcwd
#endif

static bool join(char *out, size_t size, const char *path, const char *name) {
    return snprintf(out, size, "%s%s%s", path, *path && path[strlen(path)-1] != '/' ? "/" : "", name) < (int)size;
}
static int compare(const void *left, const void *right) {
    const BrowserEntry *a = left, *b = right;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    return strcmp(a->name, b->name);
}
static bool extension_matches(const char *name, const char *extension) {
    size_t n = strlen(name), e = strlen(extension);
    if (n < e) return false;
    for (size_t i = 0; i < e; ++i)
        if (tolower((unsigned char)name[n-e+i]) != tolower((unsigned char)extension[i])) return false;
    return true;
}
bool file_browser_scan(FileBrowser *b, const char *path) {
    char full[BROWSER_PATH];
    if (strlen(path) >= sizeof(full)) return false;
    snprintf(full, sizeof(full), "%s", path);
    for (char *p = full; *p; ++p) if (*p == '\\') *p = '/';
    size_t len = strlen(full);
    while (len > 1 && full[len-1] == '/' && !(len == 3 && full[1] == ':')) full[--len] = 0;
#ifdef _WIN32
    if (!*full) {
        b->count = 0;
        for (char drive = 'A'; drive <= 'Z'; ++drive) {
            char root[] = "A:/"; root[0] = drive;
            struct stat st;
            if (!stat(root, &st)) {
                BrowserEntry *e = &b->entries[b->count++];
                snprintf(e->name, sizeof(e->name), "%s", root); e->directory = true;
            }
        }
        b->path[0] = b->location[0] = b->error[0] = 0;
        b->selected = b->count ? 0 : -1; b->scroll = 0; b->last_click = -1;
        return true;
    }
#endif
    DIR *dir = opendir(full);
    if (!dir) { snprintf(b->error, sizeof(b->error), "Cannot read folder. Check its path and permissions."); return false; }
    b->count = 0; b->error[0] = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char candidate[BROWSER_PATH]; struct stat st;
        if (!join(candidate, sizeof(candidate), full, entry->d_name) || stat(candidate, &st)) continue;
        bool directory = S_ISDIR(st.st_mode);
        if (!directory && !extension_matches(entry->d_name, b->extension)) continue;
        if (b->count == BROWSER_FILES) { snprintf(b->error, sizeof(b->error), "Showing first %d entries. Use Location to open another path.", BROWSER_FILES); break; }
        BrowserEntry *e = &b->entries[b->count++];
        if (strlen(entry->d_name) >= sizeof(e->name)) { --b->count; continue; }
        memcpy(e->name, entry->d_name, strlen(entry->d_name) + 1); e->directory = directory;
    }
    closedir(dir);
    qsort(b->entries, (size_t)b->count, sizeof(b->entries[0]), compare);
    snprintf(b->path, sizeof(b->path), "%s", full);
    snprintf(b->location, sizeof(b->location), "%s", full);
    b->selected = b->count ? 0 : -1; b->scroll = 0; b->last_click = -1;
    return true;
}
bool file_browser_open(FileBrowser *b, const char *path, const char *title, const char *extension, bool save_new) {
    char initial[BROWSER_PATH];
    if (path && *path) snprintf(initial, sizeof(initial), "%s", path);
    else if (!browser_getcwd(initial, sizeof(initial))) snprintf(initial, sizeof(initial), ".");
    b->active = true; b->editing = false; b->title = title; b->extension = extension; b->save_new = save_new;
    b->count = 0; b->selected = b->last_click = -1; b->scroll = 0;
    return file_browser_scan(b, initial);
}
void file_browser_resize(FileBrowser *b, int width, int height) {
    int w = width - 16, h = height - MENU_BAR_HEIGHT - STATUS_BAR_HEIGHT - 16;
    if (w > 640) w = 640;
    if (h > 400) h = 400;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    b->bounds = (MenuRect){(width-w)/2, MENU_BAR_HEIGHT + 8, w, h};
    b->rows = (h - 100) / 18;
    if (b->rows < 1) b->rows = 1;
}
static void parent(FileBrowser *b) {
    char path[BROWSER_PATH]; snprintf(path, sizeof(path), "%s", b->path);
#ifdef _WIN32
    if (strlen(path) == 3 && path[1] == ':') { file_browser_scan(b, ""); return; }
#endif
    char *last = strrchr(path, '/');
    if (last) { if (last == path || (last == path + 2 && path[1] == ':')) last[1] = 0; else *last = 0; }
    file_browser_scan(b, path);
}
static bool choose(FileBrowser *b, char output[BROWSER_PATH]) {
    if (b->selected < 0 || b->selected >= b->count) return false;
    char path[BROWSER_PATH];
    if (!join(path, sizeof(path), b->path, b->entries[b->selected].name)) return false;
    if (b->entries[b->selected].directory) { file_browser_scan(b, path); return false; }
    snprintf(output, BROWSER_PATH, "%s", path); b->active = false; return true;
}
static void visible(FileBrowser *b) {
    if (b->selected < b->scroll) b->scroll = b->selected;
    if (b->selected >= b->scroll + b->rows) b->scroll = b->selected - b->rows + 1;
    if (b->scroll < 0) b->scroll = 0;
}
bool file_browser_event(FileBrowser *b, const HostEvent *e, double now, char output[BROWSER_PATH]) {
    output[0] = 0;
    if (e->type == HOST_TEXTINPUT && b->editing && e->character >= 32 && e->character < 127) {
        if (b->replace_text) { b->location[0] = 0; b->replace_text = false; }
        size_t n = strlen(b->location);
        if (n + 1 < sizeof(b->location)) { b->location[n] = (char)e->character; b->location[n+1] = 0; }
    } else if (e->type == HOST_KEYDOWN) {
        HostKey key = e->key.keysym.sym;
        if (key == HOST_KEY_ESCAPE) { if (b->editing) b->editing = false; else b->active = false; }
        else if (key == 'l' && (e->key.keysym.mod & HOST_MOD_CTRL)) { b->editing = b->replace_text = true; }
        else if (b->editing) {
            size_t n = strlen(b->location);
            if (key == HOST_KEY_BACKSPACE) {
                if (b->replace_text) b->location[0] = 0; else if (n) b->location[n-1] = 0;
                b->replace_text = false;
            } else if (key == HOST_KEY_RETURN) {
                struct stat st;
                if (!stat(b->location, &st) && !S_ISDIR(st.st_mode) && extension_matches(b->location, b->extension)) {
                    snprintf(output, BROWSER_PATH, "%s", b->location); b->active = false; return true;
                }
                if (file_browser_scan(b, b->location)) b->editing = false;
            }
        } else if (key == HOST_KEY_BACKSPACE || (key == HOST_KEY_UP && (e->key.keysym.mod & HOST_MOD_ALT))) parent(b);
        else if (key == HOST_KEY_RETURN) return choose(b, output);
        else if (key == HOST_KEY_UP || key == HOST_KEY_DOWN || key == 0x40000000+75 || key == 0x40000000+78) {
            int step = (key == HOST_KEY_UP || key == 0x40000000+75) ? -1 : 1;
            if (key == 0x40000000+75 || key == 0x40000000+78) step *= b->rows;
            b->selected += step;
            if (b->selected >= b->count) b->selected = b->count-1;
            if (b->selected < 0 && b->count) b->selected = 0;
            visible(b);
        }
    } else if (e->type == HOST_MOUSEWHEEL) {
        int direction = e->wheel.y > 0 ? -1 : 1;
        if (e->wheel.direction == HOST_MOUSEWHEEL_FLIPPED) direction = -direction;
        b->scroll += direction * 3;
        if (b->scroll > b->count - b->rows) b->scroll = b->count - b->rows;
        if (b->scroll < 0) b->scroll = 0;
    } else if (e->type == HOST_MOUSEBUTTONDOWN && e->button.button == HOST_BUTTON_LEFT) {
        int x = e->button.x - b->bounds.x, y = e->button.y - b->bounds.y;
        if (x < 0 || x >= b->bounds.w || y < 0 || y >= b->bounds.h) return false;
        if (y >= 23 && y < 43) {
            b->editing = false;
            if (x < 48) parent(b);
            else if (x < 104) {
                const char *home = getenv("HOME");
#ifdef _WIN32
                home = getenv("USERPROFILE");
#endif
                if (home) file_browser_scan(b, home);
            } else { b->editing = b->replace_text = true; }
        } else if (y >= 50 && y < 50 + b->rows * 18) {
            b->editing = false;
            int row = b->scroll + (y - 50) / 18;
            if (row < b->count) {
                b->selected = row;
                if (b->last_click == row && now - b->last_click_time < 0.4) { b->last_click = -1; return choose(b, output); }
                b->last_click = row; b->last_click_time = now;
            }
        } else if (y >= b->bounds.h - 24) {
            if (x >= b->bounds.w - 72) b->active = false;
            else if (x >= b->bounds.w - 144) return choose(b, output);
            else if (b->save_new && x < 88) {
                char name[64]; time_t t = time(NULL); struct tm *tm = localtime(&t);
                if (tm) strftime(name, sizeof(name), "manual_%Y%m%d_%H%M%S.state", tm);
                else snprintf(name, sizeof(name), "manual_%ld.state", (long)t);
                if (join(output, BROWSER_PATH, b->path, name)) { b->active = false; return true; }
            }
        }
    }
    return false;
}
static void text(const MenuPainter *p, const char *s, int x, int y, int width, uint32_t color) {
    char line[128]; int n = width / 8;
    if (n < 1) return;
    if (n > 127) n = 127;
    snprintf(line, sizeof(line), "%.*s", n, s); p->text(p->context, line, x, y, color);
}
void file_browser_draw(const FileBrowser *b, const MenuPainter *p) {
    MenuRect r = b->bounds;
    p->fill(p->context, r, 0xD4D0C8);
    p->fill(p->context, (MenuRect){r.x, r.y, r.w, 20}, 0x0A246A);
    text(p, b->title, r.x+8, r.y+6, r.w-16, 0xFFFFFF);
    text(p, "Up   Home", r.x+8, r.y+29, 96, 0x202020);
    p->fill(p->context, (MenuRect){r.x+104,r.y+24,r.w-112,18}, b->editing ? 0xDDE8FF : 0xFFFFFF);
    const char *path = b->editing ? b->location : b->path;
    int chars = (r.w-120)/8;
    if (chars > 0 && strlen(path) > (size_t)chars) path += strlen(path) - (size_t)chars;
    text(p, path, r.x+108,r.y+29,r.w-120,0x202020);
    p->fill(p->context, (MenuRect){r.x+4,r.y+48,r.w-8,b->rows*18+2},0xFFFFFF);
    for (int row = 0; row < b->rows && row + b->scroll < b->count; ++row) {
        int i = row+b->scroll, y = r.y+50+row*18;
        bool selected = i == b->selected;
        if (selected) p->fill(p->context,(MenuRect){r.x+4,y,r.w-8,18},0x0A246A);
        char label[272]; snprintf(label,sizeof(label),"%s%s", b->entries[i].directory ? "[DIR] " : "      ",b->entries[i].name);
        text(p,label,r.x+8,y+5,r.w-16,selected ? 0xFFFFFF : 0x202020);
    }
    if (!b->count) text(p,"No matching files",r.x+8,r.y+56,r.w-16,0x777777);
    char info[96]; snprintf(info,sizeof(info),"%d entries | Ctrl+L: Location | Enter: Open",b->count);
    text(p,*b->error ? b->error : info,r.x+8,r.y+r.h-42,r.w-16,*b->error ? 0xA02020 : 0x202020);
    if (b->save_new) text(p,"Save New",r.x+8,r.y+r.h-17,80,0x202020);
    text(p,b->save_new ? "Save" : "Open",r.x+r.w-136,r.y+r.h-17,64,0x202020);
    text(p,"Cancel",r.x+r.w-64,r.y+r.h-17,56,0x202020);
}
