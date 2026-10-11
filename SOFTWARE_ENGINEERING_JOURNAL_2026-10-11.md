# Software Engineering Journal: Breakthrough Tournament Platform

**Journal date:** 2026-10-11  
**Generated:** 2026-10-11T14:45:22-07:00  
**Recorded engineering window:** 2026-10-11T20:06:38Z to 2026-10-11T21:45:22Z  
**Elapsed engineering window:** 1h 38m 44s  
**Repository:** `/home/sotong/aider-projects`  
**Branch:** `master`  
**Copilot session:** `aa3bee82-df6b-404b-bb3f-840d189d1380`  
**Status:** Active development; session changes are not committed

## Executive Summary

The project evolved from a standalone terminal Breakthrough game and a partial
Python harness into a language-neutral tournament platform with strict referee
behavior and six bot implementations.

The resulting platform provides:

- A persistent two-line stdin/one-line stdout bot protocol.
- Human-versus-AI and harness modes for the original C++ engine.
- Single-game and round-robin command-line runners.
- Independent White and Black bot commands.
- Optional move progress and board rendering.
- Strict timeout, crash, EOF, malformed-output, and illegal-move forfeits.
- A default 200 ms per-move deadline communicated through
  `BREAKTHROUGH_TIMEOUT_MS` and independently enforced by the harness.
- Optimized Python, Rust, tournament C++, speed-first C++, and speed-first Rust
  bitboard engines.
- Deterministic move-generation/hash self-tests and harness fault tests.
- Persistent Google-style C++/Python formatting and canonical Rust formatting.
- Reviewer-oriented comments explaining protocols, invariants, and algorithms.

The tournament-focused C++ bot won the initial balanced three-engine tournament
10-0. The speed-first C++ experiment reached depth 12 at approximately 32.2
million nodes per second. A matching Rust speed engine later averaged 30.71
million nodes per second, 2.16 times the tournament C++ engine's node rate,
while tournament C++ won both practical 200 ms comparison games.

## Starting Point

Three commits existed before this session:

| Commit | Timestamp | Description |
|---|---|---|
| `fbc796d` | 2026-10-11 12:16:51 -0700 | Add `breakthrough.cpp` |
| `1e6650f` | 2026-10-11 12:17:11 -0700 | Implement terminal Breakthrough with Negamax AI |
| `0595b99` | 2026-10-11 12:29:16 -0700 | Add the second initial pawn rank |

The C++ program was interactive and did not satisfy the tournament protocol.
The harness also lacked complete process-failure handling, independent bot
commands, configurable presentation, and a consistent timeout contract.

## Product Goals

1. Establish one authoritative rule and coordinate convention.
2. Make any line-oriented executable usable as a bot, regardless of language.
3. Treat the harness as a strict referee rather than a cooperative demo runner.
4. Support observable games without forcing tournament verbosity.
5. Build competitive reference bots in Python, Rust, and C++.
6. Measure quality through balanced games and speed through reproducible tests.
7. Keep the code reviewable, formatted, documented, and debugger-friendly.

## Timestamped Activity Log

### 20:06-20:10Z - Harness Compatibility

**Plan:** Determine the harness contract and adapt the original C++ program
without changing the referee's board semantics.

**Engineering work:**

- Traced `game_rules.py`, `match_runner.py`, `cli_adapter.py`, and `main.py`.
- Converted `breakthrough.cpp` into a persistent protocol process by default.
- Unified board orientation: row 0 is White's home rank; White advances toward
  increasing rows and Black toward decreasing rows.
- Added exact FEN-like parsing and UCI move output.
- Corrected opening double moves, path validation, and promotion detection.

**Result:** The C++ engine returned legal harness moves instead of prompts.

### 20:10-20:20Z - Example Game and Interactive UX

**Plan:** Make the harness easy to exercise while keeping presentation separate
from referee behavior.

**Engineering work:**

- Added `example_game.py` as a one-game CLI.
- Added live move timing and ASCII board rendering.
- Added `--progress` and `--board`; default output remains final-result only.
- Added `--interactive`/`-i` to `breakthrough.cpp` for human White versus AI.
- Audited `TODO`, `FIXME`, and `XXX` markers; none existed.

**Result:** The same C++ program supports automation and terminal play.

### 20:20-20:23Z - Language-Neutral Bot Commands

**Plan:** Remove the assumption that both sides execute the same command or use
one language.

**Engineering work:**

- Added `--white-bot` and `--black-bot`; retained `--bot` as shorthand.
- Kept commands opaque in `CLIPlayer` so Python, Rust, C++, and wrappers use the
  same process boundary.
