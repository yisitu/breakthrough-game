//! Time-bounded Breakthrough bot using bitboards and iterative deepening.
//!
//! The board is represented by one 64-bit integer per color. Bitwise shifts
//! generate many pawn moves at once, and moves are stored in fixed-size arrays
//! to avoid allocation in the search. Iterative deepening searches one level
//! deeper at a time and preserves the result of the deepest iteration that
//! finishes before the deadline.
//!
//! Each iteration uses negamax with alpha-beta pruning. Principal variation
//! search checks later moves through a narrow score window and repeats only
//! unexpectedly good moves with the full window. Aspiration windows begin near
//! the preceding iteration's score. A direct-mapped transposition table reuses
//! results for positions reached through different move sequences, while killer
//! moves and history scores improve move ordering. Late quiet moves receive a
//! shallower initial search, but are searched again if they appear important.
//!
//! At the depth limit, the evaluator compares material, advancement, support,
//! mobility, passed pawns, home defense, and immediate promotion threats. The
//! search periodically checks a monotonic deadline so protocol and output work
//! still fit inside the referee's wall-clock limit.
//!
//! Bit 0 is a1 and bit 63 is h8. White advances toward larger bit indices;
//! Black advances toward smaller indices. Moves pack source in bits 6+ and
//! target in bits 0-5.

use std::io::{self, BufRead, Write};
use std::time::{Duration, Instant};

const FILE_A: u64 = 0x0101_0101_0101_0101;
const FILE_H: u64 = 0x8080_8080_8080_8080;
const RANK_1: u64 = 0x0000_0000_0000_00ff;
const RANK_2: u64 = 0x0000_0000_0000_ff00;
const RANK_7: u64 = 0x00ff_0000_0000_0000;
const RANK_8: u64 = 0xff00_0000_0000_0000;
const INF: i32 = 1_000_000;
const WIN: i32 = 100_000;
const MAX_PLY: usize = 96;
const ADVANCE: [i32; 8] = [0, 5, 13, 25, 43, 78, 155, 500];
const TT_BITS: usize = 19;
const TT_SIZE: usize = 1 << TT_BITS;
const TT_MASK: usize = TT_SIZE - 1;

const EXACT: u8 = 0;
const LOWER: u8 = 1;
const UPPER: u8 = 2;
const NO_MOVE: u16 = u16::MAX;

#[derive(Clone, Copy, Default)]
struct Position {
    white: u64,
    black: u64,
    side: i8,
}

#[derive(Clone, Copy)]
struct MoveList {
    // Sixteen pawns with at most three moves each bounds the list at 48.
    moves: [u16; 48],
    len: usize,
}

impl MoveList {
    fn new() -> Self {
        Self {
            moves: [0; 48],
            len: 0,
        }
    }

    fn push(&mut self, mv: u16) {
        self.moves[self.len] = mv;
        self.len += 1;
    }
}

#[derive(Clone, Copy)]
struct TTEntry {
    // flag classifies score as exact, a lower bound, or an upper bound.
    key: u64,
    score: i32,
    best_move: u16,
    generation: u16,
    depth: i8,
    flag: u8,
}

impl Default for TTEntry {
    fn default() -> Self {
        Self {
            key: 0,
            score: 0,
            best_move: NO_MOVE,
            generation: 0,
            depth: -1,
            flag: EXACT,
        }
    }
}

struct Searcher {
    deadline: Instant,
    time_limit: Duration,
    nodes: u64,
    stopped: bool,
    generation: u16,
    table: Vec<TTEntry>,
    history: [[i32; 4096]; 2],
    killers: [[u16; 2]; MAX_PLY],
}

impl Searcher {
    fn new() -> Self {
        Self {
            deadline: Instant::now(),
            time_limit: configured_time_limit(),
            nodes: 0,
            stopped: false,
            generation: 0,
            table: vec![TTEntry::default(); TT_SIZE],
            history: [[0; 4096]; 2],
            killers: [[NO_MOVE; 2]; MAX_PLY],
        }
    }

    fn start_search(&mut self) {
        // Age ordering history between turns but keep useful prior evidence.
        self.deadline = Instant::now() + self.time_limit;
        self.nodes = 0;
        self.stopped = false;
        self.generation = self.generation.wrapping_add(1).max(1);
        self.killers = [[NO_MOVE; 2]; MAX_PLY];
        for side in &mut self.history {
            for value in side {
                *value /= 2;
            }
        }
    }

