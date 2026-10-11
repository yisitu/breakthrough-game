"""Regression tests for strict bot protocol and process lifecycle handling."""

import shlex
import sys
import unittest

from cli_adapter import CLIPlayer
from game_rules import BreakthroughBoard
from match_runner import MatchRunner


def python_command(code: str) -> str:
    return f"{shlex.quote(sys.executable)} -c {shlex.quote(code)}"


class CLIPlayerFaultTests(unittest.TestCase):

    def start_bot(self, code: str, timeout: float = 0.2) -> CLIPlayer:
        bot = CLIPlayer("test-bot", python_command(code), timeout)
        bot.start()
        self.addCleanup(bot.stop)
        return bot

    def test_nonzero_exit_is_reported_as_process_failure(self):
        bot = self.start_bot(
            "import sys; sys.stdin.readline(); sys.stdin.readline(); sys.exit(7)"
        )

        move, _, error = bot.get_move("state W", "")

        self.assertIsNone(move)
        self.assertIn("Process exited with code 7", error)

    def test_timeout_kills_process_before_returning(self):
        # A short timeout keeps the suite fast while exercising the real
        # process-group kill path rather than a mocked clock.
        bot = self.start_bot(
            "import sys, threading; "
            "sys.stdin.readline(); sys.stdin.readline(); "
            "threading.Event().wait()",
            timeout=0.03,
        )

        move, elapsed, error = bot.get_move("state W", "")

        self.assertIsNone(move)
        self.assertTrue(error.startswith("Timeout"))
        self.assertGreaterEqual(elapsed, 0.03)
        self.assertIsNotNone(bot.process.poll())

    def test_valid_flushed_move_is_returned(self):
        bot = self.start_bot(
            "import sys, threading; "
            "sys.stdin.readline(); sys.stdin.readline(); "
            "print('a2a3', flush=True); threading.Event().wait()")

        move, _, error = bot.get_move("state W", "")

        self.assertEqual(move, "a2a3")
        self.assertIsNone(error)

    def test_configured_timeout_is_provided_to_bot(self):
        bot = self.start_bot(
            "import os, sys, threading; "
            "sys.stdin.readline(); sys.stdin.readline(); "
            "print(os.environ['BREAKTHROUGH_TIMEOUT_MS'], flush=True); "
            "threading.Event().wait()",
            timeout=0.137,
        )

        value, _, error = bot.get_move("state W", "")

        self.assertEqual(value, "137")
        self.assertIsNone(error)


class MoveValidationTests(unittest.TestCase):

    def test_accepts_exact_uci_move(self):
        board = BreakthroughBoard()
        move = board.parse_move("A2A3")

        self.assertIsNotNone(move)
        self.assertTrue(board.make_move(move))

    def test_rejects_trailing_output(self):
        board = BreakthroughBoard()

        self.assertIsNone(board.parse_move("a2a3 debug"))

    def test_rejects_invalid_coordinates(self):
        board = BreakthroughBoard()

        for move in ("a0a1", "i2i3", "a2a9", "a2-a3", ""):
            with self.subTest(move=move):
                self.assertIsNone(board.parse_move(move))


class MatchRunnerFaultTests(unittest.TestCase):

    def test_crash_is_an_immediate_forfeit_and_both_bots_stop(self):
        crashing = python_command(
            "import sys; sys.stdin.readline(); sys.stdin.readline(); sys.exit(9)"
        )
        waiting = python_command("import threading; threading.Event().wait()")
        white = CLIPlayer("crashing-white", crashing, 0.2)
        black = CLIPlayer("waiting-black", waiting, 0.2)

        result = MatchRunner(white, black, timeout_per_move=0.2).play()

        self.assertEqual(result["winner"], "waiting-black")
        self.assertIn("forfeited: Process exited with code 9", result["reason"])
        self.assertIsNotNone(white.process.poll())
        self.assertIsNotNone(black.process.poll())


if __name__ == "__main__":
    unittest.main()
