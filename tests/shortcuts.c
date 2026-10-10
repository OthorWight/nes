#include "shortcuts.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    MenuCommand command;
    for (unsigned i = 0; i < app_shortcut_count; ++i) {
        const AppShortcut *s = &app_shortcuts[i];
        for (unsigned j = i + 1; j < app_shortcut_count; ++j) {
            assert(s->key != app_shortcuts[j].key || s->modifiers != app_shortcuts[j].modifiers);
            assert(s->command != app_shortcuts[j].command);
        }
        if (s->key == HOST_KEY_F5) assert(s->command == MENU_SAVE || s->command == MENU_SAVE_AS);
        if (s->key == HOST_KEY_F8) assert(s->command == MENU_LOAD || s->command == MENU_LOAD_FROM);
        HostEvent e = {.type=HOST_KEYDOWN, .key.keysym={s->key,s->modifiers}};
        assert(shortcut_match(&e,&command) && command == s->command);
        for (int modifiers = 0; modifiers < 8; ++modifiers) {
            e.key.keysym.mod = modifiers;
            if (shortcut_match(&e,&command)) {
                bool registered = false;
                for (unsigned j = 0; j < app_shortcut_count; ++j)
                    if (app_shortcuts[j].key == e.key.keysym.sym &&
                        app_shortcuts[j].modifiers == modifiers && app_shortcuts[j].command == command) registered = true;
                assert(registered);
            } else assert(command == MENU_NONE);
        }
        e.type=HOST_KEYUP;
        assert(!shortcut_match(&e,&command));
    }
    for (HostKey key=HOST_KEY_F1; key<=HOST_KEY_F12; ++key) assert(!shortcut_binding_key_allowed(key));
    assert(!shortcut_binding_key_allowed(HOST_KEY_ESCAPE));
    assert(shortcut_binding_key_allowed('f'));
    assert(shortcut_binding_key_allowed('z'));
    assert(shortcut_binding_key_allowed(HOST_KEY_UP));
    HostEvent extra={.type=HOST_KEYDOWN,.key.keysym={HOST_KEY_F10,HOST_MOD_CTRL|HOST_MOD_SHIFT}};
    assert(!shortcut_match(&extra,&command));
    /* Workspace and browser chords must not collide with application actions. */
    for (HostKey key='1'; key<='6'; ++key) {
        extra.key.keysym.sym=key;extra.key.keysym.mod=HOST_MOD_CTRL;
        assert(!shortcut_match(&extra,&command));
    }
    extra.key.keysym.sym='l';assert(!shortcut_match(&extra,&command));
    extra.key.keysym.sym=HOST_KEY_UP;extra.key.keysym.mod=HOST_MOD_ALT;
    assert(!shortcut_match(&extra,&command));
    puts("Shortcut chords/actions are unique, F5/F8 are state-only, and modifiers match exactly");
}