- Documented flushing requirements and stderr-only diagnostics.

**Result:** Independently configured cross-language bots can play each other.

### 20:23-20:31Z - Python and Rust Engines

**Python engine:**

- Two 64-bit bitboards and packed moves.
- Iterative deepening, negamax alpha-beta, PVS, transposition caching,
  killer/history ordering, LMR, and promotion-aware evaluation.
- Cooperative timing through the harness environment variable.

**Rust engine:**

- Fixed move lists and a direct-mapped transposition table.
- Allocation-free search, aspiration windows, PVS, killer/history ordering,
  and LMR.
- Three tests, including 10,000 deterministic random positions checked against
  an independent square-by-square move generator.

**Result:** The harness gained portable optimized reference engines.

### 20:31-20:45Z - Strict Referee and Timeout Contract

**Plan:** Make every protocol failure deterministic and immediately terminal.

**Findings:**

- Timed-out subprocesses remained alive until later cleanup.
- EOF and crashes could be misclassified as illegal moves.
- Move parsing accepted trailing output.
- Bot streams could leak resources.
- Bots did not know the configured deadline.

**Engineering work:**

- Started bots in new process sessions and killed process groups on timeout.
- Classified timeout, EOF, exit, I/O failure, malformed output, and illegal
  moves as forfeits.
- Required exact four-character UCI output.
- Added unconditional cleanup and stream closure.
- Standardized the default deadline at 200 ms.
- Passed each bot `BREAKTHROUGH_TIMEOUT_MS`; bots reserve at least 20 ms or 20%
  for scheduling, parsing, and flushing.
- Added eight Python lifecycle and protocol regression tests.

**Result:** The harness became authoritative even when a bot did not cooperate.

### 20:45-21:02Z - Tournament C++ Engine and Ranking

**Plan:** Build the strongest reference engine while keeping correctness checks
independent from optimized implementation paths.

**Engineering work:**

- Added `breakthrough_bot.cpp` with fixed move lists and bitboards.
- Added incremental Zobrist hashing and a persistent direct-mapped TT.
- Added aspiration windows, PVS, quiescence search, promotion extensions, LMR,
  killer/history ordering, and mate-score normalization.
- Added `--self-test` to compare move generation with an independent generator
  over 10,000 positions and verify incremental hashes.

**Tournament result:** 15 balanced games, 10 per bot, five as each color.

| Rank | Engine | Record |
|---:|---|---:|
| 1 | Tournament C++ | 10-0 |
| 2 | Rust | 5-5 |
| 3 | Python | 0-10 |

All games ended by normal promotion. There were no referee failures.

### 21:02-21:19Z - Formatting, Readability, and Debuggability

**Plan:** Prepare the code for review without changing engine behavior.

**Engineering work:**

- Added Google-based `.clang-format` and `.style.yapf` configurations.
- Added `rustfmt.toml` for Rust 2021.
- Formatted all source and verified formatter idempotence.
- Added protocol, lifecycle, coordinate, bitboard, TT, PVS, LMR, evaluation,
  timeout, and natural-language algorithm documentation.
- Replaced terse tournament, Elo, and coordinate names.
- Replaced primitive C++ aliases with direct `uint64_t`, `uint16_t`, `uint8_t`,
  `int8_t`, and `size_t` use.
- Renamed project-defined C++ functions to PascalCase.
- Required braces and multi-line bodies for every C++ conditional through
  `.clang-format`, improving breakpoint placement and single-step debugging.

**Result:** Formatting, strict compilation, tests, and diagnostics passed.

### 21:19-21:23Z - Speed-First C++ Experiment

**Plan:** Test whether cheaper evaluation and tactical search allow more full
plies under the same deadline.

**Engineering work:**

- Added `breakthrough_speed_bot.cpp` separately from the tournament champion.
- Retained bitboards, incremental Zobrist hashing, TT, PVS, aspiration windows,
  killer/history ordering, and LMR.
- Removed quiescence search and promotion extensions.
- Replaced passed-pawn, mobility, and per-pawn scans with a bounded bit-operation
  evaluator.
- Added `--self-test` and `--benchmark` modes.

**Initial result:** Depth 12 at about 17.2 million nodes/second. The bot produced
legal moves under the 200 ms harness contract but lost both initial color
assignments to tournament C++.

### 21:23-21:32Z - Profiling and Latency Optimization

**Plan:** Profile before editing, distinguish raw throughput from useful depth,
and reject changes that weaken pruning enough to reduce completed depth.

**Profile findings:**

