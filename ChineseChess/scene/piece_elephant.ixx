export module piece_elephant;

import std;
import chess_piece;

export class piece_elephant :public chess_piece
{
public:
	piece_elephant() = default;
	~piece_elephant() = default;

	void set_piece_color(bool _use_color, bool _piece_color) override;

private:

};