#include "controller_bindings.h"
#include "shortcuts.h"
#include <assert.h>
#include <stdio.h>

static BindingEditor editor;
static HostKey keys[BINDING_COUNT];
static HostButton buttons[BINDING_COUNT];
static BindingResult click(MenuRect r) {
    assert(r.w > 0 && r.h > 0);
    HostEvent e = {.type=HOST_MOUSEBUTTONDOWN,.button.button=HOST_BUTTON_LEFT,
                  .window_mouse={r.x+r.w/2,r.y+r.h/2,true}};
    return binding_editor_event(&editor,&e,7);
}
static BindingResult key(HostKey code, int modifiers) {
    HostEvent e = {.type=HOST_KEYDOWN,.key.keysym={code,modifiers}};
    return binding_editor_event(&editor,&e,7);
}
static BindingResult choose(int32_t value) {
    for (int i=0;i<editor.choice_count;++i) if (editor.choices[i] == value) {
        editor.scroll = i/editor.columns;
        binding_editor_resize(&editor,editor.width,editor.height);
        return click(editor.cells[i]);
    }
    assert(!"Missing mouse choice"); return BINDING_NONE;
}
static void open(int w, int h) {
    memcpy(keys,binding_default_keys,sizeof(keys));
    memcpy(buttons,binding_default_buttons,sizeof(buttons));
    binding_editor_open(&editor,keys,buttons); binding_editor_resize(&editor,w,h);
}
static void unique(void) {
    for (int i=0;i<BINDING_COUNT;++i) for (int j=i+1;j<BINDING_COUNT;++j) {
        assert(keys[i] != keys[j]); assert(buttons[i] != buttons[j]);
    }
    for (int i=0;i<8;++i) assert(shortcut_binding_key_allowed(keys[i]));
    assert(shortcut_state_key_allowed(keys[8]) && shortcut_state_key_allowed(keys[9]));
}
static void mouse_editing(void) {
    open(640,640);
    assert(click(editor.cells[0]) == BINDING_NONE && editor.listening);
    assert(choose('x') == BINDING_CHANGED && !editor.listening);
    assert(keys[0] == 'x' && keys[1] == 'z'); unique();
    click(editor.cells[2]); assert(choose(HOST_KEY_RETURN) == BINDING_CHANGED);
    assert(keys[2] == HOST_KEY_RETURN && keys[3] == HOST_KEY_SPACE); unique();
    click(editor.cells[8]); assert(editor.listening);
    assert(choose('s') == BINDING_CHANGED && keys[8] == 's'); unique();
    click(editor.cells[9]); assert(editor.listening);
    assert(choose(HOST_KEY_F5) == BINDING_CHANGED && keys[9] == HOST_KEY_F5); unique();
    click(editor.tabs[1]); assert(!editor.keyboard);
    click(editor.cells[0]); assert(editor.listening);
    assert(choose(HOST_CONTROLLER_BUTTON_Y) == BINDING_CHANGED);
    assert(buttons[0] == HOST_CONTROLLER_BUTTON_Y && buttons[9] == HOST_CONTROLLER_BUTTON_A); unique();
    assert(click(editor.reset) == BINDING_CHANGED);
    assert(!memcmp(buttons,binding_default_buttons,sizeof(buttons)) && keys[0] == 'x');
    click(editor.tabs[0]); assert(click(editor.reset) == BINDING_CHANGED);
    assert(!memcmp(keys,binding_default_keys,sizeof(keys))); unique();
    assert(click(editor.done) == BINDING_CLOSED);
}
static void save_load_conflicts(void) {
    open(640,640); click(editor.cells[8]);
    assert(choose('z') == BINDING_NONE && editor.listening && keys[0]=='z' && keys[8]==HOST_KEY_F5);
    assert(key(HOST_KEY_F6,0) == BINDING_NONE && editor.listening);
    assert(key('s',HOST_MOD_SHIFT) == BINDING_NONE && editor.listening);
    assert(choose('s') == BINDING_CHANGED && keys[8]=='s');
    click(editor.cells[8]); assert(choose('z') == BINDING_CHANGED);
    assert(keys[8]=='z' && keys[0]=='s'); unique();
    click(editor.cells[9]); assert(choose('z') == BINDING_CHANGED);
    assert(keys[8]==HOST_KEY_F8 && keys[9]=='z'); unique();
    click(editor.cells[8]); assert(choose(HOST_KEY_F5) == BINDING_CHANGED); unique();
}
static void capture_cancel_and_reserved_keys(void) {
    open(512,498); click(editor.cells[0]);
    assert(key(HOST_KEY_F8,0) == BINDING_NONE && editor.listening && keys[0] == 'z');
    assert(key('k',HOST_MOD_CTRL) == BINDING_NONE && editor.listening && keys[0] == 'z');
    assert(key('k',0) == BINDING_CHANGED && !editor.listening && keys[0] == 'k');
    click(editor.cells[0]); click(editor.cancel); assert(!editor.listening && keys[0] == 'k');
    click(editor.cells[0]); key(HOST_KEY_ESCAPE,0); assert(!editor.listening);
    click(editor.cells[0]); click(editor.tabs[1]); assert(!editor.listening && !editor.keyboard);
    click(editor.cells[0]);
    HostEvent e = {.type=HOST_CONTROLLERBUTTONDOWN,.cbutton={8,HOST_CONTROLLER_BUTTON_B}};
    assert(binding_editor_event(&editor,&e,7) == BINDING_NONE && editor.listening);
    e.cbutton.which = 7;
    assert(binding_editor_event(&editor,&e,-1) == BINDING_NONE && editor.listening);
    assert(binding_editor_event(&editor,&e,7) == BINDING_CHANGED);
    assert(buttons[0] == HOST_CONTROLLER_BUTTON_B && buttons[1] == HOST_CONTROLLER_BUTTON_A); unique();
    click(editor.cells[1]); assert(click(editor.close) == BINDING_CLOSED);
}
static void small_windows_and_scrolling(void) {
    const int sizes[][2] = {{128,138},{256,276},{384,404},{512,498},{1280,720}};
    for (unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s) {
        open(sizes[s][0],sizes[s][1]);
        assert(editor.bounds.x >= 0 && editor.bounds.x+editor.bounds.w <= editor.width);
        assert(editor.bounds.y >= MENU_BAR_HEIGHT);
        assert(editor.bounds.y+editor.bounds.h <= editor.height-STATUS_BAR_HEIGHT);
        click(editor.cells[0]); assert(editor.listening);
        assert(choose('m') == BINDING_CHANGED && keys[0] == 'm');
        for (int i=0;i<BINDING_COUNT;++i) {
            key(HOST_KEY_DOWN,0);
            assert(editor.cells[editor.selected].h > 0);
        }
        click(editor.tabs[1]); key(HOST_KEY_RETURN,0);
        assert(choose(HOST_CONTROLLER_BUTTON_DPAD_RIGHT) == BINDING_CHANGED); unique();
        assert(click(editor.done) == BINDING_CLOSED);
    }
    open(256,276);
    assert(editor.down.h > 0);
    int scroll = editor.scroll; click(editor.down); assert(editor.scroll == scroll+1);
    HostEvent wheel = {.type=HOST_MOUSEWHEEL,.wheel.y=1};
    binding_editor_event(&editor,&wheel,7); assert(editor.scroll == scroll);
    wheel.wheel.direction = HOST_MOUSEWHEEL_FLIPPED;
    binding_editor_event(&editor,&wheel,7); assert(editor.scroll == scroll+1);
    HostEvent outside = {.type=HOST_MOUSEBUTTONDOWN,.button.button=HOST_BUTTON_LEFT,
                         .window_mouse={0,editor.done.y,true}};
    assert(binding_editor_event(&editor,&outside,7) == BINDING_NONE);
}
int main(void) {
    mouse_editing(); save_load_conflicts(); capture_cancel_and_reserved_keys(); small_windows_and_scrolling();
    puts("Mouse binding pickers, conflict swaps, capture, cancel, reserved keys and small-window scrolling passed.");
}
