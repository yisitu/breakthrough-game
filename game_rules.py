"""Authoritative rules and coordinate conversion for 8x8 Breakthrough.

Rows are zero-based ranks: row 0 is White's home rank and row 7 is Black's.
White moves toward increasing rows; Black moves toward decreasing rows.
Columns 0 through 7 correspond to files ``a`` through ``h``.
"""

BOARD_SIZE = 8
WHITE = 'W'
BLACK = 'B'
EMPTY = '.'

Square = tuple[int, int]
Move = tuple[Square, Square]


class BreakthroughBoard:
    """Mutable board state with legal-move and terminal-state validation."""

    def __init__(self):
        self.board = [
            [EMPTY for _ in range(BOARD_SIZE)] for _ in range(BOARD_SIZE)
        ]
        for row in (0, 1):
            self.board[row] = [WHITE] * BOARD_SIZE
        for row in (6, 7):
            self.board[row] = [BLACK] * BOARD_SIZE
        self.turn = WHITE

    def to_fen_like(self) -> str:
        """Converts board to a single-line string representation."""
        rows = ["".join(row) for row in self.board]
        return f"{'/'.join(rows)} {self.turn}"

    def to_ascii(self) -> str:
        """Returns the current board in a human-readable orientation."""
        lines = ["  a b c d e f g h"]
        for row in range(7, -1, -1):
            lines.append(f"{row + 1} " + " ".join(self.board[row]))
        lines.append(f"Turn: {self.turn}")
        return "\n".join(lines)

    def parse_move(self, move_str: str) -> Move | None:
        """Parses exact UCI text into ``(source, target)`` coordinates."""
        if not isinstance(move_str, str):
            return None
        move = move_str.strip().lower()
        if len(move) != 4:
            return None

        valid_files = "abcdefgh"
        valid_ranks = "12345678"
        if (move[0] not in valid_files or move[2] not in valid_files or
                move[1] not in valid_ranks or move[3] not in valid_ranks):
            return None

        source_column = ord(move[0]) - ord('a')
        source_row = int(move[1]) - 1
        target_column = ord(move[2]) - ord('a')
        target_row = int(move[3]) - 1
        return (source_row, source_column), (target_row, target_column)

    def get_legal_moves(self, player: str) -> list[Move]:
        """Returns forward moves and forward-diagonal captures for ``player``."""
        moves: list[Move] = []
        direction = 1 if player == WHITE else -1
        enemy = BLACK if player == WHITE else WHITE
        start_row = 1 if player == WHITE else 6

        for row in range(BOARD_SIZE):
            for column in range(BOARD_SIZE):
                if self.board[row][column] != player:
                    continue

                next_row = row + direction
                if (0 <= next_row < BOARD_SIZE and
                        self.board[next_row][column] == EMPTY):
                    moves.append(((row, column), (next_row, column)))

                    # The two-square opening move also requires the one-square
                    # path above to be clear.
                    double_row = row + 2 * direction
                    if (row == start_row and
                            self.board[double_row][column] == EMPTY):
                        moves.append(((row, column), (double_row, column)))

                for column_delta in (-1, 1):
                    target_column = column + column_delta
                    if (0 <= next_row < BOARD_SIZE and
                            0 <= target_column < BOARD_SIZE and
                            self.board[next_row][target_column] == enemy):
                        moves.append(((row, column), (next_row, target_column)))

        return moves

    def make_move(self, move: Move) -> bool:
        """Applies a legal move and changes turns; rejects all other moves."""
        (source_row, source_column), (target_row, target_column) = move
        if move not in self.get_legal_moves(self.turn):
            return False

        moving_piece = self.board[source_row][source_column]
        self.board[target_row][target_column] = moving_piece
        self.board[source_row][source_column] = EMPTY
        self.turn = BLACK if self.turn == WHITE else WHITE
        return True

    def check_winner(self) -> str | None:
        """First pawn to reach enemy back rank wins."""
        if WHITE in self.board[7]:
            return WHITE
        if BLACK in self.board[0]:
            return BLACK

        # A player with no legal move loses even if pawns remain.
        if not self.get_legal_moves(self.turn):
            return BLACK if self.turn == WHITE else WHITE
        return None