    #[inline]
    fn check_time(&mut self) -> bool {
        // Reading the clock every node costs more than a bounded 1024-node
        // overshoot under the internal safety margin.
        self.nodes += 1;
        if self.nodes & 1023 == 0 && Instant::now() >= self.deadline {
            self.stopped = true;
        }
        self.stopped
    }

    fn choose_move(&mut self, position: Position) -> Option<u16> {
        let mut moves = generate_moves(position);
        if moves.len == 0 {
            return None;
        }

        let goal = if position.side == 1 { RANK_8 } else { RANK_1 };
        // Promotion is terminal, so no search can improve on this move.
        for index in 0..moves.len {
            let mv = moves.moves[index];
            if bit(move_target(mv)) & goal != 0 {
                return Some(mv);
            }
        }

        self.start_search();
        let mut best_move = moves.moves[0];
        let mut previous_score: i32 = 0;

        // Keep the deepest completed result as a safe fallback if the next
        // iteration reaches the deadline.
        for depth in 1..=64 {
            order_moves(self, position, &mut moves, best_move, 0);
            let window = if depth >= 4 { 70 } else { INF };
            let mut alpha = previous_score.saturating_sub(window).max(-INF);
            let mut beta = previous_score.saturating_add(window).min(INF);
            let mut completed_move = best_move;
            let mut completed_score = previous_score;

            // A narrow aspiration window saves work in stable positions. A
            // fail-low or fail-high repeats the iteration with wider bounds.
            loop {
                let window_alpha = alpha;
                let window_beta = beta;
                let mut iteration_best = moves.moves[0];
                let mut iteration_score = -INF;

                for index in 0..moves.len {
                    let mv = moves.moves[index];
                    let target = move_target(mv);
                    let score = if bit(target) & goal != 0 {
                        WIN
                    } else {
                        let child = apply_move(position, mv);
                        let value = if index == 0 {
                            -self.negamax(child, depth - 1, -beta, -alpha, 1)
                        } else {
                            let mut value = -self.negamax(child, depth - 1, -alpha - 1, -alpha, 1);
                            if !self.stopped && value > alpha && value < beta {
                                value = -self.negamax(child, depth - 1, -beta, -alpha, 1);
                            }
                            value
                        };
                        value
                    };
                    if self.stopped {
                        break;
                    }
                    if score > iteration_score {
                        iteration_score = score;
                        iteration_best = mv;
                    }
                    alpha = alpha.max(score);
                }

                if self.stopped {
                    break;
                }
                if iteration_score <= window_alpha && window_alpha > -INF {
                    alpha = -INF;
                    beta = (iteration_score + 160).min(INF);
                    continue;
                }
                if iteration_score >= window_beta && window_beta < INF {
                    alpha = (iteration_score - 160).max(-INF);
                    beta = INF;
                    continue;
                }
                completed_move = iteration_best;
                completed_score = iteration_score;
                break;
            }

            if self.stopped {
                break;
            }
            best_move = completed_move;
            previous_score = completed_score;
            move_to_front(&mut moves, best_move);
            if previous_score.abs() >= WIN - MAX_PLY as i32 {
                break;
            }
        }
        Some(best_move)
    }

