#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

// Algorithm overview:
//
// A position is stored as two 64-bit boards, one per color. Bitwise shifts
// generate all forward moves and captures in parallel, and each move is packed
// into 16 bits. Iterative deepening searches one level deeper at a time and
// keeps the move from the deepest iteration that finishes before the deadline.
//
// Each iteration uses negamax with alpha-beta pruning. Principal variation
// search first tests later moves with a narrow score window, then repeats only
// promising moves with the full window. An aspiration window similarly starts
// each iteration near the previous score. A direct-mapped transposition table,
// addressed by an incrementally maintained Zobrist hash, reuses results when
// different move sequences reach the same position.
//
// Promotion threats receive extra search depth, while late quiet moves receive
// less depth unless they prove important. Killer moves, history scores,
// captures, and promotion threats determine move order. At leaf nodes, a short
// quiescence search follows forcing captures and promotion threats before the
// evaluator compares material, advancement, support, mobility, passed pawns,
// and home defense. These techniques concentrate the fixed time budget on the
// lines most likely to affect the chosen move.
//
// Bit 0 is a1 and bit 63 is h8. White advances by left shifts and Black by
// right shifts. A move packs its source in bits 6+ and target in bits 0-5.
using Clock = std::chrono::steady_clock;

constexpr uint64_t FILE_A = 0x0101010101010101ULL;
constexpr uint64_t FILE_H = 0x8080808080808080ULL;
constexpr uint64_t RANK_1 = 0x00000000000000ffULL;
constexpr uint64_t RANK_2 = 0x000000000000ff00ULL;
constexpr uint64_t RANK_7 = 0x00ff000000000000ULL;
constexpr uint64_t RANK_8 = 0xff00000000000000ULL;
constexpr int INF = 1'000'000;
constexpr int WIN = 100'000;
constexpr int DEFAULT_TIMEOUT_MS = 200;
constexpr int MAX_PLY = 96;
constexpr int TT_BITS = 20;
constexpr size_t TT_SIZE = 1ULL << TT_BITS;
constexpr size_t TT_MASK = TT_SIZE - 1;
constexpr uint16_t NO_MOVE = std::numeric_limits<uint16_t>::max();
constexpr std::array<int, 8> ADVANCE = {0, 5, 14, 28, 50, 90, 180, 600};

enum Bound : uint8_t { EXACT, LOWER, UPPER };

struct Position {
  uint64_t white = 0;
  uint64_t black = 0;
  uint64_t key = 0;
  int side = 1;
};

struct MoveList {
  // Sixteen pawns with at most three moves each bounds the list at 48.
  std::array<uint16_t, 48> moves{};
  int size = 0;

  void Push(uint16_t move) { moves[size++] = move; }
  uint16_t& operator[](int index) { return moves[index]; }
  uint16_t operator[](int index) const { return moves[index]; }
};

struct TTEntry {
  // The bound says whether score is exact, a lower bound, or an upper bound.
  uint64_t key = 0;
  int score = 0;
  uint16_t move = NO_MOVE;
  uint16_t generation = 0;
  int8_t depth = -1;
  Bound bound = EXACT;
};

std::array<std::array<uint64_t, 64>, 2> g_zobrist{};
uint64_t g_sideKey = 0;

