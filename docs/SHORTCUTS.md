# Shortcuts

Each application shortcut has one action and matches its modifiers exactly.
Save/load default to F5/F8 in gameplay, the debugger, and TAS playback.
Change them, the eight NES inputs, or gamepad buttons under
**Emulation > Controller Bindings**. Shift plus your selected save/load key
opens the named-file variant. Menu labels follow the current bindings.

Panels and diagnostics occupy F2–F4. Debugger frame controls use F6,
breakpoints F7, and execution controls F9–F10; Shift reverses a frame or steps
over an instruction. TAS navigation uses the adjacent Ctrl+Left/Right pair,
with Ctrl+Home/End for restart/stop. F1 always pauses/resumes gameplay or a
movie. Escape dismisses UI and never pauses or opens a ROM.

## All shortcuts: Action - Key

This is the complete shortcut list. NES, save/load, and gamepad rows show defaults. Standard
UI navigation applies while a menu, browser, dialog, or command bar owns input;
application shortcuts are consumed there. Mouse actions are included below the
keyboard/gamepad entries.

| Action | Key |
| --- | --- |
| NES A | Z / gamepad A |
| NES B | X / gamepad B |
| NES Select | Space / gamepad Back (Share) |
| NES Start | Enter / gamepad Start (Options) |
| NES Up | Up / gamepad D-pad Up or left stick up |
| NES Down | Down / gamepad D-pad Down or left stick down |
| NES Left | Left / gamepad D-pad Left or left stick left |
| NES Right | Right / gamepad D-pad Right or left stick right |
| Open ROM | Ctrl+O |
| Open FM2 TAS movie | Ctrl+M |
| Pause/resume gameplay or TAS; cancel active movie seek | F1 |
| Toggle metrics panel | F2 |
| Toggle debug information panel | F3 |
| Capture diagnostics | F4 |
| Toggle event tracing | Ctrl+F4 |
| Toggle continuous instruction logging | Shift+F4 |
| Rolling quicksave | F5 / gamepad X |
| Save State As | Shift+F5 |
| Rolling quickload | F8 / gamepad Y |
| Load State From | Shift+F8 |
| Debugger: advance one nominal frame | F6 |
| Debugger: rewind one frame checkpoint | Shift+F6 |
| Debugger: toggle selected code breakpoint | F7 |
| Toggle debugger inspection / ordinary playback | F9 |
| Debugger: run to selected instruction | Ctrl+F9 |
| Debugger: step one CPU instruction | F10 |
| Debugger: step over | Shift+F10 |
| Debugger: step out | Ctrl+F10 |
| Debugger: rewind one instruction | Alt+F10 |
| Toggle fullscreen | F11 |
| Reset loaded game | F12 |
| Open debugger command bar | Ctrl+G |
| Debugger: select previous disassembly row | Ctrl+Up |
| Debugger: select next disassembly row | Ctrl+Down |
| Debugger: select Calls workspace | Ctrl+1 |
| Debugger: select Memory workspace | Ctrl+2 |
| Debugger: select Breaks workspace | Ctrl+3 |
| Debugger: select Trace workspace | Ctrl+4 |
| Debugger: select PPU workspace | Ctrl+5 |
| Debugger: select Help workspace | Ctrl+6 |
| TAS: rewind one movie frame | Ctrl+Left |
| TAS: advance one movie frame | Ctrl+Right |
| TAS: restart at frame zero, paused | Ctrl+Home |
| TAS: stop playback and restore the previous game | Ctrl+End |
| TAS: Go to Frame | Ctrl+J |
| Activate/dismiss desktop menu bar | Alt |
| Navigate UI entries/menus/submenus | Arrow keys |
| Activate selected UI item / submit text | Enter |
| Dismiss UI / go back one dialog level | Escape |
| Page a browser list or debugger workspace backward | Page Up |
| Page a browser list or debugger workspace forward | Page Down |
| File browser: edit location | Ctrl+L |
| File browser: open parent folder | Alt+Up / Backspace (outside text entry) |
| Edit location, filename, or debugger command | Printable text; Backspace erases |
| Go to Frame: edit completed-frame count | Digits 0–9; Backspace erases |
| Controller bindings: select action | Up / Down |
| Controller bindings: switch Keyboard / Gamepad tab | Left / Right / Tab |
| Controller bindings: open picker / capture input | Enter, then an unmodified key or gamepad button |
| Controller bindings: cancel input capture | Escape |
| Controls help: open controller bindings | Enter |
| About: close | Enter / Escape |
| Menu: select/open submenu | Left click / hover |
| Browser: open folder or choose file | Double left click |
| Browser: Home folder | Click Home button |
| Browser/workspace: scroll | Mouse wheel |
| Debugger: select disassembly row | Left click row |
| Debugger: toggle code breakpoint | Right click row |
| Debugger: select step/stop action | Click toolbar button |
| Debugger: select workspace | Click tab |
| Debugger: open command bar | Click command area |
| Debugger: prefill RAM edit | Click editable Memory byte |
| Debugger: prefill breakpoint enable/disable | Click Breaks entry |
| Controller bindings: change binding | Click binding, then click a key/button choice or press the input |
| Controller bindings: switch input type | Click Keyboard / Gamepad tab |
| Controller bindings: cancel pending edit | Click Cancel |
| Controller bindings: restore current input defaults | Click Reset defaults |
| Controller bindings: close editor | Click Done / X |
| Controller bindings: scroll actions or choices | Mouse wheel / click scroll arrows |
| Controls help / About: close | Click dialog |
| APU viewer: mute/unmute voice | Click channel name |
| Zapper: aim / fire | Mouse movement / left mouse button |

