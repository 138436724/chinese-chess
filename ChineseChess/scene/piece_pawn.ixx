export module piece_pawn;

import std;
import chess_piece;

export class piece_pawn :public chess_piece
{
public:
	piece_pawn() = default;
	~piece_pawn() = default;

	void set_piece_color(bool _use_color, bool _piece_color) override;

private:

};