- `Negamax`: about 81% self-time, including inlined bookkeeping.
- `ApplyMove`: about 8%.
- `GenerateMoves`: about 6%.
- `Evaluate` plus `FastSideScore`: about 4.7% before other bottlenecks moved.
- A no-inline profile attributed about 60% to move ordering and scoring.

**A/B experiments:**

| Variant | Throughput | Depth | Decision |
|---|---:|---:|---|
| TT `2^20` | 17.7M nodes/s | 12 | Baseline |
| TT `2^18` | 25.4M nodes/s | 12 | Cache pressure confirmed |
| TT `2^16` | 27.4M nodes/s | 12 | Selected |
| No move ordering | 43.1M nodes/s | 11 | Rejected; pruning regressed |
| Three-zone evaluator | About 2% faster | 12 | Rejected; insufficient gain |

**Accepted optimizations:**

- Reduced the TT from `2^20` entries, about 24 MiB, to `2^16` entries, about
  1.5 MiB.
- Replaced insertion sorting with stable `O(n)` stages for TT moves, promotions,
  captures, killers, history moves, advanced quiet moves, and other quiets.

**Final result:**

- Release throughput: about 32.2 million nodes/second.
- Improvement: about 82% over the measured 17.7M baseline.
- Completed depth remained 12.
- Instrumented throughput improved from 13.7M to 21.9M nodes/second.
- Two games versus tournament C++ split 1-1, both by normal promotion.
- Self-test, strict compilation, formatting, and diagnostics passed.

### 21:33-21:44Z - Project Journal and Quantitative Review

**Plan:** Preserve the project history as a timestamped engineering record with
project-management priorities, implementation detail, model disclosure, LoC,
task complexity, elapsed workstream time, and measured verification time.

**Engineering work:**

- Reconstructed 30 user-directed milestones from the local Copilot session
  index rather than relying on memory alone.
- Recorded agent metadata and explicitly marked the underlying LLM identifier
  unavailable because it was absent from local indexes and raw logs.
- Measured physical LoC with `wc -l`, tracked deltas with Git, and verification
  command durations from transcript tool timestamps.
- Added risks, decisions, validation history, and a prioritized delivery plan.

**Result:** The journal became a reproducible project artifact rather than an
unverifiable narrative summary.

### 21:44-21:45Z - Speed-First Rust Engine and C++ Comparison

**Plan:** Port the measured speed-first design to Rust and compare it with the
tournament C++ reference using identical one-second benchmark conditions.

**Engineering work:**

- Added `breakthrough_speed_bot.rs` with incremental Zobrist hashing, a `2^16`
  TT, stable staged move ordering, fixed-cost evaluation, PVS, aspiration
  windows, killer/history ordering, and aggressive LMR.
- Kept the recursive search allocation-free with fixed 48-move arrays.
- Added `--self-test` and `--benchmark`, including 10,000 randomized positions
  and verification of every generated child hash.
- Added equivalent depth/node/time benchmark instrumentation to
  `breakthrough_bot.cpp` without changing its normal protocol behavior.
- Ran five one-second release benchmarks per engine and two 200 ms harness
  games with colors reversed.

**Performance result:**

| Engine | Average nodes/second | Nominal depth | Five-run move |
|---|---:|---:|---|
| Rust speed | 30.71M | 12 | `d2d4` |
| Tournament C++ | 14.20M | 12 | `e2e4` |

Rust processed 2.16 times as many nodes. The comparison is not a claim of equal
work per node: tournament C++ performs quiescence search, threat extensions, and
richer evaluation. That extra work translated into a 2-0 practical result for
tournament C++, with both games ending by normal promotion and no referee
failure.

## Architecture and Contracts

### Bot Protocol

Each turn sends two lines:

1. FEN-like board plus side to move.
2. Space-delimited UCI move history.

A bot returns exactly one flushed UCI move. Diagnostics belong on stderr.

### Coordinates

- Row 0 corresponds to rank 1.
- White advances toward larger rows and bit indices.
- Black advances toward smaller rows and bit indices.
- Bit 0 is `a1`; bit 63 is `h8`.
- Packed moves store source in bits 6 and above and target in bits 0-5.

### Referee

- The harness enforces wall-clock time externally.
- Bots receive the limit through `BREAKTHROUGH_TIMEOUT_MS`.
- Timeout, crash, EOF, malformed output, and illegal moves cause forfeits.
- Process-group termination prevents child-process leaks.

## Engine Models and Strategies

