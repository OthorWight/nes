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
    const struct {HostKey key;int modifiers;MenuCommand action;} custom[] = {
        {'k',0,MENU_SAVE},{'k',HOST_MOD_SHIFT,MENU_SAVE_AS},
        {'l',0,MENU_LOAD},{'l',HOST_MOD_SHIFT,MENU_LOAD_FROM},
        {HOST_KEY_F5,0,MENU_NONE},{HOST_KEY_F8,0,MENU_NONE},
        {HOST_KEY_F5,HOST_MOD_SHIFT,MENU_NONE},{HOST_KEY_F8,HOST_MOD_SHIFT,MENU_NONE},
        {'k',HOST_MOD_CTRL,MENU_NONE},{'l',HOST_MOD_CTRL|HOST_MOD_SHIFT,MENU_NONE},
        {HOST_KEY_F6,0,MENU_STEP_FRAME},{HOST_KEY_F1,0,MENU_PAUSE}
    };
    for (unsigned i=0;i<sizeof(custom)/sizeof(*custom);++i) {
        extra=(HostEvent){.type=HOST_KEYDOWN,.key.keysym={custom[i].key,custom[i].modifiers}};
        assert(shortcut_match_bindings(&extra,'k','l',&command) == (custom[i].action != MENU_NONE));
        assert(command == custom[i].action);
    }
    extra=(HostEvent){.type=HOST_KEYDOWN,.key.keysym={HOST_KEY_F8,0}};
    assert(shortcut_match_bindings(&extra,HOST_KEY_F8,HOST_KEY_F5,&command) && command==MENU_SAVE);
    extra.key.keysym.sym=HOST_KEY_F5;extra.key.keysym.mod=HOST_MOD_SHIFT;
    assert(shortcut_match_bindings(&extra,HOST_KEY_F8,HOST_KEY_F5,&command) && command==MENU_LOAD_FROM);
    assert(shortcut_state_key_allowed(HOST_KEY_F5) && shortcut_state_key_allowed(HOST_KEY_F8));
    assert(shortcut_state_key_allowed('k') && !shortcut_state_key_allowed(HOST_KEY_F6));
    puts("Default/custom state shortcuts, retired default keys, exact modifiers and reserved keys passed");
}
