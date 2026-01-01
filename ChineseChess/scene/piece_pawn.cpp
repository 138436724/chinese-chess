module piece_pawn;

void piece_pawn::set_piece_color(bool _use_color, bool _piece_color)
{
	use_color = _use_color;
	piece_color = _piece_color;

	if (piece_color)
	{
		piece_name = L"兵";
	}
	else
	{
		piece_name = L"卒";
	}
}