| File | Representation | Search strategy | Role |
|---|---|---|---|
| `breakthrough.cpp` | 64-element array | Iterative negamax alpha-beta | Harness and interactive baseline |
| `breakthrough_bot.py` | Python bitboards | PVS, TT, LMR | Portable optimized bot |
| `breakthrough_bot.rs` | Rust bitboards | Fixed-list PVS, TT, LMR | Allocation-conscious bot |
| `breakthrough_bot.cpp` | C++ bitboards, incremental hash | PVS, TT, quiescence, extensions | Tournament reference |
| `breakthrough_speed_bot.cpp` | C++ bitboards, small TT | PVS, staged ordering, aggressive LMR | Latency/depth experiment |
| `breakthrough_speed_bot.rs` | Rust bitboards, incremental hash, small TT | PVS, staged ordering, aggressive LMR | Cross-language latency experiment |

## AI Assistance and Models Used

- **Assistant product:** GitHub Copilot.
- **Recorded session agent:** `panel/editAgent`.
- **Other local agent record:** `GitHub Copilot Chat`.
- **Read-only exploration:** an `Explore` subagent supported the readability
  audit.
- **Underlying LLM/model identifier:** unavailable in the local session store,
  transcript, and raw debug metadata. No unverified model name is claimed.
- **Python grounding:** the Pylance Python fact-grounded coding skill was
  consulted during Python work.

## Toolchain

- Python 3.12.3 and `unittest`.
- C++17 with `g++`, including `-Wall -Wextra -Wpedantic -Werror`.
- Rust 1.99.0 and `rustc --test -D warnings`.
- `clang-format` 18.1.3 with Google style and mandatory braces.
- YAPF with Google style and canonical Rust 2021 `rustfmt`.
- `gprof` for profiling; `perf` and Valgrind were unavailable.
- VS Code diagnostics and GitHub Copilot editing/search tools.

## Validation Record

| Area | Check | Result |
|---|---|---|
| Harness lifecycle | Python regression suite | 8/8 passed |
| Rust move generation | Rust unit tests | 3/3 passed |
| Tournament C++ | 10,000 random positions and hashes | Passed |
| Speed C++ | 10,000 positions and every generated child hash | Passed |
| Speed Rust | 3 tests plus 10,000 positions and every child hash | Passed |
| C++ builds | Warnings promoted to errors | Passed |
| Python | `py_compile` | Passed |
| Formatting | C++, Python, Rust dry checks | Passed |
| Diagnostics | All touched source files | No errors |
| Protocol | Legal move under 200 ms contract | Passed |
| Tournament | 15 initial games | No referee failures |
| Rust speed comparison | 5 benchmarks per engine and 2 balanced games | C++ won 2-0; no referee failures |

## Decisions and Rationale

1. External deadline enforcement is mandatory; cooperative timing only improves
   move quality.
2. One stdin/stdout protocol avoids language-specific adapter risk.
3. Iterative deepening retains the deepest complete result after timeout.
4. Independent reference generators catch optimized bitboard errors.
5. Profiling showed that ordering and cache behavior mattered more than leaf
   scoring.
6. Raw node throughput was rejected when it reduced completed depth.
7. Experimental engines remain separate from the tournament champion.
8. Node throughput and playing strength are reported separately because
  quiescence, extensions, and evaluation make engine nodes unequal units.

## Risks and Technical Debt

- Session work remains uncommitted and most files are untracked.
- Generated `breakthrough` and `__pycache__/` artifacts should not be committed.
- `main.py` still contains illustrative engine paths rather than bundled paths.
- No Makefile, CMake project, Cargo manifest, or CI workflow runs the complete
  validation matrix.
- Tournament samples are too small for statistically strong conclusions.
- The speed benchmark uses only the initial position.
- Rust/C++ node-rate comparisons do not normalize work per node; completed
  depth and balanced match results must accompany throughput.
- Elo persistence lacks dedicated regression tests.
- Benchmark output lacks per-depth nodes, TT hit rate, cutoff index, and PV.

## Next Project Plan

### Priority 0 - Stabilize

- Remove or ignore generated artifacts.
- Replace example tournament paths with valid commands or explicit config.
- Review and commit harness, engines, tests, formatting, and performance work in
  logical commits.

### Priority 1 - Reproducible Automation

- Add Makefile or CMake targets for every bot.
- Add one command for Python tests, C++ self-tests, Rust tests, formatter checks,
  strict builds, and protocol smoke tests.
- Run the same command in CI.

### Priority 2 - Better Performance Evidence

- Benchmark opening, tactical middle-game, race, and sparse endgame positions.
- Report depth, nodes, TT hit rate, cutoff move index, and time per depth.
- Tune TT sizes from `2^15` through `2^18` across the full suite.
- Tune staged move ordering using cutoff distributions.