    fn negamax(
        &mut self,
        position: Position,
        depth: i32,
        mut alpha: i32,
        mut beta: i32,
        ply: usize,
    ) -> i32 {
        if self.check_time() {
            return 0;
        }
        let original_alpha = alpha;
        let original_beta = beta;
        let key = position_key(position);
        let table_index = key as usize & TT_MASK;
        let entry = self.table[table_index];
        let mut tt_move = NO_MOVE;

        // The direct-mapped table supplies both cutoffs and the preferred move
        // for ordering. Depth guards prevent shallow entries replacing proof.
        if entry.generation == self.generation && entry.key == key {
            tt_move = entry.best_move;
            if entry.depth as i32 >= depth {
                match entry.flag {
                    EXACT => return entry.score,
                    LOWER => alpha = alpha.max(entry.score),
                    UPPER => beta = beta.min(entry.score),
                    _ => {}
                }
                if alpha >= beta {
                    return entry.score;
                }
            }
        }

        if depth <= 0 || ply >= MAX_PLY - 1 {
            return evaluate(position);
        }

        let mut moves = generate_moves(position);
        if moves.len == 0 {
            return -WIN + ply as i32;
        }
        order_moves(self, position, &mut moves, tt_move, ply);

        let goal = if position.side == 1 { RANK_8 } else { RANK_1 };
        let enemy = if position.side == 1 {
            position.black
        } else {
            position.white
        };
        let mut best_score = -INF;
        let mut best_move = moves.moves[0];

        for index in 0..moves.len {
            let mv = moves.moves[index];
            let target = move_target(mv);
            let target_mask = bit(target);
            let is_capture = target_mask & enemy != 0;
            let score = if target_mask & goal != 0 {
                WIN - ply as i32
            } else {
                let child = apply_move(position, mv);
                // Reduce late quiet moves. Any reduced move that improves
                // alpha is restored to full depth before it can affect PV.
                let reduction = if depth >= 4
                    && index >= 5
                    && !is_capture
                    && !creates_advanced_threat(mv, position.side)
                {
                    1
                } else {
                    0
                };

                let mut value = if index == 0 {
                    -self.negamax(child, depth - 1, -beta, -alpha, ply + 1)
                } else {
                    -self.negamax(child, depth - 1 - reduction, -alpha - 1, -alpha, ply + 1)
                };
                if self.stopped {
                    return 0;
                }
                if reduction != 0 && value > alpha {
                    value = -self.negamax(child, depth - 1, -alpha - 1, -alpha, ply + 1);
                }
                if self.stopped {
                    return 0;
                }
                if index != 0 && value > alpha && value < beta {
                    value = -self.negamax(child, depth - 1, -beta, -alpha, ply + 1);
                }
                value
            };

            if self.stopped {
                return 0;
            }
            if score > best_score {
                best_score = score;
                best_move = mv;
            }
            alpha = alpha.max(score);
            if alpha >= beta {
                if !is_capture {
                    if self.killers[ply][0] != mv {
                        self.killers[ply][1] = self.killers[ply][0];
                        self.killers[ply][0] = mv;
                    }
                    let side_index = if position.side == 1 { 0 } else { 1 };
                    let bonus = depth * depth;
                    self.history[side_index][mv as usize] =
                        (self.history[side_index][mv as usize] + bonus).min(200_000);
                }
                break;
            }
        }

        let flag = if best_score <= original_alpha {
            UPPER
        } else if best_score >= original_beta {
            LOWER
        } else {
            EXACT
        };
        let old = self.table[table_index];
        if old.generation != self.generation || depth >= old.depth as i32 {
            self.table[table_index] = TTEntry {
                key,
                score: best_score,
                best_move,
                generation: self.generation,
                depth: depth.min(i8::MAX as i32) as i8,
                flag,
            };
        }
        best_score
    }
}

#[inline]
fn bit(square: u16) -> u64 {
    1_u64 << square
}

#[inline]
fn encode_move(source: u32, target: u32) -> u16 {
    ((source << 6) | target) as u16
}

#[inline]
fn move_source(mv: u16) -> u16 {
    mv >> 6
}

#[inline]
fn move_target(mv: u16) -> u16 {
    mv & 63
}

fn add_targets(moves: &mut MoveList, mut targets: u64, source_offset: i32) {
    // Target bitboards are cheap to compute in parallel; fixed offsets recover
    // each source square without scanning all pawns.
    while targets != 0 {
        let target = targets.trailing_zeros();
        let source = (target as i32 + source_offset) as u32;
        moves.push(encode_move(source, target));
        targets &= targets - 1;
    }
}

fn generate_moves(position: Position) -> MoveList {
    // File masks prevent diagonal shifts from wrapping across board edges.
    let mut moves = MoveList::new();
    let empty = !(position.white | position.black);

    if position.side == 1 {
        add_targets(&mut moves, (position.white << 8) & empty, -8);
        let first = ((position.white & RANK_2) << 8) & empty;
        add_targets(&mut moves, (first << 8) & empty, -16);
        add_targets(
            &mut moves,
            ((position.white & !FILE_A) << 7) & position.black,
            -7,
        );
        add_targets(
            &mut moves,
            ((position.white & !FILE_H) << 9) & position.black,
            -9,
        );
    } else {
        add_targets(&mut moves, (position.black >> 8) & empty, 8);
        let first = ((position.black & RANK_7) >> 8) & empty;
        add_targets(&mut moves, (first >> 8) & empty, 16);
        add_targets(
            &mut moves,
            ((position.black & !FILE_A) >> 9) & position.white,
            9,
        );
        add_targets(
            &mut moves,
            ((position.black & !FILE_H) >> 7) & position.white,
            7,
        );
    }
    moves
}

