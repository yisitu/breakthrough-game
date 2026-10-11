#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Speed-first Breakthrough engine.
//
// The search deliberately uses a cheaper evaluator to spend more of each move
// budget descending the tree. Positions use one 64-bit board per color, moves
// use 16 bits, and child positions update their Zobrist keys incrementally.
// Iterative deepening keeps the deepest completed result when time expires.
//
// Negamax uses alpha-beta pruning, principal variation search, aspiration
// windows, a cache-sized direct-mapped transposition table, staged move
// ordering, and late-move reductions. Unlike the tournament-focused C++ bot,
// this engine has no quiescence search or threat extension. Its leaf evaluator
// performs a fixed number of bit operations: material, rank-weighted
// advancement, pawn support, exposed pawns, home coverage, and immediate
// promotion threats. It never loops over individual pawns or scans files for
// passed pawns.
//
// Bit 0 is a1 and bit 63 is h8. White shifts toward larger indices; Black
// shifts toward smaller indices. A move stores its source in bits 6+ and its
// target in bits 0-5.
using Clock = std::chrono::steady_clock;

constexpr uint64_t FILE_A = 0x0101010101010101ULL;
constexpr uint64_t FILE_H = 0x8080808080808080ULL;
constexpr uint64_t RANK_1 = 0x00000000000000ffULL;
constexpr uint64_t RANK_2 = 0x000000000000ff00ULL;
constexpr uint64_t RANK_3 = 0x0000000000ff0000ULL;
constexpr uint64_t RANK_4 = 0x00000000ff000000ULL;
constexpr uint64_t RANK_5 = 0x000000ff00000000ULL;
constexpr uint64_t RANK_6 = 0x0000ff0000000000ULL;
constexpr uint64_t RANK_7 = 0x00ff000000000000ULL;
constexpr uint64_t RANK_8 = 0xff00000000000000ULL;
constexpr std::array<uint64_t, 8> RANKS = {RANK_1, RANK_2, RANK_3, RANK_4,
                                           RANK_5, RANK_6, RANK_7, RANK_8};
constexpr std::array<int, 8> ADVANCE = {0, 4, 12, 26, 52, 105, 220, 700};
constexpr int INF = 1'000'000;
constexpr int WIN = 100'000;
constexpr int DEFAULT_TIMEOUT_MS = 200;
constexpr int MAX_PLY = 96;
constexpr int TT_BITS = 16;
constexpr size_t TT_SIZE = 1ULL << TT_BITS;
constexpr size_t TT_MASK = TT_SIZE - 1;
constexpr uint16_t NO_MOVE = std::numeric_limits<uint16_t>::max();

enum Bound : uint8_t { EXACT, LOWER, UPPER };

struct Position {
  uint64_t white = 0;
  uint64_t black = 0;
  uint64_t key = 0;
  int side = 1;
};

struct MoveList {
  std::array<uint16_t, 48> moves{};
  int size = 0;

  void Push(uint16_t move) { moves[size++] = move; }
  uint16_t& operator[](int index) { return moves[index]; }
  uint16_t operator[](int index) const { return moves[index]; }
};

struct TTEntry {
  uint64_t key = 0;
  int score = 0;
  uint16_t move = NO_MOVE;
  uint16_t generation = 0;
  int8_t depth = -1;
  Bound bound = EXACT;
};

std::array<std::array<uint64_t, 64>, 2> g_zobrist{};
uint64_t g_sideKey = 0;

