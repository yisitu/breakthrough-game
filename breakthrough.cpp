#include <iostream>
#include <array>
#include <vector>
#include <algorithm>
#include <random>
#include <string>
#include <cctype>
#include <limits>

using namespace std;

const int INF = 1000000;
const int WIN = 100000;
const int AI_DEPTH = 5;

using Board = array<int, 64>;

struct Move {
    int from = -1;
    int to = -1;
    bool capture = false;
    bool promotes = false;
    int player = 0;
};

int idx(int r, int c) {
    return r * 8 + c;
}

bool inBounds(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

Board initialBoard() {
    Board b{};
    for (int c = 0; c < 8; ++c) {
        b[idx(5, c)] = 1;   // White starts on rank 3
        b[idx(6, c)] = 1;   // White starts on rank 2
        b[idx(1, c)] = -1;  // Black starts on rank 7
        b[idx(2, c)] = -1;  // Black starts on rank 6
    }
    return b;
}

void printBoard(const Board& b) {
    cout << "  a b c d e f g h\n";
    for (int r = 0; r < 8; ++r) {
        cout << 8 - r << " ";
        for (int c = 0; c < 8; ++c) {
            int v = b[idx(r, c)];
            if (v == 1) cout << "W";
            else if (v == -1) cout << "B";
            else cout << ".";
            if (c + 1 < 8) cout << " ";
        }
        cout << "\n";
    }
}

vector<Move> generateMoves(const Board& b, int player) {
    vector<Move> moves;
    int dir = (player == 1 ? -1 : 1);

    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            if (b[idx(r, c)] != player) continue;

            auto add = [&](int nr, int nc, bool capture) {
                if (!inBounds(nr, nc)) return;

                int to = idx(nr, nc);

                if (!capture) {
                    if (b[to] != 0) return;
                } else {
                    if (b[to] != -player) return;
                }

                Move m;
                m.from = idx(r, c);
                m.to = to;
                m.capture = capture;
                m.player = player;
                m.promotes = (player == 1 && nr == 0) || (player == -1 && nr == 7);
                moves.push_back(m);
            };

            add(r + dir, c, false);
            add(r + dir, c - 1, true);
            add(r + dir, c + 1, true);
        }
    }

    return moves;
}

void makeMove(Board& b, const Move& m) {
    b[m.to] = m.player;
    b[m.from] = 0;
}

void unmakeMove(Board& b, const Move& m) {
    b[m.from] = m.player;
    b[m.to] = m.capture ? -m.player : 0;
}

int countMoves(const Board& b, int player) {
    int count = 0;
    int dir = (player == 1 ? -1 : 1);

    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            if (b[idx(r, c)] != player) continue;

            auto inc = [&](int nr, int nc, bool capture) {
                if (!inBounds(nr, nc)) return;

                int to = idx(nr, nc);

                if (!capture) {
                    if (b[to] == 0) ++count;
                } else {
                    if (b[to] == -player) ++count;
                }
            };

            inc(r + dir, c, false);
            inc(r + dir, c - 1, true);
            inc(r + dir, c + 1, true);
        }
    }

    return count;
}

int evaluate(const Board& b, int player) {
    int whiteAdv = 0;
    int blackAdv = 0;

    for (int i = 0; i < 64; ++i) {
        int v = b[i];
        int r = i / 8;

        if (v == 1) {
            whiteAdv += 7 - r;
        } else if (v == -1) {
            blackAdv += r;
        }
    }

    int whiteMob = countMoves(b, 1);
    int blackMob = countMoves(b, -1);

    int whiteScore = whiteAdv * 10 + whiteMob;
    int blackScore = blackAdv * 10 + blackMob;

    if (player == 1) {
        return whiteScore - blackScore;
    }
    return blackScore - whiteScore;
}

int moveOrderScore(const Move& m) {
    int score = 0;

    if (m.promotes) score += 100000;
    if (m.capture) score += 1000;

    int r = m.to / 8;

    if (m.player == 1) {
        score += (7 - r) * 100;
    } else {
        score += r * 100;
    }

    return score;
}

void sortMoves(vector<Move>& moves) {
    sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) {
        return moveOrderScore(a) > moveOrderScore(b);
    });
}

