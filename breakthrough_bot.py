#!/usr/bin/env python3
"""Time-bounded Breakthrough bot using bitboards and iterative deepening.

The board is represented by one 64-bit integer per color. Bitwise shifts
generate many pawn moves at once, and every move packs its source and target
into one integer. The bot searches at increasing depths until its deadline and
returns the move from the deepest iteration that completed in time.

Each iteration uses negamax with alpha-beta pruning. Principal variation search
checks later moves with a narrow score window and repeats only unexpectedly good
moves with the full window. Aspiration windows begin each iteration near the
previous score. A transposition table reuses results for positions reached by
different move orders, while killer and history heuristics search moves that
caused earlier cutoffs first. Late quiet moves receive a shallower initial
search, and near-promotion threats receive extra depth.

At the depth limit, the evaluator weighs material, advancement, connected and
protected pawns, mobility, passed pawns, home defense, and immediate promotion
threats. A timeout exception unwinds the active search without replacing the
last fully completed result.
"""

import os
import sys
import time

# Bit 0 is a1, bit 7 is h1, and bit 63 is h8. White shifts toward
# more-significant bits; Black shifts toward less-significant bits.
FULL = (1 << 64) - 1
FILE_A = 0x0101010101010101
FILE_H = 0x8080808080808080
RANK_1 = 0x00000000000000FF
RANK_2 = 0x000000000000FF00
RANK_7 = 0x00FF000000000000
RANK_8 = 0xFF00000000000000
INF = 1_000_000
WIN = 100_000
MAX_DEPTH = 64
ADVANCE = (0, 5, 13, 25, 43, 78, 155, 500)

EXACT = 0
LOWER = 1
UPPER = 2

_deadline = 0.0
_nodes = 0
# Search state is module-local to avoid threading it through every recursive
# call. TT values are (depth, score, bound flag, preferred move).
_tt: dict[tuple[int, int, int], tuple[int, int, int, int]] = {}
# History and killer heuristics prioritize quiet moves that caused prior cuts.
_history = [[0] * 4096 for _ in range(2)]
_killers = [[-1, -1] for _ in range(MAX_DEPTH + 2)]


class SearchTimeout(Exception):
    pass


