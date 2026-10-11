#include "controller_bindings.h"
#include "shortcuts.h"
#include <stdio.h>

const HostKey binding_default_keys[BINDING_COUNT] = {
    'z', 'x', HOST_KEY_SPACE, HOST_KEY_RETURN, HOST_KEY_UP, HOST_KEY_DOWN,
    HOST_KEY_LEFT, HOST_KEY_RIGHT, HOST_KEY_F5, HOST_KEY_F8
};
const HostButton binding_default_buttons[BINDING_COUNT] = {
    HOST_CONTROLLER_BUTTON_A, HOST_CONTROLLER_BUTTON_B, HOST_CONTROLLER_BUTTON_BACK,
    HOST_CONTROLLER_BUTTON_START, HOST_CONTROLLER_BUTTON_DPAD_UP,
    HOST_CONTROLLER_BUTTON_DPAD_DOWN, HOST_CONTROLLER_BUTTON_DPAD_LEFT,
    HOST_CONTROLLER_BUTTON_DPAD_RIGHT, HOST_CONTROLLER_BUTTON_X, HOST_CONTROLLER_BUTTON_Y
};
static const char *actions[BINDING_COUNT] = {
    "Button A", "Button B", "Select", "Start", "D-pad Up", "D-pad Down",
    "D-pad Left", "D-pad Right", "Quick save", "Quick load"
};
static const char *compact_actions[BINDING_COUNT] = {
    "A", "B", "Select", "Start", "Up", "Down", "Left", "Right", "Save", "Load"
};
static const char *pad_names[] = {
    "A", "B", "X", "Y", "Back / Select", "Guide", "Start", "Left stick",
    "Right stick", "Left shoulder", "Right shoulder", "D-pad Up", "D-pad Down",
    "D-pad Left", "D-pad Right"
};
enum { HIT_NONE = -1, HIT_KEYS = -2, HIT_PAD = -3, HIT_CLOSE = -4,
       HIT_RESET = -5, HIT_DONE = -6, HIT_CANCEL = -7, HIT_UP = -8, HIT_DOWN = -9 };