int negamax(Board& b, int player, int depth, int alpha, int beta) {
    if (depth == 0) {
        return evaluate(b, player);
    }

    vector<Move> moves = generateMoves(b, player);

    if (moves.empty()) {
        if (generateMoves(b, -player).empty()) {
            return 0;
        }
        return -WIN + depth;
    }

    sortMoves(moves);

    int best = -INF;

    for (const Move& m : moves) {
        int score;

        if (m.promotes) {
            score = WIN - depth;
        } else {
            makeMove(b, m);
            score = -negamax(b, -player, depth - 1, -beta, -alpha);
            unmakeMove(b, m);
        }

        if (score > best) best = score;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }

    return best;
}

Move chooseMove(const Board& initial, int player, int depth) {
    vector<Move> moves = generateMoves(initial, player);

    if (moves.empty()) {
        return Move{};
    }

    sortMoves(moves);

    Board b = initial;
    int bestScore = -INF;
    vector<Move> bestMoves;

    for (const Move& m : moves) {
        int score;

        if (m.promotes) {
            score = WIN;
        } else {
            makeMove(b, m);
            score = -negamax(b, -player, depth - 1, -INF, INF);
            unmakeMove(b, m);
        }

        if (score > bestScore + 1) {
            bestScore = score;
            bestMoves.clear();
            bestMoves.push_back(m);
        } else if (score >= bestScore - 1) {
            bestMoves.push_back(m);
        }
    }

    if (bestMoves.empty()) {
        return Move{};
    }

    static mt19937 rng{random_device{}()};
    return bestMoves[rng() % bestMoves.size()];
}

bool isQuit(const string& line) {
    string s;

    for (char ch : line) {
        if (ch != ' ' && ch != '\t' && ch != '\r') {
            s.push_back(static_cast<char>(tolower(static_cast<unsigned char>(ch))));
        }
    }

    return s == "q" || s == "quit";
}

bool parseMove(const string& line, Move& m, const Board& b, int player) {
    string s;

    for (char ch : line) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '-') {
            s.push_back(static_cast<char>(tolower(static_cast<unsigned char>(ch))));
        }
    }

    if (s.size() != 4) return false;

    if (!isalpha(static_cast<unsigned char>(s[0])) || !isdigit(static_cast<unsigned char>(s[1]))) return false;
    if (!isalpha(static_cast<unsigned char>(s[2])) || !isdigit(static_cast<unsigned char>(s[3]))) return false;

    int fromC = s[0] - 'a';
    int fromR = 8 - (s[1] - '0');
    int toC = s[2] - 'a';
    int toR = 8 - (s[3] - '0');

    if (!inBounds(fromR, fromC) || !inBounds(toR, toC)) return false;

    int from = idx(fromR, fromC);
    int to = idx(toR, toC);

    vector<Move> moves = generateMoves(b, player);

    for (const Move& candidate : moves) {
        if (candidate.from == from && candidate.to == to) {
            m = candidate;
            return true;
        }
    }

    return false;
}

int main() {
    cout << "Breakthrough: You are White (W), AI is Black (B).\n";
    cout << "First to promote a pawn wins. Enter moves like c2-c3 or c2c3.\n";

    Board board = initialBoard();
    int turn = 1;
    bool gameOver = false;
    string result;

    while (!gameOver) {
        printBoard(board);

        if (turn == 1) {
            cout << "Your move (e.g. c2-c3, or q to quit): ";
            string line;

            if (!getline(cin, line)) {
                break;
            }

            if (isQuit(line)) {
                cout << "Quitting.\n";
                printBoard(board);
                return 0;
            }

            Move m;

            if (!parseMove(line, m, board, 1)) {
                cout << "Invalid move. Try again.\n";
                continue;
            }

            makeMove(board, m);

            if (m.promotes) {
                gameOver = true;
                result = "You win by promotion!";
                break;
            }

            turn = -1;
        } else {
            cout << "AI thinking...\n";
            Move m = chooseMove(board, -1, AI_DEPTH);

            if (m.from == -1) {
                gameOver = true;
                result = "AI has no legal moves. You win!";
                break;
            }

            makeMove(board, m);

            if (m.promotes) {
                gameOver = true;
                result = "AI wins by promotion!";
                break;
            }

            turn = 1;
        }

        if (generateMoves(board, turn).empty()) {
            if (generateMoves(board, -turn).empty()) {
                gameOver = true;
                result = "Draw: no legal moves for either player.";
            } else {
                gameOver = true;

                if (turn == 1) {
                    result = "AI wins: you have no legal moves.";
                } else {
                    result = "You win: AI has no legal moves.";
                }
            }

            break;
        }
    }

    printBoard(board);

    if (gameOver) {
        cout << result << "\n";
    }

    return 0;
}
