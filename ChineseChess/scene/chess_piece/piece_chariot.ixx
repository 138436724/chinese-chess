export module piece_chariot;

import std;
import piece_base;

export class piece_chariot :public piece_base
{
public:
	piece_chariot() = default;
	~piece_chariot() = default;

	void create(const vulkan_application* _app, bool _use_color, bool _piece_color) override;

	glm::u8vec2 move_by(wchar_t _direction, uint8_t _move_number) override;
	std::pair<wchar_t, uint8_t> move_to(const glm::u8vec2& _new_location) override;

private:

};