### Priority 3 - Strength Evaluation

- Run at least 100 balanced games per engine pair.
- Separate normal wins from timeout, illegal-move, and crash results.
- Record average depth, latency percentiles, and game length.

## Final Activity and Complexity Analysis

### Physical Line Counts

These are physical lines measured with `wc -l`; they include comments and blank
lines and therefore are not logical source statements.

| Category | Lines |
|---|---:|
| C++ source: three engines | 2,414 |
| Python source, harness, tests, and utility | 1,236 |
| Rust source: two engines and tests | 1,810 |
| **All source and test files** | **5,460** |
| Documentation and formatter configuration | 169 |
| **Project footprint before this journal** | **5,629** |
| Journal | 535 |
| **Project footprint including this journal** | **6,164** |

Further attribution:

- Bot implementations account for 4,728 lines.
- Python harness/tournament infrastructure accounts for 525 lines.
- Harness regression tests account for 118 lines.
- The unrelated `tictactoe.py` example accounts for 89 lines.
- Source/configuration files known to have been created in this session account
  for at least 4,177 current physical lines: the example CLI, five optimized/new
  bots, harness tests, and formatter configurations. The journal adds another
  535 lines.
- The tracked `breakthrough.cpp` changed by 623 added and 282 deleted lines,
  ending at 741 lines versus 400 lines in `HEAD`.

Because most workspace files are untracked, an exact repository-wide
added/deleted count is not available from Git. Current physical footprint is the
most reproducible measure.

### Time by Workstream

Task durations below are elapsed wall-clock estimates derived from user-turn
boundaries. They include implementation, review, tool execution, and user
interaction inside each interval.

| Workstream | Approximate elapsed time | Complexity |
|---|---:|---|
| Harness compatibility, CLI, board display, interactive mode | 13m 18s | High: protocol and coordinate integration |
| Independent commands and language-neutral contract | 3m 02s | Medium: CLI/API ownership changes |
| Python and Rust engines | 7m 45s | High: two optimized search implementations |
| Strict referee and timeout contract | 13m 52s | High: concurrency, deadlines, process groups |
| Tournament C++ engine and ranking | 17m 24s | Very high: search, hashing, tests, tournament |
| Formatting, comments, C++ conventions, debug style | 16m 57s | Medium: broad mechanical and review work |
| Speed bot, profiling, and latency optimization | 13m 03s | Very high: measurement-driven performance work |
| Journal construction and quantitative review | 11m 00s | Medium: evidence synthesis and metrics |
| Rust speed bot and C++ comparison | 0m 34s | Very high: generated port, tests, benchmarks, games |
| Inter-turn idle/coordination time | 1m 49s | Not an engineering task |
| **Total engineering window** | **1h 38m 44s** | **High overall** |

The highest-complexity tasks were strict subprocess lifecycle enforcement,
search-engine implementation, and performance optimization. They combined
behavioral correctness with time limits, recursive algorithms, low-level data
representation, and cross-language protocol constraints. Formatting and naming
changes had broad file coverage but lower algorithmic risk.

### Verification Time

Transcript tool timestamps provide direct timing for synchronous terminal
commands. Commands were classified as verification when they contained builds,
formatters, unit tests, syntax checks, self-tests, benchmarks, or harness games.
The command metrics use a fixed cutoff of 2026-10-11T21:45:01Z, immediately
after the Rust/C++ comparison; commands used only to update this journal are
excluded so the counts remain stable.

| Verification measure | Value |
|---|---:|
| Verification commands | 80 |
| Measured verification command time | 602 seconds |
| Measured verification time | **10m 02s** |
| Share of engineering window | **10.2%** |
| All terminal commands | 102 |
| All terminal command time | 10m 32s |

The 10m02s figure is a lower bound on verification effort. It measures command
execution, including compilers, tests, games, and one-second benchmarks, but not
time spent reading output, reviewing diffs, reasoning about failures, inspecting
diagnostics, or designing corrective edits. Verification was continuous rather
than deferred: each substantive edit was followed by a focused check, then by
broader cross-language validation when the local hypothesis passed.

### Complexity Conclusion

This was a high-complexity, high-throughput session: roughly 99 minutes produced
or substantially reshaped a 5.6K-line multi-language system while preserving a
strict 200 ms runtime contract. The ratio of measured verification time to total
elapsed time is appropriate for an exploratory codebase, but the large uncommitted
change set and lack of one-command automation are now the dominant delivery
risks. The next engineering investment should be reproducibility and larger
statistical test samples, not additional isolated features.