def configured_time_limit() -> float:
    """Returns the search budget after reserving time for protocol overhead."""
    try:
        timeout_ms = int(os.environ.get("BREAKTHROUGH_TIMEOUT_MS", "200"))
    except ValueError:
        timeout_ms = 200
    timeout_ms = max(1, timeout_ms)
    margin_ms = min(timeout_ms - 1, max(20, timeout_ms // 5))
    return max(1, timeout_ms - margin_ms) / 1000


def parse_state(state: str) -> tuple[int, int, int]:
    """Parses the harness state into White/Black bitboards and side-to-move."""
    board_text, turn_text = state.split()
    rows = board_text.split("/")
    if len(rows) != 8 or any(len(row) != 8 for row in rows):
        raise ValueError("invalid board")

    white = 0
    black = 0
    for row, cells in enumerate(rows):
        for col, cell in enumerate(cells):
            bit = 1 << (row * 8 + col)
            if cell == "W":
                white |= bit
            elif cell == "B":
                black |= bit
            elif cell != ".":
                raise ValueError("invalid piece")
    if turn_text not in ("W", "B"):
        raise ValueError("invalid side")
    return white, black, 1 if turn_text == "W" else -1


def generate_moves(white: int, black: int, side: int) -> list[int]:
    """Generates packed moves, with source in bits 6+ and target in bits 0-5."""
    occupied = white | black
    empty = FULL ^ occupied
    moves: list[int] = []

    if side == 1:
        single = (white << 8) & empty & FULL
        targets = single
        while targets:
            to_bit = targets & -targets
            to = to_bit.bit_length() - 1
            moves.append(((to - 8) << 6) | to)
            targets ^= to_bit

        first = ((white & RANK_2) << 8) & empty
        targets = (first << 8) & empty
        while targets:
            to_bit = targets & -targets
            to = to_bit.bit_length() - 1
            moves.append(((to - 16) << 6) | to)
            targets ^= to_bit

        targets = ((white & ~FILE_A) << 7) & black
        while targets:
            to_bit = targets & -targets
            to = to_bit.bit_length() - 1
            moves.append(((to - 7) << 6) | to)
            targets ^= to_bit

        targets = ((white & ~FILE_H) << 9) & black
        while targets:
            to_bit = targets & -targets
            to = to_bit.bit_length() - 1
            moves.append(((to - 9) << 6) | to)
            targets ^= to_bit
    else:
        single = (black >> 8) & empty
        targets = single
        while targets:
            to_bit = targets & -targets
            to = to_bit.bit_length() - 1
            moves.append(((to + 8) << 6) | to)
            targets ^= to_bit

        first = ((black & RANK_7) >> 8) & empty
        targets = (first >> 8) & empty
        while targets:
            to_bit = targets & -targets
            to = to_bit.bit_length() - 1
            moves.append(((to + 16) << 6) | to)
            targets ^= to_bit

        targets = ((black & ~FILE_A) >> 9) & white
        while targets:
            to_bit = targets & -targets
            to = to_bit.bit_length() - 1
            moves.append(((to + 9) << 6) | to)
            targets ^= to_bit

        targets = ((black & ~FILE_H) >> 7) & white
        while targets:
            to_bit = targets & -targets
            to = to_bit.bit_length() - 1
            moves.append(((to + 7) << 6) | to)
            targets ^= to_bit

    return moves


def apply_move(white: int, black: int, move: int, side: int) -> tuple[int, int]:
    source = move >> 6
    target = move & 63
    source_bit = 1 << source
    target_bit = 1 << target
    if side == 1:
        return (white ^ source_bit) | target_bit, black & ~target_bit
    return white & ~target_bit, (black ^ source_bit) | target_bit


def attacks(pawns: int, side: int) -> int:
    if side == 1:
        return (((pawns & ~FILE_A) << 7) | ((pawns & ~FILE_H) << 9)) & FULL
    return ((pawns & ~FILE_A) >> 9) | ((pawns & ~FILE_H) >> 7)


def has_promotion_move(white: int, black: int, side: int) -> bool:
    occupied = white | black
    if side == 1:
        pawns = white & RANK_7
        return bool(((pawns << 8) & ~occupied) |
                    (attacks(pawns, 1) & black & RANK_8))
    pawns = black & RANK_2
    return bool(((pawns >> 8) & ~occupied) |
                (attacks(pawns, -1) & white & RANK_1))


def mobility(own: int, enemy: int, side: int) -> int:
    empty = FULL ^ (own | enemy)
    if side == 1:
        count = ((own << 8) & empty & FULL).bit_count()
        first = ((own & RANK_2) << 8) & empty
        count += ((first << 8) & empty).bit_count()
    else:
        count = ((own >> 8) & empty).bit_count()
        first = ((own & RANK_7) >> 8) & empty
        count += ((first >> 8) & empty).bit_count()
    return count + (attacks(own, side) & enemy).bit_count()


def side_score(own: int, enemy: int, side: int) -> int:
    """Scores material, structure, mobility, advancement, and passed pawns."""
    score = own.bit_count() * 120
    protected = own & attacks(own, side)
    attacked = own & attacks(enemy, -side)
    score += protected.bit_count() * 18
    score -= (attacked & ~protected).bit_count() * 15
    score += mobility(own, enemy, side) * 3

    home = (RANK_1 | RANK_2) if side == 1 else (RANK_7 | RANK_8)
    score += (own & home).bit_count() * 8

    pawns = own
    while pawns:
        pawn = pawns & -pawns
        square = pawn.bit_length() - 1
        row, col = divmod(square, 8)
        advance = row if side == 1 else 7 - row
        score += ADVANCE[advance]
        if 2 <= col <= 5:
            score += 4

        # A passed pawn has no enemy pawn ahead on its file or either neighbor.
        files = 0
        for nearby in range(max(0, col - 1), min(7, col + 1) + 1):
            files |= FILE_A << nearby
        if side == 1:
            ahead = FULL ^ ((1 << ((row + 1) * 8)) - 1)
        else:
            ahead = (1 << (row * 8)) - 1
        if not enemy & files & ahead:
            score += 18 + advance * advance * 5
        pawns ^= pawn

    for col in range(8):
        file_mask = FILE_A << col
        if not own & home & file_mask:
            score -= 18
    return score


def evaluate(white: int, black: int, side: int) -> int:
    if has_promotion_move(white, black, side):
        return WIN - 100
    white_score = side_score(white, black, 1)
    black_score = side_score(black, white, -1)
    score = white_score - black_score
    if has_promotion_move(white, black, -side):
        score += -850 if side == 1 else 850
    return score if side == 1 else -score


def move_order_score(white: int, black: int, move: int, side: int, tt_move: int,
                     ply: int) -> int:
    if move == tt_move:
        return 2_000_000
    source = move >> 6
    target = move & 63
    target_bit = 1 << target
    enemy = black if side == 1 else white
    goal = RANK_8 if side == 1 else RANK_1
    score = _history[0 if side == 1 else 1][move]
    if target_bit & goal:
        return 1_500_000
    if target_bit & enemy:
        score += 200_000
        own = white if side == 1 else black
        if not target_bit & attacks(own ^ (1 << source), side):
            score += 20_000
    if move == _killers[ply][0]:
        score += 90_000
    elif move == _killers[ply][1]:
        score += 80_000
    row = target >> 3
    score += (row if side == 1 else 7 - row) * 100
    return score


def check_time() -> None:
    global _nodes
    _nodes += 1
    if (_nodes & 255) == 0 and time.perf_counter() >= _deadline:
        raise SearchTimeout


def negamax(white: int, black: int, side: int, depth: int, alpha: int,
            beta: int, ply: int) -> int:
    """Searches one node with PVS, transposition bounds, and move reductions."""
    check_time()
    original_alpha = alpha
    key = (white, black, side)
    entry = _tt.get(key)
    tt_move = -1
    if entry is not None:
        stored_depth, stored_score, flag, tt_move = entry
        if stored_depth >= depth:
            if flag == EXACT:
                return stored_score
            if flag == LOWER:
                alpha = max(alpha, stored_score)
            else:
                beta = min(beta, stored_score)
            if alpha >= beta:
                return stored_score

    if depth <= 0:
        return evaluate(white, black, side)

    moves = generate_moves(white, black, side)
    if not moves:
        return -WIN + ply

    moves.sort(
        key=lambda move: move_order_score(white, black, move, side, tt_move, ply
                                         ),
        reverse=True,
    )
    best_score = -INF
    best_move = moves[0]
    enemy = black if side == 1 else white

    for move_number, move in enumerate(moves):
        target = move & 63
        target_bit = 1 << target
        if target_bit & (RANK_8 if side == 1 else RANK_1):
            score = WIN - ply
        else:
            next_white, next_black = apply_move(white, black, move, side)
            # Reduce late quiet moves, then restore full depth if the reduced
            # probe unexpectedly improves alpha.
            reduction = 0
            if (depth >= 4 and move_number >= 4 and not target_bit & enemy and
                    not has_promotion_move(next_white, next_black, side)):
                reduction = 1
            # A null-window PVS probe is cheaper for moves after the expected
            # principal variation.
            score = -negamax(
                next_white,
                next_black,
                -side,
                depth - 1 - reduction,
                -alpha - 1,
                -alpha,
                ply + 1,
            )
            if reduction and score > alpha:
                score = -negamax(
                    next_white,
                    next_black,
                    -side,
                    depth - 1,
                    -alpha - 1,
                    -alpha,
                    ply + 1,
                )
            if score > alpha and score < beta:
                score = -negamax(
                    next_white,
                    next_black,
                    -side,
                    depth - 1,
                    -beta,
                    -alpha,
                    ply + 1,
                )

        if score > best_score:
            best_score = score
            best_move = move
        if score > alpha:
            alpha = score
        if alpha >= beta:
            if not target_bit & enemy:
                if move != _killers[ply][0]:
                    _killers[ply][1] = _killers[ply][0]
                    _killers[ply][0] = move
                side_index = 0 if side == 1 else 1
                _history[side_index][move] += depth * depth
            break

    if best_score <= original_alpha:
        flag = UPPER
    elif best_score >= beta:
        flag = LOWER
    else:
        flag = EXACT
    _tt[key] = (depth, best_score, flag, best_move)
    return best_score


def choose_move(white: int, black: int, side: int) -> int:
    """Returns the best move from the deepest fully completed iteration."""
    global _deadline, _nodes, _tt
    moves = generate_moves(white, black, side)
    if not moves:
        return -1

    goal = RANK_8 if side == 1 else RANK_1
    for move in moves:
        if (1 << (move & 63)) & goal:
            return move

    _deadline = time.perf_counter() + configured_time_limit()
    _nodes = 0
    _tt = {}
    best_move = moves[0]
    previous_score = 0

    try:
        # Iterative deepening supplies a safe fallback move and increasingly
        # accurate ordering for the next depth.
        for depth in range(1, MAX_DEPTH + 1):
            tt_move = best_move
            moves.sort(
                key=lambda move: move_order_score(white, black, move, side,
                                                  tt_move, 0),
                reverse=True,
            )
            iteration_best = moves[0]
            iteration_score = -INF
            alpha = max(-INF, previous_score - 60) if depth >= 3 else -INF
            beta = min(INF, previous_score + 60) if depth >= 3 else INF

            # Stable positions usually fit a narrow aspiration window around
            # the previous score; failures are repeated with a wider bound.
            while True:
                local_best = moves[0]
                local_score = -INF
                for move in moves:
                    target_bit = 1 << (move & 63)
                    if target_bit & goal:
                        score = WIN
                    else:
                        next_white, next_black = apply_move(
                            white, black, move, side)
                        score = -negamax(
                            next_white,
                            next_black,
                            -side,
                            depth - 1,
                            -beta,
                            -alpha,
                            1,
                        )
                    if score > local_score:
                        local_score = score
                        local_best = move
                    if score > alpha:
                        alpha = score

                if local_score <= alpha - 1 and alpha > -INF:
                    alpha = -INF
                    beta = min(INF, local_score + 120)
                    continue
                if local_score >= beta and beta < INF:
                    alpha = max(-INF, local_score - 120)
                    beta = INF
                    continue
                iteration_best = local_best
                iteration_score = local_score
                break

            best_move = iteration_best
            previous_score = iteration_score
            moves.remove(best_move)
            moves.insert(0, best_move)
            if abs(iteration_score) >= WIN - MAX_DEPTH:
                break
    except SearchTimeout:
        pass
    return best_move


def move_to_text(move: int) -> str:
    source = move >> 6
    target = move & 63
    return (chr(ord("a") + source % 8) + str(source // 8 + 1) +
            chr(ord("a") + target % 8) + str(target // 8 + 1))


def main() -> int:
    """Serves persistent two-line harness requests until stdin closes."""
    while True:
        state = sys.stdin.readline()
        if not state:
            return 0
        history = sys.stdin.readline()
        if not history:
            return 0
        try:
            white, black, side = parse_state(state)
            move = choose_move(white, black, side)
        except (ValueError, SearchTimeout) as error:
            print(f"protocol error: {error}", file=sys.stderr, flush=True)
            return 1
        if move < 0:
            return 1
        print(move_to_text(move), flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
