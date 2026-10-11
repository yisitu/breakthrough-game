"""Command-line entry point for running one harness-managed example game."""

import argparse

from cli_adapter import CLIPlayer
from match_runner import MatchRunner


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run one Breakthrough example game.")
    parser.add_argument(
        "--bot",
        help=
        "Command used for both players unless overridden by a color-specific option",
    )
    parser.add_argument(
        "--white-bot",
        help="Command used by White (default: --bot or ./breakthrough)",
    )
    parser.add_argument(
        "--black-bot",
        help="Command used by Black (default: --bot or ./breakthrough)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=0.2,
        help="Maximum seconds allowed per move (default: 0.2)",
    )
    parser.add_argument(
        "--progress",
        action="store_true",
        help="Print each move as it is played",
    )
    parser.add_argument(
        "--board",
        action="store_true",
        help="Print the board after each move",
    )
    args = parser.parse_args()

    # Color-specific commands override the shared command independently.
    default_bot = args.bot or "./breakthrough"
    white_command = args.white_bot or default_bot
    black_command = args.black_bot or default_bot

    white = CLIPlayer("Breakthrough-White", white_command, args.timeout)
    black = CLIPlayer("Breakthrough-Black", black_command, args.timeout)
    result = MatchRunner(
        white,
        black,
        timeout_per_move=args.timeout,
        show_progress=args.progress or args.board,
        show_board=args.board,
    ).play()

    print(f"Winner: {result['winner']}")
    print(f"Reason: {result['reason']}")
    print(f"Moves: {result['moves_count']}")
    print("History: " + " ".join(result["history"]))


if __name__ == "__main__":
    main()
