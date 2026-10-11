"""Run with: python3 -m unittest discover -s tests -p retime_fm2.py"""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "retime-fm2.py"
SPEC = importlib.util.spec_from_file_location("fm2_timing_script", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

MOVIE = (b"version 3\r\nemuVersion 21050\r\nport0 1\r\nport1 0\r\n"
         b"romFilename synthetic\r\nromChecksum 0x00000000000000000000000000000000\r\n"
         b"guid original\r\nlength 4\r\ncomment author Original author\r\n"
         b"subtitle 1 Before\r\nsubtitle 2 After\r\nsubtitle 3 Old byte \x85\r\n"
         b"|2|.......A|||\r\n|0|R.......|||\r\n|0|......B.|||\r\n|0|........|||\r\n")


class RetimeTests(unittest.TestCase):
    def test_mixed_edits_use_original_indices_and_move_subtitles(self):
        edits = [MODULE.Edit("wait", 1, 2), MODULE.Edit("delete", 2, 1)]
        result, before, after = MODULE.retime(MOVIE, edits, "original.fm2")
        header, inputs, _ = MODULE.read_movie(result)
        self.assertEqual((before, after), (4, 5))
        self.assertEqual(inputs, [b"|2|.......A|||", b"|0|........|||",
                                  b"|0|........|||", b"|0|R.......|||", b"|0|........|||"])
        self.assertIn(b"subtitle 3 Before", header)
        self.assertIn(b"subtitle 4 After", header)
        self.assertIn(b"subtitle 4 Old byte \x85", header)
        self.assertIn(b"comment author Original author", header)
        self.assertIn(b"length 5", header)
        self.assertNotIn(b"guid original", header)

    def test_repeating_pads_does_not_repeat_power_command(self):
        result, _, _ = MODULE.retime(MOVIE, [MODULE.Edit("repeat", 0, 1)], "original.fm2")
        inputs = MODULE.read_movie(result)[1]
        self.assertEqual(inputs[:2], [b"|0|.......A|||", b"|2|.......A|||"])

    def test_two_gamepads_and_end_wait(self):
        movie = MOVIE.replace(b"port1 0", b"port1 1").replace(b"|||", b"|.L......||")
        result, _, _ = MODULE.retime(movie, [MODULE.Edit("wait", 4, 1)], "original.fm2")
        self.assertEqual(MODULE.read_movie(result)[1][-1], b"|0|........|........||")

    def test_button_edits_preserve_other_buttons_and_commands(self):
        buttons = [MODULE.ButtonEdit("release", "A", 0, 1),
                   MODULE.ButtonEdit("press", "A", 1, 2)]
        result, _, after = MODULE.retime(MOVIE, [MODULE.Edit("repeat", 1, 1)],
                                        "original.fm2", buttons)
        self.assertEqual(after, 5)
        self.assertEqual(MODULE.read_movie(result)[1], [b"|2|........|||",
                         b"|0|R......A|||", b"|0|R......A|||", b"|0|......BA|||", b"|0|........|||"])
        for edit in (MODULE.ButtonEdit("press", "A", 3, 2),
                     MODULE.ButtonEdit("press", "Q", 1, 1)):
            with self.assertRaises(ValueError):
                MODULE.retime(MOVIE, [], "original.fm2", [edit])

    def test_header_pipe_is_preserved(self):
        movie = MOVIE.replace(b"comment author Original author", b"comment author Original | author")
        result, _, _ = MODULE.retime(movie, [MODULE.Edit("wait", 1, 1)], "original.fm2")
        self.assertIn(b"comment author Original | author", MODULE.read_movie(result)[0])

    def test_pause_preserves_pads_and_counts_extra_frames(self):
        result, before, after = MODULE.retime(MOVIE, [MODULE.Edit("pause", 1, 4)], "source.fm2")
        header, inputs, _ = MODULE.read_movie(result)
        self.assertEqual((before, after), (4, 8))
        self.assertEqual(inputs, [b"|2|.......A|||", b"|0|R...T...|||",
                                  b"|0|........|||", b"|0|........|||",
                                  b"|0|........|||", b"|0|R...T...|||",
                                  b"|0|......B.|||", b"|0|........|||"])
        self.assertIn(b"subtitle 5 Before", header)
        self.assertIn(b"subtitle 6 After", header)
        self.assertIn(b"length 8", header)

    def test_rejects_unsafe_pause_ranges_and_oversized_output(self):
        for edit in (MODULE.Edit("pause", 0, 4), MODULE.Edit("pause", 1, 1),
                     MODULE.Edit("pause", 4, 4), MODULE.Edit("wait", 1, 2**32)):
            with self.subTest(edit=edit), self.assertRaises(ValueError):
                MODULE.retime(MOVIE, [edit], "source.fm2")
        for movie in (MOVIE.replace(b"R.......", b"R...T..."),
                      MOVIE.replace(b".......A", b"....T..A"),
                      MOVIE.replace(b"port0 1", b"port0 0").replace(b".......A", b"........")
                      .replace(b"R.......", b"........").replace(b"......B.", b"........")):
            with self.subTest(movie=movie), self.assertRaises(ValueError):
                MODULE.retime(movie, [MODULE.Edit("pause", 1, 4)], "source.fm2")

    def test_rejects_invalid_edits_and_unsupported_movies(self):
        for edits in ([MODULE.Edit("delete", 0, 1)], [MODULE.Edit("delete", 3, 2)],
                      [MODULE.Edit("wait", 5, 1)], [MODULE.Edit("repeat", 4, 1)],
                      [MODULE.Edit("delete", 1, 2), MODULE.Edit("wait", 2, 1)]):
            with self.subTest(edits=edits), self.assertRaises(ValueError):
                MODULE.retime(MOVIE, edits, "original.fm2")
        for movie in (MOVIE.replace(b"version 3", b"version 2"),
                      MOVIE.replace(b"version 3", b"version 3\nbinary 1"),
                      MOVIE.replace(b"version 3", b"version 3\nsavestate 0x00"),
                      MOVIE.replace(b"R.......", b".R......")):
            with self.subTest(movie=movie), self.assertRaises(ValueError):
                MODULE.retime(movie, [MODULE.Edit("wait", 1, 1)], "original.fm2")

    def test_cli_preserves_source_and_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.fm2"
            output = Path(directory) / "copy.fm2"
            source.write_bytes(MOVIE)
            command = [sys.executable, str(SCRIPT), str(source), str(output), "--wait", "1:1"]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            saved = output.read_bytes()
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), saved)
            self.assertEqual(source.read_bytes(), MOVIE)
            command[3] = str(source)
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(source.read_bytes(), MOVIE)


if __name__ == "__main__":
    unittest.main()