## Availability and behavior

- **Save/load:** your selected keys (default F5/F8) work with a loaded ROM, including when paused, inspecting
  the debugger, or playing a movie. Movie states also retain the input position.
  Gamepad X/Y require running playback with menus/dialogs and the debugger closed.
- **Pause:** F1 requires a loaded ROM. During a movie it cancels pending seeks
  or frame steps; at movie end it restarts playback. While the debugger owns
  execution, use F9 to leave inspection and resume ordinary playback.
- **Debugger:** execution/editing shortcuts are unavailable while a movie is
  open. F9 opens inspection without stepping; F10 always executes an instruction,
  entering the debugger if needed. F6 advances a nominal region-sized frame;
  reverse actions require retained history. F7 and Ctrl+Up/Down require active
  inspection. Page Up/Down page Calls, Memory, Breaks, Trace, or PPU, not Help.
  Shift+F4 can toggle logging during ordinary gameplay or inspection.
- **TAS:** Ctrl+Left/Right, Ctrl+Home/End, and Ctrl+J require an open movie.
  Recorded controller input owns both ports. Live keyboard/gamepad input cannot
  change the run. The first digit in Go to Frame replaces the prefilled count.
- **Command bar:** text entry consumes other shortcuts. Reopening with Ctrl+G
  preserves the pending command, and holding Ctrl+G does not erase it. Enter
  executes and closes; Escape closes without executing.
- **Browser:** Up/Down select entries. Escape leaves location/filename editing
  before it closes the browser. Backspace edits text when editing, otherwise it
  opens the parent. At a Windows drive root, parent navigation lists drives.
  Home is a clickable button, without a Home-key shortcut.
- **UI:** arrows, Enter, Escape, and paging retain standard navigation meanings
  within the active UI. Alt+F10 can dismiss an active menu and rewind, but cannot
  bypass a file browser, binding capture, or text-entry dialog. Other application
  shortcuts wait until UI is closed. Focus loss pauses and clears held inputs.

CRT, sprite-limit removal, nametable/APU viewers, audio volume/mute, region,
port selection, power cycle, window sizes, movie speed, help, exit, additional
clock/raster/interrupt steps, and execution-trace export remain menu/toolbar
or debugger-command actions without dedicated keyboard shortcuts.

## Preventing conflicts

The default application map lives in [shortcuts.h](../src/shortcuts.h),
with exact dispatch and configured save/load keys in [shortcuts.c](../src/shortcuts.c). Unknown modifier
combinations do nothing: Ctrl+F8 does not load, and Ctrl+Shift+F10 does not select
an arbitrary debugger action. Modified keys do not also press NES buttons.

The Controller Bindings editor has clickable Keyboard / Gamepad tabs and mouse
pickers for keys and named gamepad buttons. You can also press the input directly.
Assigning an occupied input swaps the two bindings when both actions can use
the keys, so an input never has two jobs. Keyboard capture rejects modified
chords and other application shortcut/navigation keys. F5/F8 are available only
for save/load. If a swap would move F5/F8 to a NES button, rebind that button
first. Existing settings retain valid custom bindings, including save/load;
reserved or duplicate entries are repaired to available defaults (or free
keys/buttons). A notification reports repairs. The editor explains rejected assignments, saves changes automatically,
and provides Cancel, Reset defaults (current tab), Done, and a close button.
Mouse wheel and scroll arrows reach bindings and picker choices in small windows.
Up/Down select actions, Left/Right or Tab switch input tabs, and Enter opens a
picker. Escape cancels a pending edit, then closes the editor.

The earlier collisions are resolved: debugger frame stepping moved from F8
to F6, run-to-cursor moved to Ctrl+F9, TAS frame controls moved to Ctrl+arrows,
and logging moved to Shift+F4. The bare F fullscreen alias, Escape pause/ROM-open
behavior, and broken +/- memory paging aliases were removed. Ctrl+G now
preserves pending text.

## Verification

Headless shortcut tests check unique chords/actions, default and custom state
commands, retired default keys, exact modifiers, and reserved keys. Real Sokol frontend tests cover
state loading in gameplay/debugger/TAS, instruction and frame stepping, movie
navigation, command-text preservation, mouse binding pickers, conflict swaps,
reserved binding rejection, persistence, cancellation and tab-specific resets,
legacy settings repair, and removal of old aliases. See
[debugger commands](DEBUGGER.md) and [frontend tests](../tests/sokol/run.sh).
