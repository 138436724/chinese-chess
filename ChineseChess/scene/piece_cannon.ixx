export module piece_cannon;

import std;
import chess_piece;

export class piece_cannon :public chess_piece
{
public:
	piece_cannon() = default;
	~piece_cannon() = default;

	void set_piece_color(bool _use_color, bool _piece_color) override;

private:

};