export module piece_chariot;

import std;
import chess_piece;

export class piece_chariot :public chess_piece
{
public:
	piece_chariot() = default;
	~piece_chariot() = default;

	void set_piece_color(bool _use_color, bool _piece_color) override;

private:

};