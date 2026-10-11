#include "shortcuts.h"

#define DEFINE_LABEL(name, key, mod, command, label) const char shortcut_##name[] = label;
APP_SHORTCUTS(DEFINE_LABEL)
#undef DEFINE_LABEL
#define ENTRY(name, key, mod, command, label) {key, mod, command, shortcut_##name},
const AppShortcut app_shortcuts[] = { APP_SHORTCUTS(ENTRY) };
#undef ENTRY
const unsigned app_shortcut_count = sizeof(app_shortcuts) / sizeof(app_shortcuts[0]);

bool shortcut_match(const HostEvent *event, MenuCommand *command) {
    return shortcut_match_bindings(event,HOST_KEY_F5,HOST_KEY_F8,command);
}
bool shortcut_match_bindings(const HostEvent *event, HostKey save, HostKey load, MenuCommand *command) {
    *command = MENU_NONE;
    if (event->type != HOST_KEYDOWN) return false;
    for (unsigned i = 0; i < app_shortcut_count; ++i) {
        HostKey key = app_shortcuts[i].key;
        if (app_shortcuts[i].command == MENU_SAVE || app_shortcuts[i].command == MENU_SAVE_AS) key = save;
        if (app_shortcuts[i].command == MENU_LOAD || app_shortcuts[i].command == MENU_LOAD_FROM) key = load;
        if (event->key.keysym.sym == key &&
            event->key.keysym.mod == app_shortcuts[i].modifiers) {
            *command = app_shortcuts[i].command;
            return true;
        }
    }
    return false;
}
bool shortcut_state_key_allowed(HostKey key) {
    return key == HOST_KEY_F5 || key == HOST_KEY_F8 || shortcut_binding_key_allowed(key);
}
bool shortcut_binding_key_allowed(HostKey key) {
    if (key <= 0 || (key >= HOST_KEY_F1 && key <= HOST_KEY_F12) ||
        key == HOST_KEY_ESCAPE || key == HOST_KEY_BACKSPACE || key == 9 ||
        key == HOST_KEY_HOME || key == HOST_KEY_END ||
        key == HOST_KEY_PAGEUP || key == HOST_KEY_PAGEDOWN ||
        (key >= 0x40000000 + 224 && key <= 0x40000000 + 231)) return false;
    return true;
}
