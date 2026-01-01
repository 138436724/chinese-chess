export module piece_guard;

import std;
import chess_piece;

export class piece_guard :public chess_piece
{
public:
	piece_guard() = default;
	~piece_guard() = default;

	void set_piece_color(bool _use_color, bool _piece_color) override;

private:

};