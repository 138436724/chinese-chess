module piece_general;

void piece_general::set_piece_color(bool _use_color, bool _piece_color)
{
	use_color = _use_color;
	piece_color = _piece_color;

	if (piece_color)
	{
		piece_name = L"帥";
	}
	else
	{
		piece_name = L"將";
	}
}