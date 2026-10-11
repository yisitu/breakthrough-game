"""Persists tournament results and updates player ratings using Elo."""

import json
import math
import os


class EloLeaderboard:
    """JSON-backed Elo standings with win/loss counters."""

    def __init__(self,
                 filepath="leaderboard.json",
                 k_factor=32,
                 initial_rating=1200):
        self.filepath = filepath
        self.k_factor = k_factor
        self.initial_rating = initial_rating
        self.ratings = self.load()

    def load(self) -> dict:
        if os.path.exists(self.filepath):
            with open(self.filepath, "r") as f:
                return json.load(f)
        return {}

    def save(self):
        with open(self.filepath, "w") as f:
            json.dump(self.ratings, f, indent=2)

    def get_rating(self, player: str) -> float:
        return self.ratings.get(player, {
            "elo": self.initial_rating,
            "wins": 0,
            "losses": 0
        })["elo"]

    def update_match(self, winner: str, loser: str):
        """Records a decisive game and applies the standard Elo update."""
        for player in (winner, loser):
            if player not in self.ratings:
                self.ratings[player] = {
                    "elo": self.initial_rating,
                    "wins": 0,
                    "losses": 0
                }

        winner_rating = self.ratings[winner]["elo"]
        loser_rating = self.ratings[loser]["elo"]

        # Expected scores are complementary probabilities based on the
        # pre-game rating difference.
        winner_expected = 1.0 / (
            1.0 + math.pow(10, (loser_rating - winner_rating) / 400.0))
        loser_expected = 1.0 - winner_expected

        # The K-factor controls how strongly one result moves each rating.
        self.ratings[winner]["elo"] = round(
            winner_rating + self.k_factor * (1.0 - winner_expected), 1)
        self.ratings[loser]["elo"] = round(
            loser_rating + self.k_factor * (0.0 - loser_expected), 1)

        self.ratings[winner]["wins"] += 1
        self.ratings[loser]["losses"] += 1

        self.save()

    def display(self):
        print("\n" + "=" * 45)
        print(f"{'Rank':<6}{'Player':<20}{'Elo':<8}{'W/L':<8}")
        print("=" * 45)
        sorted_players = sorted(
            self.ratings.items(),
            key=lambda player: player[1]["elo"],
            reverse=True,
        )
        for rank, (name, stats) in enumerate(sorted_players, 1):
            win_loss = f"{stats['wins']}/{stats['losses']}"
            print(f"{rank:<6}{name:<20}{stats['elo']:<8}{win_loss:<8}")
        print("=" * 45 + "\n")
