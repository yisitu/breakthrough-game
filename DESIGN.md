```
┌────────────────────────────────────────────────────────┐
│                   Tournament Engine                    │
└───────────┬────────────────────────────────┬───────────┘
            │                                │
┌───────────▼───────────┐        ┌───────────▼───────────┐
│     Match Runner      │        │    Elo Leaderboard    │
└───────────┬───────────┘        └───────────────────────┘
            │
┌───────────▼───────────┐
│  CLI Player Adapter   │ ◄───► Subprocess (STDIN / STDOUT + Timeout)
└───────────┬───────────┘
            │
┌───────────▼───────────┐
│   Breakthrough Rules  │
└───────────────────────┘


Harness (Python)                               CLI Bot Process
  │                                                   │
  ├────── 1. Write STDIN (Board + History) ──────────►│ (Reads input)
  │                                                   │ (Computes search tree)
  │◄───── 2. Read STDOUT (UCI Move String) ───────────┤ (Outputs "e2e4\n")
  │                                                   │
  ├─ 3. Validate Move & Check 0.2s Timeout ───────────┤
```

1. Data Protocol Breakdown
A. Harness to Bot (STDIN)
On every turn, the harness writes two lines to the bot's STDIN followed by a flush:

```
WWWWWWWW/WWWWWWWW/......../......../......../......../BBBBBBBB/BBBBBBBB W
e2e4 e7e5 f2f4
```
Line 1 (Board State): FEN-like 8-row string representing board positions (W = White pawn, B = Black pawn, . = Empty) followed by whose turn it is (W or B).

Line 2 (Move History): Space-delimited string of all UCI moves played in the match up to this point.

B. Bot to Harness (STDOUT)
The bot must perform its search and write a single line containing its selected move to STDOUT:

```
e2e3
```

  Implementation Requirement for Bots

Bots in any language are supported as long as they read two lines per turn
and write one flushed move line to stdout. Keep diagnostics on stderr.

The harness sets `BREAKTHROUGH_TIMEOUT_MS` in each bot process to the configured
per-move limit. Bots must finish and flush their move within that wall-clock
budget. The default is 200ms; bundled bots reserve an internal safety margin.

Python bot entry point:
```bash
python3 -u my_bot.py
```

Rust bot entry point after building with Cargo:
```bash
./target/release/my_bot
```

The Python bot should use `print(move, flush=True)`. The Rust bot should call
`stdout().flush()` after writing its move. The harness passes either command
unchanged to the subprocess adapter.

  C++ example bot main loop compatible with the harness
// Example C++ bot main loop compatible with the harness
```c++
#include <iostream>
#include <string>

int main() {
    std::string fen_state;
    std::string move_history;

    // Loop continuously reading turns until stdin closes or process is killed
    while (std::cin >> fen_state) {
        std::cin.ignore(); // consume newline
        std::getline(std::cin, move_history);

        // Update internal board representation from fen_state / move_history
        // Run time-bounded search (e.g., Iterative Deepening up to 0.45s)
        std::string best_move = compute_best_move(fen_state);

        // Output single move line + flush
        std::cout << best_move << std::endl;
    }
    return 0;
}
```

# No live progress, final result only
python3 example_game.py --bot /tmp/breakthrough_harness_test

# Show moves
python3 example_game.py --bot /tmp/breakthrough_harness_test --progress

# Show moves and board state after each move
python3 example_game.py --bot /tmp/breakthrough_harness_test --board

# Run two different bots against each other
python3 example_game.py \
  --white-bot /tmp/white_bot \
  --black-bot /tmp/black_bot \
  --progress

# Run the optimized Python bot against the C++ bot
python3 example_game.py \
  --white-bot "python3 -u breakthrough_bot.py" \
  --black-bot ./breakthrough \
  --board

# Build the optimized Rust bot
rustc --edition=2021 -O -C target-cpu=native \
  breakthrough_bot.rs -o breakthrough_rust

# Run the Rust bot against the Python bot
python3 example_game.py \
  --white-bot ./breakthrough_rust \
  --black-bot "python3 -u breakthrough_bot.py" \
  --board

# Build and validate the tournament-focused C++ bot
g++ -std=c++17 -O3 -march=native -DNDEBUG \
  breakthrough_bot.cpp -o breakthrough_cpp
./breakthrough_cpp --self-test

# Build and validate the speed-first C++ bot
g++ -std=c++17 -O3 -march=native -DNDEBUG \
  breakthrough_speed_bot.cpp -o breakthrough_speed
./breakthrough_speed --self-test
./breakthrough_speed --benchmark

# Build and validate the speed-first Rust bot
rustc --edition=2021 -O -C target-cpu=native \
  breakthrough_speed_bot.rs -o breakthrough_speed_rust
./breakthrough_speed_rust --self-test
./breakthrough_speed_rust --benchmark

# Compare one-second opening-position search performance
./breakthrough_cpp --benchmark
./breakthrough_speed_rust --benchmark

# Run the new C++ bot against the Rust bot
python3 example_game.py \
  --white-bot ./breakthrough_cpp \
  --black-bot ./breakthrough_rust \
  --board

# Compare the speed-first bot with the tournament-focused C++ bot
python3 example_game.py \
  --white-bot ./breakthrough_speed \
  --black-bot ./breakthrough_cpp \
  --progress

# Compare the Rust speed bot with the tournament-focused C++ bot
python3 example_game.py \
  --white-bot ./breakthrough_speed_rust \
  --black-bot ./breakthrough_cpp \
  --progress