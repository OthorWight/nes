# Shortcuts

Each application shortcut has one action and matches its modifiers exactly.
F5/F8 are reserved for saving/loading in gameplay, the debugger, and TAS
playback. Shift adds the named-file variant. Keyboard state shortcuts are fixed;
the eight NES inputs and gamepad buttons remain configurable under
**Emulation > Controller Bindings**.

Panels and diagnostics occupy F2–F4. Debugger frame controls use F6,
breakpoints F7, and execution controls F9–F10; Shift reverses a frame or steps
over an instruction. TAS navigation uses the adjacent Ctrl+Left/Right pair,
with Ctrl+Home/End for restart/stop. F1 always pauses/resumes gameplay or a
movie. Escape dismisses UI and never pauses or opens a ROM.

## All shortcuts: Action - Key

This is the complete shortcut list. NES/gamepad rows show defaults. Standard
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
| Controller bindings: activate row / capture input | Enter, then an unmodified key or gamepad button |
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
| Controller bindings: activate row / capture input | Click row |
| Controls help / About: close | Click dialog |
| APU viewer: mute/unmute voice | Click channel name |
| Zapper: aim / fire | Mouse movement / left mouse button |

## Availability and behavior

- **Save/load:** F5/F8 work with a loaded ROM, including when paused, inspecting
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

The application map and menu labels share [shortcuts.h](../src/shortcuts.h),
with exact dispatch in [shortcuts.c](../src/shortcuts.c). Unknown modifier
combinations do nothing: Ctrl+F8 does not load, and Ctrl+Shift+F10 does not select
an arbitrary debugger action. Modified keys do not also press NES buttons.

Keyboard capture rejects modified chords, reserved shortcut/navigation keys,
and duplicates. F5/F8 cannot be reassigned. Gamepad capture rejects duplicate
buttons. Existing settings retain valid custom bindings; reserved or duplicate
entries are repaired to available defaults (or free keys/buttons), and old
custom keyboard quickstate keys are restored to F5/F8. A notification reports
repairs. Binding rows show why a new assignment is rejected.

The earlier collisions are resolved: debugger frame stepping moved from F8
to F6, run-to-cursor moved to Ctrl+F9, TAS frame controls moved to Ctrl+arrows,
and logging moved to Shift+F4. The bare F fullscreen alias, Escape pause/ROM-open
behavior, and broken +/- memory paging aliases were removed. Ctrl+G now
preserves pending text.

## Verification

Headless shortcut tests check unique chords/actions, exclusive F5/F8 state
commands, exact modifiers, and reserved keys. Real Sokol frontend tests cover
state loading in gameplay/debugger/TAS, instruction and frame stepping, movie
navigation, command-text preservation, duplicate/reserved binding rejection,
legacy settings repair, and removal of old aliases. See
[debugger commands](DEBUGGER.md) and [frontend tests](../tests/sokol/run.sh).
