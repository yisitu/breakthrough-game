"""Coordinates a validated Breakthrough match between two CLI bots."""

from cli_adapter import CLIPlayer
from game_rules import BreakthroughBoard


class MatchRunner:
    """Runs one game and treats protocol, legality, or timing errors as forfeits."""

    def __init__(
        self,
        bot_white: CLIPlayer,
        bot_black: CLIPlayer,
        timeout_per_move=0.2,
        show_progress=False,
        show_board=False,
    ):
        self.bot_white = bot_white
        self.bot_black = bot_black
        self.timeout = timeout_per_move
        self.show_progress = show_progress
        self.show_board = show_board
        self.bot_white.timeout = timeout_per_move
        self.bot_black.timeout = timeout_per_move

    def play(self) -> dict:
        """Plays to completion and returns names, reason, count, and UCI history."""
        board = BreakthroughBoard()
        history = []

        try:
            self.bot_white.start()
            self.bot_black.start()

            winner = None
            reason = "Normal promotion"

            while winner is None:
                if board.turn == 'W':
                    current_bot = self.bot_white
                else:
                    current_bot = self.bot_black
                # Bots receive the complete history on every turn, allowing
                # stateless implementations to reconstruct prior context.
                hist_str = " ".join(history)

                raw_move, elapsed, err = current_bot.get_move(
                    board.to_fen_like(), hist_str)

                if err:
                    winner = 'B' if board.turn == 'W' else 'W'
                    reason = f"{current_bot.name} forfeited: {err}"
                    break

                parsed_move = board.parse_move(raw_move)
                if not parsed_move or not board.make_move(parsed_move):
                    winner = 'B' if board.turn == 'W' else 'W'
                    reason = f"{current_bot.name} made an illegal move: '{raw_move}'"
                    break

                if self.show_progress:
                    print(
                        f"Move {len(history) + 1}: {current_bot.name} -> "
                        f"{raw_move} ({elapsed:.3f}s)",
                        flush=True,
                    )
                history.append(raw_move)
                if self.show_board:
                    print(board.to_ascii(), flush=True)
                winner = board.check_winner()
        finally:
            # Cleanup must run for normal wins, forfeits, and harness errors.
            self.bot_white.stop()
            self.bot_black.stop()

        if winner == 'W':
            winner_name = self.bot_white.name
            loser_name = self.bot_black.name
        else:
            winner_name = self.bot_black.name
            loser_name = self.bot_white.name
        return {
            "winner": winner_name,
            "loser": loser_name,
            "reason": reason,
            "moves_count": len(history),
            "history": history,
        }
