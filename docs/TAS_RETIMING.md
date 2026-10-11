# Retiming FM2 movies

`scripts/retime-fm2.py` edits a movie's recorded inputs without changing the
emulator's CPU, PPU, mapper, or audio timing. It writes a new text FM2 file;
the source and existing output files are never overwritten.

All edit positions refer to **zero-based frames in the original movie**.
An edit at frame 1200 affects the 1201st input record. Multiple timing edits
use those original coordinates, even when earlier edits change the length.

```sh
python3 scripts/retime-fm2.py input.fm2 converted.fm2 --wait 1200:1
```

The available edits are:

| Option | Effect |
| --- | --- |
| `--wait FRAME:COUNT` | Insert neutral frames before the original frame. |
| `--repeat FRAME:COUNT` | Insert frames holding that original frame's controller buttons. |
| `--pause FRAME:COUNT` | Add a player-1 Start/neutral/Start sequence, adding `COUNT` frames. |
| `--delete FRAME:COUNT` | Remove original input records. Reset/power commands cannot be deleted. |
| `--press BUTTON:FRAME:COUNT` | Hold one player-1 button over an original frame range. |
| `--release BUTTON:FRAME:COUNT` | Release one player-1 button over an original frame range. |

Options can be repeated. Button names use FM2's `RLDUTSBA`: Right, Left,
Down, Up, Start, Select, B, A. Button edits preserve other buttons and
reset/power commands. Releases apply first, then presses; timing edits apply
afterward, so inserted repeats use the edited buttons. Inserted records never
repeat a reset/power command.

`--pause` is an **in-game pause**, using recorded controller inputs. It needs
`COUNT >= 2`, a gamepad in port0, and Start released before and on the original
frame. It replaces that frame with Start plus its original buttons, `COUNT-1`
neutral records, then Start plus the original buttons again. In SMB3 the last
record unpauses and executes the replaced input. Other games can handle Start
differently, and SMB3 ignores pauses during some transitions; replay validation
must confirm that the game actually paused. Host pause (F1) does not advance
emulated time and cannot synchronize a movie.

For example, to add one wait and extend an existing jump hold by one frame:

```sh
python3 scripts/retime-fm2.py input.fm2 converted.fm2 \
  --wait 1200:1 --press A:5000:1
```

The tool preserves the ROM checksum, author credits, controller configuration,
and other metadata. It moves subtitles with their original frames, updates
an existing length header, assigns a new movie GUID, and records the source
filename and edits in comments. Standard gamepads and disconnected ports are
supported. Binary FM2, embedded save states, and expansion devices are rejected.

Load the matching ROM, then open the converted file with **Ctrl+M**. Movie
save states belong to the exact FM2 file; use new movie states for the copy.
Verify the run through its ending. An extra frame can affect random state,
enemy movement, collisions, and later lag, so passing one repaired section
does not establish that the whole movie works.

Run the editing tests with:

```sh
python3 -B -m unittest discover -s tests -p retime_fm2.py
```

## SMB3 100% conversion

The conversion targets `build/tas/lordtom_tompa-smb3-100.fm2`, the NTSC
power-on movie by LordTom and Tompa recorded with FCEUX's old PPU. Its
232,997 original input records have source-file SHA-256
`ed58205b8f0caf6b5f0fc0be617367f5d9494abf654da05099bd4eff5cff4b5f`.
Use the US PRG0 ROM whose **PRG/CHR MD5** is
`b76f978ad3076ea67f2b0aca7399c9e9`; this checksum excludes the iNES header.
The recipe does not apply to the separate warpless movie or another ROM revision.

The saved conversion is `build/tas/lordtom_tompa-smb3-100-retimed.fm2`.
Its complete 341,308-frame controller stream was replayed from clean power-on
with the unchanged emulator core and reached the final “THE END” screen.
Every consumed controller record matched the saved FM2. All 276 world/tileset
transitions matched the original reference; final Mario position, velocity,
movement fractions, power-up, lives, random state, frame counter, and rescue
flag also matched. This establishes a completed port, rather than requiring
every RAM byte or intermediate score to match FCEUX.

The local verification report and ending image are alongside the movie:
`lordtom_tompa-smb3-100-retimed-verification.json` and
`lordtom_tompa-smb3-100-retimed-ending.png`. These local artifacts live under
the ignored `build/tas` directory; the recipe below reproduces the input edits.

SMB3 advances its random state during an in-game pause. In the states examined
here, that state repeats after 32,767 updates, while the frame counter wraps
every 256 updates. A 32,767-frame pause preserves the random state and moves
the counter back one modulo 256. A 32,768-frame pause advances the random
state once and preserves the counter. These sequences repair movie timing
using ordinary controller inputs, with no emulator timing changes or RAM edits.
Each takes about nine minutes at normal NTSC speed.

The RAM labels and game routines used during diagnosis come from
[Southbird's SMB3 disassembly](https://github.com/captainsouthbird/smb3)
([project site](http://www.sonicepoch.com/sm3mix/)). No ROM or disassembled
game code is included in the editing tool.

To reproduce the conversion from the original movie, run this from the
repository root. The destination must not already exist.

```sh
python3 scripts/retime-fm2.py \
  build/tas/lordtom_tompa-smb3-100.fm2 \
  build/tas/lordtom_tompa-smb3-100-retimed.fm2 \
  --repeat 26660:1 \
  --press A:31950:6 \
  --press A:32111:3 \
  --release A:45132:1 \
  --repeat 64103:1 \
  --pause 64135:32767 \
  --pause 84820:32768 \
  --delete 103490:1 \
  --repeat 133478:1 \
  --pause 133520:32767 \
  --delete 174045:1 \
  --repeat 174059:1 \
  --repeat 174371:1 \
  --repeat 174372:1 \
  --repeat 174415:1 \
  --repeat 174419:1 \
  --repeat 174420:1 \
  --press D:174394:1 \
  --release R:174394:1 \
  --press A:174414:1 \
  --repeat 174440:1 \
  --release R:196832:1 \
  --release R:196899:5 \
  --repeat 213873:1 \
  --wait 232997:10000
```

The neutral tail after the original final input lets the rescue and closing
sequence play through to the final “THE END” screen. The retimed movie has
341,308 input records, including the three synchronization pauses and this tail.

To skip watching a synchronization pause, use **Ctrl+J** to seek to its resume
frame, then **F1** to play. Seeking still executes the recorded pause inputs.
The values below are completed-frame counts, as shown in the emulator UI:

| Pause at original input | First paused frame | Resume frame |
| --- | --- | --- |
| 64,135 | 64,138 | 96,905 |
| 84,820 | 117,590 | 150,358 |
| 133,520 | 199,058 | 231,825 |

Keep the original movie alongside the converted copy. The separate warpless
movie remains unconverted.

The existing headless playback check can also replay the converted file and
save its final screen:

```sh
bash tests/roms/fm2.sh 'build/Super Mario Bros 3.nes' \
  build/tas/lordtom_tompa-smb3-100-retimed.fm2 build/tas/replay-ending.ppm
```

That command reports whether playback reaches EOF; inspect the saved screen
to confirm the game ending as well.
