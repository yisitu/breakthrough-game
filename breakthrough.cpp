#include <stddef.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

// The default mode implements the persistent tournament protocol. Pass
// --interactive to play White against the engine in a terminal.
//
// Algorithm overview:
//
// The board is a 64-element array containing White, Black, or an empty square.
// For each turn, the bot generates every legal move and searches progressively
// deeper until its time budget is nearly exhausted. Each completed iteration
// uses negamax, a symmetric form of minimax, with alpha-beta pruning to skip
// branches that cannot change the result. Promotion moves, captures, immediate
// threats, and safe advances are searched first so pruning happens earlier.
//
// At the depth limit, the evaluator compares material, pawn advancement,
// mobility, defended pawns, exposed pawns, home-rank coverage, and immediate
// promotion threats. Moves are applied to one mutable board and then undone to
// avoid copying at every node. The bot always returns the best move from the
// deepest fully completed iteration; nearly equal moves are randomized to make
// repeated games less predictable without sacrificing much score.
const int INF = 1000000;
const int WIN = 100000;
const int DEFAULT_TIMEOUT_MS = 200;
const int MAX_DEPTH = 64;

using Board = std::array<int, 64>;

struct Move {
  int from = -1;
  int to = -1;
  bool capture = false;
  bool promotes = false;
  int player = 0;
};

int BoardIndex(int r, int c) { return r * 8 + c; }

bool InBounds(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

int ConfiguredTimeMs() {
  int timeoutMs = DEFAULT_TIMEOUT_MS;
  if (const char* value = std::getenv("BREAKTHROUGH_TIMEOUT_MS")) {
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end != value && *end == '\0' && parsed > 0 &&
        parsed <= std::numeric_limits<int>::max()) {
      timeoutMs = static_cast<int>(parsed);
    }
  }
  int marginMs = std::min(timeoutMs - 1, std::max(20, timeoutMs / 5));
  return std::max(1, timeoutMs - marginMs);
}

Board InitialBoard() {
  Board b{};
  for (int c = 0; c < 8; ++c) {
    b[BoardIndex(0, c)] = 1;   // White starts on rank 1
    b[BoardIndex(1, c)] = 1;   // White starts on rank 2
    b[BoardIndex(6, c)] = -1;  // Black starts on rank 7
    b[BoardIndex(7, c)] = -1;  // Black starts on rank 8
  }
  return b;
}

bool ParseState(const std::string& state, Board& b, int& player) {
  // Harness rows run from rank 1 to rank 8, so row indices match UCI ranks - 1.
  size_t separator = state.find(' ');
  if (separator == std::string::npos) {
    return false;
  }

  std::string rows = state.substr(0, separator);
  std::string side = state.substr(separator + 1);
  if (rows.size() != 71 || side.size() != 1 ||
      (side[0] != 'W' && side[0] != 'B')) {
    return false;
  }

  int row = 0;
  int col = 0;
  for (char cell : rows) {
    if (cell == '/') {
      if (col != 8 || ++row >= 8) {
        return false;
      }
      col = 0;
      continue;
    }

    if (col >= 8) {
      return false;
    }
    if (cell == 'W') {
      b[BoardIndex(row, col)] = 1;
    } else if (cell == 'B') {
      b[BoardIndex(row, col)] = -1;
    } else if (cell != '.') {
      return false;
    }
    ++col;
  }

  if (row != 7 || col != 8) {
    return false;
  }
  player = side[0] == 'W' ? 1 : -1;
  return true;
}

std::string MoveToString(const Move& m) {
  std::string result;
  result += static_cast<char>('a' + m.from % 8);
  result += static_cast<char>('1' + m.from / 8);
  result += static_cast<char>('a' + m.to % 8);
  result += static_cast<char>('1' + m.to / 8);
  return result;
}

