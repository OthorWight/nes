#!/usr/bin/env python3
"""Edit the frame timing of a text FM2, always writing a new movie copy."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import uuid


@dataclass(frozen=True)
class Edit:
    kind: str
    frame: int
    count: int


@dataclass(frozen=True)
class ButtonEdit:
    kind: str
    button: str
    frame: int
    count: int


def frame_count(value):
    try:
        frame, count = map(int, value.split(":"))
        if frame < 0 or count < 1:
            raise ValueError
    except ValueError as error:
        raise argparse.ArgumentTypeError("use FRAME:COUNT (frame >= 0, count >= 1)") from error
    return frame, count


def button_frames(value):
    try:
        button, frame, count = value.split(":")
        if len(button) != 1 or button not in "RLDUTSBA":
            raise ValueError
        frame, count = frame_count(frame + ":" + count)
    except (ValueError, argparse.ArgumentTypeError) as error:
        raise argparse.ArgumentTypeError("use BUTTON:FRAME:COUNT, e.g. A:31950:1") from error
    return button, frame, count


def read_movie(data):
    if len(data) > 64 * 1024 * 1024:
        raise ValueError("FM2 exceeds the emulator's 64 MiB limit")
    # Split the header before inspecting input: binary FM2 bytes are not lines.
    start = -1
    offset = 0
    while offset < len(data):
        if data[offset:offset + 1] == b"|":
            start = offset
            break
        end = data.find(b"\n", offset)
        if end < 0:
            break
        offset = end + 1
    if start < 0:
        raise ValueError("FM2 has no input records")
    header = data[:start].splitlines()
    settings = {}
    for line in header:
        key, separator, value = line.partition(b" ")
        if separator:
            settings[key] = value.strip()
    if settings.get(b"version") != b"3":
        raise ValueError("only FM2 version 3 is supported")
    if settings.get(b"binary", b"0") != b"0":
        raise ValueError("convert binary FM2 to text in FCEUX before retiming")
    if b"savestate" in settings:
        raise ValueError("only power-on movies are supported")
    for key in (b"fourscore", b"microphone", b"FDS", b"port2"):
        if settings.get(key, b"0") != b"0":
            raise ValueError("only standard gamepads or disconnected ports are supported")
    ports = [settings.get(key) for key in (b"port0", b"port1")]
    if any(value not in (b"0", b"1") for value in ports):
        raise ValueError("port0 and port1 must specify a gamepad (1) or no device (0)")
    inputs = []
    for line in data[start:].splitlines():
        if not line:
            continue
        fields = line.split(b"|")
        if (len(fields) != 6 or fields[0] or fields[4] or fields[5] or
                fields[1] not in (b"0", b"1", b"2", b"3")):
            raise ValueError("malformed or unsupported text FM2 input record")
        for port, active in enumerate(ports):
            buttons = fields[port + 2]
            if active == b"0":
                valid = buttons in (b"", b"........")
            else:
                valid = len(buttons) == 8 and all(
                    button in (ord("."), ord("RLDUTSBA"[index]))
                    for index, button in enumerate(buttons))
            if not valid:
                raise ValueError("invalid gamepad buttons in FM2 input record")
        inputs.append(line)
    try:
        length = int(settings.get(b"length", b"-1"))
    except ValueError as error:
        raise ValueError("invalid FM2 length header") from error
    if length < -1 or length > len(inputs):
        raise ValueError("FM2 length does not match its available input records")
    if length >= 0:
        inputs = inputs[:length]
    if not inputs:
        raise ValueError("FM2 has no playable input records")
    neutral = b"|0|" + (b"........" if ports[0] == b"1" else b"")
    neutral += b"|" + (b"........" if ports[1] == b"1" else b"") + b"||"
    return header, inputs, neutral


def retime(data, edits, source_name, button_edits=()):
    header, inputs, neutral = read_movie(data)
    for edit in button_edits:
        if (edit.kind not in ("press", "release") or len(edit.button) != 1 or
                edit.button not in "RLDUTSBA" or edit.frame < 0 or edit.count < 1 or
                edit.frame + edit.count > len(inputs)):
            raise ValueError("invalid button edit or range")
        if len(neutral.split(b"|")[2]) != 8:
            raise ValueError("button edits require a standard gamepad in port0")
        position = "RLDUTSBA".index(edit.button)
        for index in range(edit.frame, edit.frame + edit.count):
            fields = inputs[index].split(b"|")
            buttons = bytearray(fields[2])
            buttons[position] = ord(edit.button if edit.kind == "press" else ".")
            fields[2] = bytes(buttons)
            inputs[index] = b"|".join(fields)
    edits = sorted(edits, key=lambda edit: edit.frame)
    previous_end = -1
    for edit in edits:
        maximum = len(inputs) if edit.kind == "wait" else len(inputs) - 1
        if edit.frame < 0 or edit.frame > maximum or edit.count < 1:
            raise ValueError("edit is outside the movie's input range")
        if edit.frame < previous_end:
            raise ValueError("edits must have distinct positions and cannot overlap deletions")
        previous_end = edit.frame + (edit.count if edit.kind == "delete" else 1)
        if edit.kind == "delete":
            removed = inputs[edit.frame:edit.frame + edit.count]
            if len(removed) != edit.count:
                raise ValueError("deletion extends past the end of the movie")
            if any(line.split(b"|")[1] != b"0" for line in removed):
                raise ValueError("cannot delete a reset or power command")
        elif edit.kind == "pause":
            fields = inputs[edit.frame].split(b"|")
            if (edit.count < 2 or len(neutral.split(b"|")[2]) != 8 or
                    len(fields[2]) != 8 or fields[1] != b"0" or
                    fields[2][4] != ord(".")):
                raise ValueError("pause needs COUNT >= 2 and a port0 gamepad frame without Start/reset/power")
        elif edit.kind not in ("wait", "repeat"):
            raise ValueError("unknown edit type")
    if not edits and not button_edits:
        raise ValueError("specify at least one timing edit")
    count = len(inputs) + sum(-edit.count if edit.kind == "delete" else edit.count
                              for edit in edits)
    if count * (len(neutral) + 1) > 64 * 1024 * 1024:
        raise ValueError("retimed FM2 exceeds the emulator's 64 MiB limit")
    output = []
    cursor = 0
    for edit in edits:
        output.extend(inputs[cursor:edit.frame])
        if edit.kind == "delete":
            cursor = edit.frame + edit.count
        elif edit.kind == "pause":
            if output and output[-1].split(b"|")[2][4:5] == b"T":
                raise ValueError("Start must be released before a pause")
            fields = inputs[edit.frame].split(b"|")
            buttons = bytearray(fields[2])
            buttons[4] = ord("T")
            fields[2] = bytes(buttons)
            toggle = b"|".join(fields)
            # Unpausing executes the replaced original input in games such as SMB3.
            # COUNT extra frames: Start, COUNT-1 neutral frames, then Start again.
            output.append(toggle)
            output.extend([neutral] * (edit.count - 1))
            output.append(toggle)
            cursor = edit.frame + 1
        else:
            record = neutral
            if edit.kind == "repeat":
                # Hold the pads, but never repeat a reset/power command.
                record = b"|0|" + inputs[edit.frame].split(b"|", 2)[2]
            output.extend([record] * edit.count)
            cursor = edit.frame
    output.extend(inputs[cursor:])
    if not output:
        raise ValueError("edits would remove every input record")

    def shifted(original):
        frame = original
        for edit in edits:
            if edit.kind == "delete":
                frame_offset = min(edit.count, max(0, original - edit.frame))
                frame -= frame_offset
            elif edit.frame <= original:
                frame += edit.count
        return frame

    updated = []
    for line in header:
        if line.startswith(b"length "):
            line = b"length " + str(len(output)).encode("ascii")
        elif line.startswith(b"guid "):
            line = b"guid " + str(uuid.uuid4()).upper().encode("ascii")
        elif line.startswith(b"subtitle "):
            try:
                _, frame, text = line.split(b" ", 2)
                original = int(frame)
                if original < 0:
                    raise ValueError
            except ValueError as error:
                raise ValueError("invalid subtitle frame") from error
            line = b"subtitle " + str(shifted(original)).encode("ascii") + b" " + text
        updated.append(line)
    source_name = source_name.replace("\n", " ").replace("\r", " ")
    updated.append(b"comment retimingSource " + source_name.encode("utf-8"))
    for edit in edits:
        updated.append(f"comment retimingEdit {edit.kind} {edit.frame} {edit.count} (original zero-based frame)".encode("ascii"))
    for edit in button_edits:
        updated.append(f"comment retimingButton {edit.kind} {edit.button} {edit.frame} {edit.count} (original zero-based frame)".encode("ascii"))
    result = b"\n".join(updated + output) + b"\n"
    if len(result) > 64 * 1024 * 1024:
        raise ValueError("retimed FM2 exceeds the emulator's 64 MiB limit")
    return result, len(inputs), len(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="new file; existing files are never overwritten")
    for kind, help_text in (("wait", "insert frames with no buttons pressed"),
                            ("repeat", "insert frames holding the specified original frame's pads"),
                            ("pause", "add a player-1 Start/neutral/Start pause (COUNT >= 2; game-specific)"),
                            ("delete", "remove original input frames")):
        parser.add_argument("--" + kind, action="append", type=frame_count, default=[],
                            metavar="FRAME:COUNT", help=help_text + "; all indices are original and zero-based")
    for kind in ("release", "press"):
        parser.add_argument("--" + kind, action="append", type=button_frames, default=[],
                            metavar="BUTTON:FRAME:COUNT", help=kind + " a player-1 button over an original frame range")
    args = parser.parse_args()
    edits = [Edit(kind, frame, count) for kind in ("wait", "repeat", "pause", "delete")
             for frame, count in getattr(args, kind)]
    # Release ranges first, then press ranges. Inserted repeats use the edited pads.
    buttons = [ButtonEdit(kind, button, frame, count) for kind in ("release", "press")
               for button, frame, count in getattr(args, kind)]
    try:
        if args.source.resolve() == args.output.resolve():
            raise ValueError("output must be a new movie copy")
        result, before, after = retime(args.source.read_bytes(), edits, args.source.name, buttons)
        with args.output.open("xb") as output:
            output.write(result)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"Wrote {args.output}: {before:,} -> {after:,} frames. Replay validation is still required.")


if __name__ == "__main__":
    main()
