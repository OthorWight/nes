#include "menu_bar.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const MenuItem children[] = {
    {"Controller", NULL, MENU_CONTROLLER, MENU_CHECKED, NULL},
    {"Zapper", NULL, MENU_ZAPPER, 0, NULL}
};
static const Menu child = {"Port 2", children, 2};
static const MenuItem items[] = {
    {"Disabled", NULL, MENU_SAVE, MENU_DISABLED, NULL},
    {NULL, NULL, MENU_NONE, MENU_SEPARATOR, NULL},
    {"Open", "Ctrl+O", MENU_OPEN, 0, NULL},
    {"Port 2", NULL, MENU_NONE, 0, &child},
    {"Exit", NULL, MENU_EXIT, 0, NULL}
};
static const Menu menus[] = {{"File", items, 5}, {"View", items, 5}, {"Help", items, 5}};
static MenuBar bar;
static MenuCommand command;
static bool key(MenuKey keycode) {
    return menu_bar_event(&bar, &(MenuEvent){.type = MENU_KEY_PRESS, .key = keycode}, &command);
}
static bool mouse(MenuEventType type, int x, int y) {
    return menu_bar_event(&bar, &(MenuEvent){.type = type, .x = x, .y = y}, &command);
}
static unsigned fills, texts;
static int shortcut_right;
static void fill(void *ctx, MenuRect r, uint32_t color) {
    (void)ctx; (void)color; assert(r.w >= 0 && r.h >= 0); ++fills;
}
static void text(void *ctx, const char *s, int x, int y, uint32_t color) {
    (void)ctx; (void)y; (void)color; ++texts;
    if (!strcmp(s, "Ctrl+O")) shortcut_right = x + 48;
}
int main(void) {
    menu_bar_init(&bar, menus, 3, NULL, NULL); menu_bar_resize(&bar, 512, 400);
    assert(!key(MENU_KEY_DOWN));
    assert(key(MENU_KEY_ACTIVATE) && bar.active && !bar.depth);
    assert(key(MENU_KEY_RIGHT) && bar.top == 1);
    assert(key(MENU_KEY_DOWN) && bar.selected[0] == 2); /* Skip separator and disabled. */
    assert(key(MENU_KEY_UP) && bar.selected[0] == 4); /* Wrap. */
    assert(key(MENU_KEY_DOWN) && bar.selected[0] == 2);
    assert(key(MENU_KEY_DOWN) && bar.selected[0] == 3);
    assert(key(MENU_KEY_RIGHT) && bar.depth == 2 && bar.selected[1] == 0);
    assert(key(MENU_KEY_DOWN) && bar.selected[1] == 1);
    assert(key(MENU_KEY_ENTER) && command == MENU_ZAPPER && !bar.active);
    assert(!key(MENU_KEY_NONE)); /* Closed menu preserves application shortcuts. */
    assert(mouse(MENU_MOVE, 10, 6) && bar.hover == 0 && !bar.active);
    assert(mouse(MENU_PRESS, 10, 6) && bar.active && bar.depth == 1);
    assert(mouse(MENU_RELEASE, 10, 6));
    assert(mouse(MENU_PRESS, 20, 30) && !command && bar.active); /* Disabled. */
    assert(mouse(MENU_MOVE, 70, 6) && bar.top == 1);
    assert(mouse(MENU_PRESS, 500, 300) && !bar.active && !command);
    assert(mouse(MENU_RELEASE, 500, 300)); /* Outside dismiss cannot leak a release. */
    assert(!mouse(MENU_PRESS, 500, 300));
    assert(mouse(MENU_PRESS, 10, 6));
    assert(key(MENU_KEY_DOWN));
    assert(key(MENU_KEY_DOWN));
    assert(key(MENU_KEY_ENTER) && bar.depth == 2);
    assert(key(MENU_KEY_ESCAPE) && bar.active && bar.depth == 1);
    assert(key(MENU_KEY_ESCAPE) && !bar.active);
    assert(key(MENU_KEY_ACTIVATE)); assert(key(MENU_KEY_ENTER));
    MenuPainter painter = {NULL, fill, text};
    menu_bar_draw(&bar, &painter);
    status_bar_draw(&(StatusBar){"60.10 FPS | 100% | Mapper 66 | Audio OK | Port 2: Controller"}, &painter, 512, 400);
    assert(fills > 5 && texts > 5);
    assert(shortcut_right == bar.popup[0].x + bar.popup[0].w - 14);
    menu_bar_resize(&bar, 360, 240);
    assert(bar.popup[0].x >= 0 && bar.popup[0].x + bar.popup[0].w <= 360);
    assert(key(MENU_KEY_DOWN)); assert(key(MENU_KEY_RIGHT));
    assert(bar.depth == 2 && bar.popup[1].x >= 0 && bar.popup[1].x + bar.popup[1].w <= 360);
    assert(!menu_bar_event(&bar, &(MenuEvent){.type = MENU_BLUR}, &command) && !bar.active);
    assert(key(MENU_KEY_ACTIVATE));
    assert(menu_bar_event(&bar, &(MenuEvent){.type = MENU_KEY_PRESS, .key = MENU_KEY_ACTIVATE, .repeat = true}, &command) && bar.active);
    assert(menu_bar_event(&bar, &(MenuEvent){.type = MENU_KEY_RELEASE, .key = MENU_KEY_ACTIVATE}, &command) && bar.active);
    puts("Menu keyboard, pointer ownership, submenus, drawing and resize checks passed");
}
