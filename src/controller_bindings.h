#ifndef NES_CONTROLLER_BINDINGS_H
#define NES_CONTROLLER_BINDINGS_H
#include "host.h"
#include "menu_bar.h"

enum { BINDING_COUNT = 10, BINDING_CHOICES = 64 };
extern const HostKey binding_default_keys[BINDING_COUNT];
extern const HostButton binding_default_buttons[BINDING_COUNT];
typedef enum BindingResult { BINDING_NONE, BINDING_CHANGED, BINDING_CLOSED } BindingResult;
typedef struct BindingEditor {
    HostKey *keys;
    HostButton *buttons;
    bool keyboard, listening;
    int selected, hover, scroll, rows, columns, choice_count;
    int width, height;
    char message[96];
    int32_t choices[BINDING_CHOICES];
    MenuRect bounds, tabs[2], close, reset, done, cancel, body, up, down;
    MenuRect cells[BINDING_CHOICES];
} BindingEditor;

void binding_editor_open(BindingEditor *, HostKey *, HostButton *);
void binding_editor_resize(BindingEditor *, int width, int height);
/* Mouse coordinates are chrome pixels. Values change only after a valid pick. */
BindingResult binding_editor_event(BindingEditor *, const HostEvent *, int controller);
void binding_editor_draw(const BindingEditor *, const MenuPainter *,
                         const char *(*key_name)(HostKey), bool controller_connected);
#endif