// SplitMix64 deterministically assigns independent-looking keys to each
// (color, square) pair. Incremental updates avoid re-hashing every child.
uint64_t SplitMix64(uint64_t& state) {
  uint64_t value = (state += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

void InitializeZobrist() {
  uint64_t seed = 0x8f3f73b5cf1c9adeULL;
  for (auto& side : g_zobrist) {
    for (uint64_t& value : side) {
      value = SplitMix64(seed);
    }
  }
  g_sideKey = SplitMix64(seed);
}

int ConfiguredTimeMs() {
  // Leave enough of the referee's wall-clock budget for parsing and flushing.
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

inline uint64_t Bit(int square) { return 1ULL << square; }
inline uint16_t EncodeMove(int source, int target) {
  return static_cast<uint16_t>((source << 6) | target);
}
inline int MoveSource(uint16_t move) { return move >> 6; }
inline int MoveTarget(uint16_t move) { return move & 63; }

uint64_t CalculateKey(const Position& position) {
  uint64_t key = position.side == -1 ? g_sideKey : 0;
  uint64_t pieces = position.white;
  while (pieces) {
    int square = __builtin_ctzll(pieces);
    key ^= g_zobrist[0][square];
    pieces &= pieces - 1;
  }
  pieces = position.black;
  while (pieces) {
    int square = __builtin_ctzll(pieces);
    key ^= g_zobrist[1][square];
    pieces &= pieces - 1;
  }
  return key;
}

bool ParseState(const std::string& line, Position& position) {
  // Protocol rows run rank 1 to rank 8, matching increasing bit indices.
  size_t separator = line.find(' ');
  if (separator == std::string::npos ||
      line.find(' ', separator + 1) != std::string::npos) {
    return false;
  }
  std::string board = line.substr(0, separator);
  std::string side = line.substr(separator + 1);
  if (board.size() != 71 || side.size() != 1 ||
      (side[0] != 'W' && side[0] != 'B')) {
    return false;
  }

  position = Position{};
  int row = 0;
  int column = 0;
  for (char cell : board) {
    if (cell == '/') {
      if (column != 8 || ++row >= 8) {
        return false;
      }
      column = 0;
      continue;
    }
    if (column >= 8) {
      return false;
    }
    int square = row * 8 + column++;
    if (cell == 'W') {
      position.white |= Bit(square);
    } else if (cell == 'B') {
      position.black |= Bit(square);
    } else if (cell != '.') {
      return false;
    }
  }
  if (row != 7 || column != 8) {
    return false;
  }
  position.side = side[0] == 'W' ? 1 : -1;
  position.key = CalculateKey(position);
  return true;
}

std::string MoveToString(uint16_t move) {
  int source = MoveSource(move);
  int target = MoveTarget(move);
  std::string text;
  text += static_cast<char>('a' + source % 8);
  text += static_cast<char>('1' + source / 8);
  text += static_cast<char>('a' + target % 8);
  text += static_cast<char>('1' + target / 8);
  return text;
}

void AddTargets(MoveList& moves, uint64_t targets, int sourceOffset) {
  while (targets) {
    int target = __builtin_ctzll(targets);
    moves.Push(EncodeMove(target + sourceOffset, target));
    targets &= targets - 1;
  }
}

MoveList GenerateMoves(const Position& position) {
  // Shifts create all destinations in parallel; source offsets reconstruct
  // packed moves without scanning every pawn.
  MoveList moves;
  uint64_t empty = ~(position.white | position.black);
  if (position.side == 1) {
    AddTargets(moves, (position.white << 8) & empty, -8);
    uint64_t first = ((position.white & RANK_2) << 8) & empty;
    AddTargets(moves, (first << 8) & empty, -16);
    AddTargets(moves, ((position.white & ~FILE_A) << 7) & position.black, -7);
    AddTargets(moves, ((position.white & ~FILE_H) << 9) & position.black, -9);
  } else {
    AddTargets(moves, (position.black >> 8) & empty, 8);
    uint64_t first = ((position.black & RANK_7) >> 8) & empty;
    AddTargets(moves, (first >> 8) & empty, 16);
    AddTargets(moves, ((position.black & ~FILE_A) >> 9) & position.white, 9);
    AddTargets(moves, ((position.black & ~FILE_H) >> 7) & position.white, 7);
  }
  return moves;
}

Position ApplyMove(const Position& position, uint16_t move) {
  Position next = position;
  int source = MoveSource(move);
  int target = MoveTarget(move);
  uint64_t sourceMask = Bit(source);
  uint64_t targetMask = Bit(target);
  int ownIndex = position.side == 1 ? 0 : 1;
  int enemyIndex = ownIndex ^ 1;

  next.key ^=
      g_zobrist[ownIndex][source] ^ g_zobrist[ownIndex][target] ^ g_sideKey;
  if (position.side == 1) {
    next.white = (position.white ^ sourceMask) | targetMask;
    if (position.black & targetMask) {
      next.black ^= targetMask;
      next.key ^= g_zobrist[enemyIndex][target];
    }
  } else {
    next.black = (position.black ^ sourceMask) | targetMask;
    if (position.white & targetMask) {
      next.white ^= targetMask;
      next.key ^= g_zobrist[enemyIndex][target];
    }
  }
  next.side = -position.side;
  return next;
}

inline uint64_t Attacks(uint64_t pawns, int side) {
  if (side == 1) {
    return ((pawns & ~FILE_A) << 7) | ((pawns & ~FILE_H) << 9);
  }
  return ((pawns & ~FILE_A) >> 9) | ((pawns & ~FILE_H) >> 7);
}

bool HasPromotionMove(const Position& position, int side) {
  uint64_t occupied = position.white | position.black;
  if (side == 1) {
    uint64_t pawns = position.white & RANK_7;
    return (((pawns << 8) & ~occupied) |
            (Attacks(pawns, 1) & position.black & RANK_8)) != 0;
  }
  uint64_t pawns = position.black & RANK_2;
  return (((pawns >> 8) & ~occupied) |
          (Attacks(pawns, -1) & position.white & RANK_1)) != 0;
}

int Mobility(uint64_t own, uint64_t enemy, int side) {
  uint64_t empty = ~(own | enemy);
  int count;
  if (side == 1) {
    uint64_t first = ((own & RANK_2) << 8) & empty;
    count = __builtin_popcountll((own << 8) & empty) +
            __builtin_popcountll((first << 8) & empty);
  } else {
    uint64_t first = ((own & RANK_7) >> 8) & empty;
    count = __builtin_popcountll((own >> 8) & empty) +
            __builtin_popcountll((first >> 8) & empty);
  }
  return count + __builtin_popcountll(Attacks(own, side) & enemy);
}

int SideScore(uint64_t own, uint64_t enemy, int side) {
  // The evaluator rewards connected, mobile, advanced pawns while preserving
  // home files. Passed pawns receive a quadratic race bonus.
  uint64_t protectedPawns = own & Attacks(own, side);
  uint64_t attackedPawns = own & Attacks(enemy, -side);
  uint64_t home = side == 1 ? RANK_1 | RANK_2 : RANK_7 | RANK_8;
  uint64_t adjacent = ((own & ~FILE_A) >> 1) | ((own & ~FILE_H) << 1);
  int score = __builtin_popcountll(own) * 125;
  score += __builtin_popcountll(protectedPawns) * 20;
  score += __builtin_popcountll(own & adjacent) * 5;
  score -= __builtin_popcountll(attackedPawns & ~protectedPawns) * 18;
  score += Mobility(own, enemy, side) * 3;
  score += __builtin_popcountll(own & home) * 8;

  uint64_t occupied = own | enemy;
  uint64_t blocked = side == 1 ? own & (occupied >> 8) : own & (occupied << 8);
  score -= __builtin_popcountll(blocked) * 5;

  uint64_t pawns = own;
  while (pawns) {
    int square = __builtin_ctzll(pawns);
    int row = square / 8;
    int column = square % 8;
    int advance = side == 1 ? row : 7 - row;
    score += ADVANCE[advance];
    if (column >= 2 && column <= 5) {
      score += 4;
    }

    uint64_t files = 0;
    for (int nearby = std::max(0, column - 1);
         nearby <= std::min(7, column + 1); ++nearby) {
      files |= FILE_A << nearby;
    }
    uint64_t ahead;
    if (side == 1) {
      ahead = row == 7 ? 0 : ~((1ULL << ((row + 1) * 8)) - 1);
    } else {
      ahead = row == 0 ? 0 : (1ULL << (row * 8)) - 1;
    }
    if ((enemy & files & ahead) == 0) {
      score += 22 + advance * advance * 6;
    }
    pawns &= pawns - 1;
  }

  for (int column = 0; column < 8; ++column) {
    if ((own & home & (FILE_A << column)) == 0) {
      score -= 18;
    }
  }
  return score;
}

int Evaluate(const Position& position) {
  if (HasPromotionMove(position, position.side)) {
    return WIN - 100;
  }
  int score = SideScore(position.white, position.black, 1) -
              SideScore(position.black, position.white, -1);
  if (HasPromotionMove(position, -position.side)) {
    score += position.side == 1 ? -900 : 900;
  }
  return position.side == 1 ? score : -score;
}

inline bool CreatesThreat(uint16_t move, int side) {
  int row = MoveTarget(move) / 8;
  return (side == 1 && row >= 6) || (side == -1 && row <= 1);
}

class Searcher {
 public:
  Searcher() : table_(TT_SIZE) {}

  uint16_t ChooseMove(const Position& position, int timeMs) {
    MoveList moves = GenerateMoves(position);
    if (moves.size == 0) {
      return NO_MOVE;
    }
    uint64_t goal = position.side == 1 ? RANK_8 : RANK_1;
    // Promotion ends the game, so no search can improve on this move.
    for (int index = 0; index < moves.size; ++index) {
      if (Bit(MoveTarget(moves[index])) & goal) {
        return moves[index];
      }
    }

    start_ = Clock::now();
    deadline_ = start_ + std::chrono::milliseconds(timeMs);
    nodes_ = 0;
    completedDepth_ = 0;
    elapsedMs_ = 0.0;
    stopped_ = false;
    generation_ = static_cast<uint16_t>(generation_ + 1);
    if (generation_ == 0) {
      generation_ = 1;
    }
    for (auto& side : history_) {
      for (int& value : side) {
        value /= 2;
      }
    }
    for (auto& pair : killers_) {
      pair = {NO_MOVE, NO_MOVE};
    }

    uint16_t bestMove = moves[0];
    int previousScore = 0;
    // Keep the result from the deepest completed iteration. A timeout during
    // the next iteration cannot corrupt the fallback move.
    for (int depth = 1; depth <= 64; ++depth) {
      OrderMoves(position, moves, bestMove, 0);
      int window = depth >= 4 ? 70 : INF;
      int alpha = std::max(-INF, previousScore - window);
      int beta = std::min(INF, previousScore + window);
      uint16_t iterationMove = bestMove;
      int iterationScore = previousScore;

      // Search a narrow aspiration window around the previous score, widening
      // only when the position falls outside that expected range.
      while (true) {
        int windowAlpha = alpha;
        int windowBeta = beta;
        auto result = SearchRoot(position, moves, depth, alpha, beta);
        if (stopped_) {
          break;
        }
        iterationMove = result.first;
        iterationScore = result.second;
        if (iterationScore <= windowAlpha && windowAlpha > -INF) {
          alpha = -INF;
          beta = std::min(INF, iterationScore + 180);
          continue;
        }
        if (iterationScore >= windowBeta && windowBeta < INF) {
          alpha = std::max(-INF, iterationScore - 180);
          beta = INF;
          continue;
        }
        break;
      }
      if (stopped_) {
        break;
      }
      bestMove = iterationMove;
      previousScore = iterationScore;
      completedDepth_ = depth;
      MoveToFront(moves, bestMove);
      if (std::abs(previousScore) >= WIN - MAX_PLY) {
        break;
      }
    }
    elapsedMs_ =
        std::chrono::duration<double, std::milli>(Clock::now() - start_)
            .count();
    return bestMove;
  }

  int CompletedDepth() const { return completedDepth_; }
  uint64_t Nodes() const { return nodes_; }
  double ElapsedMs() const { return elapsedMs_; }

 private:
  std::vector<TTEntry> table_;
  std::array<std::array<int, 4096>, 2> history_{};
  std::array<std::array<uint16_t, 2>, MAX_PLY> killers_{};
  Clock::time_point start_{};
  Clock::time_point deadline_{};
  uint64_t nodes_ = 0;
  uint16_t generation_ = 0;
  int completedDepth_ = 0;
  double elapsedMs_ = 0.0;
  bool stopped_ = false;

  bool CheckTime() {
    ++nodes_;
    if ((nodes_ & 1023) == 0 && Clock::now() >= deadline_) {
      stopped_ = true;
    }
    return stopped_;
  }

  static int ScoreToTable(int score, int ply) {
    // Normalize forced-win scores to the root so entries remain valid when
    // reached at a different ply through a transposition.
    if (score > WIN - MAX_PLY) {
      return score + ply;
    }
    if (score < -WIN + MAX_PLY) {
      return score - ply;
    }
    return score;
  }

  static int ScoreFromTable(int score, int ply) {
    if (score > WIN - MAX_PLY) {
      return score - ply;
    }
    if (score < -WIN + MAX_PLY) {
      return score + ply;
    }
    return score;
  }

  int MoveOrderScore(const Position& position, uint16_t move, uint16_t ttMove,
                     int ply) const {
    // Good ordering is essential for alpha-beta: hash move, wins, captures,
    // learned quiet moves, then advancement.
    if (move == ttMove) {
      return 2'000'000;
    }
    int target = MoveTarget(move);
    uint64_t targetMask = Bit(target);
    uint64_t goal = position.side == 1 ? RANK_8 : RANK_1;
    if (targetMask & goal) {
      return 1'500'000;
    }
    int sideIndex = position.side == 1 ? 0 : 1;
    uint64_t enemy = position.side == 1 ? position.black : position.white;
    int score = history_[sideIndex][move];
    if (targetMask & enemy) {
      score += 220'000;
      uint64_t enemyPawns = enemy & ~targetMask;
      if ((Attacks(enemyPawns, -position.side) & targetMask) == 0) {
        score += 20'000;
      }
    }
    if (killers_[ply][0] == move) {
      score += 90'000;
    } else if (killers_[ply][1] == move) {
      score += 80'000;
    }
    if (CreatesThreat(move, position.side)) {
      score += 45'000;
    }
    int row = target / 8;
    score += (position.side == 1 ? row : 7 - row) * 100;
    return score;
  }

  void OrderMoves(const Position& position, MoveList& moves, uint16_t ttMove,
                  int ply) const {
    std::array<int, 48> scores{};
    for (int index = 0; index < moves.size; ++index) {
      scores[index] = MoveOrderScore(position, moves[index], ttMove, ply);
    }
    for (int index = 1; index < moves.size; ++index) {
      uint16_t move = moves[index];
      int score = scores[index];
      int insertion = index;
      while (insertion > 0 && scores[insertion - 1] < score) {
        moves[insertion] = moves[insertion - 1];
        scores[insertion] = scores[insertion - 1];
        --insertion;
      }
      moves[insertion] = move;
      scores[insertion] = score;
    }
  }

  static void MoveToFront(MoveList& moves, uint16_t move) {
    for (int index = 0; index < moves.size; ++index) {
      if (moves[index] != move) {
        continue;
      }
      for (int current = index; current > 0; --current) {
        moves[current] = moves[current - 1];
      }
      moves[0] = move;
      return;
    }
  }

  std::pair<uint16_t, int> SearchRoot(const Position& position, MoveList& moves,
                                      int depth, int alpha, int beta) {
    uint16_t bestMove = moves[0];
    int bestScore = -INF;
    uint64_t goal = position.side == 1 ? RANK_8 : RANK_1;
    for (int index = 0; index < moves.size; ++index) {
      uint16_t move = moves[index];
      int score;
      if (Bit(MoveTarget(move)) & goal) {
        score = WIN;
      } else {
        Position child = ApplyMove(position, move);
        if (index == 0) {
          score = -Negamax(child, depth - 1, -beta, -alpha, 1);
        } else {
          score = -Negamax(child, depth - 1, -alpha - 1, -alpha, 1);
          if (!stopped_ && score > alpha && score < beta) {
            score = -Negamax(child, depth - 1, -beta, -alpha, 1);
          }
        }
      }
      if (stopped_) {
        break;
      }
      if (score > bestScore) {
        bestScore = score;
        bestMove = move;
      }
      alpha = std::max(alpha, score);
      if (alpha >= beta) {
        break;
      }
    }
    return {bestMove, bestScore};
  }

  int Quiescence(const Position& position, int alpha, int beta, int ply,
                 int remaining) {
    // Extend only captures and near-promotion threats so leaf evaluations do
    // not stop in the middle of an immediate tactical exchange.
    if (CheckTime()) {
      return 0;
    }
    if (HasPromotionMove(position, position.side)) {
      return WIN - ply;
    }

    MoveList moves = GenerateMoves(position);
    if (moves.size == 0) {
      return -WIN + ply;
    }

    int standPat = Evaluate(position);
    if (standPat >= beta) {
      return standPat;
    }
    alpha = std::max(alpha, standPat);
    if (remaining == 0) {
      return standPat;
    }

    OrderMoves(position, moves, NO_MOVE, ply);
    uint64_t enemy = position.side == 1 ? position.black : position.white;
    for (int index = 0; index < moves.size; ++index) {
      uint16_t move = moves[index];
      uint64_t targetMask = Bit(MoveTarget(move));
      if ((targetMask & enemy) == 0 && !CreatesThreat(move, position.side)) {
        continue;
      }
      int score = -Quiescence(ApplyMove(position, move), -beta, -alpha, ply + 1,
                              remaining - 1);
      if (stopped_) {
        return 0;
      }
      if (score >= beta) {
        return score;
      }
      alpha = std::max(alpha, score);
    }
    return alpha;
  }

  int Negamax(const Position& position, int depth, int alpha, int beta,
              int ply) {
    if (CheckTime()) {
      return 0;
    }
    if (depth <= 0 || ply >= MAX_PLY - 1) {
      return Quiescence(position, alpha, beta, ply, 3);
    }

    int originalAlpha = alpha;
    int originalBeta = beta;
    TTEntry& entry = table_[position.key & TT_MASK];
    uint16_t ttMove = NO_MOVE;
    if (entry.depth >= 0 && entry.key == position.key) {
      ttMove = entry.move;
      int storedScore = ScoreFromTable(entry.score, ply);
      if (entry.depth >= depth) {
        if (entry.bound == EXACT) {
          return storedScore;
        }
        if (entry.bound == LOWER) {
          alpha = std::max(alpha, storedScore);
        }
        if (entry.bound == UPPER) {
          beta = std::min(beta, storedScore);
        }
        if (alpha >= beta) {
          return storedScore;
        }
      }
    }

    MoveList moves = GenerateMoves(position);
    if (moves.size == 0) {
      return -WIN + ply;
    }
    OrderMoves(position, moves, ttMove, ply);
    uint64_t goal = position.side == 1 ? RANK_8 : RANK_1;
    uint64_t enemy = position.side == 1 ? position.black : position.white;
    uint16_t bestMove = moves[0];
    int bestScore = -INF;

    for (int index = 0; index < moves.size; ++index) {
      uint16_t move = moves[index];
      uint64_t targetMask = Bit(MoveTarget(move));
      bool capture = (targetMask & enemy) != 0;
      int score;
      if (targetMask & goal) {
        score = WIN - ply;
      } else {
        Position child = ApplyMove(position, move);
        // Near-promotion threats get one extra ply. Late quiet moves are
        // reduced, but any reduced move that raises alpha is searched again.
        int extension = CreatesThreat(move, position.side) ? 1 : 0;
        int reduction =
            depth >= 4 && index >= 5 && !capture && extension == 0 ? 1 : 0;
        int nextDepth = depth - 1 + extension;
        if (index == 0) {
          score = -Negamax(child, nextDepth, -beta, -alpha, ply + 1);
        } else {
          score = -Negamax(child, nextDepth - reduction, -alpha - 1, -alpha,
                           ply + 1);
          if (!stopped_ && reduction && score > alpha) {
            score = -Negamax(child, nextDepth, -alpha - 1, -alpha, ply + 1);
          }
          if (!stopped_ && score > alpha && score < beta) {
            score = -Negamax(child, nextDepth, -beta, -alpha, ply + 1);
          }
        }
      }
      if (stopped_) {
        return 0;
      }
      if (score > bestScore) {
        bestScore = score;
        bestMove = move;
      }
      alpha = std::max(alpha, score);
      if (alpha >= beta) {
        if (!capture) {
          if (killers_[ply][0] != move) {
            killers_[ply][1] = killers_[ply][0];
            killers_[ply][0] = move;
          }
          int sideIndex = position.side == 1 ? 0 : 1;
          history_[sideIndex][move] =
              std::min(200'000, history_[sideIndex][move] + depth * depth);
        }
        break;
      }
    }

    Bound bound = bestScore <= originalAlpha
                      ? UPPER
                      : (bestScore >= originalBeta ? LOWER : EXACT);
    if (entry.generation != generation_ || depth >= entry.depth) {
      entry = TTEntry{position.key,
                      ScoreToTable(bestScore, ply),
                      bestMove,
                      generation_,
                      static_cast<int8_t>(std::min(depth, 127)),
                      bound};
    }
    return bestScore;
  }
};

std::vector<uint16_t> ReferenceMoves(const Position& position) {
  // Deliberately simple square-by-square generator used only by --self-test.
  uint64_t own = position.side == 1 ? position.white : position.black;
  uint64_t enemy = position.side == 1 ? position.black : position.white;
  std::vector<uint16_t> result;
  int direction = position.side;
  int startRow = position.side == 1 ? 1 : 6;
  for (int source = 0; source < 64; ++source) {
    if ((own & Bit(source)) == 0) {
      continue;
    }
    int row = source / 8;
    int column = source % 8;
    int nextRow = row + direction;
    if (nextRow < 0 || nextRow >= 8) {
      continue;
    }
    int forward = nextRow * 8 + column;
    if (((own | enemy) & Bit(forward)) == 0) {
      result.push_back(EncodeMove(source, forward));
      if (row == startRow) {
        int target = (row + 2 * direction) * 8 + column;
        if (((own | enemy) & Bit(target)) == 0) {
          result.push_back(EncodeMove(source, target));
        }
      }
    }
    for (int delta : {-1, 1}) {
      int targetColumn = column + delta;
      if (targetColumn < 0 || targetColumn >= 8) {
        continue;
      }
      int target = nextRow * 8 + targetColumn;
      if (enemy & Bit(target)) {
        result.push_back(EncodeMove(source, target));
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool RunSelfTest() {
  // Compare optimized generation and incremental hashes across deterministic
  // pseudo-random positions, making engine invariants reproducible.
  uint64_t random = 0x123456789abcdef0ULL;
  for (int index = 0; index < 10'000; ++index) {
    random = random * 6364136223846793005ULL + 1;
    Position position;
    position.white = random;
    random = random * 6364136223846793005ULL + 1;
    position.black = random & ~position.white;
    position.side = index % 2 == 0 ? 1 : -1;
    position.key = CalculateKey(position);
    MoveList generated = GenerateMoves(position);
    std::vector<uint16_t> actual(generated.moves.begin(),
                                 generated.moves.begin() + generated.size);
    std::sort(actual.begin(), actual.end());
    if (actual != ReferenceMoves(position)) {
      return false;
    }
    if (generated.size > 0) {
      Position child = ApplyMove(position, generated[0]);
      if (child.key != CalculateKey(child)) {
        return false;
      }
    }
  }
  return true;
}

int RunBenchmark() {
  Position position;
  const std::string state =
      "WWWWWWWW/WWWWWWWW/......../......../......../......../BBBBBBBB/"
      "BBBBBBBB W";
  if (!ParseState(state, position)) {
    return 1;
  }
  Searcher searcher;
  uint16_t move = searcher.ChooseMove(position, 1000);
  if (move == NO_MOVE) {
    return 1;
  }
  double seconds = searcher.ElapsedMs() / 1000.0;
  uint64_t nodesPerSecond =
      seconds > 0.0 ? static_cast<uint64_t>(searcher.Nodes() / seconds) : 0;
  std::cout << "move=" << MoveToString(move)
            << " depth=" << searcher.CompletedDepth()
            << " nodes=" << searcher.Nodes() << " nps=" << nodesPerSecond
            << " elapsed_ms=" << static_cast<uint64_t>(searcher.ElapsedMs())
            << '\n';
  return 0;
}

int main(int argc, char* argv[]) {
  InitializeZobrist();
  if (argc == 2 && std::string(argv[1]) == "--self-test") {
    if (!RunSelfTest()) {
      std::cerr << "self-test failed\n";
      return 1;
    }
    std::cout << "self-test passed\n";
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--benchmark") {
    return RunBenchmark();
  }
  if (argc != 1) {
    std::cerr << "Usage: " << argv[0] << " [--self-test|--benchmark]\n";
    return 1;
  }

  Searcher searcher;
  int timeMs = ConfiguredTimeMs();
  std::string state;
  std::string history;
  while (std::getline(std::cin, state)) {
    // Every request is exactly two lines; only the move is written to stdout.
    if (!std::getline(std::cin, history)) {
      return 0;
    }
    if (!state.empty() && state.back() == '\r') {
      state.pop_back();
    }
    Position position;
    if (!ParseState(state, position)) {
      return 1;
    }
    uint16_t move = searcher.ChooseMove(position, timeMs);
    if (move == NO_MOVE) {
      return 1;
    }
    std::cout << MoveToString(move) << std::endl;
  }
  return 0;
}