uint64_t SplitMix64(uint64_t& state) {
  uint64_t value = (state += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

void InitializeZobrist() {
  uint64_t seed = 0x64f00d129b8a7c31ULL;
  for (auto& side : g_zobrist) {
    for (uint64_t& value : side) {
      value = SplitMix64(seed);
    }
  }
  g_sideKey = SplitMix64(seed);
}

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

inline uint64_t Bit(int square) { return 1ULL << square; }
inline uint16_t EncodeMove(int source, int target) {
  return static_cast<uint16_t>((source << 6) | target);
}
inline int MoveSource(uint16_t move) { return move >> 6; }
inline int MoveTarget(uint16_t move) { return move & 63; }

uint64_t CalculateKey(const Position& position) {
  uint64_t key = position.side == -1 ? g_sideKey : 0;
  uint64_t pieces = position.white;
  while (pieces != 0) {
    int square = __builtin_ctzll(pieces);
    key ^= g_zobrist[0][square];
    pieces &= pieces - 1;
  }
  pieces = position.black;
  while (pieces != 0) {
    int square = __builtin_ctzll(pieces);
    key ^= g_zobrist[1][square];
    pieces &= pieces - 1;
  }
  return key;
}

bool ParseState(const std::string& line, Position& position) {
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
  while (targets != 0) {
    int target = __builtin_ctzll(targets);
    moves.Push(EncodeMove(target + sourceOffset, target));
    targets &= targets - 1;
  }
}

MoveList GenerateMoves(const Position& position) {
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
    if ((position.black & targetMask) != 0) {
      next.black ^= targetMask;
      next.key ^= g_zobrist[enemyIndex][target];
    }
  } else {
    next.black = (position.black ^ sourceMask) | targetMask;
    if ((position.white & targetMask) != 0) {
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

int FastSideScore(uint64_t own, uint64_t enemy, int side) {
  int score = __builtin_popcountll(own) * 128;
  uint64_t protectedPawns = own & Attacks(own, side);
  uint64_t attackedPawns = own & Attacks(enemy, -side);
  score += __builtin_popcountll(protectedPawns) * 16;
  score -= __builtin_popcountll(attackedPawns & ~protectedPawns) * 14;

  for (int row = 0; row < 8; ++row) {
    int advance = side == 1 ? row : 7 - row;
    score += __builtin_popcountll(own & RANKS[row]) * ADVANCE[advance];
  }

  uint64_t home = side == 1 ? RANK_1 | RANK_2 : RANK_7 | RANK_8;
  score += __builtin_popcountll(own & home) * 6;
  return score;
}

int Evaluate(const Position& position) {
  if (HasPromotionMove(position, position.side)) {
    return WIN - 100;
  }
  int score = FastSideScore(position.white, position.black, 1) -
              FastSideScore(position.black, position.white, -1);
  if (HasPromotionMove(position, -position.side)) {
    score += position.side == 1 ? -850 : 850;
  }
  return position.side == 1 ? score : -score;
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
    for (int index = 0; index < moves.size; ++index) {
      if ((Bit(MoveTarget(moves[index])) & goal) != 0) {
        return moves[index];
      }
    }

    StartSearch(timeMs);
    uint16_t bestMove = moves[0];
    int previousScore = 0;
    for (int depth = 1; depth <= 64; ++depth) {
      OrderMoves(position, moves, bestMove, 0);
      int window = depth >= 4 ? 64 : INF;
      int alpha = std::max(-INF, previousScore - window);
      int beta = std::min(INF, previousScore + window);
      uint16_t iterationMove = bestMove;
      int iterationScore = previousScore;

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
          beta = std::min(INF, iterationScore + 160);
          continue;
        }
        if (iterationScore >= windowBeta && windowBeta < INF) {
          alpha = std::max(-INF, iterationScore - 160);
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

  void StartSearch(int timeMs) {
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
  }

  bool CheckTime() {
    ++nodes_;
    if ((nodes_ & 2047) == 0 && Clock::now() >= deadline_) {
      stopped_ = true;
    }
    return stopped_;
  }

  static int ScoreToTable(int score, int ply) {
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

  int MoveStage(const Position& position, uint16_t move, uint16_t ttMove,
                int ply) const {
    if (move == ttMove) {
      return 0;
    }
    int target = MoveTarget(move);
    uint64_t targetMask = Bit(target);
    uint64_t goal = position.side == 1 ? RANK_8 : RANK_1;
    if ((targetMask & goal) != 0) {
      return 1;
    }

    int sideIndex = position.side == 1 ? 0 : 1;
    uint64_t enemy = position.side == 1 ? position.black : position.white;
    if ((targetMask & enemy) != 0) {
      return 2;
    }
    if (killers_[ply][0] == move) {
      return 3;
    }
    if (killers_[ply][1] == move) {
      return 4;
    }
    if (history_[sideIndex][move] > 0) {
      return 5;
    }

    int row = target / 8;
    int advance = position.side == 1 ? row : 7 - row;
    return advance >= 5 ? 6 : 7;
  }

  void OrderMoves(const Position& position, MoveList& moves, uint16_t ttMove,
                  int ply) const {
    // Full sorting dominated per-node latency. Stable buckets preserve the
    // categories that drive alpha-beta cutoffs while doing two linear passes.
    constexpr int STAGE_COUNT = 8;
    std::array<uint8_t, 48> stages{};
    std::array<int, STAGE_COUNT> counts{};
    for (int index = 0; index < moves.size; ++index) {
      int stage = MoveStage(position, moves[index], ttMove, ply);
      stages[index] = static_cast<uint8_t>(stage);
      ++counts[stage];
    }

    std::array<int, STAGE_COUNT> offsets{};
    for (int stage = 1; stage < STAGE_COUNT; ++stage) {
      offsets[stage] = offsets[stage - 1] + counts[stage - 1];
    }

    std::array<uint16_t, 48> ordered{};
    for (int index = 0; index < moves.size; ++index) {
      int stage = stages[index];
      ordered[offsets[stage]++] = moves[index];
    }
    for (int index = 0; index < moves.size; ++index) {
      moves[index] = ordered[index];
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
      if ((Bit(MoveTarget(move)) & goal) != 0) {
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

  int Negamax(const Position& position, int depth, int alpha, int beta,
              int ply) {
    if (CheckTime()) {
      return 0;
    }
    if (depth <= 0 || ply >= MAX_PLY - 1) {
      return Evaluate(position);
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
      if ((targetMask & goal) != 0) {
        score = WIN - ply;
      } else {
        Position child = ApplyMove(position, move);
        int reduction = 0;
        if (depth >= 5 && index >= 8 && !capture) {
          reduction = 2;
        } else if (depth >= 3 && index >= 4 && !capture) {
          reduction = 1;
        }
        int nextDepth = std::max(0, depth - 1 - reduction);
        if (index == 0) {
          score = -Negamax(child, depth - 1, -beta, -alpha, ply + 1);
        } else {
          score = -Negamax(child, nextDepth, -alpha - 1, -alpha, ply + 1);
          if (!stopped_ && reduction != 0 && score > alpha) {
            score = -Negamax(child, depth - 1, -alpha - 1, -alpha, ply + 1);
          }
          if (!stopped_ && score > alpha && score < beta) {
            score = -Negamax(child, depth - 1, -beta, -alpha, ply + 1);
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
      if ((enemy & Bit(target)) != 0) {
        result.push_back(EncodeMove(source, target));
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool RunSelfTest() {
  uint64_t random = 0xa73c5f19d428be60ULL;
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
    for (int moveIndex = 0; moveIndex < generated.size; ++moveIndex) {
      Position child = ApplyMove(position, generated[moveIndex]);
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
