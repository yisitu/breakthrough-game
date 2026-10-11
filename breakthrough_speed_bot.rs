//! Speed-first Breakthrough engine.
//!
//! The engine spends its time budget descending the game tree rather than
//! computing a detailed leaf score. Positions use two 64-bit bitboards and an
//! incrementally maintained Zobrist key. Moves use a fixed 48-entry array and
//! are packed into 16 bits, so the recursive search performs no allocation.
//!
//! Iterative deepening, aspiration windows, principal variation search, a
//! cache-sized transposition table, staged move ordering, killer/history
//! heuristics, and late-move reductions control the branching factor. There is
//! deliberately no quiescence search or promotion extension. The evaluator
//! uses a fixed number of bit operations for material, advancement, support,
//! exposure, home coverage, and immediate promotion threats.
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
const RANK_3: u64 = 0x0000_0000_00ff_0000;
const RANK_4: u64 = 0x0000_0000_ff00_0000;
const RANK_5: u64 = 0x0000_00ff_0000_0000;
const RANK_6: u64 = 0x0000_ff00_0000_0000;
const RANK_7: u64 = 0x00ff_0000_0000_0000;
const RANK_8: u64 = 0xff00_0000_0000_0000;
const RANKS: [u64; 8] = [
    RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8,
];
const ADVANCE: [i32; 8] = [0, 4, 12, 26, 52, 105, 220, 700];
const INF: i32 = 1_000_000;
const WIN: i32 = 100_000;
const MAX_PLY: usize = 96;
const TT_BITS: usize = 16;
const TT_SIZE: usize = 1 << TT_BITS;
const TT_MASK: usize = TT_SIZE - 1;
const NO_MOVE: u16 = u16::MAX;
const EXACT: u8 = 0;
const LOWER: u8 = 1;
const UPPER: u8 = 2;

struct ZobristData {
    pieces: [[u64; 64]; 2],
    side: u64,
}

