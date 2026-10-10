#ifndef NES_SHORTCUTS_H
#define NES_SHORTCUTS_H
#include "host.h"
#include "menu_bar.h"

/* One exact chord per application action. Shared by dispatch and menu labels. */
#define APP_SHORTCUTS(X) \
    X(OPEN, 'o', HOST_MOD_CTRL, MENU_OPEN, "Ctrl+O") \
    X(MOVIE_OPEN, 'm', HOST_MOD_CTRL, MENU_MOVIE_OPEN, "Ctrl+M") \
    X(PAUSE, HOST_KEY_F1, 0, MENU_PAUSE, "F1") \
    X(METRICS, HOST_KEY_F2, 0, MENU_METRICS, "F2") \
    X(DEBUG_PANEL, HOST_KEY_F3, 0, MENU_DEBUG_PANEL, "F3") \
    X(CAPTURE, HOST_KEY_F4, 0, MENU_CAPTURE, "F4") \
    X(TRACE, HOST_KEY_F4, HOST_MOD_CTRL, MENU_TRACE, "Ctrl+F4") \
    X(LOG, HOST_KEY_F4, HOST_MOD_SHIFT, MENU_INSTRUCTION_LOG, "Shift+F4") \
    X(SAVE, HOST_KEY_F5, 0, MENU_SAVE, "F5") \
    X(SAVE_AS, HOST_KEY_F5, HOST_MOD_SHIFT, MENU_SAVE_AS, "Shift+F5") \
    X(LOAD, HOST_KEY_F8, 0, MENU_LOAD, "F8") \
    X(LOAD_FROM, HOST_KEY_F8, HOST_MOD_SHIFT, MENU_LOAD_FROM, "Shift+F8") \
    X(STEP_FRAME, HOST_KEY_F6, 0, MENU_STEP_FRAME, "F6") \
    X(BACK_FRAME, HOST_KEY_F6, HOST_MOD_SHIFT, MENU_DEBUG_BACK_FRAME, "Shift+F6") \
    X(BREAKPOINT, HOST_KEY_F7, 0, MENU_BREAKPOINT, "F7") \
    X(DEBUGGER, HOST_KEY_F9, 0, MENU_RUN, "F9") \
    X(RUN_CURSOR, HOST_KEY_F9, HOST_MOD_CTRL, MENU_RUN_CURSOR, "Ctrl+F9") \
    X(STEP, HOST_KEY_F10, 0, MENU_STEP, "F10") \
    X(OVER, HOST_KEY_F10, HOST_MOD_SHIFT, MENU_STEP_OVER, "Shift+F10") \
    X(OUT, HOST_KEY_F10, HOST_MOD_CTRL, MENU_STEP_OUT, "Ctrl+F10") \
    X(BACK, HOST_KEY_F10, HOST_MOD_ALT, MENU_DEBUG_BACK, "Alt+F10") \
    X(FULLSCREEN, HOST_KEY_F11, 0, MENU_FULLSCREEN, "F11") \
    X(RESET, HOST_KEY_F12, 0, MENU_RESET, "F12") \
    X(COMMAND, 'g', HOST_MOD_CTRL, MENU_DEBUG_COMMAND, "Ctrl+G") \
    X(MOVIE_BACK, HOST_KEY_LEFT, HOST_MOD_CTRL, MENU_MOVIE_BACK, "Ctrl+Left") \
    X(MOVIE_STEP, HOST_KEY_RIGHT, HOST_MOD_CTRL, MENU_MOVIE_STEP, "Ctrl+Right") \
    X(MOVIE_RESTART, HOST_KEY_HOME, HOST_MOD_CTRL, MENU_MOVIE_RESTART, "Ctrl+Home") \
    X(MOVIE_STOP, HOST_KEY_END, HOST_MOD_CTRL, MENU_MOVIE_STOP, "Ctrl+End") \
    X(MOVIE_SEEK, 'j', HOST_MOD_CTRL, MENU_MOVIE_SEEK, "Ctrl+J") \
    X(DEBUG_UP, HOST_KEY_UP, HOST_MOD_CTRL, MENU_DEBUG_UP, "Ctrl+Up") \
    X(DEBUG_DOWN, HOST_KEY_DOWN, HOST_MOD_CTRL, MENU_DEBUG_DOWN, "Ctrl+Down")

#define DECLARE_LABEL(name, key, mod, command, label) extern const char shortcut_##name[];
APP_SHORTCUTS(DECLARE_LABEL)
#undef DECLARE_LABEL

typedef struct AppShortcut { HostKey key; int modifiers; MenuCommand command; const char *label; } AppShortcut;
extern const AppShortcut app_shortcuts[];
extern const unsigned app_shortcut_count;
bool shortcut_match(const HostEvent *, MenuCommand *);
bool shortcut_binding_key_allowed(HostKey);
#endif
