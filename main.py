"""Runs a round-robin tournament and persists Elo ratings."""

from cli_adapter import CLIPlayer
from leaderboard import EloLeaderboard
from match_runner import MatchRunner


def run_tournament(bots_config: dict[str, str], rounds=1, timeout=0.2):
    """Plays both color assignments for every pair in each round."""
    leaderboard = EloLeaderboard()
    bot_names = list(bots_config.keys())

    print(f"Starting Breakthrough Tournament with {len(bot_names)} bots...")

    for round_index in range(rounds):
        print(f"\n--- Round {round_index + 1} ---")
        for first_index in range(len(bot_names)):
            for second_index in range(first_index + 1, len(bot_names)):
                first_name = bot_names[first_index]
                second_name = bot_names[second_index]

                # Play 2 games per pair (swapping starting colors)
                color_assignments = [(first_name, second_name),
                                     (second_name, first_name)]
                for white_name, black_name in color_assignments:
                    white_bot = CLIPlayer(white_name, bots_config[white_name],
                                          timeout)
                    black_bot = CLIPlayer(black_name, bots_config[black_name],
                                          timeout)

                    runner = MatchRunner(white_bot,
                                         black_bot,
                                         timeout_per_move=timeout)
                    result = runner.play()

                    leaderboard.update_match(result["winner"], result["loser"])
                    summary = (f"[{result['winner']}] def. [{result['loser']}] "
                               f"in {result['moves_count']} moves. "
                               f"({result['reason']})")
                    print(summary)

    leaderboard.display()


if __name__ == "__main__":
    # This registry is an example; commands are resolved from the current
    # working directory and may be replaced with any protocol-compatible bots.
    BOT_REGISTRY = {
        "BitboardEngine_v1": "./engines/bitboard_bot --fast",
        "AlphaBetaCpp": "./engines/alphabeta_bot",
        "RandomBaseline": "python3 ./engines/random_bot.py"
    }

    run_tournament(BOT_REGISTRY, rounds=2, timeout=0.2)