const fn split_mix_64(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
    let mut value = *state;
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

const fn build_zobrist() -> ZobristData {
    let mut state = 0x64f0_0d12_9b8a_7c31;
    let mut pieces = [[0_u64; 64]; 2];
    let mut side = 0;
    while side < 2 {
        let mut square = 0;
        while square < 64 {
            pieces[side][square] = split_mix_64(&mut state);
            square += 1;
        }
        side += 1;
    }
    ZobristData {
        pieces,
        side: split_mix_64(&mut state),
    }
}

const ZOBRIST: ZobristData = build_zobrist();

#[derive(Clone, Copy, Default)]
struct Position {
    white: u64,
    black: u64,
    key: u64,
    side: i8,
}

#[derive(Clone, Copy)]
struct MoveList {
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
    key: u64,
    score: i32,
    best_move: u16,
    generation: u16,
    depth: i8,
    bound: u8,
}

impl Default for TTEntry {
    fn default() -> Self {
        Self {
            key: 0,
            score: 0,
            best_move: NO_MOVE,
            generation: 0,
            depth: -1,
            bound: EXACT,
        }
    }
}

struct Searcher {
    table: Vec<TTEntry>,
    history: [[i32; 4096]; 2],
    killers: [[u16; 2]; MAX_PLY],
    start: Instant,
    deadline: Instant,
    nodes: u64,
    generation: u16,
    completed_depth: i32,
    elapsed: Duration,
    stopped: bool,
}

impl Searcher {
    fn new() -> Self {
        let now = Instant::now();
        Self {
            table: vec![TTEntry::default(); TT_SIZE],
            history: [[0; 4096]; 2],
            killers: [[NO_MOVE; 2]; MAX_PLY],
            start: now,
            deadline: now,
            nodes: 0,
            generation: 0,
            completed_depth: 0,
            elapsed: Duration::ZERO,
            stopped: false,
        }
    }

    fn start_search(&mut self, time_limit: Duration) {
        self.start = Instant::now();
        self.deadline = self.start + time_limit;
        self.nodes = 0;
        self.completed_depth = 0;
        self.elapsed = Duration::ZERO;
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
        self.nodes += 1;
        if self.nodes & 2047 == 0 && Instant::now() >= self.deadline {
            self.stopped = true;
        }
        self.stopped
    }

    fn choose_move(&mut self, position: Position, time_limit: Duration) -> Option<u16> {
        let mut moves = generate_moves(position);
        if moves.len == 0 {
            return None;
        }

        let goal = if position.side == 1 { RANK_8 } else { RANK_1 };
        for index in 0..moves.len {
            let mv = moves.moves[index];
            if bit(move_target(mv)) & goal != 0 {
                return Some(mv);
            }
        }

        self.start_search(time_limit);
        let mut best_move = moves.moves[0];
        let mut previous_score = 0_i32;
        for depth in 1..=64 {
            order_moves(self, position, &mut moves, best_move, 0);
            let window = if depth >= 4 { 64 } else { INF };
            let mut alpha = previous_score.saturating_sub(window).max(-INF);
            let mut beta = previous_score.saturating_add(window).min(INF);
            let mut iteration_move = best_move;
            let mut iteration_score = previous_score;

            loop {
                let window_alpha = alpha;
                let window_beta = beta;
                let result = self.search_root(position, &mut moves, depth, alpha, beta);
                if self.stopped {
                    break;
                }
                iteration_move = result.0;
                iteration_score = result.1;
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
                break;
            }

            if self.stopped {
                break;
            }
            best_move = iteration_move;
            previous_score = iteration_score;
            self.completed_depth = depth;
            move_to_front(&mut moves, best_move);
            if previous_score.abs() >= WIN - MAX_PLY as i32 {
                break;
            }
        }
        self.elapsed = self.start.elapsed();
        Some(best_move)
    }

    fn search_root(
        &mut self,
        position: Position,
        moves: &mut MoveList,
        depth: i32,
        mut alpha: i32,
        beta: i32,
    ) -> (u16, i32) {
        let mut best_move = moves.moves[0];
        let mut best_score = -INF;
        let goal = if position.side == 1 { RANK_8 } else { RANK_1 };
        for index in 0..moves.len {
            let mv = moves.moves[index];
            let score = if bit(move_target(mv)) & goal != 0 {
                WIN
            } else {
                let child = apply_move(position, mv);
                if index == 0 {
                    -self.negamax(child, depth - 1, -beta, -alpha, 1)
                } else {
                    let mut value = -self.negamax(child, depth - 1, -alpha - 1, -alpha, 1);
                    if !self.stopped && value > alpha && value < beta {
                        value = -self.negamax(child, depth - 1, -beta, -alpha, 1);
                    }
                    value
                }
            };
            if self.stopped {
                break;
            }
            if score > best_score {
                best_score = score;
                best_move = mv;
            }
            alpha = alpha.max(score);
            if alpha >= beta {
                break;
            }
        }
        (best_move, best_score)
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
        if depth <= 0 || ply >= MAX_PLY - 1 {
            return evaluate(position);
        }

        let original_alpha = alpha;
        let original_beta = beta;
        let table_index = position.key as usize & TT_MASK;
        let entry = self.table[table_index];
        let mut tt_move = NO_MOVE;
        if entry.depth >= 0 && entry.key == position.key {
            tt_move = entry.best_move;
            let stored_score = score_from_table(entry.score, ply);
            if entry.depth as i32 >= depth {
                match entry.bound {
                    EXACT => return stored_score,
                    LOWER => alpha = alpha.max(stored_score),
                    UPPER => beta = beta.min(stored_score),
                    _ => {}
                }
                if alpha >= beta {
                    return stored_score;
                }
            }
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
        let mut best_move = moves.moves[0];
        let mut best_score = -INF;

        for index in 0..moves.len {
            let mv = moves.moves[index];
            let target_mask = bit(move_target(mv));
            let capture = target_mask & enemy != 0;
            let score = if target_mask & goal != 0 {
                WIN - ply as i32
            } else {
                let child = apply_move(position, mv);
                let reduction = if depth >= 5 && index >= 8 && !capture {
                    2
                } else if depth >= 3 && index >= 4 && !capture {
                    1
                } else {
                    0
                };
                let next_depth = (depth - 1 - reduction).max(0);
                let mut value = if index == 0 {
                    -self.negamax(child, depth - 1, -beta, -alpha, ply + 1)
                } else {
                    -self.negamax(child, next_depth, -alpha - 1, -alpha, ply + 1)
                };
                if self.stopped {
                    return 0;
                }
                if index != 0 && reduction != 0 && value > alpha {
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
                if !capture {
                    if self.killers[ply][0] != mv {
                        self.killers[ply][1] = self.killers[ply][0];
                        self.killers[ply][0] = mv;
                    }
                    let side_index = usize::from(position.side != 1);
                    let bonus = depth * depth;
                    self.history[side_index][mv as usize] =
                        (self.history[side_index][mv as usize] + bonus).min(200_000);
                }
                break;
            }
        }

        let bound = if best_score <= original_alpha {
            UPPER
        } else if best_score >= original_beta {
            LOWER
        } else {
            EXACT
        };
        let old = self.table[table_index];
        if old.generation != self.generation || depth >= old.depth as i32 {
            self.table[table_index] = TTEntry {
                key: position.key,
                score: score_to_table(best_score, ply),
                best_move,
                generation: self.generation,
                depth: depth.min(i8::MAX as i32) as i8,
                bound,
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

fn calculate_key(position: Position) -> u64 {
    let mut key = if position.side == -1 { ZOBRIST.side } else { 0 };
    let mut pieces = position.white;
    while pieces != 0 {
        let square = pieces.trailing_zeros() as usize;
        key ^= ZOBRIST.pieces[0][square];
        pieces &= pieces - 1;
    }
    pieces = position.black;
    while pieces != 0 {
        let square = pieces.trailing_zeros() as usize;
        key ^= ZOBRIST.pieces[1][square];
        pieces &= pieces - 1;
    }
    key
}

fn add_targets(moves: &mut MoveList, mut targets: u64, source_offset: i32) {
    while targets != 0 {
        let target = targets.trailing_zeros();
        let source = (target as i32 + source_offset) as u32;
        moves.push(encode_move(source, target));
        targets &= targets - 1;
    }
}

fn generate_moves(position: Position) -> MoveList {
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
    let source = move_source(mv) as usize;
    let target = move_target(mv) as usize;
    let source_mask = 1_u64 << source;
    let target_mask = 1_u64 << target;
    let own_index = usize::from(position.side != 1);
    let enemy_index = own_index ^ 1;
    let mut next = position;
    next.key ^=
        ZOBRIST.pieces[own_index][source] ^ ZOBRIST.pieces[own_index][target] ^ ZOBRIST.side;
    if position.side == 1 {
        next.white = (position.white ^ source_mask) | target_mask;
        if position.black & target_mask != 0 {
            next.black ^= target_mask;
            next.key ^= ZOBRIST.pieces[enemy_index][target];
        }
    } else {
        next.black = (position.black ^ source_mask) | target_mask;
        if position.white & target_mask != 0 {
            next.white ^= target_mask;
            next.key ^= ZOBRIST.pieces[enemy_index][target];
        }
    }
    next.side = -position.side;
    next
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
        (((pawns << 8) & !occupied) | (attacks(pawns, 1) & position.black & RANK_8)) != 0
    } else {
        let pawns = position.black & RANK_2;
        (((pawns >> 8) & !occupied) | (attacks(pawns, -1) & position.white & RANK_1)) != 0
    }
}

fn fast_side_score(own: u64, enemy: u64, side: i8) -> i32 {
    let protected = own & attacks(own, side);
    let attacked = own & attacks(enemy, -side);
    let mut score = own.count_ones() as i32 * 128;
    score += protected.count_ones() as i32 * 16;
    score -= (attacked & !protected).count_ones() as i32 * 14;
    for (row, rank) in RANKS.iter().enumerate() {
        let advance = if side == 1 { row } else { 7 - row };
        score += (own & rank).count_ones() as i32 * ADVANCE[advance];
    }
    let home = if side == 1 {
        RANK_1 | RANK_2
    } else {
        RANK_7 | RANK_8
    };
    score + (own & home).count_ones() as i32 * 6
}

fn evaluate(position: Position) -> i32 {
    if has_promotion_move(position, position.side) {
        return WIN - 100;
    }
    let mut score = fast_side_score(position.white, position.black, 1)
        - fast_side_score(position.black, position.white, -1);
    if has_promotion_move(position, -position.side) {
        score += if position.side == 1 { -850 } else { 850 };
    }
    if position.side == 1 {
        score
    } else {
        -score
    }
}

fn move_stage(searcher: &Searcher, position: Position, mv: u16, tt_move: u16, ply: usize) -> usize {
    if mv == tt_move {
        return 0;
    }
    let target = move_target(mv);
    let target_mask = bit(target);
    let goal = if position.side == 1 { RANK_8 } else { RANK_1 };
    if target_mask & goal != 0 {
        return 1;
    }
    let side_index = usize::from(position.side != 1);
    let enemy = if position.side == 1 {
        position.black
    } else {
        position.white
    };
    if target_mask & enemy != 0 {
        return 2;
    }
    if searcher.killers[ply][0] == mv {
        return 3;
    }
    if searcher.killers[ply][1] == mv {
        return 4;
    }
    if searcher.history[side_index][mv as usize] > 0 {
        return 5;
    }
    let row = target as usize / 8;
    let advance = if position.side == 1 { row } else { 7 - row };
    if advance >= 5 {
        6
    } else {
        7
    }
}

fn order_moves(
    searcher: &Searcher,
    position: Position,
    moves: &mut MoveList,
    tt_move: u16,
    ply: usize,
) {
    const STAGE_COUNT: usize = 8;
    let mut stages = [0_u8; 48];
    let mut counts = [0_usize; STAGE_COUNT];
    for (index, stage_slot) in stages.iter_mut().enumerate().take(moves.len) {
        let stage = move_stage(searcher, position, moves.moves[index], tt_move, ply);
        *stage_slot = stage as u8;
        counts[stage] += 1;
    }
    let mut offsets = [0_usize; STAGE_COUNT];
    for stage in 1..STAGE_COUNT {
        offsets[stage] = offsets[stage - 1] + counts[stage - 1];
    }
    let mut ordered = [0_u16; 48];
    for (index, stage) in stages.iter().enumerate().take(moves.len) {
        let stage = *stage as usize;
        ordered[offsets[stage]] = moves.moves[index];
        offsets[stage] += 1;
    }
    moves.moves[..moves.len].copy_from_slice(&ordered[..moves.len]);
}

fn move_to_front(moves: &mut MoveList, mv: u16) {
    if let Some(index) = moves.moves[..moves.len].iter().position(|item| *item == mv) {
        moves.moves[..=index].rotate_right(1);
    }
}

fn score_to_table(score: i32, ply: usize) -> i32 {
    if score > WIN - MAX_PLY as i32 {
        score + ply as i32
    } else if score < -WIN + MAX_PLY as i32 {
        score - ply as i32
    } else {
        score
    }
}

fn score_from_table(score: i32, ply: usize) -> i32 {
    if score > WIN - MAX_PLY as i32 {
        score - ply as i32
    } else if score < -WIN + MAX_PLY as i32 {
        score + ply as i32
    } else {
        score
    }
}

fn configured_time_limit() -> Duration {
    let timeout_ms = std::env::var("BREAKTHROUGH_TIMEOUT_MS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(200);
    let margin_ms = (timeout_ms / 5).max(20).min(timeout_ms.saturating_sub(1));
    Duration::from_millis(timeout_ms.saturating_sub(margin_ms).max(1))
}

fn parse_state(state: &str) -> Result<Position, &'static str> {
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
        for (column, cell) in cells.bytes().enumerate() {
            let square = (row * 8 + column) as u16;
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
    position.key = calculate_key(position);
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
        let column = source as i32 % 8;
        let next_row = row + direction;
        if !(0..8).contains(&next_row) {
            continue;
        }
        let forward = (next_row * 8 + column) as u32;
        if (own | enemy) & (1_u64 << forward) == 0 {
            result.push(encode_move(source, forward));
            if row == start_row {
                let target = ((row + 2 * direction) * 8 + column) as u32;
                if (own | enemy) & (1_u64 << target) == 0 {
                    result.push(encode_move(source, target));
                }
            }
        }
        for delta in [-1, 1] {
            let target_column = column + delta;
            if !(0..8).contains(&target_column) {
                continue;
            }
            let target = (next_row * 8 + target_column) as u32;
            if enemy & (1_u64 << target) != 0 {
                result.push(encode_move(source, target));
            }
        }
    }
    result.sort_unstable();
    result
}

fn run_self_test() -> bool {
    let mut random = 0xa73c_5f19_d428_be60_u64;
    for index in 0..10_000 {
        random = random
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1);
        let white = random;
        random = random
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1);
        let mut position = Position {
            white,
            black: random & !white,
            key: 0,
            side: if index % 2 == 0 { 1 } else { -1 },
        };
        position.key = calculate_key(position);
        let generated = generate_moves(position);
        let mut actual = generated.moves[..generated.len].to_vec();
        actual.sort_unstable();
        if actual != reference_moves(position) {
            return false;
        }
        for mv in &generated.moves[..generated.len] {
            let child = apply_move(position, *mv);
            if child.key != calculate_key(child) {
                return false;
            }
        }
    }
    true
}

fn benchmark() -> Result<(), String> {
    let position =
        parse_state("WWWWWWWW/WWWWWWWW/......../......../......../......../BBBBBBBB/BBBBBBBB W")?;
    let mut searcher = Searcher::new();
    let mv = searcher
        .choose_move(position, Duration::from_secs(1))
        .ok_or_else(|| "no legal move".to_string())?;
    let elapsed_seconds = searcher.elapsed.as_secs_f64();
    let nodes_per_second = if elapsed_seconds > 0.0 {
        (searcher.nodes as f64 / elapsed_seconds) as u64
    } else {
        0
    };
    println!(
        "move={} depth={} nodes={} nps={} elapsed_ms={}",
        move_to_text(mv),
        searcher.completed_depth,
        searcher.nodes,
        nodes_per_second,
        searcher.elapsed.as_millis()
    );
    Ok(())
}

fn run_protocol() -> Result<(), String> {
    let stdin = io::stdin();
    let mut input = stdin.lock();
    let stdout = io::stdout();
    let mut output = io::BufWriter::new(stdout.lock());
    let mut state = String::new();
    let mut history = String::new();
    let mut searcher = Searcher::new();
    let time_limit = configured_time_limit();

    loop {
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
            .choose_move(position, time_limit)
            .ok_or_else(|| "no legal move".to_string())?;
        writeln!(output, "{}", move_to_text(mv)).map_err(|error| error.to_string())?;
        output.flush().map_err(|error| error.to_string())?;
    }
}

fn main() {
    let mut args = std::env::args();
    let program = args
        .next()
        .unwrap_or_else(|| "breakthrough_speed_bot".to_string());
    let result = match (args.next().as_deref(), args.next()) {
        (None, None) => run_protocol(),
        (Some("--self-test"), None) => {
            if run_self_test() {
                println!("self-test passed");
                Ok(())
            } else {
                Err("self-test failed".to_string())
            }
        }
        (Some("--benchmark"), None) => benchmark(),
        _ => Err(format!("Usage: {program} [--self-test|--benchmark]")),
    };
    if let Err(error) = result {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn optimized_generation_and_hashes_match_references() {
        assert!(run_self_test());
    }
}