#[inline]
fn apply_move(position: Position, mv: u16) -> Position {
    let source_mask = bit(move_source(mv));
    let target_mask = bit(move_target(mv));
    if position.side == 1 {
        Position {
            white: (position.white ^ source_mask) | target_mask,
            black: position.black & !target_mask,
            side: -1,
        }
    } else {
        Position {
            white: position.white & !target_mask,
            black: (position.black ^ source_mask) | target_mask,
            side: 1,
        }
    }
}

#[inline]
fn attacks(pawns: u64, side: i8) -> u64 {
    if side == 1 {
        ((pawns & !FILE_A) << 7) | ((pawns & !FILE_H) << 9)
    } else {
        ((pawns & !FILE_A) >> 9) | ((pawns & !FILE_H) >> 7)
    }
}

fn has_promotion_move(position: Position, side: i8) -> bool {
    let occupied = position.white | position.black;
    if side == 1 {
        let pawns = position.white & RANK_7;
        ((pawns << 8) & !occupied) | (attacks(pawns, 1) & position.black & RANK_8) != 0
    } else {
        let pawns = position.black & RANK_2;
        ((pawns >> 8) & !occupied) | (attacks(pawns, -1) & position.white & RANK_1) != 0
    }
}

fn mobility(own: u64, enemy: u64, side: i8) -> i32 {
    let empty = !(own | enemy);
    let count = if side == 1 {
        let first = ((own & RANK_2) << 8) & empty;
        ((own << 8) & empty).count_ones() + ((first << 8) & empty).count_ones()
    } else {
        let first = ((own & RANK_7) >> 8) & empty;
        ((own >> 8) & empty).count_ones() + ((first >> 8) & empty).count_ones()
    };
    (count + (attacks(own, side) & enemy).count_ones()) as i32
}

fn side_score(own: u64, enemy: u64, side: i8) -> i32 {
    // Reward material, support, mobility, advancement, and home-rank coverage.
    let protected = own & attacks(own, side);
    let attacked = own & attacks(enemy, -side);
    let home = if side == 1 {
        RANK_1 | RANK_2
    } else {
        RANK_7 | RANK_8
    };
    let mut score = own.count_ones() as i32 * 120 + protected.count_ones() as i32 * 18
        - (attacked & !protected).count_ones() as i32 * 15
        + mobility(own, enemy, side) * 3
        + (own & home).count_ones() as i32 * 8;

    let mut pawns = own;
    while pawns != 0 {
        let square = pawns.trailing_zeros() as usize;
        let row = square / 8;
        let col = square % 8;
        let advance = if side == 1 { row } else { 7 - row };
        score += ADVANCE[advance];
        if (2..=5).contains(&col) {
            score += 4;
        }

        // A passed pawn has no enemy ahead on its file or either neighbor.
        let left_file = col.saturating_sub(1);
        let right_file = (col + 1).min(7);
        let mut files = 0_u64;
        for nearby in left_file..=right_file {
            files |= FILE_A << nearby;
        }
        let ahead = if side == 1 {
            if row == 7 {
                0
            } else {
                !((1_u64 << ((row + 1) * 8)) - 1)
            }
        } else if row == 0 {
            0
        } else {
            (1_u64 << (row * 8)) - 1
        };
        if enemy & files & ahead == 0 {
            score += 18 + (advance * advance * 5) as i32;
        }
        pawns &= pawns - 1;
    }

    for col in 0..8 {
        if own & home & (FILE_A << col) == 0 {
            score -= 18;
        }
    }
    score
}

fn evaluate(position: Position) -> i32 {
    if has_promotion_move(position, position.side) {
        return WIN - 100;
    }
    let mut score = side_score(position.white, position.black, 1)
        - side_score(position.black, position.white, -1);
    if has_promotion_move(position, -position.side) {
        score += if position.side == 1 { -850 } else { 850 };
    }
    if position.side == 1 {
        score
    } else {
        -score
    }
}

#[inline]
fn creates_advanced_threat(mv: u16, side: i8) -> bool {
    let row = move_target(mv) as usize / 8;
    (side == 1 && row >= 6) || (side == -1 && row <= 1)
}

