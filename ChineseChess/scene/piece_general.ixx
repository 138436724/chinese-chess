export module piece_general;

import std;
import chess_piece;

export class piece_general :public chess_piece
{
public:
	piece_general() = default;
	~piece_general() = default;

	void set_piece_color(bool _use_color, bool _piece_color) override;

private:

};