void PrintBoard(const Board& b) {
  std::cout << "  a b c d e f g h\n";
  for (int r = 7; r >= 0; --r) {
    std::cout << r + 1 << " ";
    for (int c = 0; c < 8; ++c) {
      int v = b[BoardIndex(r, c)];
      if (v == 1) {
        std::cout << "W";
      } else if (v == -1) {
        std::cout << "B";
      } else {
        std::cout << ".";
      }
      if (c + 1 < 8) {
        std::cout << " ";
      }
    }
    std::cout << "\n";
  }
}

std::vector<Move> GenerateMoves(const Board& b, int player) {
  std::vector<Move> moves;
  int dir = (player == 1 ? 1 : -1);

  for (int r = 0; r < 8; ++r) {
    for (int c = 0; c < 8; ++c) {
      if (b[BoardIndex(r, c)] != player) {
        continue;
      }

      auto add = [&](int nr, int nc, bool capture) {
        if (!InBounds(nr, nc)) {
          return;
        }

        int to = BoardIndex(nr, nc);

        if (!capture) {
          if (b[to] != 0) {
            return;
          }
        } else {
          if (b[to] != -player) {
            return;
          }
        }

        Move m;
        m.from = BoardIndex(r, c);
        m.to = to;
        m.capture = capture;
        m.player = player;
        m.promotes = (player == 1 && nr == 7) || (player == -1 && nr == 0);
        moves.push_back(m);
      };

      add(r + dir, c, false);
      if (((player == 1 && r == 1) || (player == -1 && r == 6)) &&
          b[BoardIndex(r + dir, c)] == 0) {
        add(r + 2 * dir, c, false);
      }
      add(r + dir, c - 1, true);
      add(r + dir, c + 1, true);
    }
  }

  return moves;
}

void MakeMove(Board& b, const Move& m) {
  b[m.to] = m.player;
  b[m.from] = 0;
}

void UnmakeMove(Board& b, const Move& m) {
  b[m.from] = m.player;
  b[m.to] = m.capture ? -m.player : 0;
}

int CountMoves(const Board& b, int player) {
  int count = 0;
  int dir = (player == 1 ? 1 : -1);

  for (int r = 0; r < 8; ++r) {
    for (int c = 0; c < 8; ++c) {
      if (b[BoardIndex(r, c)] != player) {
        continue;
      }

      auto inc = [&](int nr, int nc, bool capture) {
        if (!InBounds(nr, nc)) {
          return;
        }

        int to = BoardIndex(nr, nc);

        if (!capture) {
          if (b[to] == 0) {
            ++count;
          }
        } else {
          if (b[to] == -player) {
            ++count;
          }
        }
      };

      inc(r + dir, c, false);
      if (((player == 1 && r == 1) || (player == -1 && r == 6)) &&
          b[BoardIndex(r + dir, c)] == 0) {
        inc(r + 2 * dir, c, false);
      }
      inc(r + dir, c - 1, true);
      inc(r + dir, c + 1, true);
    }
  }

  return count;
}

// Value of a pawn by how far it has advanced toward the goal row. The curve is
// steep near the end so the search values real breakthrough threats far more
// than generic forward shuffling.
const int ADVANCE_VALUE[8] = {0, 4, 10, 18, 30, 55, 140, 400};

const int MATERIAL_VALUE = 110;
const int PROTECTED_BONUS = 14;
const int ATTACKED_PENALTY = 10;
const int MOBILITY_WEIGHT = 2;
const int HOME_DEFENSE_BONUS = 16;
const int HOME_HOLE_PENALTY = 26;
const int THREAT_PENALTY = 700;

// Row offset a pawn of `player` moves toward.
int ForwardDirection(int player) { return player == 1 ? 1 : -1; }

// Goal row for `player`; reaching it wins the game.
int GoalRow(int player) { return player == 1 ? 7 : 0; }

// How many rows this pawn has advanced from its own back rank.
int Advancement(int r, int player) { return player == 1 ? r : 7 - r; }

