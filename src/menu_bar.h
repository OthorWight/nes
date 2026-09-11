#ifndef NES_MENU_BAR_H
#define NES_MENU_BAR_H
#include <stdbool.h>
#include <stdint.h>

enum { MENU_BAR_HEIGHT = 24, STATUS_BAR_HEIGHT = 20, MENU_DEPTH = 4 };
typedef enum MenuCommand {
    MENU_NONE, MENU_OPEN, MENU_SAVE, MENU_LOAD, MENU_EXIT, MENU_PAUSE,
    MENU_RESET, MENU_POWER, MENU_CONTROLLER, MENU_ZAPPER,
    MENU_SIZE_1, MENU_SIZE_2, MENU_SIZE_3, MENU_SIZE_4, MENU_MAXIMIZED,
    MENU_FULLSCREEN, MENU_DEBUG_PANEL, MENU_METRICS, MENU_STEP, MENU_RUN,
    MENU_BREAKPOINT, MENU_TRACE, MENU_CONTROLS, MENU_ABOUT,
    MENU_RECENT_1, MENU_RECENT_2, MENU_RECENT_3, MENU_RECENT_4,
    MENU_SAVE_AS, MENU_LOAD_FROM, MENU_MUTE, MENU_VOLUME_DOWN, MENU_VOLUME_UP,
    MENU_INHERIT, MENU_GLOBAL_DEFAULTS, MENU_BINDINGS
} MenuCommand;
enum { MENU_DISABLED = 1, MENU_CHECKED = 2, MENU_SEPARATOR = 4 };
typedef struct Menu Menu;
typedef struct MenuItem {
    const char *label, *shortcut;
    MenuCommand command;
    unsigned flags;
    const Menu *submenu;
} MenuItem;
struct Menu { const char *label; const MenuItem *items; int count; };
typedef struct MenuRect { int x, y, w, h; } MenuRect;
typedef unsigned (*MenuState)(void *context, MenuCommand command);
typedef struct MenuPainter {
    void *context;
    void (*fill)(void *, MenuRect, uint32_t);
    void (*text)(void *, const char *, int, int, uint32_t);
} MenuPainter;
typedef enum MenuKey { MENU_KEY_NONE, MENU_KEY_ACTIVATE, MENU_KEY_LEFT,
    MENU_KEY_RIGHT, MENU_KEY_UP, MENU_KEY_DOWN, MENU_KEY_ENTER, MENU_KEY_ESCAPE } MenuKey;
typedef enum MenuEventType { MENU_KEY_PRESS, MENU_KEY_RELEASE, MENU_MOVE,
    MENU_PRESS, MENU_RELEASE, MENU_WHEEL, MENU_BLUR } MenuEventType;
typedef struct MenuEvent {
    MenuEventType type;
    MenuKey key;
    int x, y;
    bool repeat;
} MenuEvent;
typedef struct MenuBar {
    const Menu *menus;
    int count, top, hover, depth, width, height;
    const Menu *path[MENU_DEPTH];
    int selected[MENU_DEPTH];
    MenuRect popup[MENU_DEPTH];
    bool active, swallow_release;
    MenuState state;
    void *context;
} MenuBar;
typedef struct StatusBar { const char *text; } StatusBar;

void menu_bar_init(MenuBar *, const Menu *, int, MenuState, void *);
void menu_bar_resize(MenuBar *, int width, int height);
void menu_bar_close(MenuBar *);
/* Returns event ownership; commands are dispatched by the caller, never drawing. */
bool menu_bar_event(MenuBar *, const MenuEvent *, MenuCommand *);
void menu_bar_draw(const MenuBar *, const MenuPainter *);
void status_bar_draw(const StatusBar *, const MenuPainter *, int width, int height);
#endif