fn move_order_score(
    searcher: &Searcher,
    position: Position,
    mv: u16,
    tt_move: u16,
    ply: usize,
) -> i32 {
    // Search hash moves, wins, captures, learned cutoffs, and threats first.
    if mv == tt_move {
        return 2_000_000;
    }
    let target = move_target(mv);
    let target_mask = bit(target);
    let goal = if position.side == 1 { RANK_8 } else { RANK_1 };
    if target_mask & goal != 0 {
        return 1_500_000;
    }
    let side_index = if position.side == 1 { 0 } else { 1 };
    let enemy = if position.side == 1 {
        position.black
    } else {
        position.white
    };
    let mut score = searcher.history[side_index][mv as usize];
    if target_mask & enemy != 0 {
        score += 200_000;
    }
    if searcher.killers[ply][0] == mv {
        score += 90_000;
    } else if searcher.killers[ply][1] == mv {
        score += 80_000;
    }
    let row = target as i32 / 8;
    score += if position.side == 1 { row } else { 7 - row } * 100;
    if creates_advanced_threat(mv, position.side) {
        score += 40_000;
    }
    score
}

fn order_moves(
    searcher: &Searcher,
    position: Position,
    moves: &mut MoveList,
    tt_move: u16,
    ply: usize,
) {
    // Insertion sort is efficient for this small fixed-size, nearly ordered
    // list and requires no heap allocation.
    let mut scored = [(0_i32, 0_u16); 48];
    for (index, slot) in scored.iter_mut().enumerate().take(moves.len) {
        let mv = moves.moves[index];
        *slot = (move_order_score(searcher, position, mv, tt_move, ply), mv);
    }
    for index in 1..moves.len {
        let value = scored[index];
        let mut insertion = index;
        while insertion > 0 && scored[insertion - 1].0 < value.0 {
            scored[insertion] = scored[insertion - 1];
            insertion -= 1;
        }
        scored[insertion] = value;
    }
    for (index, entry) in scored.iter().enumerate().take(moves.len) {
        moves.moves[index] = entry.1;
    }
}

fn move_to_front(moves: &mut MoveList, mv: u16) {
    if let Some(index) = moves.moves[..moves.len].iter().position(|item| *item == mv) {
        moves.moves[..=index].rotate_right(1);
    }
}

fn position_key(position: Position) -> u64 {
    // Mix full bitboards instead of maintaining incremental state; Rust still
    // searches deeply enough that this simpler invariant is worthwhile.
    fn mix(mut value: u64) -> u64 {
        value ^= value >> 30;
        value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9);
        value ^= value >> 27;
        value = value.wrapping_mul(0x94d0_49bb_1331_11eb);
        value ^ (value >> 31)
    }
    mix(position.white)
        ^ mix(position.black.rotate_left(23))
        ^ if position.side == 1 {
            0x9e37_79b9_7f4a_7c15
        } else {
            0xd1b5_4a32_d192_ed03
        }
}

fn configured_time_limit() -> Duration {
    // Reserve wall-clock time for protocol parsing, scheduling, and flushing.
    let timeout_ms = std::env::var("BREAKTHROUGH_TIMEOUT_MS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(200);
    let margin_ms = (timeout_ms / 5).max(20).min(timeout_ms.saturating_sub(1));
    Duration::from_millis(timeout_ms.saturating_sub(margin_ms).max(1))
}

fn parse_state(state: &str) -> Result<Position, &'static str> {
    // Harness rows run rank 1 to rank 8, matching increasing bit indices.
    let mut fields = state.split_whitespace();
    let board = fields.next().ok_or("missing board")?;
    let turn = fields.next().ok_or("missing side")?;
    if fields.next().is_some() {
        return Err("too many state fields");
    }
    let rows: Vec<&str> = board.split('/').collect();
    if rows.len() != 8 || rows.iter().any(|row| row.len() != 8) {
        return Err("invalid board");
    }

    let mut position = Position::default();
    for (row, cells) in rows.iter().enumerate() {
        for (col, cell) in cells.bytes().enumerate() {
            let square = (row * 8 + col) as u16;
            match cell {
                b'W' => position.white |= bit(square),
                b'B' => position.black |= bit(square),
                b'.' => {}
                _ => return Err("invalid piece"),
            }
        }
    }
    position.side = match turn {
        "W" => 1,
        "B" => -1,
        _ => return Err("invalid side"),
    };
    Ok(position)
}

