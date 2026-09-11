#include "menu_bar.h"
#include <string.h>

static int text_width(const char *s) { return s ? (int)strlen(s) * 8 : 0; }
static bool contains(MenuRect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static unsigned flags(const MenuBar *b, const MenuItem *i) {
    return i->flags | (b->state && i->command ? b->state(b->context, i->command) : 0);
}
static bool enabled(const MenuBar *b, const MenuItem *i) {
    return !(flags(b, i) & (MENU_DISABLED | MENU_SEPARATOR));
}
static int row_height(const MenuItem *i) { return i->flags & MENU_SEPARATOR ? 7 : 18; }
static MenuRect top_rect(const MenuBar *b, int index) {
    MenuRect r = {2, 0, 0, MENU_BAR_HEIGHT - 1};
    for (int i = 0; i <= index; ++i) {
        r.x += r.w;
        r.w = text_width(b->menus[i].label) + 8;
    }
    return r;
}
static MenuRect row_rect(const MenuBar *b, int level, int index) {
    MenuRect r = b->popup[level];
    r.x += 2; r.y += 2; r.w -= 4;
    for (int i = 0; i < index; ++i) r.y += row_height(&b->path[level]->items[i]);
    r.h = row_height(&b->path[level]->items[index]);
    return r;
}
static int next(const MenuBar *b, const Menu *m, int selected, int direction) {
    for (int n = 0; n < m->count; ++n) {
        selected = (selected + direction + m->count) % m->count;
        if (enabled(b, &m->items[selected])) return selected;
    }
    return -1;
}
static void open_popup(MenuBar *b, const Menu *m, int x, int y) {
    if (b->depth >= MENU_DEPTH) return;
    int label = 0, shortcut = 0, height = 4;
    for (int i = 0; i < m->count; ++i) {
        int w = text_width(m->items[i].label), s = text_width(m->items[i].shortcut);
        if (w > label) label = w;
        if (s > shortcut) shortcut = s;
        height += row_height(&m->items[i]);
    }
    int width = label + shortcut + 48;
    if (width > b->width) width = b->width;
    if (x + width > b->width) x = b->depth ? b->popup[b->depth - 1].x - width + 2 : b->width - width;
    if (x < 0) x = 0;
    if (y + height > b->height - STATUS_BAR_HEIGHT) y = b->height - STATUS_BAR_HEIGHT - height;
    if (y < MENU_BAR_HEIGHT) y = MENU_BAR_HEIGHT;
    int d = b->depth++;
    b->path[d] = m; b->selected[d] = -1;
    b->popup[d] = (MenuRect){x, y, width, height};
}
static void open_top(MenuBar *b, int index, bool select) {
    b->active = true; b->top = index; b->depth = 0;
    open_popup(b, &b->menus[index], top_rect(b, index).x, MENU_BAR_HEIGHT);
    if (select) b->selected[0] = next(b, b->path[0], -1, 1);
}
static void activate(MenuBar *b, MenuCommand *command) {
    int d = b->depth - 1;
    if (d < 0 || b->selected[d] < 0) return;
    const MenuItem *i = &b->path[d]->items[b->selected[d]];
    if (!enabled(b, i)) return;
    if (i->submenu) {
        MenuRect row = row_rect(b, d, b->selected[d]);
        open_popup(b, i->submenu, b->popup[d].x + b->popup[d].w - 2, row.y);
        if (b->depth > d + 1) b->selected[d + 1] = next(b, i->submenu, -1, 1);
    } else if (i->command) {
        *command = i->command; menu_bar_close(b);
    }
}
void menu_bar_init(MenuBar *b, const Menu *menus, int count, MenuState state, void *context) {
    memset(b, 0, sizeof(*b));
    b->menus = menus; b->count = count; b->state = state; b->context = context; b->hover = -1;
}
void menu_bar_close(MenuBar *b) { b->active = false; b->depth = 0; b->hover = -1; }
void menu_bar_resize(MenuBar *b, int width, int height) {
    if (b->width == width && b->height == height) return;
    b->width = width; b->height = height;
    if (b->depth) open_top(b, b->top, true);
}
bool menu_bar_event(MenuBar *b, const MenuEvent *e, MenuCommand *command) {
    *command = MENU_NONE;
    if (e->type == MENU_BLUR) { menu_bar_close(b); b->swallow_release = false; return false; }
    if (e->type == MENU_KEY_PRESS || e->type == MENU_KEY_RELEASE) {
        if (e->key == MENU_KEY_ACTIVATE) {
            if (e->type == MENU_KEY_PRESS && !e->repeat) {
                if (b->active) menu_bar_close(b);
                else { b->active = true; b->top = 0; }
            }
            return true;
        }
        if (!b->active) return false;
        if (e->type == MENU_KEY_RELEASE) return true;
        if (e->key == MENU_KEY_ESCAPE) {
            if (b->depth > 1) --b->depth; else menu_bar_close(b);
        } else if (e->key == MENU_KEY_LEFT || e->key == MENU_KEY_RIGHT) {
            int dir = e->key == MENU_KEY_LEFT ? -1 : 1;
            if (dir < 0 && b->depth > 1) --b->depth;
            else if (dir > 0 && b->depth && b->selected[b->depth - 1] >= 0 &&
                     b->path[b->depth - 1]->items[b->selected[b->depth - 1]].submenu) activate(b, command);
            else {
                int top = (b->top + dir + b->count) % b->count;
                if (b->depth) open_top(b, top, true); else b->top = top;
            }
        } else if (e->key == MENU_KEY_UP || e->key == MENU_KEY_DOWN) {
            int dir = e->key == MENU_KEY_UP ? -1 : 1;
            if (!b->depth) {
                open_top(b, b->top, false);
                b->selected[0] = next(b, b->path[0], dir < 0 ? 0 : -1, dir);
            } else {
                int d = b->depth - 1;
                b->selected[d] = next(b, b->path[d], b->selected[d] < 0 && dir < 0 ? 0 : b->selected[d], dir);
            }
        } else if (e->key == MENU_KEY_ENTER) {
            if (!b->depth) open_top(b, b->top, true); else activate(b, command);
        }
        return true;
    }
    if (e->type == MENU_RELEASE && b->swallow_release) { b->swallow_release = false; return true; }
    if (e->type == MENU_WHEEL || e->type == MENU_RELEASE) return b->active || e->y < MENU_BAR_HEIGHT || e->y >= b->height - STATUS_BAR_HEIGHT;
    int top = -1;
    for (int i = 0; i < b->count; ++i) if (contains(top_rect(b, i), e->x, e->y)) top = i;
    b->hover = top;
    if (e->type == MENU_PRESS) b->swallow_release = b->active || e->y < MENU_BAR_HEIGHT || e->y >= b->height - STATUS_BAR_HEIGHT;
    if (top >= 0) {
        if (e->type == MENU_PRESS) {
            if (b->active && b->top == top && b->depth) menu_bar_close(b); else open_top(b, top, false);
        } else if (b->active && b->top != top) open_top(b, top, false);
        return true;
    }
    if (!b->active) return e->y < MENU_BAR_HEIGHT || e->y >= b->height - STATUS_BAR_HEIGHT;
    for (int d = b->depth - 1; d >= 0; --d) {
        if (!contains(b->popup[d], e->x, e->y)) continue;
        for (int j = 0; j < b->path[d]->count; ++j) {
            if (!contains(row_rect(b, d, j), e->x, e->y)) continue;
            const MenuItem *item = &b->path[d]->items[j];
            int selection = enabled(b, item) ? j : -1;
            if (b->selected[d] != selection || e->type == MENU_PRESS) {
                b->depth = d + 1; b->selected[d] = selection;
                if (selection >= 0 && (item->submenu || e->type == MENU_PRESS)) activate(b, command);
            }
            return true;
        }
        return true;
    }
    if (e->type == MENU_PRESS) menu_bar_close(b);
    return true;
}
static void clipped_text(const MenuPainter *p, const char *text, int x, int y, int width, uint32_t color) {
    char buffer[128];
    int n = width / 8;
    if (n <= 0 || !text) return;
    if (n > 127) n = 127;
    size_t len = strlen(text);
    if (len < (size_t)n) n = (int)len;
    memcpy(buffer, text, (size_t)n); buffer[n] = 0;
    p->text(p->context, buffer, x, y, color);
}
void menu_bar_draw(const MenuBar *b, const MenuPainter *p) {
    p->fill(p->context, (MenuRect){0, 0, b->width, MENU_BAR_HEIGHT}, 0xD4D0C8);
    p->fill(p->context, (MenuRect){0, MENU_BAR_HEIGHT - 1, b->width, 1}, 0x888888);
    for (int i = 0; i < b->count; ++i) {
        MenuRect r = top_rect(b, i);
        bool selected = b->active && b->top == i;
        if (selected || b->hover == i) p->fill(p->context, r, selected ? 0x0A246A : 0xE8E6E0);
        p->text(p->context, b->menus[i].label, r.x + 4, (MENU_BAR_HEIGHT - 8) / 2, selected ? 0xFFFFFF : 0x202020);
    }
    for (int d = 0; d < b->depth; ++d) {
        MenuRect r = b->popup[d];
        p->fill(p->context, r, 0x808080);
        p->fill(p->context, (MenuRect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 0xF0EEE8);
        for (int j = 0; j < b->path[d]->count; ++j) {
            const MenuItem *item = &b->path[d]->items[j];
            unsigned f = flags(b, item);
            MenuRect row = row_rect(b, d, j);
            if (f & MENU_SEPARATOR) {
                p->fill(p->context, (MenuRect){row.x + 5, row.y + 3, row.w - 10, 1}, 0xACA899);
                continue;
            }
            bool selected = b->selected[d] == j;
            uint32_t color = f & MENU_DISABLED ? 0x888888 : selected ? 0xFFFFFF : 0x202020;
            if (selected) p->fill(p->context, row, 0x0A246A);
            if (f & MENU_CHECKED) {
                for (int k = 0; k < 3; ++k) p->fill(p->context, (MenuRect){row.x + 4 + k, row.y + 8 + k, 1, 2}, color);
                for (int k = 0; k < 5; ++k) p->fill(p->context, (MenuRect){row.x + 6 + k, row.y + 10 - k, 1, 2}, color);
            }
            int shortcut = text_width(item->shortcut);
            clipped_text(p, item->label, row.x + 18, row.y + 5, row.w - 36 - (shortcut ? shortcut + 8 : 0), color);
            if (shortcut) p->text(p->context, item->shortcut, row.x + row.w - 12 - shortcut, row.y + 5, color);
            if (item->submenu) p->text(p->context, ">", row.x + row.w - 10, row.y + 5, color);
        }
    }
}
void status_bar_draw(const StatusBar *b, const MenuPainter *p, int width, int height) {
    int y = height - STATUS_BAR_HEIGHT;
    p->fill(p->context, (MenuRect){0, y, width, STATUS_BAR_HEIGHT}, 0xD4D0C8);
    p->fill(p->context, (MenuRect){0, y, width, 1}, 0x888888);
    clipped_text(p, b->text, 5, y + (STATUS_BAR_HEIGHT - 8) / 2, width - 10, 0x202020);
}
