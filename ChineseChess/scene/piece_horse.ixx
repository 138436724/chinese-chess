export module piece_horse;

import std;
import chess_piece;

export class piece_horse :public chess_piece
{
public:
	piece_horse() = default;
	~piece_horse() = default;

	void set_piece_color(bool _use_color, bool _piece_color) override;

private:

};