// True if a friendly pawn sits diagonally behind, so a capture can be answered.
bool IsProtected(const Board& b, int r, int c, int player) {
  int back = -ForwardDirection(player);

  for (int dc = -1; dc <= 1; dc += 2) {
    if (InBounds(r + back, c + dc) &&
        b[BoardIndex(r + back, c + dc)] == player) {
      return true;
    }
  }

  return false;
}

// True if an enemy pawn can capture onto (r, c) next move.
bool IsAttacked(const Board& b, int r, int c, int byPlayer) {
  int back = -ForwardDirection(byPlayer);

  for (int dc = -1; dc <= 1; dc += 2) {
    if (InBounds(r + back, c + dc) &&
        b[BoardIndex(r + back, c + dc)] == byPlayer) {
      return true;
    }
  }

  return false;
}

// True if `player` can step or capture onto the goal row immediately.
bool HasPromotingMove(const Board& b, int player) {
  int goal = GoalRow(player);
  int r = goal - ForwardDirection(player);

  for (int c = 0; c < 8; ++c) {
    if (b[BoardIndex(r, c)] != player) {
      continue;
    }
    if (b[BoardIndex(goal, c)] == 0) {
      return true;
    }
    if (InBounds(goal, c - 1) && b[BoardIndex(goal, c - 1)] == -player) {
      return true;
    }
    if (InBounds(goal, c + 1) && b[BoardIndex(goal, c + 1)] == -player) {
      return true;
    }
  }

  return false;
}

int SideScore(const Board& b, int player) {
  int score = 0;
  int home = GoalRow(-player);
  int secondHome = home + ForwardDirection(player);

  for (int r = 0; r < 8; ++r) {
    for (int c = 0; c < 8; ++c) {
      if (b[BoardIndex(r, c)] != player) {
        continue;
      }

      score += MATERIAL_VALUE;
      score += ADVANCE_VALUE[Advancement(r, player)];

      if (IsProtected(b, r, c, player)) {
        score += PROTECTED_BONUS;
      } else if (IsAttacked(b, r, c, -player)) {
        score -= ATTACKED_PENALTY;
      }

      if (r == home || r == secondHome) {
        score += HOME_DEFENSE_BONUS;
      }
    }
  }

  // A file with no defender on either home rank is a lane an enemy pawn can
  // run down unopposed.
  for (int c = 0; c < 8; ++c) {
    if (b[BoardIndex(home, c)] != player &&
        b[BoardIndex(secondHome, c)] != player) {
      score -= HOME_HOLE_PENALTY;
    }
  }

  score += CountMoves(b, player) * MOBILITY_WEIGHT;

  return score;
}

int Evaluate(const Board& b, int player) {
  // The side to move promotes next ply, which ends the game.
  if (HasPromotingMove(b, player)) {
    return WIN - 100;
  }

  int score = SideScore(b, player) - SideScore(b, -player);

  // The opponent promotes unless our single remaining move stops it.
  if (HasPromotingMove(b, -player)) {
    score -= THREAT_PENALTY;
  }

  return score;
}

int MoveOrderScore(const Board& b, const Move& m) {
  int score = 0;

  if (m.promotes) {
    return 1000000;
  }

  int r = m.to / 8;
  int c = m.to % 8;

  if (m.capture) {
    score += 2000;
    // Capturing an unprotected pawn wins material outright.
    if (!IsProtected(b, r, c, -m.player)) {
      score += 1500;
    }
  }

  // Landing one row from the goal creates an immediate promotion threat.
  if (r == GoalRow(m.player) - ForwardDirection(m.player)) {
    score += 5000;
  }

  score += ADVANCE_VALUE[Advancement(r, m.player)];

  if (IsAttacked(b, r, c, -m.player) && !IsProtected(b, r, c, m.player)) {
    score -= 800;
  }

  return score;
}