fn move_to_text(mv: u16) -> String {
    let source = move_source(mv);
    let target = move_target(mv);
    format!(
        "{}{}{}{}",
        (b'a' + (source % 8) as u8) as char,
        source / 8 + 1,
        (b'a' + (target % 8) as u8) as char,
        target / 8 + 1
    )
}

fn run() -> Result<(), String> {
    let stdin = io::stdin();
    let mut input = stdin.lock();
    let stdout = io::stdout();
    let mut output = io::BufWriter::new(stdout.lock());
    let mut state = String::new();
    let mut history = String::new();
    let mut searcher = Searcher::new();

    loop {
        // Every request is two lines. Only one flushed move line may be sent
        // to stdout; diagnostics belong on stderr.
        state.clear();
        if input
            .read_line(&mut state)
            .map_err(|error| error.to_string())?
            == 0
        {
            return Ok(());
        }
        history.clear();
        if input
            .read_line(&mut history)
            .map_err(|error| error.to_string())?
            == 0
        {
            return Ok(());
        }
        let position = parse_state(&state).map_err(str::to_string)?;
        let mv = searcher
            .choose_move(position)
            .ok_or_else(|| "no legal move".to_string())?;
        writeln!(output, "{}", move_to_text(mv)).map_err(|error| error.to_string())?;
        output.flush().map_err(|error| error.to_string())?;
    }
}

fn main() {
    if let Err(error) = run() {
        eprintln!("protocol error: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn reference_moves(position: Position) -> Vec<u16> {
        let own = if position.side == 1 {
            position.white
        } else {
            position.black
        };
        let enemy = if position.side == 1 {
            position.black
        } else {
            position.white
        };
        let direction = position.side as i32;
        let start_row = if position.side == 1 { 1 } else { 6 };
        let mut result = Vec::new();

        for source in 0..64_u32 {
            if own & (1_u64 << source) == 0 {
                continue;
            }
            let row = source as i32 / 8;
            let col = source as i32 % 8;
            let next_row = row + direction;
            if !(0..8).contains(&next_row) {
                continue;
            }

            let forward = (next_row * 8 + col) as u32;
            if (own | enemy) & (1_u64 << forward) == 0 {
                result.push(encode_move(source, forward));
                let double_row = row + direction * 2;
                if row == start_row {
                    let target = (double_row * 8 + col) as u32;
                    if (own | enemy) & (1_u64 << target) == 0 {
                        result.push(encode_move(source, target));
                    }
                }
            }

            for column_delta in [-1, 1] {
                let target_col = col + column_delta;
                if !(0..8).contains(&target_col) {
                    continue;
                }
                let target = (next_row * 8 + target_col) as u32;
                if enemy & (1_u64 << target) != 0 {
                    result.push(encode_move(source, target));
                }
            }
        }
        result.sort_unstable();
        result
    }

    #[test]
    fn initial_position_has_sixteen_white_moves() {
        let position = parse_state(
            "WWWWWWWW/WWWWWWWW/......../......../......../......../BBBBBBBB/BBBBBBBB W",
        )
        .unwrap();
        assert_eq!(generate_moves(position).len, 16);
    }

    #[test]
    fn parses_capture_and_promotion_moves() {
        let position = parse_state(
            "......../......../......../......../......../......../W.B...../.B...... W",
        )
        .unwrap();
        let moves = generate_moves(position);
        let text: Vec<String> = moves.moves[..moves.len]
            .iter()
            .map(|mv| move_to_text(*mv))
            .collect();
        assert!(text.contains(&"a7a8".to_string()));
        assert!(text.contains(&"a7b8".to_string()));
    }

    #[test]
    fn bitboards_match_reference_generator() {
        // A deterministic LCG creates 10,000 reproducible positions and checks
        // optimized bitboards against the simple square-by-square generator.
        let mut random = 0x1234_5678_9abc_def0_u64;
        for index in 0..10_000 {
            random = random
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1);
            let white = random;
            random = random
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1);
            let position = Position {
                white,
                black: random & !white,
                side: if index % 2 == 0 { 1 } else { -1 },
            };
            let generated = generate_moves(position);
            let mut actual = generated.moves[..generated.len].to_vec();
            actual.sort_unstable();
            assert_eq!(actual, reference_moves(position));
        }
    }
}