static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }
static bool contains(MenuRect r, int x, int y) {
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x+r.w && y < r.y+r.h;
}
static int used(const int32_t *values, int32_t value, int except) {
    for (int i = 0; i < BINDING_COUNT; ++i)
        if (i != except && values[i] == value) return i;
    return -1;
}
void binding_editor_open(BindingEditor *b, HostKey *keys, HostButton *buttons) {
    *b = (BindingEditor){.keys = keys, .buttons = buttons, .keyboard = true, .hover = HIT_NONE};
}
void binding_editor_resize(BindingEditor *b, int width, int height) {
    b->width = width; b->height = height;
    int w = minimum(480, maximum(80, width-16));
    int h = minimum(460, maximum(76, height-MENU_BAR_HEIGHT-STATUS_BAR_HEIGHT-16));
    int y = MENU_BAR_HEIGHT+(height-MENU_BAR_HEIGHT-STATUS_BAR_HEIGHT-h)/2;
    MenuRect r = {(width-w)/2, maximum(MENU_BAR_HEIGHT,y), w, h}; b->bounds = r;
    bool small = h < 260, tiny = h < 140;
    int title = tiny ? 16 : small ? 20 : 28, intro = h >= 340 ? 16 : 0;
    int tabs_y = r.y+title+intro;
    int margin = w < 200 ? 6 : 12;
    b->close = (MenuRect){r.x+r.w-24, r.y+3, 20, title-4};
    int tab_w = (w-2*margin-4)/2;
    int tab_h = tiny ? 18 : 24;
    b->tabs[0] = (MenuRect){r.x+margin,tabs_y,tab_w,tab_h};
    b->tabs[1] = (MenuRect){r.x+margin+tab_w+4,tabs_y,tab_w,tab_h};
    int footer = tiny ? 18 : small ? 28 : 48;
    int head = h < 180 ? 0 : 20;
    b->body = (MenuRect){r.x+margin,tabs_y+tab_h+head,w-2*margin,maximum(16,r.y+h-footer-tabs_y-tab_h-head)};
    int cell_h = tiny ? 16 : h >= 400 ? 28 : 20;
    b->rows = maximum(1,b->body.h/cell_h);
    b->columns = 1; b->choice_count = BINDING_COUNT;
    if (b->listening) {
        if (b->keyboard) {
            const char *keys = "1234567890abcdefghijklmnopqrstuvwxyz-=[]\\;',./`";
            b->choice_count = 0;
            for (; *keys; ++keys) b->choices[b->choice_count++] = (unsigned char)*keys;
            const HostKey extra[] = {HOST_KEY_SPACE,HOST_KEY_RETURN,HOST_KEY_UP,HOST_KEY_DOWN,HOST_KEY_LEFT,HOST_KEY_RIGHT};
            for (unsigned i=0;i<sizeof(extra)/sizeof(*extra);++i) b->choices[b->choice_count++] = extra[i];
            if (b->selected >= 8) {
                b->choices[b->choice_count++] = HOST_KEY_F5;
                b->choices[b->choice_count++] = HOST_KEY_F8;
            }
            b->columns = maximum(1,minimum(10,(b->body.w-14)/40));
        } else {
            b->choice_count = HOST_CONTROLLER_BUTTON_DPAD_RIGHT+1;
            for (int i=0;i<b->choice_count;++i) b->choices[i] = i;
            b->columns = b->body.w >= 300 ? 2 : 1;
        }
    }
    int total_rows = (b->choice_count+b->columns-1)/b->columns;
    b->scroll = maximum(0,minimum(b->scroll,maximum(0,total_rows-b->rows)));
    int gutter = total_rows > b->rows ? 14 : 0;
    int cell_w = (b->body.w-gutter)/b->columns;
    memset(b->cells,0,sizeof(b->cells));
    for (int i=0;i<b->choice_count;++i) {
        int row = i/b->columns-b->scroll;
        if (row >= 0 && row < b->rows)
            b->cells[i] = (MenuRect){b->body.x+(i%b->columns)*cell_w,b->body.y+row*cell_h,cell_w-4,cell_h-3};
    }
    int arrow_h = minimum(16,b->body.h/2);
    b->up = gutter ? (MenuRect){b->body.x+b->body.w-12,b->body.y,12,arrow_h} : (MenuRect){0};
    b->down = gutter ? (MenuRect){b->body.x+b->body.w-12,b->body.y+b->body.h-arrow_h,12,arrow_h} : (MenuRect){0};
    int footer_y = r.y+r.h-(tiny ? 18 : 26), footer_h = tiny ? 16 : 22;
    b->reset = (MenuRect){r.x+margin,footer_y,minimum(144,w-2*margin-56),footer_h};
    b->cancel = b->reset;
    b->done = (MenuRect){r.x+r.w-margin-48,footer_y,48,footer_h};
}
static int hit(const BindingEditor *b, int x, int y) {
    if (contains(b->close,x,y)) return HIT_CLOSE;
    if (contains(b->tabs[0],x,y)) return HIT_KEYS;
    if (contains(b->tabs[1],x,y)) return HIT_PAD;
    if (contains(b->done,x,y)) return HIT_DONE;
    if (contains(b->reset,x,y)) return b->listening ? HIT_CANCEL : HIT_RESET;
    if (contains(b->up,x,y)) return HIT_UP;
    if (contains(b->down,x,y)) return HIT_DOWN;
    for (int i=0;i<b->choice_count;++i) if (contains(b->cells[i],x,y)) return i;
    return HIT_NONE;
}
static void cancel(BindingEditor *b) {
    b->listening = false; b->scroll = maximum(0,b->selected-b->rows+1);
    b->message[0] = 0; b->hover = HIT_NONE;
    binding_editor_resize(b,b->width,b->height);
}
static BindingResult assign(BindingEditor *b, int32_t value, int modifiers) {
    bool allowed = b->selected >= 8 ? shortcut_state_key_allowed(value) : shortcut_binding_key_allowed(value);
    if (b->keyboard && (modifiers || !allowed)) {
        snprintf(b->message,sizeof(b->message),"%s",modifiers ? "Use a key without Ctrl, Alt or Shift." : "That key is reserved for shortcuts.");
        return BINDING_NONE;
    }
    if (!b->keyboard && (value < 0 || value > HOST_CONTROLLER_BUTTON_DPAD_RIGHT)) return BINDING_NONE;
    int32_t *values = b->keyboard ? b->keys : b->buttons;
    int other = used(values,value,b->selected);
    if (b->keyboard && other >= 0 && other < 8 && !shortcut_binding_key_allowed(values[b->selected])) {
        snprintf(b->message,sizeof(b->message),"Used by %s. Rebind that action first.",actions[other]);
        return BINDING_NONE;
    }
    if (other >= 0) values[other] = values[b->selected];
    values[b->selected] = value;
    cancel(b);
    snprintf(b->message,sizeof(b->message),other >= 0 ? "Swapped with %s. Saved." : "%s binding saved.",other >= 0 ? actions[other] : actions[b->selected]);
    return BINDING_CHANGED;
}
static void start(BindingEditor *b, int selected) {
    b->selected = selected; b->message[0] = 0;
    b->listening = true; b->scroll = 0; b->hover = HIT_NONE;
    binding_editor_resize(b,b->width,b->height);
}
BindingResult binding_editor_event(BindingEditor *b, const HostEvent *e, int controller) {
    if (e->type == HOST_KEYDOWN && !e->key.repeat) {
        HostKey key = e->key.keysym.sym;
        if (key == HOST_KEY_ESCAPE) {
            if (b->listening) cancel(b); else return BINDING_CLOSED;
        } else if (b->listening && b->keyboard) return assign(b,key,e->key.keysym.mod);
        else if (!b->listening) {
            if (key == HOST_KEY_LEFT || key == HOST_KEY_RIGHT || key == 9) {
                b->keyboard = !b->keyboard; b->scroll = 0; b->message[0] = 0;
            } else if (key == HOST_KEY_UP || key == HOST_KEY_DOWN) {
                b->selected = (b->selected+(key == HOST_KEY_UP ? BINDING_COUNT-1 : 1))%BINDING_COUNT;
                if (b->selected < b->scroll) b->scroll = b->selected;
                if (b->selected >= b->scroll+b->rows) b->scroll = b->selected-b->rows+1;
            } else if (key == HOST_KEY_RETURN) start(b,b->selected);
        }
    } else if (b->listening && !b->keyboard && e->type == HOST_CONTROLLERBUTTONDOWN &&
               controller >= 0 && e->cbutton.which == controller) return assign(b,e->cbutton.button,0);
    else if (e->type == HOST_MOUSEWHEEL) {
        if (e->wheel.y) {
            float delta = e->wheel.direction == HOST_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
            b->scroll += delta > 0 ? -1 : 1;
            b->hover = HIT_NONE;
        }
    } else if (e->window_mouse.valid && (e->type == HOST_MOUSEMOTION ||
               (e->type == HOST_MOUSEBUTTONDOWN && e->button.button == HOST_BUTTON_LEFT))) {
        int target = hit(b,e->window_mouse.x,e->window_mouse.y); b->hover = target;
        if (e->type == HOST_MOUSEBUTTONDOWN) {
            if (target == HIT_CLOSE || target == HIT_DONE) return BINDING_CLOSED;
            if (target == HIT_CANCEL) cancel(b);
            else if (target == HIT_KEYS || target == HIT_PAD) {
                b->keyboard = target == HIT_KEYS; cancel(b); b->scroll = 0;
            } else if (target == HIT_RESET) {
                if (b->keyboard) memcpy(b->keys,binding_default_keys,sizeof(binding_default_keys));
                else memcpy(b->buttons,binding_default_buttons,sizeof(binding_default_buttons));
                snprintf(b->message,sizeof(b->message),"%s defaults saved.",b->keyboard ? "Keyboard" : "Gamepad");
                return BINDING_CHANGED;
            } else if (target == HIT_UP || target == HIT_DOWN) { b->scroll += target == HIT_UP ? -1 : 1; b->hover = HIT_NONE; }
            else if (target >= 0) {
                if (b->listening) return assign(b,b->choices[target],0);
                start(b,target);
            }
        }
    }
    binding_editor_resize(b,b->width,b->height);
    return BINDING_NONE;
}
static void label(const MenuPainter *p, const char *text, MenuRect r, uint32_t color, bool centered) {
    if (r.w < 8 || r.h < 8) return;
    char line[96]; int cells = minimum((int)sizeof(line)-1,r.w/8);
    snprintf(line,sizeof(line),"%s",text);
    if ((int)strlen(line) > cells) { line[cells] = 0; if (cells > 1) line[cells-1] = '~'; }
    int x = centered ? r.x+(r.w-(int)strlen(line)*8)/2 : r.x;
    p->text(p->context,line,x,r.y+(r.h-8)/2,color);
}
static void button(const MenuPainter *p, MenuRect r, const char *text, bool hot, bool active) {
    p->fill(p->context,r,active ? 0x2767B1 : hot ? 0xA6BCD3 : 0xBCC7D2);
    p->fill(p->context,(MenuRect){r.x+1,r.y+1,r.w-2,r.h-2},active ? 0x2767B1 : hot ? 0xE6EFF8 : 0xFFFFFF);
    int padding = r.w < 24 ? 0 : 4;
    label(p,text,(MenuRect){r.x+padding,r.y,r.w-2*padding,r.h},active ? 0xFFFFFF : 0x24364A,true);
}
void binding_editor_draw(const BindingEditor *b, const MenuPainter *p,
                         const char *(*key_name)(HostKey), bool connected) {
    MenuRect r = b->bounds;
    p->fill(p->context,(MenuRect){r.x+3,r.y+3,r.w,r.h},0x27313C);
    p->fill(p->context,r,0xBDC8D3);
    p->fill(p->context,(MenuRect){r.x+1,r.y+1,r.w-2,r.h-2},0xF2F5F8);
    label(p,r.w < 248 ? "Controls" : "Controller bindings",(MenuRect){r.x+12,r.y+2,r.w-40,b->tabs[0].y-r.y-(r.h>=340 ? 16 : 0)-2},0x182C43,false);
    button(p,b->close,"X",b->hover == HIT_CLOSE,false);
    if (r.h >= 340) label(p,"Choose an input to change its binding.",(MenuRect){r.x+12,r.y+28,r.w-24,16},0x5B6C7F,false);
    button(p,b->tabs[0],r.w < 200 ? "Keys" : "Keyboard",b->hover == HIT_KEYS,b->keyboard);
    button(p,b->tabs[1],r.w < 200 ? "Pad" : "Gamepad",b->hover == HIT_PAD,!b->keyboard);
    int header_y = b->tabs[0].y+b->tabs[0].h;
    int split = minimum(r.w < 248 ? 64 : 120,(b->body.w-4)/2);
    if (b->listening) {
        char title[96];
        snprintf(title,sizeof(title),"%s: choose or press a %s",actions[b->selected],b->keyboard ? "key" : "button");
        label(p,title,(MenuRect){b->body.x,header_y,b->body.w,b->body.y-header_y},0x5B6C7F,false);
    } else {
        label(p,"Action",(MenuRect){b->body.x+4,header_y,split-4,b->body.y-header_y},0x5B6C7F,false);
        label(p,b->keyboard ? "Key" : "Gamepad button",(MenuRect){b->body.x+split,header_y,b->body.w-split-4,b->body.y-header_y},0x5B6C7F,true);
    }
    for (int i=0;i<b->choice_count;++i) {
        MenuRect cell = b->cells[i]; if (!cell.h) continue;
        if (b->listening) {
            int32_t value = b->choices[i];
            const int32_t *values = b->keyboard ? b->keys : b->buttons;
            const char *name = b->keyboard ? key_name(value) : pad_names[value];
            if (b->keyboard && value == HOST_KEY_SPACE) name = "Spc";
            if (b->keyboard && value == HOST_KEY_RETURN) name = "Ent";
            if (b->keyboard && value == HOST_KEY_LEFT) name = "<";
            if (b->keyboard && value == HOST_KEY_RIGHT) name = ">";
            if (b->keyboard && value == HOST_KEY_DOWN) name = "Down";
            button(p,cell,name,b->hover == i,values[b->selected] == value);
            if (used(values,value,b->selected) >= 0)
                p->fill(p->context,(MenuRect){cell.x+3,cell.y+3,2,cell.h-6},0xD99733);
        } else {
            p->fill(p->context,cell,0xFFFFFF);
            int split = r.w < 248 ? 64 : 120;
            split = minimum(split,cell.w/2);
            label(p,r.w < 248 ? compact_actions[i] : actions[i],(MenuRect){cell.x+4,cell.y,split-4,cell.h},0x34485F,false);
            MenuRect value = {cell.x+split,cell.y,cell.w-split,cell.h};
            const char *name = b->keyboard ? key_name(b->keys[i]) : pad_names[b->buttons[i]];
            button(p,value,name,b->hover == i,b->selected == i);
            if (i == 8) p->fill(p->context,(MenuRect){cell.x,cell.y,cell.w,1},0xBCC7D2);
        }
    }
    if (b->up.h) { button(p,b->up,"^",b->hover == HIT_UP,false); button(p,b->down,"v",b->hover == HIT_DOWN,false); }
    if (r.h >= 260) {
        const char *message = b->message;
        char hint[96];
        if (!*message && b->listening && b->hover >= 0 && b->hover < b->choice_count) {
            int32_t value = b->choices[b->hover];
            const int32_t *values = b->keyboard ? b->keys : b->buttons;
            const char *name = b->keyboard ? key_name(value) : pad_names[value];
            int other = used(values,value,b->selected);
            snprintf(hint,sizeof(hint),other >= 0 ? "%s: swaps with %s." : "%s: assign to %s.",name,other >= 0 ? actions[other] : actions[b->selected]);
            message = hint;
        }
        if (!*message) message = b->listening ? "Used bindings swap when reassigned." :
            !b->keyboard && !connected ? "No gamepad connected. Use the mouse picker." : "Changes save automatically.";
        label(p,message,(MenuRect){b->body.x,r.y+r.h-46,b->body.w,16},*b->message && b->listening ? 0xA43F31 : 0x5B6C7F,false);
    }
    button(p,b->reset,b->listening ? "Cancel" : r.w < 248 ? "Reset" : "Reset defaults",b->hover == (b->listening ? HIT_CANCEL : HIT_RESET),false);
    button(p,b->done,"Done",b->hover == HIT_DONE,false);
}