void SortMoves(const Board& b, std::vector<Move>& moves) {
  std::sort(moves.begin(), moves.end(), [&b](const Move& x, const Move& y) {
    return MoveOrderScore(b, x) > MoveOrderScore(b, y);
  });
}

using Clock = std::chrono::steady_clock;

Clock::time_point g_deadline;
bool g_abort = false;
unsigned long long g_nodes = 0;

// Checked once per node; the clock itself is only read every 1024 nodes.
bool OutOfTime() {
  if (g_abort) {
    return true;
  }
  if ((++g_nodes & 1023u) == 0 && Clock::now() >= g_deadline) {
    g_abort = true;
  }
  return g_abort;
}

int Negamax(Board& b, int player, int depth, int alpha, int beta) {
  if (OutOfTime()) {
    return 0;
  }

  if (depth == 0) {
    return Evaluate(b, player);
  }

  std::vector<Move> moves = GenerateMoves(b, player);

  if (moves.empty()) {
    if (GenerateMoves(b, -player).empty()) {
      return 0;
    }
    return -WIN + depth;
  }

  SortMoves(b, moves);

  int best = -INF;

  for (const Move& m : moves) {
    int score;

    if (m.promotes) {
      score = WIN - depth;
    } else {
      MakeMove(b, m);
      score = -Negamax(b, -player, depth - 1, -beta, -alpha);
      UnmakeMove(b, m);
    }

    if (score > best) {
      best = score;
    }
    if (best > alpha) {
      alpha = best;
    }
    if (alpha >= beta) {
      break;
    }
  }

  return best;
}

Move ChooseMove(const Board& initial, int player, int timeMs) {
  std::vector<Move> moves = GenerateMoves(initial, player);

  if (moves.empty()) {
    return Move{};
  }

  for (const Move& m : moves) {
    if (m.promotes) {
      return m;
    }
  }

  SortMoves(initial, moves);

  g_deadline = Clock::now() + std::chrono::milliseconds(timeMs);
  g_abort = false;
  g_nodes = 0;

  const Clock::time_point start = Clock::now();
  const Clock::duration budget = g_deadline - start;

  static std::mt19937 rng{std::random_device{}()};

  Board b = initial;
  Move best = moves.front();

  for (int depth = 1; depth <= MAX_DEPTH; ++depth) {
    // Each iteration costs several times the previous one, so don't start one
    // that cannot plausibly finish.
    if (depth > 1 && (Clock::now() - start) * 2 > budget) {
      break;
    }

    std::vector<int> scores(moves.size(), -INF);
    int bestScore = -INF;
    bool completed = true;

    for (size_t i = 0; i < moves.size(); ++i) {
      // Keep the window slightly below the best score so moves within the
      // tie-break tolerance still come back with exact values.
      int alpha = (bestScore == -INF) ? -INF : bestScore - 2;

      MakeMove(b, moves[i]);
      int score = -Negamax(b, -player, depth - 1, -INF, -alpha);
      UnmakeMove(b, moves[i]);

      if (g_abort) {
        completed = false;
        break;
      }

      scores[i] = score;
      if (score > bestScore) {
        bestScore = score;
      }
    }

    if (!completed) {
      break;
    }

    std::vector<Move> bestMoves;

    for (size_t i = 0; i < moves.size(); ++i) {
      if (scores[i] >= bestScore - 1) {
        bestMoves.push_back(moves[i]);
      }
    }

    best = bestMoves[rng() % bestMoves.size()];

    // Searching deeper cannot change a forced result.
    if (bestScore >= WIN - MAX_DEPTH || bestScore <= -WIN + MAX_DEPTH) {
      break;
    }

    // Best-first ordering for the next, deeper iteration.
    std::vector<size_t> order(moves.size());
    for (size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&scores](size_t x, size_t y) {
      return scores[x] > scores[y];
    });

    std::vector<Move> reordered;
    reordered.reserve(moves.size());
    for (size_t i : order) {
      reordered.push_back(moves[i]);
    }
    moves.swap(reordered);
  }

  return best;
}

bool IsQuit(const std::string& line) {
  std::string s;

  for (char ch : line) {
    if (ch != ' ' && ch != '\t' && ch != '\r') {
      s.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }

  return s == "q" || s == "quit";
}

bool ParseMove(const std::string& line, Move& m, const Board& b, int player) {
  std::string s;

  for (char ch : line) {
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '-') {
      s.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }

  if (s.size() != 4) {
    return false;
  }

  if (!std::isalpha(static_cast<unsigned char>(s[0])) ||
      !std::isdigit(static_cast<unsigned char>(s[1]))) {
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(s[2])) ||
      !std::isdigit(static_cast<unsigned char>(s[3]))) {
    return false;
  }

  int fromC = s[0] - 'a';
  int fromR = s[1] - '1';
  int toC = s[2] - 'a';
  int toR = s[3] - '1';

  if (!InBounds(fromR, fromC) || !InBounds(toR, toC)) {
    return false;
  }

  int from = BoardIndex(fromR, fromC);
  int to = BoardIndex(toR, toC);

  std::vector<Move> moves = GenerateMoves(b, player);

  for (const Move& candidate : moves) {
    if (candidate.from == from && candidate.to == to) {
      m = candidate;
      return true;
    }
  }

  return false;
}

int RunInteractive(int timeMs) {
  Board board = InitialBoard();
  int turn = 1;

  std::cout << "Breakthrough interactive mode. You are White (W).\n";
  std::cout << "Enter moves like c2-c4 or c2c4; enter q to quit.\n";

  while (true) {
    PrintBoard(board);

    if (turn == 1) {
      std::cout << "Your move: " << std::flush;
      std::string line;
      if (!std::getline(std::cin, line)) {
        return 0;
      }
      if (IsQuit(line)) {
        return 0;
      }

      Move move;
      if (!ParseMove(line, move, board, turn)) {
        std::cout << "Invalid move. Try again.\n";
        continue;
      }

      MakeMove(board, move);
      if (move.promotes) {
        PrintBoard(board);
        std::cout << "You win by promotion!\n";
        return 0;
      }
    } else {
      std::cout << "AI thinking...\n";
      Move move = ChooseMove(board, turn, timeMs);
      if (move.from == -1) {
        std::cout << "AI has no legal moves. You win!\n";
        return 0;
      }

      std::cout << "AI plays " << MoveToString(move) << "\n";
      MakeMove(board, move);
      if (move.promotes) {
        PrintBoard(board);
        std::cout << "AI wins by promotion!\n";
        return 0;
      }
    }

    turn = -turn;
    if (GenerateMoves(board, turn).empty()) {
      PrintBoard(board);
      if (GenerateMoves(board, -turn).empty()) {
        std::cout << "Draw: no legal moves for either player.\n";
      } else if (turn == 1) {
        std::cout << "AI wins: you have no legal moves.\n";
      } else {
        std::cout << "You win: AI has no legal moves.\n";
      }
      return 0;
    }
  }
}

int main(int argc, char* argv[]) {
  int timeMs = ConfiguredTimeMs();
  if (argc > 1 && (std::string(argv[1]) == "-i" ||
                   std::string(argv[1]) == "--interactive")) {
    return RunInteractive(timeMs);
  }

  if (argc > 1) {
    std::cerr << "Usage: " << argv[0] << " [--interactive]\n";
    return 1;
  }

  std::string state;
  while (std::getline(std::cin, state)) {
    std::string history;
    if (!std::getline(std::cin, history)) {
      break;
    }

    if (!state.empty() && state.back() == '\r') {
      state.pop_back();
    }

    Board board{};
    int player = 0;
    if (!ParseState(state, board, player)) {
      return 1;
    }

    Move best = ChooseMove(board, player, timeMs);
    if (best.from == -1) {
      return 1;
    }

    std::cout << MoveToString(best) << std::endl;
  }

  return 0;
}
